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
#include <tuple>
#include <vector>

#include "game/avatar.h"
#include "game/brush.h"
#include "game/persist.h"
#include "game/player.h"
#include "gpu/rhi.h"
#include "sim/chunkstore.h"
#include "sim/mattable.h"
#include "sim/stream.h"
#include "sim/pagetable.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- save-load ---------------------------------------------------------
//
// THREE CLAIMS, one gate (PLAN_save_system.md S1):
//
//  A. ROUND TRIP THROUGH THE DELTA. Snapshot at tick 100, diverge 50 ticks,
//     load -- the world hash must return exactly to the snapshot value. The
//     save now stores ONLY modified chunks, so this is also the proof that
//     every pristine chunk regenerates identically from (seed, generator) and
//     that the unpublished snapshot tail was folded (the last SelftestOps land
//     inside it). The gate FAILS if the delta path was not taken -- a silent
//     fallback to the full flush would pass the hash and prove nothing.
//  B. AN UNTOUCHED WORLD COSTS ALMOST NOTHING. Fresh worldgen + 30 idle ticks
//     (so gen-settle activity is included -- PLAN S2 attributes that), saved:
//     the bytes must stay under tests/baseline.json saveUntouchedMaxBytes, and
//     the reload must hash identically. SANDVOX_SAVE_MEASURE_FULL=1 also saves
//     the same world through the pre-S1 full flush and prints both numbers.
//  C. THE WORLDGEN FINGERPRINT (SVM6). The unperturbed reload reports it
//     known and matching; a meta.svm whose stored fingerprint was faked loads
//     ANYWAY with worldgenMismatch set; an SVM5-shaped meta loads with the
//     fingerprint unknown and no mismatch.
//
// The harness drives SubmitTick without Stream::Update, so it calls
// FoldSnapshot after every tick -- exactly the one duty the game's Update
// performs for the save path (stream.h, FlushResident).
Status GateSaveLoad(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  const char* kPath = "selftest_world.svd";
  const char* kPathU = "selftest_world_untouched.svd";
  const char* kPathF = "selftest_world_full.svd";
  std::filesystem::remove_all(kPath);
  std::filesystem::remove_all(kPathU);
  std::filesystem::remove_all(kPathF);
  // Detach from any directory an earlier gate left bound, and tell the stream
  // the window is about to be regenerated from nothing -- what the game's
  // regen does (main.cpp: OnRegen, then SubmitWorldgen).
  stream.Store().Unbind();

  // ---- A: round trip through the delta ----
  stream.OnRegen();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 3000;
  for (int i = 0; i < 100; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i, kDefaultSeed), {}, {}, false,
               {8, 3, 8}, false, false);
    stream.FoldSnapshot();
  }
  ctx.WaitIdle();
  const uint32_t h1 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  SaveReport repA;
  const bool saved = SaveWorld(ctx, world, stream, kPath, c.mats, nullptr, {}, &repA);
  for (int i = 100; i < 150; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i, kDefaultSeed), {}, {}, false,
               {8, 3, 8}, false, false);
  ctx.WaitIdle();
  const uint32_t hDiverged = HashWorldNow(ctx, world, sim, kDefaultSeed);
  WorldStamp lsA;
  const bool loaded = LoadWorld(ctx, world, sim, stream, kPath, c.mats, nullptr, &lsA);
  const uint32_t h2 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  const bool okA = saved && loaded && h1 == h2 && h1 != hDiverged && repA.flush.delta;
  std::printf("save/load: %s (hash %08x -> diverged %08x -> restored %08x; %s, %u of "
              "%u chunks stored, %u via the %u-tick tail, %.2f MB)\n",
              okA ? "PASS" : "FAIL", h1, hDiverged, h2,
              repA.flush.delta ? "DELTA" : repA.flush.why.c_str(), repA.flush.stored,
              kNumChunks, repA.flush.tailOnly, repA.flush.tailTicks, repA.bytes / 1e6);
  stream.Store().Unbind();
  std::filesystem::remove_all(kPath);

  // ---- B: an untouched world ----
  stream.OnRegen();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  t = 5000;
  for (int i = 0; i < 30; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8}, false, false);
    stream.FoldSnapshot();
  }
  ctx.WaitIdle();
  const uint32_t hU = HashWorldNow(ctx, world, sim, kDefaultSeed);
  SaveReport repU;
  const bool savedU = SaveWorld(ctx, world, stream, kPathU, c.mats, nullptr, {}, &repU);
  const double maxBytes = BaselineNumber("saveUntouchedMaxBytes", -1.0);
  RecordObserved("saveUntouchedBytes", (double)repU.bytes);
  RecordObserved("saveUntouchedChunks", (double)repU.flush.stored);
  const bool sizeOk = repU.flush.delta && (maxBytes < 0.0 || (double)repU.bytes <= maxBytes);

  // ---- C: the worldgen fingerprint, on the untouched save (small, fast) ----
  WorldStamp lsU;
  const bool loadedU = LoadWorld(ctx, world, sim, stream, kPathU, c.mats, nullptr, &lsU);
  const uint32_t hU2 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  const bool roundU = savedU && loadedU && hU == hU2;
  const bool fpClear = lsU.fingerprintKnown && !lsU.worldgenMismatch &&
                       lsU.worldgenFingerprint == repU.fingerprint.value;

  const std::string metaPath = std::string(kPathU) + "/meta.svm";
  std::vector<uint8_t> meta;
  {
    FILE* fp = std::fopen(metaPath.c_str(), "rb");
    if (fp) {
      std::fseek(fp, 0, SEEK_END);
      meta.resize((size_t)std::ftell(fp));
      std::fseek(fp, 0, SEEK_SET);
      if (std::fread(meta.data(), 1, meta.size(), fp) != meta.size()) meta.clear();
      std::fclose(fp);
    }
  }
  auto writeMeta = [&](const std::vector<uint8_t>& bytes) {
    FILE* fp = std::fopen(metaPath.c_str(), "wb");
    if (!fp) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
    std::fclose(fp);
    return ok;
  };
  // SVM6 tail (worldio.cpp): ... tick seed | fpLo fpHi | nParts | parts[nParts]
  const size_t kFpTail = 4 * (2 + 1 + WorldgenFingerprint::kParts);
  bool fpFlagged = false, fpLoadedAnyway = false, svm5Ok = false;
  if (meta.size() > kFpTail + 8) {
    // FAKE THE STORED VALUE: the generator that "wrote" this save differs from
    // this build's in its code (part 0). The load must succeed and say so.
    std::vector<uint8_t> faked = meta;
    faked[meta.size() - kFpTail] ^= 0x5A;                  // fingerprint low byte
    faked[meta.size() - 4 * WorldgenFingerprint::kParts] ^= 0x5A;  // parts[0]
    WorldStamp lsF;
    if (writeMeta(faked)) {
      fpLoadedAnyway = LoadWorld(ctx, world, sim, stream, kPathU, c.mats, nullptr, &lsF);
      fpFlagged = lsF.fingerprintKnown && lsF.worldgenMismatch;
    }
    // AN SVM5 FILE: the same record, cut before the fingerprint, old magic.
    std::vector<uint8_t> v5(meta.begin(), meta.end() - kFpTail);
    v5[3] = '5';
    WorldStamp ls5;
    if (writeMeta(v5)) {
      const bool l5 = LoadWorld(ctx, world, sim, stream, kPathU, c.mats, nullptr, &ls5);
      svm5Ok = l5 && ls5.known && !ls5.fingerprintKnown && !ls5.worldgenMismatch;
    }
    writeMeta(meta);
  }

  // ---- the "before" number, on demand: the pre-S1 full flush ----
  uint64_t fullBytes = 0;
  if (const char* e = std::getenv("SANDVOX_SAVE_MEASURE_FULL"); e && e[0] == '1') {
    stream.Store().Unbind();
    SaveReport repF;
    repF.forceFullFlush = true;
    if (SaveWorld(ctx, world, stream, kPathF, c.mats, nullptr, {}, &repF)) {
      fullBytes = repF.bytes;
      std::printf("save-load: untouched world, FULL flush (pre-S1): %.2f MB in %zu "
                  "regions vs DELTA %.2f MB in %zu regions\n",
                  repF.bytes / 1e6, repF.regions, repU.bytes / 1e6, repU.regions);
    }
    stream.Store().Unbind();
    std::filesystem::remove_all(kPathF);
  }
  stream.Store().Unbind();
  std::filesystem::remove_all(kPathU);

  const bool okB = sizeOk && roundU;
  const bool okC = fpClear && fpFlagged && fpLoadedAnyway && svm5Ok &&
                   lsA.fingerprintKnown && !lsA.worldgenMismatch;
  detail = Format(
      "A: delta=%d stored=%u/%u tailOnly=%u tailTicks=%u restored=%d | B: untouched "
      "bytes=%llu (max %.0f) stored=%u roundtrip=%d%s | C: fp=%016llx (%.1f ms) "
      "clear=%d worldgenMismatch=%d loadedAnyway=%d svm5=%d",
      repA.flush.delta ? 1 : 0, repA.flush.stored, kNumChunks, repA.flush.tailOnly,
      repA.flush.tailTicks, okA ? 1 : 0, (unsigned long long)repU.bytes, maxBytes,
      repU.flush.stored, roundU ? 1 : 0,
      fullBytes ? Format(" fullFlushBytes=%llu", (unsigned long long)fullBytes).c_str() : "",
      (unsigned long long)repU.fingerprint.value, repU.fingerprint.ms, fpClear ? 1 : 0,
      fpFlagged ? 1 : 0, fpLoadedAnyway ? 1 : 0, svm5Ok ? 1 : 0);
  const bool ok = okA && okB && okC;
  std::printf("save-load: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
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

  // --- the S4 layout: world, player, and the mob's region bucket ---
  bool layoutOk = false;
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string dir(kPath);
    size_t buckets = 0;
    for (const auto& de : fs::directory_iterator(dir, ec))
      if (de.path().extension() == ".sve" &&
          de.path().filename().string().rfind("r_", 0) == 0)
        buckets++;
    layoutOk = fs::exists(dir + "/world.sve", ec) &&
               (!haveAvatar || fs::exists(dir + "/players/local.svp", ec)) &&
               !fs::exists(dir + "/entities.sve", ec) &&
               (dummyDef < 0 || buckets >= 1);
  }

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
  // (world.sve since S4: the same SVE1 container entities.sve was.)
  bool unknownOk = false;
  {
    const std::string ent = std::string(kPath) + "/world.sve";
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

  // --- a mismatched meta.svm voxel size must be REFUSED, and say why ---
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
    // A different material NAME table is no longer a refusal (rule-unification
    // W1-D, sim/mattable.h): every file this build wrote names its own table,
    // and meta.svm's only decides pre-W1-D (untagged) files, remapped by name.
    // So the load must GO AHEAD. `save-material-remap` asserts the remap.
    bool matRefused = flipByteAndTryLoad(36);  // first material's name
    refuseOk = vmRefused && !matRefused;
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
            avatarOk && unknownOk && refuseOk && sleepOk && layoutOk;
  // counts=... was asserted right after the FIRST load; the current BodyCount
  // may legitimately be lower by now (settle-back converts long-asleep loaded
  // bodies to grid during the settle run — which is itself the loaded world
  // behaving normally).
  detail = Format(
      "saved=%d loaded=%d counts=%d(%u bodies, %u limbs) asleep=%d pose=%d "
      "carve=%d avatar=%d unknown-skip=%d refuse=%d settle=%d(awake %u) "
      "s4-layout=%d",
      saved ? 1 : 0, loaded ? 1 : 0, countsOk ? 1 : 0, debrisBefore,
      limbBodiesBefore, asleepOk ? 1 : 0, poseOk ? 1 : 0,
      carveOk ? 1 : 0, avatarOk ? 1 : 0, unknownOk ? 1 : 0, refuseOk ? 1 : 0,
      sleepOk ? 1 : 0, awakeAfter, layoutOk ? 1 : 0);
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

// ---- region-codec ------------------------------------------------------
// SVR3 (docs/PLAN_save_system.md S3): the region files' zstd plane codec.
//
// The chunk set is REAL: the determinism gate's own mutation script
// (SelftestOps/SelftestExps — a sand pile, a water pool, lava into it, fire
// on the wood platform, seeds, the melt laser, a terrain crater and a pool
// blast) run for 150 ticks over fresh worldgen, and every chunk of the box it
// touches read back. "Modified" = differs from the same chunk straight after
// worldgen, under kPersistMask — the set S1's delta save would store. Plus
// synthetic edge cases the terrain never produces: a chunk of 4,096 distinct
// words (worst case), a JITTER-stone chunk at NEGATIVE coordinates built from
// the definition JitterStateFor (the predictor must zero it: proof the codec's
// row-form predictor is the worldgen rule), and a word with stamp bits (must
// fall back to raw and survive verbatim).
//
// Claims, each its own check:
//   A. codec round-trip is bit-exact for every chunk, at levels 1/3/9;
//   B. a ChunkStore written as SVR3 reads back identical through Get AND
//      ForEachStored — under a DIFFERENT SetSeed (the header's seed rules);
//   C. SVR2 files (written here in the legacy layout) load under this build,
//      and a dirtied SVR2 region is rewritten as SVR3 without losing chunks;
//   D. modified-set SVR2/SVR3 bytes >= regionCodec.modifiedRatioMin.
Status GateRegionCodec(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  constexpr uint32_t kSeed = kDefaultSeed;

  // ---- the box the mutation script touches (x,z 48..223) ----
  int gMin = 1 << 30, gMax = -(1 << 30);
  for (int z = 48; z < 224; z += 4)
    for (int x = 48; x < 224; x += 4) {
      const int h = World::TerrainHeight(x, z, kSeed);
      gMin = std::min(gMin, h);
      gMax = std::max(gMax, h);
    }
  std::vector<IVec3> box;
  for (int cz = 3; cz <= 13; cz++)
    for (int cy = (gMin - 24) >> 4; cy <= (gMax + 40) >> 4; cy++)
      for (int cx = 3; cx <= 13; cx++)
        if (world.ChunkInWindow({cx, cy, cz})) box.push_back({cx, cy, cz});
  auto readBox = [&](std::vector<std::vector<uint32_t>>& out) {
    out.assign(box.size(), std::vector<uint32_t>(kChunkVol));
    for (size_t i = 0; i < box.size(); i++)
      ReadVoxelsSync(ctx, world, World::SlotChunkIndex(box[i]), 1,
                     out[i].data(), "regionCodec");
  };

  SubmitWorldgen(ctx, world, sim, kSeed);
  ctx.WaitIdle();
  std::vector<std::vector<uint32_t>> pristine, after;
  readBox(pristine);
  for (uint32_t t = 1; t <= 150; t++)
    SubmitTick(ctx, world, sim, t, kSeed, SelftestOps(t, kSeed),
               SelftestExps(t, kSeed), {}, false, {8, 3, 8}, false,
               SelftestParticlesActive(t));
  ctx.WaitIdle();
  readBox(after);

  struct Chunk {
    IVec3 wc;
    std::vector<uint32_t> rle;
    bool modified;
  };
  std::vector<Chunk> set;
  for (size_t i = 0; i < box.size(); i++) {
    bool mod = false;
    for (uint32_t k = 0; k < kChunkVol && !mod; k++)
      mod = (pristine[i][k] & kPersistMask) != (after[i][k] & kPersistMask);
    Chunk ch{box[i], {}, mod};
    RleEncodeChunk(after[i].data(), ch.rle);
    set.push_back(std::move(ch));
  }
  size_t nModified = 0;
  for (const Chunk& ch : set) nModified += ch.modified;

  // ---- synthetic edge cases ----
  {
    std::vector<uint32_t> w(kChunkVol);
    uint32_t s = 0x9E3779B9u;
    for (uint32_t k = 0; k < kChunkVol; k++) {  // all distinct, masked
      s = s * 1664525u + 1013904223u;
      w[k] = ((k & 0xFFFu) | (s & 0xFF00F000u)) & kPersistMask;
    }
    Chunk ch{{-7, 2, 5}, {}, false};
    RleEncodeChunk(w.data(), ch.rle);
    set.push_back(std::move(ch));
  }
  const size_t jitterIdx = set.size();
  {
    const IVec3 wc{-5, -3, -9};
    std::vector<uint32_t> w(kChunkVol);
    for (uint32_t k = 0; k < kChunkVol; k++) {
      const int x = wc.x * 16 + (int)(k % 16), y = wc.y * 16 + (int)((k / 16) % 16),
                z = wc.z * 16 + (int)(k / 256);
      w[k] = PackVoxNew(kMatStone, JitterStateFor(x, y, z, kSeed));
    }
    Chunk ch{wc, {}, false};
    RleEncodeChunk(w.data(), ch.rle);
    set.push_back(std::move(ch));
  }
  const size_t rawIdx = set.size();
  set.push_back({{2, -1, 3}, {100u, 0x00050001u, kChunkVol - 100u, 1u}, false});

  // ---- A: codec round-trip at 1/3/9, bytes and time ----
  bool aOk = true;
  std::string firstBad;
  const int kLevels[3] = {1, 3, 9};
  uint64_t v3Mod[3] = {}, v3All[3] = {}, v2Mod = 0, v2All = 0;
  double encUs[3] = {}, decUs = 0;
  size_t timedChunks = 0;
  std::vector<uint8_t> rec;
  std::vector<uint32_t> back;
  size_t jitterBytes = 0;
  ChunkStore::Codec rawCodec = ChunkStore::Codec::ZstdPlanes;
  for (int li = 0; li < 3; li++) {
    for (size_t i = 0; i < set.size(); i++) {
      const Chunk& ch = set[i];
      const size_t pairs = ch.rle.size() / 2;
      const double t0 = NowSeconds();
      const ChunkStore::Codec codec = ChunkStore::EncodeRecord(
          ch.rle.data(), pairs, ch.wc, kSeed, rec, kLevels[li]);
      const double t1 = NowSeconds();
      const bool dec = ChunkStore::DecodeRecord(codec, rec.data(), rec.size(),
                                                ch.wc, kSeed, back);
      const double t2 = NowSeconds();
      if (!dec || back != ch.rle) {
        aOk = false;
        if (firstBad.empty())
          firstBad = Format(" A: chunk (%d,%d,%d) level %d codec %u %s",
                            ch.wc.x, ch.wc.y, ch.wc.z, kLevels[li],
                            (unsigned)codec, dec ? "differs" : "refused");
      }
      if (i < jitterIdx) {  // real terrain only
        v3All[li] += 16 + rec.size();
        if (ch.modified) v3Mod[li] += 16 + rec.size();
        if (li == 0) {
          v2All += 16 + pairs * 8;
          if (ch.modified) v2Mod += 16 + pairs * 8;
        }
        if (pairs > 1) {  // the chunks that actually reach zstd
          encUs[li] += (t1 - t0) * 1e6;
          if (li == 1) {
            decUs += (t2 - t1) * 1e6;
            timedChunks++;
          }
        }
      }
      if (li == 1 && i == jitterIdx) jitterBytes = rec.size();
      if (li == 1 && i == rawIdx) rawCodec = codec;
    }
  }
  const size_t timedPerLevel = timedChunks ? timedChunks : 1;
  // The predictor must turn definition-built jitter into all-zero state: a
  // uniform-material, zero-residue chunk is a few dozen bytes of zstd frame.
  const bool jitterOk = jitterBytes > 0 && jitterBytes <= 64;
  const bool rawOk = rawCodec == ChunkStore::Codec::RawRle;

  // ---- B: through the store, SVR3, read back under a different seed ----
  namespace fs = std::filesystem;
  const char* kDir3 = "selftest_codec3.svd";
  const char* kDir2 = "selftest_codec2.svd";
  auto sameAsSet = [&](ChunkStore& cs, const char* tag) {
    bool ok = true;
    for (const Chunk& ch : set) {
      const std::vector<uint32_t>* got = cs.Get(ch.wc);
      if (!got || *got != ch.rle) {
        ok = false;
        if (firstBad.empty())
          firstBad = Format(" %s: Get(%d,%d,%d) %s", tag, ch.wc.x, ch.wc.y,
                            ch.wc.z, got ? "differs" : "missing");
      }
    }
    return ok;
  };
  auto walkSameAsSet = [&](ChunkStore& cs, const char* tag) {
    std::map<std::tuple<int, int, int>, const std::vector<uint32_t>*> want;
    for (const Chunk& ch : set) want[{ch.wc.x, ch.wc.y, ch.wc.z}] = &ch.rle;
    size_t visited = 0;
    bool ok = true;
    cs.ForEachStored([&](IVec3 wc, const uint32_t* rle, size_t pairs) {
      visited++;
      auto it = want.find({wc.x, wc.y, wc.z});
      if (it == want.end() || it->second->size() != pairs * 2 ||
          !std::equal(rle, rle + pairs * 2, it->second->begin()))
        ok = false;
    });
    ok = ok && visited == set.size();
    if (!ok && firstBad.empty())
      firstBad = Format(" %s: ForEachStored visited %zu of %zu or differed",
                        tag, visited, set.size());
    return ok;
  };
  bool bOk = false;
  size_t regions3 = 0;
  uint64_t bytes3 = 0;
  {
    fs::remove_all(kDir3);
    ChunkStore cs;
    cs.SetSeed(kSeed);
    bOk = cs.BindSave(kDir3);
    for (const Chunk& ch : set) cs.Put(ch.wc, ch.rle);
    bOk = bOk && cs.Flush(&regions3, &bytes3);
    cs.Unbind();
    ChunkStore rd;
    rd.SetSeed(kSeed ^ 0x5A5A5A5Au);  // wrong on purpose: header's seed rules
    bOk = bOk && rd.BindLoad(kDir3) && sameAsSet(rd, "B") &&
          walkSameAsSet(rd, "B");
    ChunkStore walk;  // a fresh store: ForEachStored's disk-only branch
    bOk = bOk && walk.BindLoad(kDir3) && walkSameAsSet(walk, "B-disk");
    rd.Unbind();
    walk.Unbind();
    fs::remove_all(kDir3);
  }

  // ---- C: SVR2 (the pre-S3 layout, written by hand) loads, and upgrades ----
  bool cOk = false;
  uint64_t svr2FileBytes = 0;
  {
    fs::remove_all(kDir2);
    fs::create_directories(kDir2);
    std::map<std::tuple<int, int, int>, std::vector<const Chunk*>> byRegion;
    for (const Chunk& ch : set)
      byRegion[{ch.wc.x >> ChunkStore::kRegionShift,
                ch.wc.y >> ChunkStore::kRegionShift,
                ch.wc.z >> ChunkStore::kRegionShift}]
          .push_back(&ch);
    cOk = true;
    for (const auto& [rc, chunks] : byRegion) {
      const std::string path = Format("%s/r_%d_%d_%d.svr", kDir2, std::get<0>(rc),
                                      std::get<1>(rc), std::get<2>(rc));
      FILE* fp = std::fopen(path.c_str(), "wb");
      if (!fp) { cOk = false; break; }
      const uint32_t hdr[2] = {0x32525653u /* 'SVR2' */, (uint32_t)chunks.size()};
      std::fwrite(hdr, 4, 2, fp);
      for (const Chunk* ch : chunks) {
        const int32_t wc[3] = {ch->wc.x, ch->wc.y, ch->wc.z};
        const uint32_t pairs = (uint32_t)(ch->rle.size() / 2);
        std::fwrite(wc, 4, 3, fp);
        std::fwrite(&pairs, 4, 1, fp);
        std::fwrite(ch->rle.data(), 4, ch->rle.size(), fp);
      }
      std::fclose(fp);
      svr2FileBytes += fs::file_size(path);
    }
    ChunkStore rd;
    rd.SetSeed(kSeed);
    cOk = cOk && rd.BindLoad(kDir2) && sameAsSet(rd, "C-svr2") &&
          walkSameAsSet(rd, "C-svr2");
    // Dirty every region (re-Put one chunk of each) and flush: each file must
    // come back as the CURRENT layout (SVR4 since W1-D: SVR3 plus the
    // material table tag) carrying ALL its chunks, the disk-only ones included.
    for (const auto& [rc, chunks] : byRegion)
      rd.Put(chunks.front()->wc, chunks.front()->rle);
    cOk = cOk && rd.Flush();
    rd.Unbind();
    for (const auto& de : fs::directory_iterator(kDir2)) {
      if (de.path().extension() != ".svr") continue;
      FILE* fp = std::fopen(de.path().string().c_str(), "rb");
      uint32_t magic = 0;
      if (!fp || std::fread(&magic, 4, 1, fp) != 1 || magic != 0x34525653u /* SVR4 */)
        cOk = false;
      if (fp) std::fclose(fp);
    }
    ChunkStore up;
    cOk = cOk && up.BindLoad(kDir2) && sameAsSet(up, "C-upgraded") &&
          walkSameAsSet(up, "C-upgraded");
    up.Unbind();
    fs::remove_all(kDir2);
  }

  // ---- D: the ratio floor ----
  const double modRatio = v3Mod[1] ? (double)v2Mod / (double)v3Mod[1] : 0.0;
  const double allRatio = v3All[1] ? (double)v2All / (double)v3All[1] : 0.0;
  const double ratioMin = BaselineNumber("regionCodec.modifiedRatioMin", 0.0);
  const bool dOk = nModified >= 8 && modRatio >= ratioMin;
  RecordObserved("regionCodec.modifiedRatio", modRatio);
  RecordObserved("regionCodec.encodeUsPerChunk", encUs[1] / timedPerLevel);
  RecordObserved("regionCodec.decodeUsPerChunk", decUs / timedPerLevel);

  const bool ok = aOk && jitterOk && rawOk && bOk && cOk && dOk;
  std::printf(
      "region codec: %s (%zu chunks, %zu modified; modified SVR2 %llu B -> "
      "SVR3 %llu/%llu/%llu B at zstd 1/3/9 = %.1fx at %d (floor %.1fx); all "
      "%llu -> %llu B = %.1fx; SVR3 store %llu B in %zu regions, SVR2 files "
      "%llu B)\n",
      ok ? "PASS" : "FAIL", set.size() - 3, nModified,
      (unsigned long long)v2Mod, (unsigned long long)v3Mod[0],
      (unsigned long long)v3Mod[1], (unsigned long long)v3Mod[2], modRatio,
      ChunkStore::kZstdLevel, ratioMin, (unsigned long long)v2All,
      (unsigned long long)v3All[1], allRatio, (unsigned long long)bytes3,
      regions3, (unsigned long long)svr2FileBytes);
  std::printf(
      "region codec: encode us/chunk %.1f/%.1f/%.1f at 1/3/9, decode %.1f "
      "(over %zu multi-run chunks)%s; jitter chunk %zu B %s, raw fallback %s; "
      "A %s B %s C %s\n",
      encUs[0] / timedPerLevel, encUs[1] / timedPerLevel,
      encUs[2] / timedPerLevel, decUs / timedPerLevel, timedChunks,
      encUs[1] / timedPerLevel > 200.0 ? " OVER the 200 us budget" : "",
      jitterBytes, jitterOk ? "ok" : "NOT ZEROED", rawOk ? "ok" : "NOT TAKEN",
      aOk ? "ok" : "FAIL", bOk ? "ok" : "FAIL", cOk ? "ok" : "FAIL");
  detail = Format("modified %.1fx, all %.1fx, enc %.0f us, dec %.0f us%s",
                  modRatio, allRatio, encUs[1] / timedPerLevel,
                  decUs / timedPerLevel, firstBad.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- save-split --------------------------------------------------------
//
// PLAN_save_system.md S4: entity state split by OWNER. One fixture, five
// claims, all asserted in one run:
//
//  A. LAYOUT. Two creatures and two ground items placed ~430 voxels apart
//     (so in two different 256-voxel regions whatever the window origin),
//     a player kit and an engaged celestial clock. The save must write
//     world.sve, players/local.svp, and EXACTLY one r_*.sve per region that
//     holds a live creature or item -- the set computed from the live
//     positions, compared to the files on disk -- and no entities.sve.
//  B. ROUND TRIP. Wreck everything, load: both creatures, both items, the
//     kit slot and every field of the celestial clock come back.
//  C. PRE-S4 FILE. The same live state written as ONE entities.sve (every
//     section's whole payload, the old container) with the S4 files deleted
//     loads identically, and the next save distributes it: S4 files present,
//     entities.sve gone, and that save round-trips too.
//  D. PARKED RECORDS SURVIVE. A creature record appended to a region far
//     outside the window (the S5b entry point) is written by the save, NOT
//     applied by the load (it is outside the window), and a second save
//     leaves its file byte-identical -- unrelated regions are not rewritten.
//  E. THE SKY FOLLOWS A SAVE the sim clock cannot (ResumeWorldClock).
//
// Debris is world.sve's (no per-body entry point yet); its body count is
// reported, not asserted -- save-entities owns the debris round trip.
Status GateSaveSplit(Ctx& c, std::string& detail) {
  namespace fs = std::filesystem;
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  const char* kPath = "selftest_split.svd";
  const std::string dir(kPath);

  // The two process globals this gate touches, put back on every exit.
  const uint64_t idCounterWas = mobs.NextIdCounter();
  const CelestialClock celestialWas = Celestial();

  int dummyDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == "dummy") dummyDef = (int)i;
  int itemIdx = -1;
  for (size_t i = 0; i < c.items.items.size() && itemIdx < 0; i++) {
    uint32_t sc = 1;
    if (ItemGroundVoxels(c.items.items[i], sc)) itemIdx = (int)i;
  }
  if (dummyDef < 0 || itemIdx < 0) {
    detail = "no 'dummy' mob def or no droppable item";
    return Status::Skip;
  }
  const ItemDef& itemDef = c.items.items[itemIdx];

  stream.Store().Unbind();
  std::filesystem::remove_all(kPath);
  debris.Reset();
  mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  WorldItems ground;
  debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });
  PlayerCaster caster;
  GlyphLibrary glyphs;
  Kit kit;
  Inventory& hb = kit.hotbar;
  PlayerKitRefs kitRefs{&caster, &glyphs, &kit, &c.items};
  WorldItemRefs groundRefs{&ground, &c.phys, &debris, nullptr, &c.items};
  EntityIO eio = MakeEntityIO(debris, mobs, nullptr, &kitRefs, &groundRefs);

  const IVec3 wo = world.WindowOrigin();
  const int x0 = wo.x * (int)kChunk, z0 = wo.z * (int)kChunk;
  const int sites[2][2] = {{x0 + 40, z0 + 40}, {x0 + 470, z0 + 470}};
  for (const auto& s : sites) {
    const int h = World::TerrainHeight(s[0], s[1], kDefaultSeed);
    mobs.Spawn(dummyDef, {s[0], h + 1, s[1]});
    const int hi = World::TerrainHeight(s[0] + 8, s[1] + 8, kDefaultSeed);
    DropItemToWorld(itemDef, ItemInstance{itemDef.name},
                    Vec3{(float)s[0] + 8, (float)(hi + 3), (float)s[1] + 8},
                    Vec3{}, c.phys, debris, nullptr, ground);
  }
  hb.slots[2] = StackOf(c.items, itemIdx);
  // An ENGAGED clock at 3x, advanced off the sim tick, so every field of the
  // 'TIME' section carries something a default clock would not.
  Celestial() = CelestialClock{};
  Celestial().SetScale(3.0f, 777);
  for (int i = 0; i < 5; i++) Celestial().Advance();
  const CelestialClock clockSaved = Celestial();

  const uint32_t mobsBefore = mobs.MobCount();
  const size_t itemsBefore = ground.Count();
  const uint32_t debrisBefore = debris.BodyCount();

  // The regions that MUST hold a bucket: every live creature's and item's.
  auto expectedRegions = [&]() {
    std::map<std::tuple<int, int, int>, int> want;
    for (uint32_t i = 0; i < mobs.MobCount(); i++)
      if (const Mob* m = mobs.MobAt(i); m && m->Alive()) {
        const IVec3 rc = ChunkStore::RegionOfVoxel(m->Origin());
        want[{rc.x, rc.y, rc.z}]++;
      }
    for (const WorldItem& w : ground.All()) {
      BodyTransform xf{};
      c.phys.GetTransform(w.body, xf);
      const IVec3 rc = ChunkStore::RegionOfVoxel(xf.pos);
      want[{rc.x, rc.y, rc.z}]++;
    }
    return want;
  };
  auto bucketFiles = [&]() {
    std::map<std::tuple<int, int, int>, int> have;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
      const std::string n = de.path().filename().string();
      int x = 0, y = 0, z = 0;
      if (de.path().extension() == ".sve" &&
          std::sscanf(n.c_str(), "r_%d_%d_%d.sve", &x, &y, &z) == 3)
        have[{x, y, z}] = 1;
    }
    return have;
  };
  auto sameKeys = [](const std::map<std::tuple<int, int, int>, int>& a,
                     const std::map<std::tuple<int, int, int>, int>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a)
      if (!b.count(k)) return false;
    return true;
  };
  auto wreck = [&]() {
    debris.Reset();  // fires OnBodyGone: the registry empties with it
    mobs.Reset();
    ground.Clear();
    hb.slots[2] = ItemStack{};
    Celestial() = CelestialClock{};
  };
  auto restored = [&]() {
    const CelestialClock& k = Celestial();
    return mobs.MobCount() == mobsBefore && ground.Count() == itemsBefore &&
           !hb.slots[2].Empty() && hb.slots[2].name == itemDef.name &&
           k.engaged == clockSaved.engaged && k.scaleNum == clockSaved.scaleNum &&
           k.scaleDen == clockSaved.scaleDen && k.ticks == clockSaved.ticks &&
           k.rem == clockSaved.rem && k.prevTicks == clockSaved.prevTicks;
  };
  std::error_code ec;

  // ---- A: layout ----
  const auto want = expectedRegions();
  SaveReport rep1;
  const bool saved1 = SaveWorld(ctx, world, stream, kPath, c.mats, &eio, {}, &rep1);
  const auto have1 = bucketFiles();
  const bool okA = saved1 && want.size() >= 2 && sameKeys(want, have1) &&
                   fs::exists(dir + "/world.sve", ec) &&
                   fs::exists(dir + "/players/local.svp", ec) &&
                   !fs::exists(dir + "/entities.sve", ec);

  // ---- B: round trip ----
  wreck();
  EntityFileReport lr1;
  const bool loaded1 =
      LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio, nullptr, &lr1);
  const uint32_t debrisAfter = debris.BodyCount();
  const bool okB = loaded1 && restored() && !lr1.legacy &&
                   lr1.recordsApplied == (uint32_t)(mobsBefore + itemsBefore);

  // ---- C: a pre-S4 entities.sve ----
  bool okC = false;
  EntityFileReport lr2;
  {
    // The old container, byte for byte: SVE1, then every section's WHOLE
    // payload -- exactly what the pre-S4 WriteEntities produced.
    std::vector<uint8_t> buf;
    ByteWriter w{buf};
    w.U32(0x31455653u);  // 'SVE1'
    w.U32((uint32_t)eio.sections.size());
    std::vector<uint8_t> payload;
    for (const EntitySection& s : eio.sections) {
      payload.clear();
      s.save(payload);
      w.U32(s.id);
      w.U32(s.version);
      w.U32((uint32_t)payload.size());
      w.Bytes(payload.data(), payload.size());
    }
    for (const auto& de : fs::directory_iterator(dir, ec))
      if (de.path().extension() == ".sve") fs::remove(de.path(), ec);
    fs::remove_all(dir + "/players", ec);
    if (FILE* fp = std::fopen((dir + "/entities.sve").c_str(), "wb")) {
      std::fwrite(buf.data(), 1, buf.size(), fp);
      std::fclose(fp);
    }
    wreck();
    const bool loaded2 =
        LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio, nullptr, &lr2);
    const bool legacyOk = loaded2 && lr2.legacy && restored();
    // The next save distributes it...
    const auto want2 = expectedRegions();
    const bool saved2 = SaveWorld(ctx, world, stream, kPath, c.mats, &eio);
    const bool split2 = saved2 && sameKeys(want2, bucketFiles()) &&
                        fs::exists(dir + "/world.sve", ec) &&
                        fs::exists(dir + "/players/local.svp", ec) &&
                        !fs::exists(dir + "/entities.sve", ec);
    // ...and what it distributed loads back.
    wreck();
    const bool loaded3 = LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio);
    okC = legacyOk && split2 && loaded3 && restored();
  }

  // ---- D: a parked record far outside the window ----
  bool okD = false;
  uint32_t parkedLeft = 0;
  {
    const Vec3 far{(float)(x0 - 2000), (float)(wo.y * (int)kChunk + 100),
                   (float)(z0 - 2000)};
    const IVec3 frc = ChunkStore::RegionOfVoxel(far);
    ChunkStore::EntityRecord rec;
    rec.section = eio.sections[0].id;
    for (const EntitySection& s : eio.sections)
      if (s.scope == EntityScope::Region && s.saveRecords) {
        std::vector<EntityRecord> recs;
        s.saveRecords(recs);
        if (!recs.empty() && recs[0].bytes.size() > 0 &&
            s.id == (uint32_t)('M' | ('O' << 8) | ('B' << 16) | ((uint32_t)'S' << 24))) {
          rec = recs[0];
          rec.section = s.id;
          rec.version = s.version;
        }
      }
    rec.pos = far;
    stream.Store().DormantEntities(frc).push_back(rec);
    const bool saved3 = SaveWorld(ctx, world, stream, kPath, c.mats, &eio);
    const std::string farPath = stream.Store().EntityRegionPath(frc);
    std::vector<uint8_t> farBytes;
    auto readFar = [&](std::vector<uint8_t>& out) {
      out.clear();
      FILE* fp = std::fopen(farPath.c_str(), "rb");
      if (!fp) return false;
      std::fseek(fp, 0, SEEK_END);
      out.resize((size_t)std::ftell(fp));
      std::fseek(fp, 0, SEEK_SET);
      const bool ok = std::fread(out.data(), 1, out.size(), fp) == out.size();
      std::fclose(fp);
      return ok;
    };
    const bool farWritten = saved3 && readFar(farBytes) && !farBytes.empty();
    wreck();
    EntityFileReport lr4;
    const bool loaded4 =
        LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio, nullptr, &lr4);
    parkedLeft = lr4.recordsDormant;
    // Not applied (the window does not reach it)...
    const bool notApplied = loaded4 && mobs.MobCount() == mobsBefore;
    // ...and a save that did not touch that region leaves its file alone.
    SaveReport rep5;
    const bool saved5 = SaveWorld(ctx, world, stream, kPath, c.mats, &eio, {}, &rep5);
    std::vector<uint8_t> farAfter;
    const bool farKept = saved5 && readFar(farAfter) && farAfter == farBytes;
    okD = farWritten && notApplied && farKept;
  }

  // ---- E: the sky after a reload the sim clock could not follow ----
  bool okE = false;
  {
    Celestial() = CelestialClock{};
    const bool engaged = ResumeWorldClock(1000, 5000);
    const bool atSave = Celestial().SimTick(5000) == 1000;
    Celestial() = CelestialClock{};
    const bool noop = !ResumeWorldClock(5000, 5000) && !Celestial().engaged;
    okE = engaged && atSave && noop;
  }

  const bool ok = okA && okB && okC && okD && okE;
  detail = Format(
      "A layout=%d (%zu regions expected, %zu bucket files, %u records, %.1f KB) | "
      "B roundtrip=%d (applied %u, mobs %u, items %zu, debris %u->%u) | "
      "C preS4=%d | D parked=%d (dormant %u) | E skyResume=%d",
      okA ? 1 : 0, want.size(), have1.size(), rep1.entities.regionRecords,
      rep1.entities.bytes / 1e3, okB ? 1 : 0, lr1.recordsApplied, mobs.MobCount(),
      ground.Count(), debrisBefore, debrisAfter, okC ? 1 : 0, okD ? 1 : 0, parkedLeft,
      okE ? 1 : 0);
  std::printf("save-split: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  // teardown: nothing of this gate survives into the next one
  debris.Reset();
  mobs.Reset();
  ground.Clear();
  debris.SetOnBodyGone(nullptr);
  mobs.SetNextIdCounter(idCounterWas);
  Celestial() = celestialWas;
  stream.Store().Unbind();
  std::filesystem::remove_all(kPath);
  return ok ? Status::Pass : Status::Fail;
}

