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
#include "sim/stream.h"
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
        for (uint32_t sl = 0; sl < kNumSlots; sl++) {
          const uint32_t e = world.PageEntryOfSlot(sl);
          if ((e & kPtSentinelBit) == 0u && e < poolPages) slotOfPage[e] = sl;
        }
        std::vector<uint32_t> digRaw(kNumSlots, 2166136261u);
        std::vector<uint32_t> digHsh(kNumSlots, 2166136261u);
        std::vector<uint32_t> occ(kNumSlots, 0u);
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
        for (uint32_t sl = 0; sl < kNumSlots; sl++) {
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
        std::vector<uint32_t> d0(kNumSlots, 0u), d1(kNumSlots, 0u),
            occBuf(kNumSlots, 0u);
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.dirty[0], 0,
                              d0.data(), kNumSlots * 4, "p5id0");
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.dirty[1], 0,
                              d1.data(), kNumSlots * 4, "p5id1");
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.occupancy, 0,
                              occBuf.data(), kNumSlots * 4, "p5iocc");
        char dpath[512];
        std::snprintf(dpath, sizeof(dpath), "%s", digestOut);
        if (FILE* df = std::fopen(dpath, "w")) {
          std::fprintf(df, "# slot entry raw hashlike nonair wc.x wc.y wc.z d0 d1 occ tick %u hash %08x origin %d %d %d\n", t, hh, world.WindowOrigin().x, world.WindowOrigin().y, world.WindowOrigin().z);
          for (uint32_t sl = 0; sl < kNumSlots; sl++) {
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
    TickInput in{};
    in.forward = 1.0f;
    in.SetHeld(TB_SPRINT, true);
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


// ---- chunk-exchange ----------------------------------------------------
//
// M9.5-A: streaming's two openings onto the network (sim/stream.h's
// ChunkExchange) plus the ChunkStore tick tag that tells two machines whose
// copy of a chunk is newer.
//
// THE FAKE END. Everything here is in-process: there is no socket, no peer
// and no protocol, because none of those are what this gate is about. What
// it is about is the four claims Stream now makes to whoever IS on the other
// end, and each of them is a claim a wire implementation would otherwise only
// discover under two running processes:
//
//   (a) an eviction is REPORTED, once, with the right coordinate, the right
//       tick and the right bytes - at BOTH Put sites, which are a
//       synchronous one (sentinel chunks) and an asynchronous one (real
//       pages, ticks later through the readback);
//   (b) a refill the exchange claims is HELD INERT - no procgen, no wake,
//       reads as air - and a DeliverRemote afterwards lands the peer's words
//       so exactly that the world hash equals a control that streamed the
//       same chunk out of the store instead;
//   (c) a DeliverMiss falls back to procgen;
//   (d) the whole of (b) is reproducible, and the tick tag survives a
//       Flush/BindLoad round trip.
//
// Arm (b) is the load-bearing one and its CONTROL is the point: "the hash did
// not move" would be satisfied by doing nothing at all, so the control has
// the chunk arriving by the ORDINARY path (a store hit) and the subject has
// it arriving by the new one, with the same bytes, at the same tick, into the
// same world. Equal hashes then mean the new door installs a chunk exactly
// the way the old door does - which is the property M9.5-B's wire needs and
// cannot itself test.
struct FakeExchange : ChunkExchange {
  struct Ev {
    IVec3 wc{};
    uint32_t tick = 0;
    std::vector<uint32_t> rle;
  };
  std::vector<Ev> evicted;
  std::map<uint64_t, uint32_t> evictCount;  // packed chunk key -> times seen
  std::map<uint64_t, uint32_t> want;        // chunks to claim a peer holds
  std::vector<IVec3> requested;

  void OnEvicted(IVec3 wc, uint32_t tick,
                 const std::vector<uint32_t>& rle) override {
    const uint64_t k = World::PackChunkKey(wc);
    evictCount[k]++;
    // Only the chunks under test are kept whole: a shift plane is 1,024
    // chunks and a surface one RLEs to 32 KiB, so keeping them all would be
    // tens of megabytes for two assertions.
    if (want.count(k) || keep.count(k)) evicted.push_back({wc, tick, rle});
  }
  bool Wanted(IVec3 wc) override {
    return want.count(World::PackChunkKey(wc)) != 0;
  }
  void Request(IVec3 wc) override { requested.push_back(wc); }

  std::map<uint64_t, uint32_t> keep;  // chunks whose bytes to record
  const Ev* Find(IVec3 wc) const {
    for (const Ev& e : evicted)
      if (e.wc.x == wc.x && e.wc.y == wc.y && e.wc.z == wc.z) return &e;
    return nullptr;
  }
};

Status GateChunkExchange(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  const bool paged = world.residency == World::Residency::Paged;

  std::string fails;
  auto fail = [&](const std::string& m) {
    if (!fails.empty()) fails += "; ";
    fails += m;
  };
  // Whatever happens below, the Stream must not be left pointing at a
  // stack object of this function.
  struct Unbind {
    Stream& s;
    ~Unbind() { s.SetChunkExchange(nullptr); }
  } unbind{stream};

  uint32_t t = 9000;
  const IVec3 pc{16, 16, 16};  // the window centre: Update here never shifts
  auto tickAt = [&](const std::vector<BrushOp>& ops, IVec3 who) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, ops, {}, {},
               /*hashEnable=*/true, who, /*wantReadback=*/true,
               /*particlesActive=*/false);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  };
  auto regen = [&]() {
    stream.OnRegen();
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
  };
  // A buried chunk, MEASURED rather than assumed (the chunk-resync gate's
  // rule): two chunks under the surface, where the CA has nothing to do, so
  // the arms below compare a quiet world and not a landslide. `cx` picks the
  // column band; arm (a) needs chunk-x 0 because that plane is what the first
  // +X shift evicts.
  auto buriedChunk = [&](int cx, int& outH) {
    int bestH = -1, bestZ = 0;
    for (int cz = 4; cz <= 12; cz++) {
      const int h = World::TerrainHeight(cx * (int)kChunk + 8,
                                         cz * (int)kChunk + 8, kDefaultSeed);
      if (h > bestH) { bestH = h; bestZ = cz; }
    }
    outH = bestH;
    return IVec3{cx, (bestH >> 4) - 2, bestZ};
  };
  auto markBox = [&](IVec3 wc) {
    const IVec3 lo{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    stream.MarkModifiedBox(
        lo, {lo.x + (int)kChunk - 1, lo.y + (int)kChunk - 1,
             lo.z + (int)kChunk - 1});
  };
  // The marker: a glass ball at the chunk's centre, sealed inside stone. It
  // is what separates "the peer's copy" from "procgen ran" in every arm -
  // worldgen never puts glass underground, so one non-zero count answers the
  // question without trusting a hash to do it.
  auto glassOp = [&](IVec3 wc) {
    return BrushOp{wc.x * (int)kChunk + 8, wc.y * (int)kChunk + 8,
                   wc.z * (int)kChunk + 8, 3, (uint32_t)kMatGlass, 1, 0, 0};
  };
  auto countMat = [&](const std::vector<uint32_t>& w, uint32_t mat) {
    uint32_t n = 0;
    for (uint32_t v : w)
      if ((v & 0xFFFu) == mat) n++;
    return n;
  };
  auto nonAir = [&](const std::vector<uint32_t>& w) {
    uint32_t n = 0;
    for (uint32_t v : w)
      if ((v & 0xFFFu) != 0u) n++;
    return n;
  };

  // ======== ARM A: both eviction hooks ===================================
  uint32_t evTickSent = 0, evTickReal = 0, evGlass = 0;
  uint32_t sentCount = 0, realCount = 0, skyPairs = 0;
  {
    int h0 = 0;
    const IVec3 evictChunk = buriedChunk(0, h0);
    // A chunk of open sky in the SAME plane. It has no page at all, so it
    // leaves by the synchronous sentinel path while the buried chunk leaves
    // by the asynchronous readback one - the two Put sites, in one shift.
    const IVec3 skyChunk{0, 28, evictChunk.z};
    if (evictChunk.y < 2) {
      detail = Format("no buried chunk in plane x=0 (terrain height %d)", h0);
      std::printf("chunk-exchange: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
    regen();
    tickAt({glassOp(evictChunk)}, pc);
    markBox(evictChunk);
    markBox(skyChunk);
    for (int k = 0; k < 4; k++) tickAt({}, pc);

    FakeExchange ex;
    ex.keep[World::PackChunkKey(evictChunk)] = 1;
    ex.keep[World::PackChunkKey(skyChunk)] = 1;
    stream.SetChunkExchange(&ex);
    // ONE +X shift: the interest is 3 chunks past centre, which is past the
    // 2-chunk hysteresis on x and nothing on y/z. The leaving plane is
    // x = origin.x = 0, which is where both subjects live.
    const uint32_t shiftTick = ++t;
    stream.Update(IVec3{19, 16, 16}, shiftTick);
    // ...then run until the asynchronous half lands. The real page's bytes
    // come back through a mapped readback several ticks later, which is the
    // entire reason OnEvicted carries the eviction's OWN tick rather than the
    // clock at completion.
    for (int k = 0; k < 40 && stream.PendingEvictions() > 0; k++) {
      tickAt({}, IVec3{17, 16, 16});
      stream.Update(IVec3{17, 16, 16}, t);  // centre of the shifted window
    }
    stream.SetChunkExchange(nullptr);

    sentCount = ex.evictCount[World::PackChunkKey(skyChunk)];
    realCount = ex.evictCount[World::PackChunkKey(evictChunk)];
    const FakeExchange::Ev* sentEv = ex.Find(skyChunk);
    const FakeExchange::Ev* realEv = ex.Find(evictChunk);
    if (paged) {
      // Dense has no sentinels at all - every slot owns a page - so the
      // synchronous arm cannot exist there and asserting it would fail the
      // `--residency dense` differential for a reason that is not a bug.
      if (sentCount != 1)
        fail(Format("sentinel eviction fired %u times, want 1", sentCount));
      if (sentEv) {
        evTickSent = sentEv->tick;
        skyPairs = (uint32_t)(sentEv->rle.size() / 2);
        if (sentEv->tick != shiftTick)
          fail(Format("sentinel eviction tick %u, want %u", sentEv->tick,
                      shiftTick));
        // Open sky RLEs to a single {kChunkVol, air} run.
        if (skyPairs != 1 || sentEv->rle[0] != (uint32_t)kChunkVol ||
            (sentEv->rle[1] & 0xFFFu) != 0u)
          fail("sentinel eviction did not carry the sky chunk's RLE");
      } else {
        fail("sentinel eviction never fired");
      }
    }
    if (realCount != 1)
      fail(Format("real-page eviction fired %u times, want 1", realCount));
    if (realEv) {
      evTickReal = realEv->tick;
      if (realEv->tick != shiftTick)
        fail(Format("real-page eviction tick %u, want %u (the tick the "
                    "eviction was DECIDED at, not the harvest tick)",
                    realEv->tick, shiftTick));
      std::vector<uint32_t> w(kChunkVol, 0u);
      if (RleDecodeChunk(realEv->rle.data(), realEv->rle.size() / 2, w.data()))
        evGlass = countMat(w, (uint32_t)kMatGlass);
      if (evGlass == 0)
        fail("the evicted RLE did not carry the chunk's edit");
    } else {
      fail("real-page eviction never fired");
    }
  }

  // ======== ARMS B/C/D: the hold, the delivery and the miss ==============
  //
  // ONE SCENE, THREE MODES, identical in every respect that reaches the
  // world: the same worldgen, the same edit, the same authority bytes, the
  // same tick schedule. The only difference is HOW the chunk gets back into
  // the window (CLAUDE.md rule 7's "run both arms at the same scope").
  enum Mode { kControl = 0, kDeliver = 1, kMiss = 2 };
  struct ArmOut {
    uint32_t hash = 0;
    uint32_t glass = 0;      // marker voxels in the subject at the end
    uint32_t heldNonAir = 0; // non-air in the subject WHILE held
    uint32_t heldEntry = 0;  // its page-table entry while held
    uint32_t requested = 0;
    Stream::ExchangeStats stats;
  };
  int hB = 0;
  const IVec3 subject = buriedChunk(8, hB);
  if (subject.y < 2) {
    detail = Format("no buried chunk in plane x=8 (terrain height %d)", hB);
    std::printf("chunk-exchange: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const uint32_t slot = World::SlotChunkIndex(subject);
  const uint32_t kArmTicks = 24;
  const uint32_t kDeliverTick = 4242;  // an arbitrary tag, checked below

  auto runArm = [&](Mode mode, ArmOut& o) {
    // ---- phase 1: build the authority's copy ----------------------------
    stream.SetChunkExchange(nullptr);
    regen();
    t = 9000;
    tickAt({glassOp(subject)}, pc);
    markBox(subject);
    for (int k = 0; k < 4; k++) tickAt({}, pc);
    std::vector<uint32_t> words(kChunkVol, 0u);
    ReadVoxelsSync(ctx, world, slot, 1, words.data(), "xchgCap");
    std::vector<uint32_t> rle;
    RleEncodeChunk(words.data(), rle);

    // ---- phase 2: a world with no memory of that edit -------------------
    // OnRegen empties the store, so the refill below is a genuine MISS for
    // every chunk. The control then puts the authority copy in the store
    // (the ordinary door); the other two leave the store empty and let the
    // exchange claim it (the new door).
    regen();
    FakeExchange ex;
    if (mode == kControl) {
      stream.Store().Put(subject, rle);
    } else {
      ex.want[World::PackChunkKey(subject)] = 1;
      stream.SetChunkExchange(&ex);
    }
    stream.ReloadWindow({0, 0, 0});
    if (mode != kControl) {
      o.requested = (uint32_t)ex.requested.size();
      o.heldEntry = world.PageEntryOfSlot(slot);
      std::vector<uint32_t> held(kChunkVol, 0u);
      ReadVoxelsSync(ctx, world, slot, 1, held.data(), "xchgHeld");
      o.heldNonAir = nonAir(held);
      o.stats = stream.Exchange();
    }

    // ---- phase 3: the answer, at the phase-B position -------------------
    // Before the first tick's submit, which is where session.cpp installs a
    // live sync - so the CA of tick 1 runs on the answered chunk in every
    // arm and the tick schedules stay comparable.
    if (mode == kDeliver) {
      if (!stream.DeliverRemote(subject, kDeliverTick, rle))
        fail("DeliverRemote refused a resident, held chunk");
      if (stream.Store().TickOf(subject) != kDeliverTick)
        fail("DeliverRemote did not tag the store with the delivered tick");
    } else if (mode == kMiss) {
      if (!stream.DeliverMiss(subject))
        fail("DeliverMiss did not recognise the held slot");
      // The requeued slot is generated by the NEXT Update, not by the call
      // above (a network pump may not submit). The interest is the window
      // centre, so this Update shifts nothing.
      stream.Update(pc, t);
    }
    for (uint32_t i = 0; i < kArmTicks; i++) tickAt({}, pc);
    o.hash = ReadHashSync(ctx, world);
    std::vector<uint32_t> fin(kChunkVol, 0u);
    ReadVoxelsSync(ctx, world, slot, 1, fin.data(), "xchgFin");
    o.glass = countMat(fin, (uint32_t)kMatGlass);
    if (mode != kControl) o.stats = stream.Exchange();
    stream.SetChunkExchange(nullptr);
  };

  ArmOut ctrl, deliv, deliv2, miss;
  runArm(kControl, ctrl);
  runArm(kDeliver, deliv);
  runArm(kDeliver, deliv2);  // (d): the whole of arm B, twice
  runArm(kMiss, miss);

  // (b) the hold: inert, and unmistakably not procgen.
  if (deliv.requested != 1)
    fail(Format("the hold requested %u chunks, want 1", deliv.requested));
  if (deliv.heldNonAir != 0)
    fail(Format("a held slot is not air (%u non-air voxels) - procgen ran, "
                "or the hold did not clear the slot",
                deliv.heldNonAir));
  if (paged && deliv.heldEntry != kPtEmpty)
    fail(Format("a held slot is not a PT_EMPTY sentinel (entry 0x%08x)",
                deliv.heldEntry));
  if (deliv.stats.held != 1 || deliv.stats.delivered != 1)
    fail(Format("hold/deliver counters read %llu/%llu, want 1/1",
                (unsigned long long)deliv.stats.held,
                (unsigned long long)deliv.stats.delivered));
  if (ctrl.glass == 0) fail("the control lost its own marker");
  if (deliv.glass != ctrl.glass)
    fail(Format("delivered chunk has %u marker voxels, control has %u",
                deliv.glass, ctrl.glass));
  // THE CLAIM THE WIRE NEEDS: the new door and the old door leave the same
  // world. Not "the hash did not move" - a hash that never moves is also
  // what doing nothing produces - but "the hash equals the control's".
  if (deliv.hash != ctrl.hash)
    fail(Format("delivered world hash %08x != store-streamed control %08x",
                deliv.hash, ctrl.hash));
  // (d) twice-run.
  if (deliv2.hash != deliv.hash)
    fail(Format("two runs of the delivery arm disagree: %08x vs %08x",
                deliv.hash, deliv2.hash));
  // (c) the miss regenerates: real terrain back, and NOT the peer's edit.
  if (miss.stats.missed != 1)
    fail(Format("miss counter read %llu, want 1",
                (unsigned long long)miss.stats.missed));
  if (miss.glass != 0)
    fail(Format("a missed chunk kept %u marker voxels - it was not "
                "regenerated", miss.glass));
  uint32_t missNonAir = 0;
  {
    // Re-read rather than trust the arm's own count: the claim here is that
    // procgen RAN, which is "the chunk is full of terrain again", not "the
    // marker is gone" (an empty chunk satisfies the second and not the first).
    std::vector<uint32_t> w(kChunkVol, 0u);
    ReadVoxelsSync(ctx, world, slot, 1, w.data(), "xchgMiss");
    missNonAir = nonAir(w);
    if (missNonAir == 0)
      fail("a missed chunk is still air - DeliverMiss never regenerated it");
  }

  // ======== ARM E: the tick tag round-trips ==============================
  //
  // A pure ChunkStore test beside `region-store`, because that is the scope
  // the claim lives at: the tag is persisted as manifest.svt beside the
  // regions, the `.svr` format is untouched, and an absent manifest reads as
  // tag 0 for everything.
  bool tagOk = false;
  {
    const char* kDir = "selftest_xchg.svd";
    std::filesystem::remove_all(kDir);
    const IVec3 a{3, 4, 5}, b{-9, 0, 2};
    std::vector<uint32_t> rleA = {(uint32_t)kChunkVol, (uint32_t)kMatStone};
    std::vector<uint32_t> rleB = {(uint32_t)kChunkVol, (uint32_t)kMatGlass};
    ChunkStore cs;
    tagOk = cs.BindSave(kDir);
    cs.Put(a, rleA, 7777);
    cs.Put(b, rleB);  // untagged: the single-player signature, still legal
    tagOk = tagOk && cs.TickOf(a) == 7777u && cs.TickOf(b) == 0u;
    std::vector<std::pair<IVec3, uint32_t>> man;
    cs.Manifest(man);
    tagOk = tagOk && man.size() == 2;
    tagOk = tagOk && cs.Flush();
    ChunkStore cs2;
    tagOk = tagOk && cs2.BindLoad(kDir);
    // Read through the tag map, which BindLoad filled from manifest.svt -
    // no region was touched to answer this, which is the point of the side
    // map (chunkstore.h).
    tagOk = tagOk && cs2.TickOf(a) == 7777u && cs2.TickOf(b) == 0u;
    std::vector<std::pair<IVec3, uint32_t>> man2;
    cs2.Manifest(man2);
    tagOk = tagOk && man2.size() == 2;
    uint32_t tagA = 0, tagB = 1;
    for (const auto& e : man2) {
      if (e.first.x == a.x && e.first.y == a.y && e.first.z == a.z)
        tagA = e.second;
      if (e.first.x == b.x && e.first.y == b.y && e.first.z == b.z)
        tagB = e.second;
    }
    tagOk = tagOk && tagA == 7777u && tagB == 0u;
    // ...and the region bytes still read back, which is the "no save-format
    // change" half of the claim.
    const std::vector<uint32_t>* got = cs2.Get(a);
    tagOk = tagOk && got && got->size() == 2 &&
            (*got)[1] == (uint32_t)kMatStone;
    // An ABSENT manifest is tag 0 for everything, not a load failure.
    std::filesystem::remove(std::string(kDir) + "/manifest.svt");
    ChunkStore cs3;
    tagOk = tagOk && cs3.BindLoad(kDir) && cs3.TickOf(a) == 0u;
    const std::vector<uint32_t>* got3 = cs3.Get(a);
    tagOk = tagOk && got3 && got3->size() == 2;
    cs.Unbind();
    cs2.Unbind();
    cs3.Unbind();
    std::filesystem::remove_all(kDir);
    if (!tagOk) fail("the tick tag did not survive Flush/BindLoad");
  }

  // Leave the world pristine at the origin: the gates after this one build
  // their own worlds, but none of them should inherit a shifted window.
  stream.SetChunkExchange(nullptr);
  regen();

  const bool ok = fails.empty();
  detail = Format(
      "evict sent=%u@t%u(%u pairs) real=%u@t%u(%u glass) | hold entry=%08x "
      "air=%u req=%u | hash deliver=%08x control=%08x twice=%08x | "
      // CUMULATIVE over the whole gate, not per arm: ExchangeStats lives on
      // the Stream and the arms share one, so the expected reading here is
      // h=3 (the three arms that hold) d=2 (the two delivery arms) m=1. The
      // per-arm assertions above snapshot it at the end of each arm instead.
      "miss nonair=%u glass=%u | counters(cumulative) "
      "h=%llu d=%llu m=%llu f=%llu r=%llu "
      "| tag=%d%s%s",
      sentCount, evTickSent, skyPairs, realCount, evTickReal, evGlass,
      deliv.heldEntry, deliv.heldNonAir, deliv.requested, deliv.hash,
      ctrl.hash, deliv2.hash, missNonAir, miss.glass,
      (unsigned long long)miss.stats.held,
      (unsigned long long)miss.stats.delivered,
      (unsigned long long)miss.stats.missed,
      (unsigned long long)miss.stats.forgotten,
      (unsigned long long)miss.stats.rejected, tagOk ? 1 : 0,
      ok ? "" : " | ", ok ? "" : fails.c_str());
  std::printf("chunk-exchange: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WorldIoGates() {
  static const std::vector<Gate> g = {
      {"save-load", "worldio", {}, false, GateSaveLoad},
      {"save-entities", "worldio", {}, false, GateSaveEntities},
      {"region-store", "worldio", {}, false, GateRegionStore},
      {"streaming", "worldio", {}, false, GateStreaming},
      {"chunk-exchange", "worldio", {}, false, GateChunkExchange},
  };
  return g;
}

}  // namespace selftest
