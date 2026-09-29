// selftest_npc.cpp — NPC RESIDENTS (docs/PLAN_world_editor.md P7,
// world/refs_npc.h).
//
//   npc-schedule  Two small rooms built by ops with a door between them, a
//                 waynode chain through the doorway, and one villager on a
//                 four-row schedule (sleep / work / eat / wander) driven
//                 across a whole day by a FAST CLOCK (one in-game minute per
//                 tick, schedule::SetMinuteOverride). Every row's anchor is
//                 reached within npc.arriveMaxTicks (tests/baseline.json), in
//                 the right room; the door is opened, walked through and
//                 closed behind on every crossing and ends the day shut; and
//                 the whole day run TWICE from the same start gives the same
//                 trace (feet, row, phase and door phase, tick by tick).
//   npc-catchup   The same fixture. First appearance at the clock's anchor
//                 (08:00: at work, not where the ref is authored); then the
//                 window leaves (the villager parks, the ref goes dormant),
//                 the clock moves to night, the window returns -- and the
//                 villager comes back ON ITS BED, not where it froze.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/mob.h"
#include "game/persist.h"
#include "game/schedule.h"
#include "phys/debris.h"
#include "sim/chunkstore.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "world/refs.h"
#include "world/refs_doors.h"
#include "world/refs_npc.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSchedName = "npc_fixture_day";
// The fixture's day, through the real parser. Four rows, three door
// crossings a day plus the walk home to bed.
constexpr const char* kSchedText = R"JSON({
  "about": "npc-schedule / npc-catchup fixture (src/test/selftest_npc.cpp)",
  "rows": [
    { "from": "22:00", "to": "06:00", "do": "sleep", "at": "bed" },
    { "from": "06:00", "to": "11:00", "do": "work", "at": "work" },
    { "from": "11:00", "to": "14:00", "do": "eat", "at": "home" },
    { "from": "14:00", "to": "22:00", "do": "wander", "at": "gather", "radius": 0.8 }
  ]
})JSON";

constexpr const char* kNpc = "npcfix/resident";
constexpr const char* kDoor = "npcfix/door";

