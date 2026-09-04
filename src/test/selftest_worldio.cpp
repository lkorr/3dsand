// selftest_worldio.cpp — worldio selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/brush.h"
#include "game/persist.h"
#include "game/player.h"
#include "gpu/rhi.h"
#include "sim/chunkstore.h"
#include "sim/pagetable.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- save-load ---------------------------------------------------------
Status GateSaveLoad(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
// M2 save/load: snapshot at tick 100, diverge 50 ticks, load — the world
// hash must return exactly to the snapshot value (stamp bytes excluded).
bool saveOk = false;
{
  const char* kPath = "selftest_world.svd";
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 3000;
  for (int i = 0; i < 100; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i, kDefaultSeed), {}, {}, false,
               {8, 3, 8}, false, false);
  ctx.WaitIdle();
  uint32_t h1 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  bool saved = SaveWorld(ctx, world, stream, kPath, c.mats);
  for (int i = 100; i < 150; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i, kDefaultSeed), {}, {}, false,
               {8, 3, 8}, false, false);
  ctx.WaitIdle();
  uint32_t hDiverged = HashWorldNow(ctx, world, sim, kDefaultSeed);
  bool loaded = LoadWorld(ctx, world, sim, stream, kPath, c.mats);
  uint32_t h2 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  saveOk = saved && loaded && h1 == h2 && h1 != hDiverged;
  std::printf("save/load: %s (hash %08x -> diverged %08x -> restored %08x)\n",
              saveOk ? "PASS" : "FAIL", h1, hDiverged, h2);
  stream.Store().Unbind();  // detach before deleting the directory
  std::filesystem::remove_all(kPath);
}

  // Verdict: the flag the moved body already computed.
  return saveOk ? Status::Pass : Status::Fail;
}

