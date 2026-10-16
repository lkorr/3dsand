// =============================================================================
// distant-schedule / distant-save -- the distant tier's LIFE PAST THE WINDOW
// (docs/PLAN_distant_npcs.md packages D5+D6, §4; game/distant.h, mob.h
// MobSystem::ActivateParked, world/refs_npc.cpp's distant residents).
// =============================================================================
//
// distant-schedule -- a village 90 m away is alive before you arrive:
//
//   K. PARKED. Two villagers spawned by their refs at the home window (V1 on a
//      two-row day: work at marker A until noon, eat at marker B after; V2 on
//      one all-day row at marker C), then the window teleports ~90 m away with
//      `distant.enabled` OFF: both PARK (records in their buckets, the refs
//      dormant).
//   A. ACTIVATED. The switch on, the next ticks of THE tick (EntityParking in
//      phase H): both parked records come back as DISTANT creatures within
//      distantSchedule.activateMaxTicks -- under fresh ids, their refs kept,
//      the buckets emptied, nothing near -- and LocateByRef finds each.
//   W. THE DAY WALKS. The clock moves to 13:00: V1, distant, its ref dormant,
//      WALKS from A to B (a movement goal: no tick moves it more than its own
//      speed allows) and arrives within distantSchedule.arriveMaxTicks, then
//      holds at its anchor and the tier sleeps it (rule 2: steps stop). V2,
//      already at C, never moves.
//   P. PROMOTED WHERE IT WALKED. The window comes home: V1 promotes at B
//      exactly where it walked to (x / z bit-identical to its distant origin:
//      NO catch-up teleport -- it moved while distant), within the row's
//      arrive radius of B; V2, which never moved, promotes THROUGH the
//      catch-up placer (it is the never-moved path `npc-catchup` holds for a
//      parked record): the placer is asked for V2 only.
//   D. TWICE-RUN. K..P from two fresh worlds: the per-tick trace (V1's origin
//      and heading bits, its resident row and phase; both promoted origins)
//      is identical.
//
// distant-save -- a save keeps what happened at distance:
//
//   S. Five distant creatures (demoted through the park rule's own door, then
//      stood 140+ voxels outside the window): a duelist with a sword; one
//      MAIMED at distance (its sword hand cut off through the impact pass's
//      own sever, MobSystem::SeverDistantForTest -- the stump bleeds, the
//      sword falls); one BURNING; one in armour (the record's worn slots); a
//      corpse. SaveWorld writes K records.
//   L. LOADED, ACTIVATED. A Reset, LoadWorld (no record is in the window: none
//      applied), then ONE Unpark call (DIRECT PHASE CALL ON PURPOSE: the claim
//      is about that call -- activation needs no load-specific path): the same
//      K are distant again. Matched by (def, origin): every limb's hp
//      bit-identical, every sever kept, the bleed rate, the burn ticks and
//      fraction, the open stumps and the last wound bit-identical, the corpse
//      dead with its cause, the maimed one's sword hand empty, the plain
//      duelist's style list the one its rig gave it.
//   N. THE NEAR DOOR. The maimed record loaded straight into the near tier
//      (LoadOne -- an unpark or a load of a retired creature): the cut limb has
//      no body, every other limb's hp is the record's, the owed blood is
//      re-opened; the burning record re-lights its trunk.
//   U. v12 -> v13. A v13 record with its status tail cut off reads as v12 and
//      upgrades back to the same bytes (MobSystem::UpgradeRecordToCurrent).
//
// LEAVES NOTHING: tuning, the minute override, the residents and the id
// counter restored; mobs, debris and the tier reset; the park function and the
// placer removed; the store cleared; scratch dirs deleted; the window home and
// worldgen regenerated there.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/distant.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/persist.h"
#include "game/schedule.h"
#include "phys/debris.h"
#include "sim/chunkstore.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/worldgen_run.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "world/refs.h"
#include "world/refs_npc.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;

// FNV-1a 64 over raw bytes: the trace digests.
struct Digest {
  uint64_t h = 1469598103934665603ull;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void F(float v) { Bytes(&v, sizeof v); }
  void U(uint64_t v) { Bytes(&v, sizeof v); }
};

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// The tier with the gate's own numbers (whatever tuning.json says), on or off.
void SetTier(bool on) {
  Tuning t = CurrentTuning();
  t.distant.enabled = on;
  t.distant.radius = 1280.0f;
  t.distant.hysteresis = 256.0f;
  t.distant.idleStride = 4;
  t.distant.surfaceSnapMaxVox = 24.0f;
  t.distant.promoteMaxDropVox = 48.0f;
  SetCurrentTuning(t);
}

// A teleport, done the way LoadWorld moves the window (npc-catchup's idiom).
void Teleport(Ctx& c, IVec3 origin) {
  c.world.InvalidateSnapshot();
  c.stream.ReloadWindow(origin);
  rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
  c.sim.EncodeLoadReset(enc);
  rhi::CommandBuffer cmd = enc.Finish();
  c.ctx.queue.Submit(1, &cmd);
  c.ctx.WaitIdle();
}

// ============================================================================
// distant-schedule
// ============================================================================