uint32_t MatId(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

// The fixture: its coordinates and its build ops.
struct Fixture {
  int X0 = 112, Z0 = 176, G = 0, Y0 = 0;
  static constexpr int kWallX = 25, kW = 50, kD = 24, kH = 26;   // room geometry
  std::vector<CellOp> clear, build;
  std::string root;   // scratch map dir
  IVec3 Chunk() const { return {(X0 + 25) >> 4, Y0 >> 4, (Z0 + 12) >> 4}; }
  int WallX() const { return X0 + kWallX; }
};

Fixture MakeFixture(const Ctx& c) {
  Fixture f;
  f.G = FixtureYOver(f.X0 - 4, f.Z0 - 4, f.X0 + Fixture::kW + 4, f.Z0 + Fixture::kD + 4,
                     kDefaultSeed, 0);
  f.Y0 = f.G + 1;
  const uint32_t stone = MatId(c, "stone");
  uint32_t leafMat = MatId(c, "door_wood");
  if (leafMat == 0) leafMat = MatId(c, "wood");
  auto put = [](std::vector<CellOp>& v, int x, int y, int z, uint32_t m, uint32_t st) {
    v.push_back({World::SlotCellIndex({x, y, z}), m == 0 ? 0u : PackVoxNew(m, st)});
  };
  // Tick 1: clear the air (grass, bushes) over the whole footprint.
  for (int x = f.X0 - 4; x <= f.X0 + Fixture::kW + 4; x++)
    for (int z = f.Z0 - 4; z <= f.Z0 + Fixture::kD + 4; z++)
      for (int y = f.Y0; y <= f.Y0 + Fixture::kH + 2; y++) put(f.clear, x, y, z, 0, 0);
  // Tick 2: the floor slab, the walls, the door leaf.
  for (int x = f.X0 - 4; x <= f.X0 + Fixture::kW + 4; x++)
    for (int z = f.Z0 - 4; z <= f.Z0 + Fixture::kD + 4; z++)
      for (int y = f.G - 4; y <= f.G; y++) put(f.build, x, y, z, stone, 0);
  for (int y = f.Y0; y < f.Y0 + Fixture::kH; y++) {
    for (int x = f.X0; x <= f.X0 + Fixture::kW; x++) {
      put(f.build, x, y, f.Z0, stone, 0);
      put(f.build, x, y, f.Z0 + Fixture::kD, stone, 0);
    }
    for (int z = f.Z0 + 1; z < f.Z0 + Fixture::kD; z++) {
      put(f.build, f.X0, y, z, stone, 0);
      put(f.build, f.X0 + Fixture::kW, y, z, stone, 0);
      // The dividing wall; the doorway (z 8..16, 22 high) is the leaf.
      const bool leaf = z >= f.Z0 + 8 && z <= f.Z0 + 16 && y < f.Y0 + 22;
      put(f.build, f.WallX(), y, z, leaf ? leafMat : stone, leaf ? (uint32_t)((z + y) % 3) : 0u);
    }
  }
  f.root = "build/selftest_npc_fixture";
  std::error_code ec;
  fs::remove_all(f.root, ec);
  fs::create_directories(f.root + "/worldmap/npcfix/refs", ec);
  return f;
}

bool PlaceFixtureRefs(refs::RefStore& st, const Fixture& f, std::string& err) {
  auto ref = [&](const char* id, const char* kind, int dx, int dz, int yaw, refs::Json props,
                 const char* base = "") {
    refs::Ref r;
    r.id = id;
    r.kind = kind;
    r.base = base;
    r.pos = {f.X0 + dx, f.Y0, f.Z0 + dz};
    r.yaw = yaw;
    r.props = std::move(props);
    return refs::Place(st, r, &err);
  };
  using J = refs::Json;
  // The door: in the dividing wall, opens toward +X (room B), hinge at z 16.
  bool ok = ref(kDoor, "door", Fixture::kWallX, 16, 90,
                J{{"width", 9}, {"height", 22}, {"hinge", "left"}});
  ok = ok && ref("npcfix/bed", "bed", 4, 5, 90, J{{"length", 14}});
  ok = ok && ref("npcfix/hearth", "marker", 12, 20, 180, J{{"tags", J::array({"hearth"})}});
  ok = ok && ref("npcfix/anvil", "marker", 42, 20, 0, J::object());
  ok = ok && ref("npcfix/green", "marker", 40, 6, 0, J{{"tags", J::array({"gather"})}});
  // The walking graph. Links written on ONE end only (they are two-way), and
  // b_room names b_door by its bare name (resolved in the group).
  ok = ok && ref("npcfix/a_room", "waynode", 9, 12, 0, J{{"links", J::array({"npcfix/a_door"})}});
  ok = ok && ref("npcfix/a_door", "waynode", 17, 12, 0, J{{"links", J::array({"npcfix/b_door"})}});
  ok = ok && ref("npcfix/b_door", "waynode", 37, 12, 0, J::object());
  ok = ok && ref("npcfix/b_room", "waynode", 42, 17, 0, J{{"links", J::array({"b_door"})}});
  ok = ok && ref(kNpc, "npc", 10, 12, 0,
                 J{{"name", "Fixture Resident"}, {"schedule", kSchedName}, {"bed", "npcfix/bed"},
                   {"work", "npcfix/anvil"}, {"home", "npcfix/hearth"}, {"behavior", "villager"}},
                 "human");
  return ok;
}

bool InstallSchedule(std::string& why) {
  schedule::Schedule sc;
  std::vector<std::string> probs;
  if (!schedule::Parse(kSchedName, kSchedText, "(npc fixture)", sc, probs) || !probs.empty() ||
      sc.rows.size() != 4) {
    why = "fixture schedule did not parse: " + (probs.empty() ? std::string("?") : probs[0]);
    return false;
  }
  // Round trip: the writer's text parses back to the same rows.
  schedule::Schedule back;
  std::vector<std::string> p2;
  schedule::Parse(kSchedName, schedule::Write(sc), "(rewrite)", back, p2);
  if (schedule::Write(back) != schedule::Write(sc)) {
    why = "schedule Write/Parse is not stable";
    return false;
  }
  refs::Schedules().Add(sc);
  return true;
}

// The world, empty and at the harness origin, for one run.
struct Scene {
  Ctx& c;
  IVec3 savedOrigin;
  uint64_t idWas;
  Scene(Ctx& ctx) : c(ctx), savedOrigin(ctx.world.WindowOrigin()), idWas(ctx.mobs.NextIdCounter()) {}
  void Fresh() {
    c.stream.Store().Clear();
    c.debris.Reset();
    c.mobs.Reset();
    c.mobs.SetNextIdCounter(5000);   // both runs spawn the same ids (hash salts)
    refs::ResetResidents();
    c.world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  }
  void Leave(refs::RefStore* st) {
    schedule::SetMinuteOverride(-1);
    if (st != nullptr) {
      refs::RefCtx rc{st, &c.mobs, &c.world, &c.stream.Store()};
      rc.phys = &c.phys;
      rc.debris = &c.debris;
      st->DeactivateAll(rc, refs::RefEvent::Reset);
    }
    refs::ResetResidents();
    c.debris.Reset();
    c.mobs.Reset();
    c.mobs.SetParkFn(nullptr);
    c.mobs.SetUnparkPlacer(nullptr);
    c.mobs.ClearPlayerActors();
    c.mobs.SetNextIdCounter(idWas);
    c.stream.Store().Clear();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  }
};

Vec3 FootOf(const Mob& m) {
  const Vec3 o = m.Origin(), s = m.Def()->worldSize;
  return Vec3{o.x + s.x * 0.5f, o.y, o.z + s.z * 0.5f};
}
float Planar(const Vec3& a, const Vec3& b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

// ---- npc-schedule ---------------------------------------------------------------

struct DayResult {
  bool ok = false;
  std::string why;
  uint64_t trace = 1469598103934665603ull;
  std::vector<uint64_t> perTick;   // running hash, for the first divergence
  // Every row the day entered, in order: which row, ticks from its start to
  // the anchor (-1 = never), and whether that was in the right room.
  struct Visit {
    int row = -1;
    int arrive = -1;
    bool rightRoom = false;
  };
  std::vector<Visit> visits;
  bool asleepAtEnd = false;
  uint32_t opens = 0, closes = 0, arrivals = 0, replans = 0;
  std::string finalDoor, lastNote;
  int ticks = 0;
};

void Mix(uint64_t& h, int64_t v) {
  for (int i = 0; i < 8; i++) {
    h ^= (uint64_t)((v >> (i * 8)) & 0xFF);
    h *= 1099511628211ull;
  }
}

DayResult RunDay(Ctx& c, Scene& sc) {
  DayResult out;
  sc.Fresh();
  Fixture f = MakeFixture(c);
  refs::RefStore st;
  st.LoadMap(f.root, "npcfix");
  std::string err;
  if (!PlaceFixtureRefs(st, f, err)) {
    out.why = "placing the fixture refs: " + err;
    return out;
  }
  st.BindChunkStore(&c.stream.Store());
  const int maxArrive = (int)BaselineNumber("npc.arriveMaxTicks", 200);
  const int kStart = 5 * 60 + 30;          // 05:30, asleep
  const int kEnd = 22 * 60 + 240;          // run 240 min (= ticks) past 22:00
  schedule::SetMinuteOverride(kStart);
  uint32_t t = 16000;
  {
    auto rig = std::make_unique<support::TickRig>(c, t, f.Chunk());
    rig->Authority().refs = &st;
    support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = f.clear; });
    support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = f.build; });
    support::RunTicks(*rig, 6);
    const std::string rowActs[4] = {"sleep", "work", "eat", "wander"};
    uint32_t lastSince = 0;
    for (int i = 0; kStart + i <= kEnd; i++) {
      const int minute = kStart + i;
      schedule::SetMinuteOverride(minute % 1440);
      support::RunTicks(*rig, 1);
      out.ticks++;
      refs::ResidentStatus rs;
      const bool have = refs::ResidentStatusOf(kNpc, rs) && rs.mobId != 0;
      refs::DoorStatus ds;
      const refs::DoorPhase dph = refs::DoorStatusOf(kDoor, ds) ? ds.phase : refs::DoorPhase::Closed;
      Mix(out.trace, i);
      Mix(out.trace, have ? 1 : 0);
      if (have) {
        Mix(out.trace, (int64_t)std::floor(rs.foot.x * 8.0f));
        Mix(out.trace, (int64_t)std::floor(rs.foot.y * 8.0f));
        Mix(out.trace, (int64_t)std::floor(rs.foot.z * 8.0f));
        Mix(out.trace, rs.row);
        Mix(out.trace, (int)rs.phase);
      }
      Mix(out.trace, (int)dph);
      out.perTick.push_back(out.trace);
      if (!have) continue;
      if (rs.rowSince != lastSince) {   // a new row began
        lastSince = rs.rowSince;
        DayResult::Visit v;
        v.row = rs.row;
        out.visits.push_back(v);
      }
      DayResult::Visit& v = out.visits.back();
      if (rs.row >= 0 && rs.row < 4 && v.arrive < 0 && rs.arrivedTick != 0 &&
          rs.activity == rowActs[rs.row]) {
        v.arrive = std::max(0, (int)(rs.arrivedTick - rs.rowSince));
        // Which room: work and wander are in B (east of the wall), sleep and
        // eat in A.
        const bool east = rs.foot.x > (float)f.WallX();
        v.rightRoom = (rs.row == 1 || rs.row == 3) ? east : !east;
      }
      out.opens = rs.doorOpens;
      out.closes = rs.doorCloses;
      out.arrivals = rs.arrivals;
      out.replans = rs.replans;
      if (!rs.note.empty()) out.lastNote = rs.note;
    }
    if (const Mob* m = c.mobs.FindMobByRef(kNpc)) out.asleepAtEnd = m->Activity() == "sleep";
    refs::DoorStatus ds;
    out.finalDoor = refs::DoorStatusOf(kDoor, ds) ? refs::DoorPhaseName(ds.phase) : "closed";
    rig.reset();
  }
  std::vector<std::string> why;
  // The day: sleep (spawned on the bed), work, eat, wander, sleep again.
  const int wantRows[5] = {0, 1, 2, 3, 0};
  if (out.visits.size() != 5) why.push_back(Format("%zu rows entered, want 5", out.visits.size()));
  for (size_t i = 0; i < out.visits.size() && i < 5; i++) {
    const DayResult::Visit& v = out.visits[i];
    if (v.row != wantRows[i])
      why.push_back(Format("visit %zu was row %d, want %d", i, v.row, wantRows[i]));
    else if (v.arrive < 0 || v.arrive > maxArrive)
      why.push_back(Format("row %d (visit %zu) never reached its anchor within %d ticks (%d)",
                           v.row, i, maxArrive, v.arrive));
    else if (!v.rightRoom)
      why.push_back(Format("row %d (visit %zu) arrived in the wrong room", v.row, i));
  }
  if (!out.asleepAtEnd) why.push_back("not lying down (activity sleep) on the bed at the end");
  // Four crossings: bed->work, work->home, home->green, green->bed.
  if (out.opens < 4) why.push_back(Format("door opened %u times, want >= 4", out.opens));
  if (out.closes < 4) why.push_back(Format("door closed behind %u times, want >= 4", out.closes));
  if (out.finalDoor != "closed") why.push_back("the door ends the day " + out.finalDoor);
  out.ok = why.empty();
  if (!why.empty()) out.why = why[0] + (out.lastNote.empty() ? "" : " (last note: " + out.lastNote + ")");
  sc.Leave(&st);
  std::error_code ec;
  fs::remove_all(f.root, ec);
  return out;
}