// ---- save-entities -----------------------------------------------------
// The entities.sve round-trip (worldio.h): spawn debris + a mob + an avatar,
// sever and carve, save, WRECK the live state, load — and require the state
// back: body counts and poses, the mob's missing arm, the carved limb's
// missing voxels, the avatar's missing part. Plus the format's two contracts:
// an UNKNOWN section id must be skipped (forward compat), and a meta.svm
// whose kVoxelMeters or material table disagrees must be REFUSED.
Status GateSaveEntities(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  Stream& stream = c.stream;
  const char* kPath = "selftest_entities.svd";

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  debris.Reset();
  mobs.Reset();

  // Anchor to the LIVE window origin (chunks), never a fixed world position —
  // an earlier gate may have left the window elsewhere (selftest.h).
  IVec3 wo = world.WindowOrigin();
  const int bx = wo.x * (int)kChunk + 120;
  const int bz = wo.z * (int)kChunk + 120;
  int h = World::TerrainHeight(bx, bz, kDefaultSeed);

  std::vector<float> dens;
  for (const auto& m : c.mats) dens.push_back((float)m.gpu.density);

  // A local avatar, like the avatar gate: Ctx carries no avatar, and the gate
  // must exercise the AVTR section against the same def the game wears.
  PlayerAvatar avatar;
  avatar.Init(&phys, &world, &debris, c.mats, &c.mobs);
  avatar.SetDefs(&mobs.Defs(), kAvatarDefName);
  Player pl;
  pl.fly = false;
  pl.grounded = true;
  pl.pos = Vec3{(float)bx + 8.5f, (float)(h + 2) + Player::kHalfY,
                (float)bz + 8.5f};

  uint32_t t = 11000;
  auto entTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    if (avatar.Spawned())
      avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               {bx / 16, h / 16, bz / 16}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
    if (avatar.Spawned()) avatar.PostStep();
  };

  // --- build the scene: one plain debris body ---
  std::vector<DebrisVoxel> cube;
  for (int z = 0; z < 4; z++)
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
        cube.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
  BodyTransform cxf{};
  cxf.pos = Vec3{(float)bx, (float)(h + 3), (float)bz};
  cxf.quat[3] = 1;
  uint64_t ch = phys.CreateDebrisBody(cube, {bx, h + 3, bz}, dens);
  debris.AdoptBody(ch, cube, cxf);

  // --- a mob, with one arm severed and its torso carved ---
  int dummyDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == "dummy") dummyDef = (int)i;
  uint64_t mobId = 0;
  int carveLimb = -1, severLimb = -1;
  uint32_t carvedVox = 0, carvedVoxAtSpawn = 0, limbBodiesBefore = 0;
  if (dummyDef >= 0) {
    const MobDef& dd = mobs.Defs()[dummyDef];
    mobId = mobs.Spawn(dummyDef, {bx - 8, h + 1, bz - 8});
    for (size_t i = 0; i < dd.limbs.size(); i++) {
      if (dd.limbs[i].name == "torso") carveLimb = (int)i;
      if (dd.limbs[i].name == "arm.L") severLimb = (int)i;
    }
    for (int i = 0; i < 5; i++) entTick();
    if (mobId && severLimb >= 0) mobs.Sever(mobId, severLimb);
    if (mobId && carveLimb >= 0) {
      // A small ragged-free bite out of the torso corner: enough to change
      // the lattice, far below the 25% collapse threshold.
      std::vector<ParticleSpawn> cs;
      mobs.CarveLimbRadial(mobs.LimbBody(mobId, carveLimb),
                           mobs.LimbVoxelPos(mobId, carveLimb, 0), 1.2f,
                           false, false, world, cs);
      carvedVox = mobs.LimbVoxelCount(mobId, carveLimb);
      carvedVoxAtSpawn = mobs.LimbVoxelsAtSpawn(mobId, carveLimb);
    }
  }

  // --- the avatar, with one severable part taken off ---
  bool haveAvatar = avatar.HasDef() && avatar.Spawn(pl, 0.0f);
  int avLiveBefore = 0, avSevered = -1;
  int32_t avHealthBefore = 0;
  if (haveAvatar) {
    const MobDef& ad = *avatar.Def();
    for (size_t i = 0; i < ad.limbs.size(); i++)
      if ((int)i != ad.rootLimb && ad.limbs[i].severable && !ad.limbs[i].vital &&
          ad.limbs[i].tag == "arm") {
        avSevered = (int)i;
        break;
      }
    if (avSevered >= 0) avatar.Sever(avSevered);
    avatar.SpendHealth(10);  // distributed hp damage, so hp round-trips too
    avLiveBefore = avatar.LivePartCount();
  }

  // Let severed pieces land so the save is not full of mid-air bodies.
  for (int i = 0; i < 45; i++) entTick();
  // The hp reading is taken HERE, after the settle ticks and immediately
  // before the save: the severed arm left an open stump and the overcast a
  // bleed budget, and since blood is hp (sim/tuning.h Gore §F) those 45 ticks
  // drain it. What must round-trip is what was saved, not what the avatar had
  // before it bled for a second and a half.
  if (haveAvatar) avHealthBefore = avatar.TotalHealth();

  const uint32_t debrisBefore = debris.BodyCount();
  limbBodiesBefore = mobs.LimbBodyCount();
  if (mobId && carveLimb >= 0) carvedVox = mobs.LimbVoxelCount(mobId, carveLimb);
  BodyTransform body0Before{};
  phys.GetTransform(debris.BodyHandle(0), body0Before);

  EntityIO eio = MakeEntityIO(debris, mobs, &avatar);
  bool saved = SaveWorld(ctx, world, stream, kPath, c.mats, &eio);

  // --- wreck the live state, so a "pass" cannot be leftovers ---
  debris.Reset();
  mobs.Reset();
  avatar.Despawn();

  bool loaded = LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio);

  // --- everything came back? ---
  bool countsOk = debris.BodyCount() == debrisBefore &&
                  mobs.LimbBodyCount() == limbBodiesBefore &&
                  (dummyDef < 0 || mobs.MobCount() == 1);
  // Rigidbodies reload ASLEEP at their saved pose (worldio.h rule).
  bool asleepOk = debris.ActiveBodyCount() == 0;
  BodyTransform body0After{};
  bool poseOk = phys.GetTransform(debris.BodyHandle(0), body0After) &&
                (body0After.pos - body0Before.pos).len() < 0.05f;
  bool carveOk = true;
  if (mobId && carveLimb >= 0) {
    uint64_t newId = mobs.MobIdAt(0);
    carveOk = newId != 0 &&
              mobs.LimbVoxelCount(newId, carveLimb) == carvedVox &&
              carvedVox < carvedVoxAtSpawn;  // the carve was real AND survived
  }
  bool avatarOk = true;
  if (haveAvatar) {
    // The load reset despawned the avatar; respawning applies the saved
    // damage state (avatar.h) — the severed arm must stay gone, hp must hold.
    avatarOk = avatar.Spawn(pl, 0.0f) &&
               avatar.LivePartCount() == avLiveBefore &&
               (avSevered < 0 || !avatar.PartAlive(avSevered)) &&
               avatar.TotalHealth() == avHealthBefore;
  }

  // --- forward compat: an UNKNOWN section id must be skipped, not fatal ---
  bool unknownOk = false;
  {
    const std::string ent = std::string(kPath) + "/entities.sve";
    FILE* fp = std::fopen(ent.c_str(), "rb");
    std::vector<uint8_t> buf;
    if (fp) {
      std::fseek(fp, 0, SEEK_END);
      buf.resize((size_t)std::ftell(fp));
      std::fseek(fp, 0, SEEK_SET);
      if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) buf.clear();
      std::fclose(fp);
    }
    if (buf.size() >= 8) {
      uint32_t count = 0;
      std::memcpy(&count, buf.data() + 4, 4);
      count += 1;
      std::memcpy(buf.data() + 4, &count, 4);
      // Append a section from "the future": id 'ZZZZ', version 9, 4 bytes.
      const uint32_t zid = 0x5A5A5A5Au, zver = 9, zlen = 4, zpay = 0xDEADBEEF;
      auto app = [&](const void* p) {
        buf.insert(buf.end(), (const uint8_t*)p, (const uint8_t*)p + 4);
      };
      app(&zid);
      app(&zver);
      app(&zlen);
      app(&zpay);
      fp = std::fopen(ent.c_str(), "wb");
      if (fp) {
        std::fwrite(buf.data(), 1, buf.size(), fp);
        std::fclose(fp);
        unknownOk = LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio) &&
                    debris.BodyCount() == debrisBefore;
      }
    }
  }

  // --- a mismatched meta.svm must be REFUSED, and say why ---
  // meta layout (worldio.cpp): magic[0..3] N[4..7] chunk[8..11] vmBits[12..15]
  // origin[16..27] matCount[28..31] len0[32..35] name0[36..].
  bool refuseOk = false;
  {
    const std::string metaPath = std::string(kPath) + "/meta.svm";
    auto flipByteAndTryLoad = [&](size_t offset) {
      FILE* fp = std::fopen(metaPath.c_str(), "rb");
      if (!fp) return false;
      std::vector<uint8_t> meta;
      std::fseek(fp, 0, SEEK_END);
      meta.resize((size_t)std::ftell(fp));
      std::fseek(fp, 0, SEEK_SET);
      bool ok = std::fread(meta.data(), 1, meta.size(), fp) == meta.size();
      std::fclose(fp);
      if (!ok || offset >= meta.size()) return false;
      std::vector<uint8_t> bad = meta;
      bad[offset] ^= 0x1;
      fp = std::fopen(metaPath.c_str(), "wb");
      if (!fp) return false;
      std::fwrite(bad.data(), 1, bad.size(), fp);
      std::fclose(fp);
      bool refused = !LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio);
      fp = std::fopen(metaPath.c_str(), "wb");  // restore the good meta
      if (fp) {
        std::fwrite(meta.data(), 1, meta.size(), fp);
        std::fclose(fp);
      }
      return refused;
    };
    bool vmRefused = flipByteAndTryLoad(12);   // kVoxelMeters bit pattern
    bool matRefused = flipByteAndTryLoad(36);  // first material's name
    refuseOk = vmRefused && matRefused;
  }

  // --- rule 2: the loaded world still settles ---
  // (A REFUSED load returns before it touches anything — grid, store and
  // entities all keep the pre-call state — so this reload is belt-and-braces
  // for a known-good baseline, not a repair.)
  // Bodies reload asleep, but blood the save caught mid-flow keeps its chunks
  // wet for a while and terrain refreshes wake bodies resting in it by design
  // (the mob gate documents the same slow settle) — so run to rest with a
  // bounded deadline rather than asserting an instant.
  bool reloaded = LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio);
  uint32_t awakeAfter = debris.ActiveBodyCount();
  for (int i = 0; i < 1200 && !(awakeAfter == 0 && i >= 60); i++) {
    entTick();
    awakeAfter = debris.ActiveBodyCount();
  }
  bool sleepOk = reloaded && awakeAfter == 0;

  bool ok = saved && loaded && countsOk && asleepOk && poseOk && carveOk &&
            avatarOk && unknownOk && refuseOk && sleepOk;
  // counts=... was asserted right after the FIRST load; the current BodyCount
  // may legitimately be lower by now (settle-back converts long-asleep loaded
  // bodies to grid during the settle run — which is itself the loaded world
  // behaving normally).
  detail = Format(
      "saved=%d loaded=%d counts=%d(%u bodies, %u limbs) asleep=%d pose=%d "
      "carve=%d avatar=%d unknown-skip=%d refuse=%d settle=%d(awake %u)",
      saved ? 1 : 0, loaded ? 1 : 0, countsOk ? 1 : 0, debrisBefore,
      limbBodiesBefore, asleepOk ? 1 : 0, poseOk ? 1 : 0,
      carveOk ? 1 : 0, avatarOk ? 1 : 0, unknownOk ? 1 : 0, refuseOk ? 1 : 0,
      sleepOk ? 1 : 0, awakeAfter);
  std::printf("save-entities: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());

  // teardown: this gate's world dir must not leak into later gates
  avatar.Despawn();
  mobs.Reset();
  debris.Reset();
  stream.Store().Unbind();
  std::filesystem::remove_all(kPath);
  return ok ? Status::Pass : Status::Fail;
}