constexpr const char* kDayName = "dsched_day";
constexpr const char* kDayText = R"JSON({
  "about": "distant-schedule fixture, V1 (src/test/selftest_distant_life.cpp)",
  "rows": [
    { "from": "00:00", "to": "12:00", "do": "work", "at": "work" },
    { "from": "12:00", "to": "24:00", "do": "eat", "at": "home" }
  ]
})JSON";
constexpr const char* kStillName = "dsched_still";
constexpr const char* kStillText = R"JSON({
  "about": "distant-schedule fixture, V2 (src/test/selftest_distant_life.cpp)",
  "rows": [
    { "from": "00:00", "to": "24:00", "do": "work", "at": "work" }
  ]
})JSON";
constexpr const char* kV1 = "dsched/v1";
constexpr const char* kV2 = "dsched/v2";

bool InstallDays(std::string& why) {
  for (const auto& [name, text] : {std::pair<const char*, const char*>{kDayName, kDayText},
                                   std::pair<const char*, const char*>{kStillName, kStillText}}) {
    schedule::Schedule sc;
    std::vector<std::string> probs;
    if (!schedule::Parse(name, text, "(distant-schedule fixture)", sc, probs) || !probs.empty()) {
      why = std::string("fixture schedule ") + name + " did not parse: " +
            (probs.empty() ? std::string("?") : probs[0]);
      return false;
    }
    refs::Schedules().Add(sc);
  }
  return true;
}

struct DayRun {
  bool ok = false;
  std::vector<std::string> fails;
  uint64_t trace = 0;
  // K
  uint64_t parked = 0;
  int liveAway = -1, distantAway = -1;
  // A
  int activatedAfter = -1;
  uint64_t activated = 0;
  int bucketsLeft = -1;
  bool locateV1 = false, locateV2 = false;
  // W
  int arriveTicks = -1, asleepTicks = -1;
  float walked = 0.0f, fromB = -1.0f, maxStep = 0.0f, stepCap = 0.0f;
  bool v1Moved = false, v2Moved = true;
  uint64_t stepsAsleep = ~0ull;
  uint64_t schedSteps = 0, schedHolds = 0;
  // P
  int promoteTicks = -1;
  bool v1Back = false, v2Back = false, v1Kept = false, v2Placed = false;
  float v1FromB = -1.0f;
  std::vector<std::string> placerAsked;
  uint64_t catchUps = 0;
  Vec3 v1Promoted{}, v2Promoted{};
};