Status GateNpcSchedule(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  std::string why;
  if (!InstallSchedule(why)) {
    detail = why;
    return Status::Fail;
  }
  Scene sc(c);
  const DayResult a = RunDay(c, sc);
  const DayResult b = RunDay(c, sc);
  int firstDiff = -1;
  for (size_t i = 0; i < std::min(a.perTick.size(), b.perTick.size()) && firstDiff < 0; i++)
    if (a.perTick[i] != b.perTick[i]) firstDiff = (int)i;
  const bool same = a.trace == b.trace && a.perTick.size() == b.perTick.size();
  int worst = 0;
  std::string arr;
  static const char* kAct[4] = {"sleep", "work", "eat", "wander"};
  for (const DayResult::Visit& v : a.visits) {
    worst = std::max(worst, v.arrive);
    arr += Format("%s%s %d", arr.empty() ? "" : " / ",
                  v.row >= 0 && v.row < 4 ? kAct[v.row] : "gap", v.arrive);
  }
  RecordObserved("npc.arriveWorstTicks", (double)worst);
  detail = Format(
      "day of %d ticks: at the anchor %s ticks after each row began (bound %d), door opened %u "
      "closed %u, ends %s, %s at the end, re-plans %u | twice-run %s%s",
      a.ticks, arr.c_str(), (int)BaselineNumber("npc.arriveMaxTicks", 200), a.opens, a.closes,
      a.finalDoor.c_str(), a.asleepAtEnd ? "lying on the bed" : "NOT lying down", a.replans,
      same ? "identical" : "DIFFERS", same ? "" : Format(" (first at tick %d)", firstDiff).c_str());
  if (!a.ok) detail += " | FAIL: " + a.why;
  else if (!same) detail += " | FAIL: the second run of the same day differs";
  return a.ok && b.ok && same ? Status::Pass : Status::Fail;
}