// ---- region-store ------------------------------------------------------
Status GateRegionStore(Ctx& c, std::string& detail) {
// region store: RAM must stay bounded past kMaxRamRegions (LRU spill to
// region files) and spilled chunks must read back from disk intact.
bool storeOk = false;
{
  const char* kDir = "selftest_store.svd";
  ChunkStore cs;
  storeOk = cs.BindSave(kDir);
  const size_t kRegions = ChunkStore::kMaxRamRegions + 16;
  for (size_t i = 0; i < kRegions; i++) {
    // one chunk per region: a full-chunk run of a per-region material
    std::vector<uint32_t> rle = {(uint32_t)kChunkVol,
                                 (uint32_t)(kMatStone + (i % 3))};
    cs.Put({(int)i * 16, 0, 0}, std::move(rle));
  }
  size_t ramAfterPuts = cs.Count();
  for (size_t i = 0; i < kRegions && storeOk; i++) {
    const std::vector<uint32_t>* rle = cs.Get({(int)i * 16, 0, 0});
    storeOk = rle && rle->size() == 2 && (*rle)[0] == (uint32_t)kChunkVol &&
              (*rle)[1] == (uint32_t)(kMatStone + (i % 3));
  }
  storeOk = storeOk && ramAfterPuts <= ChunkStore::kMaxRamRegions;
  std::printf("region store: %s (%zu regions written, %zu chunks in RAM "
              "after puts)\n",
              storeOk ? "PASS" : "FAIL", kRegions, ramAfterPuts);
  cs.Unbind();
  std::filesystem::remove_all(kDir);
}

  // Verdict: the flag the moved body already computed.
  return storeOk ? Status::Pass : Status::Fail;
}