DayRun RunDistantDay(Ctx& c, uint32_t t0) {
  DayRun out;
  auto check = [&](bool ok, const std::string& w) {
    if (!ok) out.fails.push_back(w);
    return ok;
  };
  // ---- a fresh world at home ----
  const IVec3 home{0, 0, 0};
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(6000);   // both runs issue the same ids (hash salts)
  c.mobs.SetItems(&c.items);
  refs::ResetResidents();
  c.world.SetWindowOrigin(home);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  SetTier(false);
  c.mobs.SetDistantStream(&c.stream);

  // ---- the refs: three markers on open ground near the window centre ----
  const std::string root = "build/selftest_distant_sched";
  {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root + "/worldmap/dsched/refs", ec);
  }
  refs::RefStore st;
  st.LoadMap(root, "dsched");
  const int bx = (int)kWorldN / 2, bz = (int)kWorldN / 2;
  auto groundY = [&](int x, int z) { return World::TerrainHeight(x, z, kDefaultSeed) + 1; };
  std::string err;
  auto place = [&](const char* id, const char* kind, int x, int z, refs::Json props,
                   const char* base = "") {
    refs::Ref r;
    r.id = id;
    r.kind = kind;
    r.base = base;
    r.pos = {x, groundY(x, z), z};
    r.yaw = 0;
    r.props = std::move(props);
    return refs::Place(st, r, &err);
  };
  using J = refs::Json;
  const int ax = bx - 30, az = bz, bxB = bx + 30, bzB = bz, cx = bx, cz = bz + 40;
  bool placed = place("dsched/a", "marker", ax, az, J::object()) &&
                place("dsched/b", "marker", bxB, bzB, J::object()) &&
                place("dsched/c", "marker", cx, cz, J::object()) &&
                place(kV1, "npc", ax, az,
                      J{{"name", "Distant Walker"}, {"schedule", kDayName},
                        {"work", "dsched/a"}, {"home", "dsched/b"}, {"behavior", "villager"}},
                      "human") &&
                place(kV2, "npc", cx, cz,
                      J{{"name", "Distant Stander"}, {"schedule", kStillName},
                        {"work", "dsched/c"}, {"behavior", "villager"}},
                      "human");
  if (!check(placed, "placing the fixture refs: " + err)) return out;
  st.BindChunkStore(&c.stream.Store());
  const refs::Anchor anchorB = refs::ResolveAnchor(st, *st.Find(kV1), "home");
  const refs::Anchor anchorC = refs::ResolveAnchor(st, *st.Find(kV2), "work");

  // ---- the catch-up placer, counting (the npc kind installs its own only
  // when none is there): who it is asked for, then the resident layer's own
  // answer (refs::ScheduledPlace) ----
  uint32_t t = t0;
  std::vector<std::string>& asked = out.placerAsked;
  c.mobs.SetUnparkPlacer([&st, &t, &asked](const std::string& refId, Vec3& foot, float& heading) {
    asked.push_back(refId);
    const refs::Ref* nr = st.Find(refId);
    if (nr == nullptr || nr->kind != "npc") return false;
    if (!refs::ScheduledPlace(st, *nr, t, foot, heading)) return false;
    refs::ForgetResident(refId);   // re-plan from where it now stands
    return true;
  });

  EntityParking parking;
  parking.Bind(c.mobs, c.stream.Store(), true);
  const int n = (int)kNChunk;
  const IVec3 away{home.x + n + 8, home.y, home.z + n + 8};
  const IVec3 homeChunk{bx >> 4, groundY(bx, bz) >> 4, bz >> 4};
  const IVec3 awayChunk{away.x + n / 2, homeChunk.y, away.z + n / 2};
  std::unique_ptr<support::TickRig> rig;
  auto rigAt = [&](IVec3 chunk) {
    rig = std::make_unique<support::TickRig>(c, t, chunk);
    rig->Authority().refs = &st;
    rig->Authority().parking = &parking;   // THE tick's phase-H door
  };
  auto tick = [&]() {
    support::RunTicks(*rig, 1);
    t = rig->tick;
  };
  auto countNear = [&](const char* ref) {
    int k = 0;
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++)
      if (const Mob* m = c.mobs.MobAt(i); m != nullptr && m->RefId() == ref) k++;
    return k;
  };
  auto countFar = [&](const char* ref) {
    int k = 0;
    for (const DistantMob& d : c.mobs.Distant().All()) k += d.refId == ref ? 1 : 0;
    return k;
  };
  const DistantMobs& tier = c.mobs.Distant();

  // ======== K: spawned by the refs at home, then PARKED (switch off) =========
  schedule::SetMinuteOverride(8 * 60);   // 08:00: V1 at work (A), V2 at work (C)
  rigAt(homeChunk);
  int waited = 0;
  while ((countNear(kV1) == 0 || countNear(kV2) == 0) && waited < 150) {
    tick();
    waited++;
  }
  for (int i = 0; i < 20; i++) tick();
  check(countNear(kV1) == 1 && countNear(kV2) == 1,
        Format("K: the refs spawned V1 %d / V2 %d after %d ticks", countNear(kV1),
               countNear(kV2), waited));
  const uint64_t parked0 = parking.GetStats().parked;
  Teleport(c, away);
  parking.ResetWaits();
  rigAt(awayChunk);
  for (int i = 0; i < 3; i++) tick();
  out.parked = parking.GetStats().parked - parked0;
  out.liveAway = countNear(kV1) + countNear(kV2);
  out.distantAway = (int)tier.Count();
  check(out.parked == 2 && out.liveAway == 0 && out.distantAway == 0 && !st.IsActive(kV1),
        Format("K: away with the switch off: parked %llu, near %d, distant %d, ref active %d",
               (unsigned long long)out.parked, out.liveAway, out.distantAway, st.IsActive(kV1)));

  // ======== A: the switch on -- ACTIVATED ======================================
  SetTier(true);
  const DistantMobs::Stats a0 = tier.GetStats();
  const int actMax = (int)BaselineNumber("distantSchedule.activateMaxTicks", 10);
  for (int i = 0; i < actMax && (countFar(kV1) == 0 || countFar(kV2) == 0); i++) {
    tick();
    if (countFar(kV1) == 1 && countFar(kV2) == 1) out.activatedAfter = i + 1;
  }
  out.activated = tier.GetStats().activated - a0.activated;
  {
    int left = 0;
    std::vector<IVec3> regs;
    c.stream.Store().EntityRegionsAny(regs);
    for (const IVec3& rc : regs)
      for (const ChunkStore::EntityRecord& r : c.stream.Store().DormantEntities(rc))
        left += r.section == ('M' | ('O' << 8) | ('B' << 16) | ((uint32_t)'S' << 24)) ? 1 : 0;
    out.bucketsLeft = left;
  }
  const MobSystem::CreatureWhere w1 = c.mobs.LocateByRef(kV1), w2 = c.mobs.LocateByRef(kV2);
  out.locateV1 = w1.exists && w1.distant && w1.alive;
  out.locateV2 = w2.exists && w2.distant && w2.alive;
  const uint64_t id1 = w1.id, id2 = w2.id;
  check(out.activatedAfter >= 1 && out.activated == 2 && out.bucketsLeft == 0 &&
            out.locateV1 && out.locateV2 && countNear(kV1) + countNear(kV2) == 0,
        Format("A: activated %llu after %d ticks (max %d), %d records left parked, located "
               "V1 %d V2 %d",
               (unsigned long long)out.activated, out.activatedAfter, actMax, out.bucketsLeft,
               out.locateV1, out.locateV2));
  const DistantMob* d2start = tier.Find(id2);
  const Vec3 v2Start = d2start != nullptr ? d2start->origin : Vec3{};

  // ======== W: the day walks at distance =======================================
  schedule::SetMinuteOverride(13 * 60);   // 13:00: V1 eats at B
  const int arriveMax = (int)BaselineNumber("distantSchedule.arriveMaxTicks", 900);
  const float speed = c.mobs.Defs()[(size_t)c.mobs.FindDef("human")].speed;
  out.stepCap = speed * (1.0f / 30.0f) * 4.0f + 1.0f;   // a stride's worth, a voxel slack
  Digest dg;
  Vec3 prev = tier.Find(id1) != nullptr ? tier.Find(id1)->origin : Vec3{};
  const DistantMobs::Stats w0 = tier.GetStats();
  for (int i = 0; i < arriveMax; i++) {
    tick();
    const DistantMob* d = tier.Find(id1);
    if (d == nullptr) break;
    dg.F(d->origin.x);
    dg.F(d->origin.y);
    dg.F(d->origin.z);
    dg.F(d->steer.heading);
    refs::ResidentStatus rs;
    if (refs::ResidentStatusOf(kV1, rs)) {
      dg.U((uint64_t)(int64_t)rs.row);
      dg.U((uint64_t)rs.phase);
    }
    const float dx = d->origin.x - prev.x, dz = d->origin.z - prev.z;
    out.maxStep = std::max(out.maxStep, std::sqrt(dx * dx + dz * dz));
    prev = d->origin;
    if (out.arriveTicks < 0 && refs::ResidentStatusOf(kV1, rs) && rs.row == 1 &&
        rs.phase == refs::ResidentPhase::AtAnchor && rs.arrivedTick != 0)
      out.arriveTicks = i + 1;
    if (out.arriveTicks >= 0 && d->asleep) {
      out.asleepTicks = i + 1;
      break;
    }
  }
  if (const DistantMob* d = tier.Find(id1)) {
    out.walked = d->walked;
    out.v1Moved = d->movedWhileDistant;
    const Vec3 cc = d->Centre();
    out.fromB = std::sqrt((cc.x - anchorB.foot.x) * (cc.x - anchorB.foot.x) +
                          (cc.z - anchorB.foot.z) * (cc.z - anchorB.foot.z));
  }
  if (const DistantMob* d = tier.Find(id2))
    out.v2Moved = d->movedWhileDistant || std::memcmp(&d->origin, &v2Start, sizeof(Vec3)) != 0;
  // ...and asleep it costs nothing: 60 ticks with no step.
  {
    const uint64_t s0 = tier.GetStats().steps;
    for (int i = 0; i < 60; i++) tick();
    out.stepsAsleep = tier.GetStats().steps - s0;
  }
  out.schedSteps = tier.GetStats().scheduleSteps - w0.scheduleSteps;
  out.schedHolds = tier.GetStats().scheduleHolds - w0.scheduleHolds;
  const float arriveR = 3.0f + 1.5f;   // refs_npc.cpp ArriveRadius("eat") + a stride's slack
  check(out.arriveTicks > 0 && out.asleepTicks > 0 && out.v1Moved && out.walked >= 40.0f &&
            out.fromB <= arriveR && out.maxStep <= out.stepCap && !out.v2Moved &&
            out.stepsAsleep == 0 && out.schedSteps > 0,
        Format("W: V1 arrived at B after %d ticks (max %d), asleep after %d, walked %.1f, %.2f "
               "from B (radius %.1f), largest tick step %.2f (cap %.2f), moved %d; V2 moved %d; "
               "%llu steps while asleep; %llu resident steps, %llu holds",
               out.arriveTicks, arriveMax, out.asleepTicks, (double)out.walked,
               (double)out.fromB, (double)arriveR, (double)out.maxStep, (double)out.stepCap,
               out.v1Moved, out.v2Moved, (unsigned long long)out.stepsAsleep,
               (unsigned long long)out.schedSteps, (unsigned long long)out.schedHolds));
  const DistantMob* d1end = tier.Find(id1);
  const Vec3 v1Distant = d1end != nullptr ? d1end->origin : Vec3{};
  const Vec3 v2Distant = tier.Find(id2) != nullptr ? tier.Find(id2)->origin : Vec3{};

  // ======== P: home -- promoted where it walked ================================
  asked.clear();
  const DistantMobs::Stats p0 = tier.GetStats();
  Teleport(c, home);
  parking.ResetWaits();
  rigAt(homeChunk);
  const int promoteMax = (int)BaselineNumber("distantSchedule.promoteMaxTicks", 240);
  for (int i = 0; i < promoteMax; i++) {
    tick();
    if (c.mobs.FindMobById(id1) != nullptr && c.mobs.FindMobById(id2) != nullptr) {
      out.promoteTicks = i + 1;
      break;
    }
  }
  out.catchUps = tier.GetStats().promoteCatchUps - p0.promoteCatchUps;
  if (const Mob* m = c.mobs.FindMobById(id1)) {
    out.v1Back = true;
    out.v1Promoted = m->Origin();
    out.v1Kept = SameBits(out.v1Promoted.x, v1Distant.x) && SameBits(out.v1Promoted.z, v1Distant.z);
    const Vec3 ws = m->Def()->worldSize;
    const float fx = out.v1Promoted.x + ws.x * 0.5f, fz = out.v1Promoted.z + ws.z * 0.5f;
    out.v1FromB = std::sqrt((fx - anchorB.foot.x) * (fx - anchorB.foot.x) +
                            (fz - anchorB.foot.z) * (fz - anchorB.foot.z));
  }
  if (const Mob* m = c.mobs.FindMobById(id2)) {
    out.v2Back = true;
    out.v2Promoted = m->Origin();
    const Vec3 ws = m->Def()->worldSize;
    // THE PLACER'S ANSWER: the anchor's foot, not floored (its parked origin
    // was the first spawn's floored one).
    out.v2Placed = SameBits(out.v2Promoted.x, anchorC.foot.x - ws.x * 0.5f) &&
                   SameBits(out.v2Promoted.z, anchorC.foot.z - ws.z * 0.5f) &&
                   !(SameBits(out.v2Promoted.x, v2Distant.x) && SameBits(out.v2Promoted.z, v2Distant.z));
  }
  const bool askedV1 = std::find(asked.begin(), asked.end(), kV1) != asked.end();
  const bool askedV2 = std::find(asked.begin(), asked.end(), kV2) != asked.end();
  check(out.v1Back && out.v2Back && out.v1Kept && out.v1FromB <= arriveR && !askedV1 && askedV2 &&
            out.v2Placed && out.catchUps == 1,
        Format("P: back after %d ticks (V1 %d, V2 %d); V1 x/z kept %d, %.2f from B, placer asked "
               "for V1 %d; V2 placed by the catch-up %d (asked %d), catch-ups %llu",
               out.promoteTicks, out.v1Back, out.v2Back, out.v1Kept, (double)out.v1FromB,
               askedV1, out.v2Placed, askedV2, (unsigned long long)out.catchUps));
  dg.F(out.v1Promoted.x);
  dg.F(out.v1Promoted.y);
  dg.F(out.v1Promoted.z);
  dg.F(out.v2Promoted.x);
  dg.F(out.v2Promoted.y);
  dg.F(out.v2Promoted.z);
  out.trace = dg.h;

  // ---- leave nothing (the next run starts from Fresh anyway) ----
  rig.reset();
  {
    refs::RefCtx rc{&st, &c.mobs, &c.world, &c.stream.Store()};
    rc.phys = &c.phys;
    rc.debris = &c.debris;
    st.DeactivateAll(rc, refs::RefEvent::Reset);
  }
  refs::ResetResidents();
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.SetParkFn(nullptr);
  c.mobs.SetUnparkPlacer(nullptr);
  c.mobs.ClearPlayerActors();
  c.stream.Store().Clear();
  {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  out.ok = out.fails.empty();
  return out;
}