// ---- save-material-remap -------------------------------------------------
//
// MATERIAL NAMES IN SAVES (rule-unification W1-D, sim/mattable.h). A material
// id is its position in materials.json; a save now names the table its ids
// were written under and the loader remaps BY NAME. Two claims:
//
//  A. THE STORE, CPU ONLY, against a saved table that INSERTS a material
//     nobody has, SWAPS two, and SWAPS two stain slots. Synthetic chunks cover
//     every saved id and stain type; read back under the running table, every
//     voxel must name the same material and the same stain, the unknown
//     material must become air, and the RLE must come back canonical. Then the
//     legacy path: an UNTAGGED region reads under meta.svm's table, which
//     LoadWorld pins to mat_legacy.svmt so a later meta rewrite cannot lose it;
//     and a region whose table file is gone loads as stored (identity).
//  B. END TO END, GPU. A world with edits, a creature soaked in blood and a
//     debris body of stone and wood is SAVED under a permuted copy of the real
//     materials (stone<->wood, blood<->water, sand<->dirt, and two stain
//     slots) -- which is exactly "the build that saved had a different
//     materials.json" -- and LOADED under the real one. Every stored chunk's
//     region, read back off the GPU, must hash identically BY NAME to what was
//     saved; the debris lattice and the creature's coat must have followed the
//     names too. The permutation keeps every id inside the running table, so
//     the sim's own material table stays valid throughout.
Status GateSaveMaterialRemap(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  const MaterialNameTable R = MaterialNameTableOf(c.mats);
  auto idOf = [&](const char* n) -> int {
    for (size_t i = 0; i < R.mats.size(); i++)
      if (R.mats[i] == n) return (int)i;
    return -1;
  };
  // Two distinct non-empty stain slots of the running table (0 if none).
  uint32_t slotA = 0, slotB = 0;
  for (uint32_t s = 1; s < R.stains.size(); s++) {
    if (R.stains[s].empty()) continue;
    if (slotA == 0)
      slotA = s;
    else if (slotB == 0)
      slotB = s;
  }
  const int mStone = idOf("stone"), mWood = idOf("wood"), mBlood = idOf("blood"),
            mWater = idOf("water"), mSand = idOf("sand"), mDirt = idOf("dirt");
  if (mStone < 0 || mWood < 0 || mBlood < 0 || mWater < 0 || mSand < 0 || mDirt < 0) {
    detail = "stone/wood/blood/water/sand/dirt not all in materials.json";
    return Status::Fail;
  }

  // ======== A: the store ========================================================
  bool aOk = true;
  std::string whyA;
  auto failA = [&](const std::string& w) {
    if (whyA.empty()) whyA = w;
    aOk = false;
  };
  uint32_t aVoxels = 0, aMoved = 0, aToAir = 0, aStains = 0;
  {
    namespace fs = std::filesystem;
    const char* kDir = "selftest_matremap.svd";
    const char* kDirL = "selftest_matremap_legacy.svd";
    fs::remove_all(kDir);
    fs::remove_all(kDirL);
    MaterialNameTable S = R;
    S.mats.insert(S.mats.begin() + 3, "w1d_not_a_material");
    // The class list travels with the names (mattable.h `classes`): a saved
    // table whose classes did not follow its own ids would claim every
    // material past the insert changed class.
    if (!S.classes.empty()) S.classes.insert(S.classes.begin() + 3, (uint8_t)CLASS_SOLID);
    int sStone = -1, sWater = -1;
    for (size_t i = 0; i < S.mats.size(); i++) {
      if (S.mats[i] == "stone") sStone = (int)i;
      if (S.mats[i] == "water") sWater = (int)i;
    }
    std::swap(S.mats[sStone], S.mats[sWater]);
    if (!S.classes.empty()) std::swap(S.classes[sStone], S.classes[sWater]);
    if (slotB != 0) std::swap(S.stains[slotA], S.stains[slotB]);

    constexpr uint32_t kSeed = 0x5eed;
    const IVec3 wcs[3] = {{2, 3, 4}, {2, 3, 5}, {40, 3, 4}};  // two regions
    std::vector<std::vector<uint32_t>> saved(3, std::vector<uint32_t>(kChunkVol));
    for (int ci = 0; ci < 3; ci++)
      for (uint32_t k = 0; k < kChunkVol; k++) {
        // Runs of 3 so the RLE has something to merge, every saved id hit.
        const uint32_t id = (k / 3 + (uint32_t)ci * 17) % (uint32_t)S.mats.size();
        uint32_t w = id | (((k / 11) & 0xFu) << 12);
        const uint32_t st = (k / 5) % 3 == 0 ? (k % 2 ? slotA : slotB) : 0u;
        if (st != 0) w |= ((1u + k % 15u) << 24) | (st << 28);
        saved[ci][k] = w;
      }
    // What the running table must read each saved word as.
    auto expectWord = [&](uint32_t w, bool stainsKnown, bool matsKnown) {
      if (!matsKnown) return w;
      const std::string& name = S.mats[w & 0xFFFu];
      int to = -1;
      for (size_t i = 0; i < R.mats.size() && to < 0; i++)
        if (R.mats[i] == name) to = (int)i;
      if (to < 0) return 0u;  // a substance this build lacks: clean air
      uint32_t e = (w & ~0xFFFu) | (uint32_t)to;
      const uint32_t t = (w >> 28) & 7u;
      if (t != 0 && stainsKnown) {
        uint32_t nt = 0;
        for (uint32_t s = 1; s < R.stains.size() && nt == 0; s++)
          if (R.stains[s] == S.stains[t]) nt = s;
        e = nt == 0 ? (e & ~0x7F000000u) : ((e & ~0x70000000u) | (nt << 28));
      }
      return e;
    };
    auto readBack = [&](ChunkStore& cs, bool stainsKnown, bool matsKnown,
                        const char* tag) {
      std::vector<uint32_t> got(kChunkVol);
      for (int ci = 0; ci < 3; ci++) {
        const std::vector<uint32_t>* rle = cs.Get(wcs[ci]);
        if (!rle || !RleDecodeChunk(rle->data(), rle->size() / 2, got.data())) {
          failA(Format("%s: chunk %d unreadable", tag, ci));
          return;
        }
        for (size_t p = 2; p + 1 < rle->size(); p += 2)
          if ((*rle)[p + 1] == (*rle)[p - 1]) {
            failA(Format("%s: chunk %d RLE not canonical at pair %zu", tag, ci, p / 2));
            return;
          }
        for (uint32_t k = 0; k < kChunkVol; k++) {
          const uint32_t want = expectWord(saved[ci][k], stainsKnown, matsKnown);
          if (got[k] != want) {
            failA(Format("%s: chunk %d cell %u saved %08x ('%s') read %08x ('%s') "
                         "want %08x",
                         tag, ci, k, saved[ci][k], S.mats[saved[ci][k] & 0xFFFu].c_str(),
                         got[k], (got[k] & 0xFFFu) < R.mats.size()
                                     ? R.mats[got[k] & 0xFFFu].c_str() : "?",
                         want));
            return;
          }
        }
      }
    };

    // ---- A1: tagged ----
    {
      ChunkStore w;
      w.SetSeed(kSeed);
      w.Tables().SetRunning(S);
      if (!w.BindSave(kDir)) failA("A1: BindSave");
      for (int ci = 0; ci < 3; ci++) {
        std::vector<uint32_t> rle;
        RleEncodeChunk(saved[ci].data(), rle);
        w.Put(wcs[ci], std::move(rle));
      }
      if (!w.Flush()) failA("A1: Flush");
      w.Unbind();
      if (!fs::exists(MatTableSet::TablePath(kDir, S.Hash())))
        failA("A1: the saved table's file was not written beside the regions");
      ChunkStore r;
      r.SetSeed(kSeed);
      r.Tables().SetRunning(R);
      if (!r.BindLoad(kDir)) failA("A1: BindLoad");
      readBack(r, true, true, "A1 tagged");
      for (int ci = 0; ci < 3; ci++)
        for (uint32_t k = 0; k < kChunkVol; k++) {
          const uint32_t sv = saved[ci][k], e = expectWord(sv, true, true);
          aVoxels++;
          if ((e & 0xFFFu) != (sv & 0xFFFu)) aMoved++;
          if ((e & 0xFFFu) == 0 && (sv & 0xFFFu) != 0) aToAir++;
          if (((sv >> 28) & 7u) != ((e >> 28) & 7u)) aStains++;
        }
      // The walk (FarEdits' rebuild path) must see the same remapped words.
      size_t walked = 0;
      r.ForEachStored([&](IVec3 wc, const uint32_t* rle, size_t pairs) {
        std::vector<uint32_t> got(kChunkVol);
        for (int ci = 0; ci < 3; ci++)
          if (wc.x == wcs[ci].x && wc.y == wcs[ci].y && wc.z == wcs[ci].z &&
              RleDecodeChunk(rle, pairs, got.data()) &&
              got[5] == expectWord(saved[ci][5], true, true))
            walked++;
      });
      if (walked != 3) failA(Format("A1: ForEachStored matched %zu of 3", walked));
      r.Unbind();
    }
    // ---- A2: missing table file -> identity, loudly ----
    {
      std::error_code ec;
      fs::remove(MatTableSet::TablePath(kDir, S.Hash()), ec);
      ChunkStore r;
      r.SetSeed(kSeed);
      r.Tables().SetRunning(R);
      r.BindLoad(kDir);
      readBack(r, false, false, "A2 no-table");
      r.Unbind();
    }
    // ---- A3: untagged (pre-W1-D) regions read under meta's table ----
    {
      ChunkStore w;
      w.SetSeed(kSeed);  // no running table: every write is untagged
      if (!w.BindSave(kDirL)) failA("A3: BindSave");
      for (int ci = 0; ci < 3; ci++) {
        std::vector<uint32_t> rle;
        RleEncodeChunk(saved[ci].data(), rle);
        w.Put(wcs[ci], std::move(rle));
      }
      w.Flush();
      w.Unbind();
      MaterialNameTable metaS;
      metaS.mats = S.mats;  // meta.svm carries names, never stain slots
      ChunkStore r;
      r.SetSeed(kSeed);
      r.Tables().SetRunning(R);
      r.BindLoad(kDirL);
      r.Tables().AdoptLegacy(metaS);
      readBack(r, false, true, "A3 legacy");
      r.Unbind();
      if (!fs::exists(MatTableSet::LegacyPath(kDirL)))
        failA("A3: the legacy table was not pinned to mat_legacy.svmt");
      // A later save rewrites meta.svm with the RUNNING table; the untagged
      // regions must still read under the pinned one.
      MaterialNameTable metaR;
      metaR.mats = R.mats;
      ChunkStore r2;
      r2.SetSeed(kSeed);
      r2.Tables().SetRunning(R);
      r2.BindLoad(kDirL);
      r2.Tables().AdoptLegacy(metaR);
      readBack(r2, false, true, "A3 after meta rewrite");
      r2.Unbind();
    }
    fs::remove_all(kDir);
    fs::remove_all(kDirL);
    if (aMoved == 0 || aToAir == 0 || (slotB != 0 && aStains == 0))
      failA(Format("A: the fixture moved nothing (%u moved, %u to air, %u stains)",
                   aMoved, aToAir, aStains));
  }

  // ======== B: end to end through SaveWorld / LoadWorld ==========================
  bool bOk = false;
  std::string whyB;
  uint32_t bChunks = 0, bCells = 0, bMoved = 0, coatBefore = 0, coatAfter = 0;
  uint32_t dbStoneBefore = 0, dbStoneAfter = 0, dbWoodAfter = 0;
  bool loaded = false;
  {
    const char* kPath = "selftest_matremap_world.svd";
    std::filesystem::remove_all(kPath);
    stream.Store().Unbind();
    c.debris.Reset();
    c.mobs.Reset();
    stream.OnRegen();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 7000;
    for (int i = 0; i < 60; i++) {
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i, kDefaultSeed), {}, {},
                 false, {8, 3, 8}, false, false);
      stream.FoldSnapshot();
    }
    ctx.WaitIdle();

    // The SAVING build's materials: the real table, permuted in place.
    std::vector<MaterialDef> P = c.mats;
    std::swap(P[mStone], P[mWood]);
    std::swap(P[mBlood], P[mWater]);
    std::swap(P[mSand], P[mDirt]);
    if (slotB != 0)
      for (MaterialDef& m : P)
        if (m.stainSlot == slotA) m.stainSlot = slotB;
        else if (m.stainSlot == slotB) m.stainSlot = slotA;
    const MaterialNameTable Pt = MaterialNameTableOf(P);

    // A creature with a blood coat on one limb (so the limb is STORED, not
    // pristine) and a debris body that is 40 stone + 24 wood.
    const IVec3 o = world.WindowOrigin();
    const int hx = (o.x + (int)kNChunk / 2) * (int)kChunk + 20,
              hz = (o.z + (int)kNChunk / 2) * (int)kChunk + 20;
    const int hy = World::TerrainHeight(hx, hz, kDefaultSeed) + 2;
    const int human = c.mobs.FindDef("human");
    const uint64_t mid = human >= 0 ? c.mobs.Spawn(human, {hx, hy, hz}) : 0;
    const int soakLimb = 0;
    uint32_t soaked = 0;
    if (mid != 0) soaked = c.mobs.SoakLimb(mid, soakLimb, (uint32_t)mBlood, 6, t);
    auto coatCount = [&](uint64_t id, int mat) {
      uint32_t n = 0;
      for (const PrefabVoxel& v : c.mobs.LimbLattice(id, soakLimb))
        if (BodyStainAmt(v.stain) > 0 && (int)BodyStainMat(v.stain) == mat) n++;
      return n;
    };
    coatBefore = mid != 0 ? coatCount(mid, mBlood) : 0;

    std::vector<DebrisVoxel> vox;
    for (int8_t z = 0; z < 4; z++)
      for (int8_t y = 0; y < 4; y++)
        for (int8_t x = 0; x < 4; x++)
          vox.push_back(DebrisVoxel{x, y, z, 0,
                                    (uint16_t)(vox.size() < 40 ? mStone : mWood)});
    std::vector<float> density(c.mats.size(), 1000.0f);
    for (size_t i = 0; i < c.mats.size(); i++)
      density[i] = std::max(1.0f, (float)c.mats[i].gpu.density);
    BodyTransform bxf{};
    bxf.pos = Vec3{(float)hx - 10, (float)hy + 4, (float)hz};
    bxf.quat[3] = 1;
    const uint64_t bh = c.phys.CreateDebrisBodyXf(vox, bxf, density);
    if (bh != 0) c.debris.AdoptBody(bh, vox, bxf);
    dbStoneBefore = 40;

    EntityIO eio = MakeEntityIO(c.debris, c.mobs, nullptr);
    SaveReport rep;
    const bool saved = SaveWorld(ctx, world, stream, kPath, P, &eio, {}, &rep);

    // The region: every stored chunk inside the window, as the GPU holds it
    // NOW (ids of the running table, which the files label as `P`).
    std::vector<IVec3> chunks;
    stream.Store().ForEachStored([&](IVec3 wc, const uint32_t*, size_t) {
      if (chunks.size() < 96 && world.ChunkInWindow(wc)) chunks.push_back(wc);
    });
    std::vector<uint32_t> before(chunks.size() * kChunkVol), after(before.size());
    for (size_t i = 0; i < chunks.size(); i++)
      ReadVoxelsSync(ctx, world, World::SlotChunkIndex(chunks[i]), 1,
                     before.data() + i * kChunkVol, "matremap-before");

    // Break the live state so a load that did nothing cannot pass.
    c.debris.Reset();
    c.mobs.Reset();
    loaded = LoadWorld(ctx, world, sim, stream, kPath, c.mats, &eio);
    ctx.WaitIdle();
    for (size_t i = 0; i < chunks.size(); i++)
      ReadVoxelsSync(ctx, world, World::SlotChunkIndex(chunks[i]), 1,
                     after.data() + i * kChunkVol, "matremap-after");

    // BY NAME: (name, stain name, the rest of the word) of every cell, the
    // saved side read through `P`, the loaded side through the real table.
    auto nameHash = [](const std::vector<uint32_t>& words, const MaterialNameTable& T,
                       uint32_t& moved, const std::vector<uint32_t>* other) {
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) {
          h ^= b[i];
          h *= 1099511628211ull;
        }
      };
      for (size_t k = 0; k < words.size(); k++) {
        const uint32_t w = words[k] & kPersistMask;
        const uint32_t m = w & 0xFFFu, st = (w >> 28) & 7u;
        const std::string& mn = m < T.mats.size() ? T.mats[m] : std::string("?");
        const std::string& sn = st < T.stains.size() ? T.stains[st] : std::string("?");
        mix(mn.data(), mn.size());
        mix("|", 1);
        mix(sn.data(), sn.size());
        const uint32_t rest = w & ~0x70000FFFu;
        mix(&rest, 4);
        if (other && ((*other)[k] & 0xFFFu) != m) moved++;
      }
      return h;
    };
    uint32_t unused = 0;
    const uint64_t hSaved = nameHash(before, Pt, unused, nullptr);
    const uint64_t hLoaded = nameHash(after, R, bMoved, &before);
    bChunks = (uint32_t)chunks.size();
    bCells = (uint32_t)before.size();

    // The entities followed the names too.
    const Mob* lm = c.mobs.MobCount() > 0 ? c.mobs.MobAt(0) : nullptr;
    coatAfter = lm ? coatCount(lm->Id(), mWater) : 0;
    const uint32_t bloodAfter = lm ? coatCount(lm->Id(), mBlood) : 0;
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
      std::vector<PrefabVoxel> lat;
      uint32_t sc = 1;
      if (!c.debris.BodyLatticeOf(c.debris.BodyHandle(i), lat, sc) || lat.size() != 64)
        continue;
      for (const PrefabVoxel& v : lat) {
        if ((int)v.material == mStone) dbStoneAfter++;
        if ((int)v.material == mWood) dbWoodAfter++;
      }
    }
    const bool regionOk = saved && loaded && bChunks > 0 && hSaved == hLoaded && bMoved > 0;
    const bool coatOk = soaked > 0 && coatBefore > 0 && coatAfter == coatBefore && bloodAfter == 0;
    const bool debrisOk = bh != 0 && dbWoodAfter == 40 && dbStoneAfter == 24;
    bOk = regionOk && coatOk && debrisOk;
    if (!regionOk)
      whyB = Format(" region: saved %d loaded %d chunks %u name-hash %016llx vs %016llx, "
                    "%u cells moved",
                    saved ? 1 : 0, loaded ? 1 : 0, bChunks, (unsigned long long)hSaved,
                    (unsigned long long)hLoaded, bMoved);
    else if (!coatOk)
      whyB = Format(" coat: soaked %u, blood before %u, water after %u, blood after %u",
                    soaked, coatBefore, coatAfter, bloodAfter);
    else if (!debrisOk)
      whyB = Format(" debris: body %d, after stone %u wood %u (want 24/40)", bh != 0 ? 1 : 0,
                    dbStoneAfter, dbWoodAfter);

    // Teardown: nothing of this gate survives into the next one.
    c.debris.Reset();
    c.mobs.Reset();
    stream.Store().Unbind();
    stream.Store().Tables().SetRunning(R);
    std::filesystem::remove_all(kPath);
    stream.OnRegen();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
  }

  // ======== C: a material that CHANGED CLASS keeps its eighths =============
  // (docs/PLAN_powder_mass.md P4) A table that recorded classes and says
  // "sand" was a LIQUID: its 3/8 fullness must load as 3/8 of grains, a full
  // cell as full; and a saved powder "water" of 5/8 as fullness 5/8. Pure
  // table arithmetic -- no GPU.
  bool cOk = true;
  {
    MaterialNameTable S2 = R;
    if (!S2.classes.empty()) {
      S2.classes[(size_t)mSand] = (uint8_t)CLASS_LIQUID;
      S2.classes[(size_t)mWater] = (uint8_t)CLASS_POWDER;
      const MatRemap r2 = BuildMatRemap(S2, R);
      const uint32_t sand38 = r2.Word((uint32_t)mSand | (2u << 12));      // fullness 3/8
      const uint32_t sandFull = r2.Word((uint32_t)mSand | (7u << 12));    // fullness 8/8
      const uint32_t water58 = r2.Word((uint32_t)mWater | (7u << 12));    // mass 5/8
      cOk = PowderMassOfState((sand38 >> 12) & 0xFu) == 3u &&
            PowderMassOfState((sandFull >> 12) & 0xFu) == kPowderFull &&
            ((water58 >> 12) & 0xFu) == 4u && !r2.identity;
      // ...and a table WITHOUT classes (every save before 2026-09-26) converts
      // nothing: its nibbles load exactly as stored.
      MaterialNameTable S3 = S2;
      S3.classes.clear();
      const MatRemap r3 = BuildMatRemap(S3, R);
      cOk = cOk && r3.Word((uint32_t)mSand | (2u << 12)) == ((uint32_t)mSand | (2u << 12));
    } else {
      cOk = false;
    }
  }

  const bool ok = aOk && bOk && cOk;
  detail = Format(
      "A store=%d (%u cells, %u ids moved, %u to air, %u stain types moved)%s | "
      "B end-to-end=%d (%u chunks / %u cells, %u cells moved by name; coat %u "
      "blood -> %u water; debris 40 stone -> %u wood)%s | C class change "
      "keeps eighths=%d",
      aOk ? 1 : 0, aVoxels, aMoved, aToAir, aStains, whyA.empty() ? "" : (" " + whyA).c_str(),
      bOk ? 1 : 0, bChunks, bCells, bMoved, coatBefore, coatAfter, dbWoodAfter,
      whyB.c_str(), cOk ? 1 : 0);
  std::printf("save-material-remap: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WorldIoGates() {
  static const std::vector<Gate> g = {
      {"save-load", "worldio", {}, false, GateSaveLoad},
      {"save-entities", "worldio", {}, false, GateSaveEntities},
      {"save-split", "worldio", {}, false, GateSaveSplit},
      {"save-material-remap", "worldio", {}, false, GateSaveMaterialRemap},
      {"region-store", "worldio", {}, false, GateRegionStore},
      {"region-codec", "worldio", {}, false, GateRegionCodec},
      {"streaming", "worldio", {}, false, GateStreaming},
      {"chunk-exchange", "worldio", {}, false, GateChunkExchange},
  };
  return g;
}

}  // namespace selftest