// ---- streaming ---------------------------------------------------------
Status GateStreaming(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
  Stream& stream = c.stream;
// M2/M7 streaming: (a) slide the residency window +X across many shifts,
// twice — the per-tick hash sequences must match exactly (streaming +
// procgen are deterministic); (b) edit a far chunk, walk past its eviction,
// walk back, and verify the edit survived the store roundtrip.
bool streamOk = false;
{
  std::vector<uint32_t> shash[2];
  // P5-I instrument: per-tick record of run 0, so a paged-vs-dense diff names
  // the FIRST divergent tick instead of only folding to a different word.
  // Env-gated (`SANDVOX_SEQ_DUMP=<path>`); costs nothing when unset and
  // changes no behaviour when set.
  struct SeqRow { uint32_t tick; int chunkX; uint32_t shifts; int32_t originX;
                  uint32_t hash; };
  std::vector<SeqRow> seqRows;
  const char* digEnv = std::getenv("SANDVOX_SEQ_DIGEST");
  const int digestAt = digEnv ? std::atoi(digEnv) : -1;
  const char* digestOut = std::getenv("SANDVOX_SEQ_DIGEST_OUT")
                              ? std::getenv("SANDVOX_SEQ_DIGEST_OUT")
                              : "seq_digest.txt";
  // SANDVOX_SEQ_WORDS=<slot,slot,...>: with SEQ_DIGEST, dump those slots'
  // 4,096 words verbatim beside the digest, so the two modes can be diffed
  // cell by cell.
  std::vector<uint32_t> wantWords;
  if (const char* ws = std::getenv("SANDVOX_SEQ_WORDS")) {
    const char* q = ws;
    while (*q) {
      wantWords.push_back((uint32_t)strtoul(q, (char**)&q, 10));
      while (*q == ',' || *q == ' ') q++;
    }
  }
  for (int run = 0; run < 2; run++) {
    stream.OnRegen();
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 5000;
    for (int i = 0; i < 300; i++) {
      IVec3 pc{8 + i / 10, 8, 8};  // one chunk every 10 ticks -> 30 shifts
      stream.Update(pc, t);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, true, pc,
                 false, false);
      const uint32_t hh = ReadHashSync(ctx, world);
      shash[run].push_back(hh);
      if (run == 0)
        seqRows.push_back({t, pc.x, stream.ShiftCount(),
                           world.WindowOrigin().x, hh});
      // P5-I: per-chunk digest at ONE tick (PLAN_page_table.md 9.4's
      // dump-and-diff), so a paged run and a dense run name the divergent
      // SLOT instead of a different fold. Two digests per slot: `raw` over
      // every word, and `hsh` over exactly what sim_occupancy's hash sees
      // (non-air cells only, material+state+stain, stamp and excite bits
      // excluded) - the pair separates "the words differ" from "the words
      // agree and the hash of them does not".
      if (run == 0 && digestAt >= 0 && i == digestAt) {
        ctx.WaitIdle();
        const uint32_t poolPages = world.PoolPages();
        std::vector<uint32_t> slotOfPage(poolPages, 0xFFFFFFFFu);
        for (uint32_t sl = 0; sl < kNumChunks; sl++) {
          const uint32_t e = world.PageEntryOfSlot(sl);
          if ((e & kPtSentinelBit) == 0u && e < poolPages) slotOfPage[e] = sl;
        }
        std::vector<uint32_t> digRaw(kNumChunks, 2166136261u);
        std::vector<uint32_t> digHsh(kNumChunks, 2166136261u);
        std::vector<uint32_t> occ(kNumChunks, 0u);
        std::map<uint32_t, std::vector<uint32_t>> gotWords;
        auto fold = [](uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; };
        auto digestWords = [&](uint32_t sl, const uint32_t* w) {
          uint32_t r = 2166136261u, q = 2166136261u, n = 0;
          for (uint32_t k = 0; k < kChunkVol; k++) {
            r = fold(r, w[k]);
            if ((w[k] & 0xFFFu) != 0u) {
              n++;
              const uint32_t v =
                  (w[k] & 0xFFFFu) | ((w[k] & kStainBits) >> 8u);
              q = fold(fold(q, k), v);
            }
          }
          digRaw[sl] = r; digHsh[sl] = q; occ[sl] = n;
          for (uint32_t wsl : wantWords)
            if (wsl == sl) gotWords[sl].assign(w, w + kChunkVol);
        };
        // resident pages, read back in 2048-page batches (32 MiB each)
        const uint32_t kBatch = 2048;
        std::vector<uint32_t> buf((size_t)kBatch * kChunkVol);
        for (uint32_t p0 = 0; p0 < poolPages; p0 += kBatch) {
          const uint32_t n = std::min(kBatch, poolPages - p0);
          bool any = false;
          for (uint32_t k = 0; k < n; k++)
            if (slotOfPage[p0 + k] != 0xFFFFFFFFu) any = true;
          if (!any) continue;
          if (!rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels,
                                     (uint64_t)p0 * kChunkVol * 4, buf.data(),
                                     (size_t)n * kChunkVol * 4, "p5idig"))
            continue;
          for (uint32_t k = 0; k < n; k++)
            if (slotOfPage[p0 + k] != 0xFFFFFFFFu)
              digestWords(slotOfPage[p0 + k], buf.data() + (size_t)k * kChunkVol);
        }
        // sentinel slots: synthesize what they would materialize into
        std::vector<uint32_t> syn(kChunkVol);
        for (uint32_t sl = 0; sl < kNumChunks; sl++) {
          const uint32_t e = world.PageEntryOfSlot(sl);
          if ((e & kPtSentinelBit) == 0u) continue;
          const IVec3 wc = world.SlotToWorldChunk(sl);
          const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                    bz = wc.z * (int)kChunk;
          for (uint32_t k = 0; k < kChunkVol; k++)
            syn[k] = SynthWordAt(e, bx + (int)(k % kChunk),
                                 by + (int)((k / kChunk) % kChunk),
                                 bz + (int)(k / (kChunk * kChunk)),
                                 world.pages->WorldSeed());
          digestWords(sl, syn.data());
        }
        std::vector<uint32_t> d0(kNumChunks, 0u), d1(kNumChunks, 0u),
            occBuf(kNumChunks, 0u);
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.dirty[0], 0,
                              d0.data(), kNumChunks * 4, "p5id0");
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.dirty[1], 0,
                              d1.data(), kNumChunks * 4, "p5id1");
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.occupancy, 0,
                              occBuf.data(), kNumChunks * 4, "p5iocc");
        char dpath[512];
        std::snprintf(dpath, sizeof(dpath), "%s", digestOut);
        if (FILE* df = std::fopen(dpath, "w")) {
          std::fprintf(df, "# slot entry raw hashlike nonair wc.x wc.y wc.z d0 d1 occ tick %u hash %08x origin %d %d %d\n", t, hh, world.WindowOrigin().x, world.WindowOrigin().y, world.WindowOrigin().z);
          for (uint32_t sl = 0; sl < kNumChunks; sl++) {
            const IVec3 wc = world.SlotToWorldChunk(sl);
            std::fprintf(df, "%u %08X %08X %08X %u %d %d %d %u %u %08X\n",
                         sl, world.PageEntryOfSlot(sl), digRaw[sl], digHsh[sl],
                         occ[sl], wc.x, wc.y, wc.z, d0[sl], d1[sl], occBuf[sl]);
          }
          std::fclose(df);
          std::printf("  digest at i=%d tick %u -> %s\n", i, t, dpath);
        }
        if (!gotWords.empty()) {
          char wpath[600];
          std::snprintf(wpath, sizeof(wpath), "%s.words", digestOut);
          if (FILE* wf = std::fopen(wpath, "w")) {
            for (auto& kv : gotWords) {
              const IVec3 wc = world.SlotToWorldChunk(kv.first);
              std::fprintf(wf, "# slot %u entry %08X chunk %d %d %d\n",
                           kv.first, world.PageEntryOfSlot(kv.first), wc.x,
                           wc.y, wc.z);
              for (uint32_t k = 0; k < kChunkVol; k++)
                std::fprintf(wf, "%u %u %08X\n", kv.first, k, kv.second[k]);
            }
            std::fclose(wf);
            std::printf("  words -> %s\n", wpath);
          }
        }
      }
    }
  }
  if (const char* dumpPath = std::getenv("SANDVOX_SEQ_DUMP")) {
    if (FILE* f = std::fopen(dumpPath, "w")) {
      std::fprintf(f, "# i tick chunkX shifts originX hash\n");
      for (size_t i = 0; i < seqRows.size(); i++)
        std::fprintf(f, "%zu %u %d %u %d %08x\n", i, seqRows[i].tick,
                     seqRows[i].chunkX, seqRows[i].shifts, seqRows[i].originX,
                     seqRows[i].hash);
      std::fclose(f);
      std::printf("  seq dump -> %s (%zu ticks)\n", dumpPath, seqRows.size());
    }
  }
  bool sdet = shash[0] == shash[1];
  // ---- THE CROSS-MODE NUMBER (docs/RESEARCH_streaming_hitch.md R2) --------
  //
  // `sdet` is a TWICE-RUN comparison inside one process and one residency
  // mode: it proves the streamed world is reproducible, and it cannot say
  // anything about whether `--residency dense` produces the same world as
  // paged. That claim is the only live oracle the page table has, and until
  // this line it could only be made by inspection — the gate printed a shift
  // count and a glass-voxel count, both of which are equal across modes for
  // reasons that have nothing to do with the voxels.
  //
  // So the 300-tick hash SEQUENCE is folded into one word and printed. Two
  // runs of this gate in the two modes agree on it or they do not, and the
  // fold is over the whole sequence rather than the final hash because a
  // divergence that heals is still a divergence.
  //
  // The FIRST tick is printed beside the fold for attribution, and it is what
  // turns "the two modes disagree" into "and here is which half". Tick 1 of
  // this loop is one CA tick after a fresh SubmitWorldgen with the window at
  // the origin and BEFORE stream.Update has moved the player a whole chunk, so
  // no shift has happened yet: a `t1` that already differs across modes is a
  // worldgen/materialization difference and has nothing to do with streaming,
  // while a matching `t1` under a differing `seq` puts it in the shift path.
  uint32_t sseq = 0x811C9DC5u;
  for (uint32_t h : shash[0]) sseq = (sseq ^ h) * 0x01000193u;
  const uint32_t st1 = shash[0].empty() ? 0u : shash[0][0];

  // persistence roundtrip (live readbacks so eviction filters see reality)
  stream.OnRegen();
  world.SetWindowOrigin({0, 0, 0});
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 7000;
  auto tickAt = [&](IVec3 pc, std::vector<BrushOp> ops) {
    stream.Update(pc, t);
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, ops, {}, {}, false, pc,
               true, false);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  };
  const int ballX = 25 * 16 + 8, ballZ = 128;
  int ballH = World::TerrainHeight(ballX, ballZ, kDefaultSeed);
  IVec3 ballCell{ballX, ballH + 1, ballZ};
  IVec3 ballChunk{ballCell.x >> 4, ballCell.y >> 4, ballCell.z >> 4};
  auto walkTo = [&](int fromCx, int toCx) {
    int step = toCx > fromCx ? 1 : -1;
    for (int cx = fromCx; cx != toCx; cx += step)
      for (int k = 0; k < 10; k++) tickAt({cx, 8, 8}, {});
  };
  walkTo(8, 25);
  // glass ball half-buried at the surface (anchored: resting on the ground)
  tickAt({25, 8, 8}, {{ballCell.x, ballCell.y, ballCell.z, 3, kMatGlass, 1, 0, 0}});
  stream.MarkModifiedBox({ballCell.x - 3, ballCell.y - 3, ballCell.z - 3},
                         {ballCell.x + 3, ballCell.y + 3, ballCell.z + 3});
  for (int k = 0; k < 10; k++) tickAt({25, 8, 8}, {});
  walkTo(25, 60);  // ball chunk streams out (origin.x reaches 52 > 25)
  bool evicted = !world.ChunkInWindow(ballChunk);
  walkTo(60, 25);  // and back in
  world.RequestChunkFetch(ballChunk);
  uint32_t glass = 0;
  for (int k = 0; k < 90; k++) {
    tickAt({25, 8, 8}, {});
    const CachedChunk* cc = world.Cached(ballChunk);
    if (cc && cc->version > t - 30 && cc->voxels.size() == kChunkVol) {
      for (uint32_t w : cc->voxels)
        if ((w & 0xFFFu) == kMatGlass) glass++;
      break;
    }
    world.RequestChunkFetch(ballChunk);
  }
  // a REAL player (collision through the async mirror) flies +X far beyond
  // the original 256-box — the literal M2 exit criterion. Catches any
  // leftover fixed-world assumption in the player/collision path (a v0
  // position clamp produced exactly this bug: an invisible wall at x=254).
  bool crossed = false;
  {
    // Same COLLISION table the game builds (passable vegetation reads as
    // gas — sim/materials.h), so this gate tests real behaviour.
    std::vector<uint32_t> classOf = BuildCollisionClasses(mats);
    Player p2;
    p2.fly = true;
    // ABOVE THE GROUND, not at an absolute Y. The comment here used to say
    // "above the tallest hills (~90)", which was a second, unowned copy of the
    // terrain band — and the terrain overhaul moves that band. 26 voxels of
    // clearance is more than the flight loop needs and is measured from the
    // height contract, so it stays true at any datum.
    p2.pos = Vec3{140.5f,
                  (float)(World::TerrainHeight(140, 140, kDefaultSeed) + 26),
                  140.5f};
    auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
    PlayerInput in{};
    in.forward = 1.0f;
    in.sprint = true;
    for (int i = 0; i < 1200 && !crossed; i++) {
      IVec3 pc{ifloor(p2.pos.x) >> 4, ifloor(p2.pos.y) >> 4,
               ifloor(p2.pos.z) >> 4};
      stream.Update(pc, t);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc,
                 true, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      p2.Update(1.0f / 30.0f, in, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0},
                kindAt);
      crossed = p2.pos.x > 600.0f;
    }
    std::printf("  player flight: %s (reached x=%.0f, window origin.x=%d)\n",
                crossed ? "crossed" : "BLOCKED", (double)p2.pos.x,
                world.WindowOrigin().x);
  }

  streamOk = sdet && evicted && glass > 0 && crossed;
  std::printf("streaming: %s (hash sequences %s over %u shifts, seq %08x "
              "t1 %08x, ball chunk evicted=%d, %u glass voxels after "
              "re-entry, player crossed=%d, store %zu chunks)\n",
              streamOk ? "PASS" : "FAIL", sdet ? "match" : "DIVERGE",
              stream.ShiftCount(), sseq, st1, evicted ? 1 : 0, glass,
              crossed ? 1 : 0, stream.Store().Count());
}

  // Verdict: the flag the moved body already computed.
  return streamOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WorldIoGates() {
  static const std::vector<Gate> g = {
      {"save-load", "worldio", {}, false, GateSaveLoad},
      {"save-entities", "worldio", {}, false, GateSaveEntities},
      {"region-store", "worldio", {}, false, GateRegionStore},
      {"streaming", "worldio", {}, false, GateStreaming},
  };
  return g;
}

}  // namespace selftest