Status GateDistantSchedule(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  const IVec3 savedOrigin = c.world.WindowOrigin();
  refs::RegisterAllKinds();
  std::string why;
  if (!InstallDays(why)) {
    detail = why;
    return Status::Fail;
  }
  const DayRun a = RunDistantDay(c, 23000);
  const DayRun b = RunDistantDay(c, 23000);
  const bool same = a.trace == b.trace && a.trace != 0;
  // ---- LEAVES NOTHING ----
  schedule::SetMinuteOverride(-1);
  SetCurrentTuning(tuneWas);
  c.mobs.DistantGroundMut().InvalidateAll();
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  RecordObserved("distantSchedule.activatedAfterTicks", (double)a.activatedAfter);
  RecordObserved("distantSchedule.arriveTicks", (double)a.arriveTicks);
  RecordObserved("distantSchedule.asleepTicks", (double)a.asleepTicks);
  RecordObserved("distantSchedule.walkedVox", (double)a.walked);
  RecordObserved("distantSchedule.promoteTicks", (double)a.promoteTicks);
  RecordObserved("distantSchedule.v1FromBVox", (double)a.v1FromB);
  detail = Format(
      "K %s: 2 parked, refs dormant | A %s: 2 activated after %d ticks, buckets empty | W %s: "
      "V1 walked %.1f vox A->B, arrived after %d ticks, asleep after %d (largest step %.2f), "
      "V2 still | P %s: V1 back %.2f from B where it walked (no catch-up), V2 by the "
      "catch-up after %d ticks | D %s: twice-run %016llx / %016llx",
      a.parked == 2 ? "ok" : "FAIL", a.activated == 2 ? "ok" : "FAIL", a.activatedAfter,
      a.arriveTicks > 0 ? "ok" : "FAIL", (double)a.walked, a.arriveTicks, a.asleepTicks,
      (double)a.maxStep, a.v1Kept && a.v2Placed ? "ok" : "FAIL", (double)a.v1FromB,
      a.promoteTicks, same ? "ok" : "FAIL", (unsigned long long)a.trace,
      (unsigned long long)b.trace);
  std::vector<std::string> fails = a.fails;
  for (const std::string& f : b.fails) fails.push_back("(run 2) " + f);
  if (!same) fails.push_back("D: the two runs of the same day differ");
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ============================================================================
// distant-save
// ============================================================================

// What a distant creature must still be after save -> load -> activation.
struct Expect {
  std::string def;
  Vec3 origin{};
  std::vector<float> hp;
  std::vector<uint8_t> attached;
  float bleed = 0.0f, burnFrac = 0.0f;
  uint16_t burn = 0;
  uint8_t stumps = 0;
  int16_t lastWound = -1;
  bool dead = false;
  std::string cause;
  bool weapon[2] = {false, false};
  std::vector<std::string> styles;
  std::string label;
};

Expect ExpectOf(const DistantMob& d, const char* label) {
  Expect e;
  e.def = d.defName;
  e.origin = d.origin;
  e.hp = d.limbHp;
  for (const DistantLimbRole& r : d.limbRole)
    e.attached.push_back((r.flags & kDistantLimbAttached) != 0 ? 1 : 0);
  e.bleed = d.bloodLossPerTick;
  e.burnFrac = d.burnFrac;
  e.burn = d.burnTicksLeft;
  e.stumps = d.stumps;
  e.lastWound = d.bloodLossPerTick > 0.0f ? d.lastWoundLimb : (int16_t)-1;
  e.dead = d.dead;
  e.cause = d.cause;
  for (int h = 0; h < kHands; h++) e.weapon[h] = d.hands[h].weapon;
  for (const distant::DistantStyle& s : d.kit.styles) e.styles.push_back(s.name);
  e.label = label;
  return e;
}

// The first field that differs, or "" (CLAUDE.md rule 6: name what differs).
std::string CompareTo(const Expect& e, const DistantMob& d, bool styles) {
  if (d.defName != e.def) return "def";
  if (d.limbHp.size() != e.hp.size()) return Format("limb count %zu vs %zu", d.limbHp.size(), e.hp.size());
  for (size_t i = 0; i < e.hp.size(); i++)
    if (!SameBits(d.limbHp[i], e.hp[i]))
      return Format("limb %zu hp %.3f vs %.3f", i, (double)d.limbHp[i], (double)e.hp[i]);
  for (size_t i = 0; i < e.attached.size() && i < d.limbRole.size(); i++)
    if (((d.limbRole[i].flags & kDistantLimbAttached) != 0 ? 1 : 0) != e.attached[i])
      return Format("limb %zu attached %d vs %d", i, (d.limbRole[i].flags & kDistantLimbAttached) != 0,
                    e.attached[i]);
  if (d.dead != e.dead) return Format("dead %d vs %d", d.dead, e.dead);
  if (e.dead) return d.cause == e.cause ? std::string() : "cause '" + d.cause + "' vs '" + e.cause + "'";
  if (!SameBits(d.bloodLossPerTick, e.bleed))
    return Format("bleed %.6f vs %.6f", (double)d.bloodLossPerTick, (double)e.bleed);
  if (d.burnTicksLeft != e.burn) return Format("burn ticks %u vs %u", d.burnTicksLeft, e.burn);
  if (!SameBits(d.burnFrac, e.burnFrac)) return "burn fraction";
  if (d.stumps != e.stumps) return Format("stumps %u vs %u", d.stumps, e.stumps);
  const int16_t lw = d.bloodLossPerTick > 0.0f ? d.lastWoundLimb : (int16_t)-1;
  if (lw != e.lastWound) return Format("last wound %d vs %d", lw, e.lastWound);
  for (int h = 0; h < kHands; h++)
    if (d.hands[h].weapon != e.weapon[h])
      return Format("hand %d weapon %d vs %d", h, d.hands[h].weapon, e.weapon[h]);
  if (styles) {
    std::vector<std::string> got;
    for (const distant::DistantStyle& s : d.kit.styles) got.push_back(s.name);
    if (got != e.styles) return Format("styles (%zu vs %zu)", got.size(), e.styles.size());
  }
  return std::string();
}

Status GateDistantSave(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  const char* kPath = "selftest_distant_save.svd";
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& why) {
    if (!ok) fails.push_back(why);
    return ok;
  };
  {
    std::error_code ec;
    fs::remove_all(kPath, ec);
  }
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(7000);
  c.mobs.SetItems(&c.items);
  c.mobs.SetParkFn(nullptr);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  SetTier(true);
  c.mobs.SetDistantStream(&c.stream);
  const int human = c.mobs.FindDef("human");
  const ItemDef* sword = c.items.Named("sword");
  if (human < 0 || sword == nullptr) {
    detail = human < 0 ? "no 'human' def" : "no 'sword' item";
    SetCurrentTuning(tuneWas);
    return Status::Fail;
  }
  const IVec3 home = c.world.WindowOrigin();
  const Vec3 wlo{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
                 (float)(home.z * (int)kChunk)};
  const int bx = (int)wlo.x + (int)kWorldN / 2, bz = (int)wlo.z + (int)kWorldN / 2;
  uint32_t t = 24000;
  support::TickCursor realTick{c, t, IVec3{bx >> 4, World::TerrainHeight(bx, bz, kDefaultSeed) >> 4,
                                           bz >> 4}};
  DistantGround& ground = c.mobs.DistantGroundMut();
  DistantMobs& tierMut = c.mobs.DistantMut();
  const DistantMobs& tier = c.mobs.Distant();

  // ======== S: five distant creatures, stood outside the window =============
  // Made at the window centre (posed by 4 real ticks: a fresh rig's reach is 0
  // until it has been posed, D2b's note), demoted through the park rule's own
  // door, then stood 140+ voxels outside the -X face.
  enum { kPlain = 0, kMaimed, kBurning, kArmoured, kCorpse, kCount };
  const char* labels[kCount] = {"plain", "maimed", "burning", "armoured", "corpse"};
  uint64_t ids[kCount] = {};
  int worn = 0;
  for (int k = 0; k < kCount; k++) {
    const uint64_t id = c.mobs.Spawn(human, {bx, World::TerrainHeight(bx, bz, kDefaultSeed) + 1, bz});
    if (id == 0) continue;
    c.mobs.SetMobBehavior(id, "duelist");
    if (k != kArmoured) c.mobs.EquipItem(id, sword);
    if (k == kArmoured) {
      bool used[kEquipSlotCount] = {};
      for (const ItemDef& it : c.items.items) {
        if (worn >= 2 || !ItemKindIsWorn(it.kind) || it.name.rfind("iron_", 0) != 0) continue;
        for (int sl = 0; sl < kEquipSlotCount; sl++)
          if (!used[sl] && EquipSlotAccepts(sl, it.kind)) {
            if (c.mobs.WearItem(id, &it, sl, 0)) {
              used[sl] = true;
              worn++;
            }
            break;
          }
      }
    }
    for (int i = 0; i < 4; i++) realTick();
    if (k == kCorpse) {
      const MobDef& hd = c.mobs.Defs()[(size_t)human];
      if (const uint64_t rb = c.mobs.LimbBody(id, hd.rootLimb))
        c.mobs.Damage(rb, 1.0e6f, c.mobs.LimbVoxelPos(id, hd.rootLimb, 0), 0.0f);
      for (int i = 0; i < 4; i++) realTick();
    }
    if (!c.mobs.DemoteForTest(id)) continue;
    // Stood on its ground outside the window.
    if (DistantMob* d = tierMut.Find(id)) {
      const float x = wlo.x - 140.0f - 30.0f * (float)k, z = wlo.z + (float)kWorldN * 0.5f;
      const int cx = ifloor(x), cz = ifloor(z);
      const Vec3 ws = d->worldSize;
      tierMut.Relocate(id, Vec3{(float)cx + 0.5f - ws.x * 0.5f,
                                (float)(ground.TerrainTop(cx, cz) + 1) + (d->dead ? 0.0f : 0.0f),
                                (float)cz + 0.5f - ws.z * 0.5f});
      ids[k] = id;
    }
  }
  // ...the MAIMED one: its sword hand off through the impact pass's own sever
  // (the stump, the gout's owed blood, the grip rule drops the sword)...
  int severed = -1;
  if (DistantMob* d = tierMut.Find(ids[kMaimed])) {
    // The first part of the sword arm's chain the tier may take off (the
    // hand, else up the arm): what comes off takes the hand with it.
    for (const int16_t p : d->hands[0].parts)
      if (severed < 0 && p >= 0 && (size_t)p < d->kit.limbs.size() &&
          d->kit.limbs[(size_t)p].severable && !d->kit.limbs[(size_t)p].vital)
        severed = p;
    if (severed >= 0 && !c.mobs.SeverDistantForTest(ids[kMaimed], severed, t)) severed = -1;
  }
  // ...the BURNING one: a fire it carries (D2b's statuses, written as the
  // tier would hold them)...
  if (DistantMob* d = tierMut.Find(ids[kBurning])) {
    d->burnTicksLeft = 300;
    d->burnFrac = 0.125f;
  }
  // ...and nobody else bleeds: the plain and armoured ones are clean.
  for (int k : {(int)kPlain, (int)kArmoured})
    if (DistantMob* d = tierMut.Find(ids[k])) d->bloodLossPerTick = 0.0f;
  int made = 0;
  std::vector<Expect> want;
  for (int k = 0; k < kCount; k++)
    if (const DistantMob* d = tier.Find(ids[k])) {
      made++;
      want.push_back(ExpectOf(*d, labels[k]));
    }
  const DistantMob* dm = tier.Find(ids[kMaimed]);
  const bool maimedOk = dm != nullptr && severed >= 0 && dm->bloodLossPerTick > 0.0f &&
                        dm->stumps > 0 && !dm->hands[0].weapon;
  check(made == kCount && maimedOk && worn > 0 && tier.Find(ids[kCorpse]) != nullptr &&
            tier.Find(ids[kCorpse])->dead,
        Format("S: %d/%d distant, maimed %d (limb %d, bleed %.4f, stumps %u, sword hand empty %d), "
               "armour pieces %d",
               made, (int)kCount, maimedOk, severed, dm ? (double)dm->bloodLossPerTick : 0.0,
               dm ? (unsigned)dm->stumps : 0u, dm ? !dm->hands[0].weapon : 0, worn));
  // The records the save will write, for arm N and U.
  std::vector<uint8_t> maimedRec, burningRec, plainRec;
  if (const DistantMob* d = tier.Find(ids[kMaimed])) c.mobs.DistantRecord(*d, maimedRec);
  if (const DistantMob* d = tier.Find(ids[kBurning])) c.mobs.DistantRecord(*d, burningRec);
  if (const DistantMob* d = tier.Find(ids[kPlain])) c.mobs.DistantRecord(*d, plainRec);
  EntityIO eio = MakeEntityIO(c.debris, c.mobs, nullptr);
  SaveReport sr;
  const bool saved = SaveWorld(c.ctx, c.world, c.stream, kPath, c.mats, &eio, {}, &sr);
  check(saved && sr.entities.regionRecords == (uint32_t)made,
        Format("S: saved %d, %u region records (want %d)", saved, sr.entities.regionRecords, made));

  // ======== L: loaded, then ACTIVATED by one Unpark call =====================
  c.mobs.Reset();
  EntityFileReport lr;
  const bool loaded = LoadWorld(c.ctx, c.world, c.sim, c.stream, kPath, c.mats, &eio, nullptr, &lr);
  const size_t afterLoad = tier.Count();
  EntityParking parking;
  parking.Bind(c.mobs, c.stream.Store(), true);
  // DIRECT PHASE CALL ON PURPOSE (W2-O): the claim is that THIS call -- the
  // one phase H makes every tick -- brings a save's parked creatures back as
  // distant ones, with no load-specific path; ticking would step them (their
  // bleed would move) before they could be compared to what was saved.
  const DistantMobs::Stats l0 = tier.GetStats();
  parking.Unpark(c.mobs, c.stream.Store(), c.world, ++t);
  const uint64_t activated = tier.GetStats().activated - l0.activated;
  const uint64_t activatedDead = tier.GetStats().activatedDead - l0.activatedDead;
  int matched = 0;
  std::string whyL;
  for (const Expect& e : want) {
    const DistantMob* hit = nullptr;
    for (const DistantMob& d : tier.All())
      if (d.defName == e.def && std::memcmp(&d.origin, &e.origin, sizeof(Vec3)) == 0) hit = &d;
    if (hit == nullptr) {
      if (whyL.empty()) whyL = e.label + ": not distant after the load";
      continue;
    }
    const std::string diff = CompareTo(e, *hit, e.label == std::string("plain"));
    if (diff.empty()) matched++;
    else if (whyL.empty()) whyL = e.label + ": " + diff;
  }
  check(loaded && lr.recordsApplied == 0 && afterLoad == 0 && activated == (uint64_t)made &&
            activatedDead == 1 && tier.Count() == (size_t)made && matched == made,
        Format("L: loaded %d (applied %u, distant %zu), one Unpark activated %llu (%llu dead), "
               "%d/%d exact%s%s",
               loaded, lr.recordsApplied, afterLoad, (unsigned long long)activated,
               (unsigned long long)activatedDead, matched, made, whyL.empty() ? "" : ", first: ",
               whyL.c_str()));
  // The record-built kit's reach beside the rig's (the approximation, reported).
  float reachRig = 0.0f, reachRec = 0.0f;
  if (!want.empty())
    for (const DistantMob& d : tier.All())
      if (d.defName == want[0].def && std::memcmp(&d.origin, &want[0].origin, sizeof(Vec3)) == 0 &&
          !d.kit.styles.empty())
        reachRec = d.kit.styles[0].reach;

  // ======== N: the near door -- the same records, loaded near ===============
  bool nSevered = false, nHp = true, nBled = false, nLit = false;
  {
    const DistantMobs::Stats n0 = tier.GetStats();
    if (!maimedRec.empty()) {
      ByteReader rd{maimedRec.data(), maimedRec.size()};
      if (Mob* m = c.mobs.LoadOne(rd, MobSystem::kSaveVersion, /*placeLimbs=*/true)) {
        const uint64_t mid = m->Id();
        nSevered = severed >= 0 && c.mobs.LimbBody(mid, severed) == 0;
        const Expect& e = want.size() > 1 ? want[1] : want[0];
        for (size_t li = 0; li < c.mobs.Defs()[(size_t)human].limbs.size() && li < e.hp.size(); li++)
          if (e.attached[li] && !SameBits(c.mobs.LimbHp(mid, (int)li), e.hp[li])) nHp = false;
        c.mobs.RemoveMob(mid);
      }
    }
    if (!burningRec.empty()) {
      ByteReader rd{burningRec.data(), burningRec.size()};
      if (Mob* m = c.mobs.LoadOne(rd, MobSystem::kSaveVersion, /*placeLimbs=*/true))
        c.mobs.RemoveMob(m->Id());
    }
    nBled = tier.GetStats().rebled > n0.rebled;
    nLit = tier.GetStats().relit > n0.relit;
  }
  check(nSevered && nHp && nBled && nLit,
        Format("N: near load of the maimed record: limb %d off %d, hp exact %d, blood re-opened "
               "%d; the burning record re-lit %d",
               severed, nSevered, nHp, nBled, nLit));

  // ======== U: a v12 record upgrades to the same v13 bytes ====================
  bool uOk = false;
  if (plainRec.size() > 4) {
    std::vector<uint8_t> v12(plainRec.begin(), plainRec.end() - 4);   // the 0 flag off
    uint32_t ver = 12;
    uOk = MobSystem::UpgradeRecordToCurrent(v12, ver) && ver == MobSystem::kSaveVersion &&
          v12 == plainRec;
  }
  check(uOk, "U: a v12 record did not upgrade to the v13 bytes");

  RecordObserved("distantSave.activated", (double)activated);
  RecordObserved("distantSave.reachRecordVox", (double)reachRec);
  (void)reachRig;
  detail = Format(
      "S: %d distant (plain, maimed: limb %d off + bleeding, burning, armoured, corpse), %u "
      "records saved | L: one Unpark after the load activated %llu, %d/%d exact (hp, severs, "
      "bleed, burn, stumps, hands, life) | N: near load keeps the sever %d, re-bleeds %d, "
      "re-lights %d | U: v12 -> v13 %d",
      made, severed, sr.entities.regionRecords, (unsigned long long)activated, matched, made,
      nSevered, nBled, nLit, uOk);
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];

  // ---- LEAVES NOTHING ----
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.SetParkFn(nullptr);
  c.mobs.ClearPlayerActors();
  c.mobs.DistantGroundMut().InvalidateAll();
  c.stream.Store().Clear();
  {
    std::error_code ec;
    fs::remove_all(kPath, ec);
  }
  SetCurrentTuning(tuneWas);
  c.world.SetWindowOrigin(home);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DistantLifeGates() {
  static const std::vector<Gate> g = {
      // The distant tier past the window (docs/PLAN_distant_npcs.md D5+D6, §4).
      {"distant-schedule", "world", {}, false, GateDistantSchedule, /*needsRender=*/false},
      {"distant-save", "mob", {}, false, GateDistantSave, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