// ---- npc-catchup ------------------------------------------------------------------

Status GateNpcCatchup(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  std::string why;
  if (!InstallSchedule(why)) {
    detail = why;
    return Status::Fail;
  }
  Scene sc(c);
  sc.Fresh();
  Fixture f = MakeFixture(c);
  refs::RefStore st;
  st.LoadMap(f.root, "npcfix");
  std::string err;
  if (!PlaceFixtureRefs(st, f, err)) {
    sc.Leave(nullptr);
    detail = "placing the fixture refs: " + err;
    return Status::Fail;
  }
  st.BindChunkStore(&c.stream.Store());
  MobParking parking;
  parking.Bind(c.mobs, c.stream.Store(), true);
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& w) {
    if (!ok) fails.push_back(w);
    return ok;
  };
  auto countRef = [&]() {
    int n = 0;
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++)
      if (const Mob* m = c.mobs.MobAt(i); m && m->RefId() == kNpc) n++;
    return n;
  };
  const refs::Ref* npcRef = st.Find(kNpc);
  const refs::Anchor work = refs::ResolveAnchor(st, *npcRef, "work");
  const refs::Anchor bed = refs::ResolveAnchor(st, *npcRef, "bed");
  const float tol = (float)BaselineNumber("npc.catchupTolerance", 6.0);

  uint32_t t = 17000;
  auto rig = std::make_unique<support::TickRig>(c, t, f.Chunk());
  rig->Authority().refs = &st;
  auto ticks = [&](int n) {
    for (int i = 0; i < n; i++) {
      support::RunTicks(*rig, 1);
      parking.Unpark(c.mobs, c.stream.Store(), c.world, rig->tick);
    }
  };
  schedule::SetMinuteOverride(8 * 60);   // 08:00: at work
  support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = f.clear; });
  support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = f.build; });

  // A. FIRST APPEARANCE AT THE CLOCK'S ANCHOR (work, in room B), not at the
  //    ref's authored pos (room A).
  int waited = 0;
  while (countRef() == 0 && waited < 120) {
    ticks(1);
    waited++;
  }
  const Mob* m = c.mobs.FindMobByRef(kNpc);
  const float dA = m ? Planar(FootOf(*m), work.foot) : 1e9f;
  const bool okA = check(m != nullptr && dA <= tol,
                         Format("A: spawned %d ticks, %.1f vox from the work anchor", m != nullptr, dA));

  // B. THE WINDOW LEAVES: it parks, the ref goes dormant.
  auto teleport = [&](IVec3 origin) {
    c.world.InvalidateSnapshot();
    c.stream.ReloadWindow(origin);
    rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
    c.sim.EncodeLoadReset(enc);
    rhi::CommandBuffer cmd = enc.Finish();
    c.ctx.queue.Submit(1, &cmd);
    c.ctx.WaitIdle();
  };
  const int n = (int)kNChunk;
  const IVec3 home{0, 0, 0};
  const IVec3 away{n + 8, 0, n + 8};
  teleport(away);
  t = rig->tick;
  rig = std::make_unique<support::TickRig>(c, t, IVec3{away.x + n / 2, 11, away.z + n / 2});
  rig->Authority().refs = &st;
  ticks(3);
  const bool parked = countRef() == 0 && !st.IsActive(kNpc) && parking.GetStats().parked >= 1;
  check(parked, Format("B: away: live %d, active %d, parked %llu", countRef(), st.IsActive(kNpc),
                       (unsigned long long)parking.GetStats().parked));

  // C. NIGHT FALLS WHILE IT IS AWAY; THE WINDOW RETURNS: it comes back on
  //    its bed (room A), not at the anvil where it froze.
  schedule::SetMinuteOverride(23 * 60 + 30);
  teleport(home);
  parking.ResetWaits();
  t = rig->tick;
  rig = std::make_unique<support::TickRig>(c, t, f.Chunk());
  rig->Authority().refs = &st;
  int back = 0;
  while (countRef() == 0 && back < 240) {
    ticks(1);
    back++;
  }
  m = c.mobs.FindMobByRef(kNpc);
  const float dC = m ? Planar(FootOf(*m), bed.foot) : 1e9f;
  const float dCwork = m ? Planar(FootOf(*m), work.foot) : 1e9f;
  const bool okC = check(m != nullptr && countRef() == 1 && dC <= tol,
                         Format("C: back after %d ticks: %.1f vox from the bed (%.1f from the "
                                "anvil it froze at)",
                                back, dC, dCwork));
  // ...and it lies down there within a few ticks (the sleep activity).
  ticks(10);
  refs::ResidentStatus rs;
  const bool asleep = refs::ResidentStatusOf(kNpc, rs) && rs.activity == "sleep" &&
                      rs.phase == refs::ResidentPhase::AtAnchor;
  check(asleep, Format("C: after the unpark: activity '%s', phase %s", rs.activity.c_str(),
                       refs::ResidentPhaseName(rs.phase)));

  rig.reset();
  sc.Leave(&st);
  std::error_code ec;
  fs::remove_all(f.root, ec);
  detail = Format("A %s: first spawn %.1f vox from the 08:00 anchor (work) after %d ticks | B %s: "
                  "parked + dormant | C %s: back at 23:30 %.1f vox from the bed (not the "
                  "anvil, %.1f away), %s",
                  okA ? "ok" : "FAIL", dA, waited, parked ? "ok" : "FAIL", okC ? "ok" : "FAIL",
                  dC, dCwork, asleep ? "asleep" : "NOT asleep");
  if (!fails.empty()) detail += " | FAIL: " + fails[0];
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& NpcGates() {
  static const std::vector<Gate> g = {
      {"npc-schedule", "world", {}, false, GateNpcSchedule, /*needsRender=*/false},
      {"npc-catchup", "world", {}, false, GateNpcCatchup, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
