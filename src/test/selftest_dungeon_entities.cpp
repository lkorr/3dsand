// selftest_dungeon_entities.cpp — DUNGEON ENTITIES (docs/PLAN_dungeons.md
// §3.7 / §6, package D3; world/refs_dungeon.h, world/dungeon_refs.h).
//
//   dungeon-entities  A fixture dungeon written to a scratch asset dir
//                     (`_entities_fixture`, placed by a scratch harness
//                     map.json as `entfix` at a quarter turn, so the
//                     local-frame transform AND the id re-prefix are both
//                     exercised) on a stone pad built by ops on the harness
//                     map. Through THE tick (support::TickRig):
//                       A  placement: the refs land where the transform says;
//                          a rewrite of the group file is byte-identical;
//                       B  lever -> gate: the portcullis is built, a pull
//                          lifts every bar out of its box, a second pull
//                          drops them back;
//                       C  plate -> trap: a body on the plate collapses the
//                          floor -- ops emitted, no stone left in the region,
//                          its gravel twin in the region + the pit under it;
//                       D  key -> door: the locked door refuses, the key is
//                          taken into the harness body's kit, the door opens
//                          for it and remembers it is unlocked;
//                       E  loot: the chest's first open rolls cave_cache
//                          seeded by its id, == a direct roll;
//                       F  spawner + patrol: a spawned creature walks its
//                          patrol schedule's waynode route in order.
//                     The whole fixture runs TWICE from a fresh worldgen and
//                     the traces (spawns, gate rows, trap ops, loot, patrol
//                     visits, door phases) must be identical. Thresholds:
//                     tests/baseline.json `dungeon.*`.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/schedule.h"
#include "phys/debris.h"
#include "sim/chunkstore.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "world/loot.h"
#include "world/refs.h"
#include "world/refs_doors.h"
#include "world/refs_dungeon.h"
#include "world/refs_npc.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;
using J = refs::Json;

constexpr const char* kSched = "dungeon_fixture_patrol";
constexpr const char* kSchedText = R"JSON({
  "about": "dungeon-entities fixture (src/test/selftest_dungeon_entities.cpp)",
  "rows": [
    { "from": "00:00", "to": "24:00", "do": "patrol", "route": ["wn_a", "wn_b", "wn_c"], "mode": "loop", "wait": 0.3 }
  ]
})JSON";

// The placement: anchor column and the quarter turn.
constexpr int kAX = 150, kAZ = 300, kYaw = 90;
const std::string kG = "dungeon/entfix/";

uint32_t MatId(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

void Mix(uint64_t& h, int64_t v) {
  for (int i = 0; i < 8; i++) {
    h ^= (uint64_t)((v >> (i * 8)) & 0xFF);
    h *= 1099511628211ull;
  }
}
void MixStr(uint64_t& h, const std::string& s) {
  for (char ch : s) Mix(h, (unsigned char)ch);
}

// The fixture refs, in the DUNGEON-LOCAL frame (file ids use the asset name).
refs::RefGroupFile FixtureGroup() {
  refs::RefGroupFile g;
  g.group = "dungeon/_entities_fixture";
  auto add = [&](const char* name, const char* kind, IVec3 pos, int yaw, J props, const char* base = "") {
    refs::Ref r;
    r.id = std::string("dungeon/_entities_fixture/") + name;
    r.kind = kind;
    r.base = base;
    r.pos = pos;
    r.yaw = yaw;
    r.props = std::move(props);
    g.refs.push_back(std::move(r));
  };
  add("lever_a", "lever", {2, 0, 2}, 0, J{{"signal", "gate_a"}});
  add("gate_a", "gate", {6, 0, 8}, 0, J{{"width", 10}, {"height", 14}, {"on", "gate_a"}});
  add("plate_b", "plate", {30, 0, 4}, 0, J{{"size", J::array({5, 2, 5})}, {"latch", true}});
  add("trap_b", "trap", {28, -4, 2}, 0,
      J{{"type", "collapse_floor"}, {"on", "plate_b"}, {"region", J::array({9, 4, 9})}});
  add("door_c", "door", {6, 0, 20}, 0, J{{"width", 9}, {"height", 20}, {"lock", "iron_key"}});
  add("key_c", "key", {2, 0, 24}, 0, J{{"item", "iron_key"}});
  add("chest_d", "container", {24, 0, 24}, 0, J{{"loot", "cave_cache"}, {"title", "cache"}});
  // The patrol: three waynodes (links name siblings by their bare names, the
  // group rule) and a spawner whose creature walks them.
  add("wn_a", "waynode", {28, 0, 30}, 0, J{{"links", J::array({"wn_b"})}});
  add("wn_b", "waynode", {46, 0, 30}, 0, J{{"links", J::array({"wn_c"})}});
  add("wn_c", "waynode", {46, 0, 46}, 0, J{{"links", J::array({"wn_a"})}});
  add("spawner_p", "spawner", {36, 0, 36}, 0,
      J{{"mobs", J::array({J{{"base", "human"}}})}, {"count", 1}, {"radius", 0.5},
        {"schedule", kSched}, {"behavior", "zombie"}});
  // Determinism of the draw: two slots, two bases by weight, a radius.
  add("spawner_f", "spawner", {10, 0, 44}, 180,
      J{{"mobs", J::array({J{{"base", "human"}, {"weight", 1}}, J{{"base", "dummy"}, {"weight", 2}}})},
        {"count", 2}, {"radius", 1.2}, {"behavior", "dummy"}});
  return g;
}

struct Fixture {
  std::string root = "build/selftest_dungeon_fixture";
  int G = 0;                      // top ground cell under the pad
  refs::DungeonPlacement place;   // as the store resolved it
  std::vector<std::vector<CellOp>> build;   // one batch per tick
};

bool WriteText(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << text;
  return true;
}

// Scratch assets: the map's dungeons[] and the dungeon's refs.json.
bool WriteAssets(Fixture& f, std::string& why) {
  std::error_code ec;
  fs::remove_all(f.root, ec);
  fs::create_directories(f.root + "/worldmap/harness", ec);
  fs::create_directories(f.root + "/dungeons/_entities_fixture", ec);
  // The pad's world footprint at a quarter turn: local (x, z) -> (AX + z, AZ - x).
  f.G = FixtureYOver(kAX - 6, kAZ - 54, kAX + 54, kAZ + 6, kDefaultSeed, 0);
  const int bare = worldmap::BareGroundHeight(kAX, kAZ, kDefaultSeed);
  const int yOffset = f.G + 1 - bare;
  J map = J::object();
  map["dungeons"] = J::array({J{{"id", "entfix"}, {"name", "Entities fixture"},
                                {"dungeon", "_entities_fixture"}, {"at", J::array({kAX, kAZ})},
                                {"yOffset", yOffset}, {"yaw", kYaw}}});
  if (!WriteText(f.root + "/worldmap/harness/map.json", map.dump(2)) ||
      !WriteText(f.root + "/dungeons/_entities_fixture/dungeon.json",
                 "{ \"version\": 1, \"name\": \"Entities fixture\", \"refs\": \"refs.json\" }\n") ||
      !WriteText(f.root + "/dungeons/_entities_fixture/refs.json", refs::WriteGroup(FixtureGroup()))) {
    why = "could not write the scratch assets under " + f.root;
    return false;
  }
  return true;
}

// The pad, in batches under the per-tick cell-op cap: air over it, a stone
// slab, a pit under the collapse region, a wall with the door's leaf.
void MakeBuild(const Ctx& c, Fixture& f) {
  const refs::DungeonPlacement& p = f.place;
  const uint32_t stone = MatId(c, "stone");
  uint32_t leaf = MatId(c, "door_wood");
  if (leaf == 0) leaf = MatId(c, "wood");
  std::vector<CellOp> cur;
  auto flush = [&] {
    if (!cur.empty()) f.build.push_back(std::move(cur));
    cur.clear();
  };
  auto put = [&](IVec3 local, uint32_t m, uint32_t st) {
    const IVec3 w = p.ToWorld(local);
    cur.push_back({World::SlotCellIndex(w), m == 0 ? 0u : PackVoxNew(m, st)});
    if (cur.size() >= 60000) flush();
  };
  for (int y = 0; y <= 24; y++)
    for (int z = -4; z <= 52; z++)
      for (int x = -4; x <= 52; x++) put({x, y, z}, 0, 0);
  flush();
  for (int y = -4; y <= -1; y++)
    for (int z = -4; z <= 52; z++)
      for (int x = -4; x <= 52; x++) put({x, y, z}, stone, 0);
  // The pit under the collapse region: air, in a stone box of its own so the
  // gravel stays where the gate counts it whatever the terrain is down there.
  for (int y = -11; y <= -5; y++)
    for (int z = 1; z <= 11; z++)
      for (int x = 27; x <= 37; x++) {
        const bool wall = y == -11 || z == 1 || z == 11 || x == 27 || x == 37;
        put({x, y, z}, wall ? stone : 0u, 0);
      }
  // The gate's FRAME: two posts and a lintel over its top row. A portcullis
  // hangs from something -- lifted bars with nothing above them are an
  // island, and island detection drops them as debris.
  for (int y = 0; y <= 14; y++) {
    put({5, y, 8}, stone, 0);
    put({16, y, 8}, stone, 0);
  }
  for (int x = 6; x <= 15; x++) put({x, 14, 8}, stone, 0);
  for (int y = 0; y <= 21; y++)
    for (int x = 0; x <= 20; x++) {
      const bool isLeaf = x >= 6 && x <= 14 && y <= 19;
      put({x, y, 20}, isLeaf ? leaf : stone, isLeaf ? (uint32_t)((x + y) % 3) : 0u);
    }
  flush();
}

// Count cells of material `m` in an inclusive WORLD box (blocking readback,
// the selftest's sanctioned exception).
int CountMat(Ctx& c, IVec3 lo, IVec3 hi, uint32_t m) {
  std::vector<uint32_t> w(kChunkVol);
  int n = 0;
  for (int cz = lo.z >> 4; cz <= hi.z >> 4; cz++)
    for (int cy = lo.y >> 4; cy <= hi.y >> 4; cy++)
      for (int cx = lo.x >> 4; cx <= hi.x >> 4; cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!c.world.ChunkInWindow(wc)) continue;
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(wc), 1, w.data(), "dungeon-entities");
        for (int z = 0; z < 16; z++)
          for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
              const int X = cx * 16 + x, Y = cy * 16 + y, Z = cz * 16 + z;
              if (X < lo.x || X > hi.x || Y < lo.y || Y > hi.y || Z < lo.z || Z > hi.z) continue;
              if ((w[((uint32_t)z * kChunk + (uint32_t)y) * kChunk + (uint32_t)x] & 0xFFFu) == m) n++;
            }
      }
  return n;
}

void WorldBox(const refs::DungeonPlacement& p, IVec3 a, IVec3 b, IVec3& lo, IVec3& hi) {
  const IVec3 wa = p.ToWorld(a), wb = p.ToWorld(b);
  lo = {std::min(wa.x, wb.x), std::min(wa.y, wb.y), std::min(wa.z, wb.z)};
  hi = {std::max(wa.x, wb.x), std::max(wa.y, wb.y), std::max(wa.z, wb.z)};
}

struct RunResult {
  bool ok = false;
  std::vector<std::string> fails;
  uint64_t trace = 1469598103934665603ull;
  std::string detail;
};

RunResult RunFixture(Ctx& c) {
  RunResult out;
  auto fail = [&](const std::string& w) { out.fails.push_back(w); };
  // ---- a fresh world at the harness origin ----
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint64_t idWas = c.mobs.NextIdCounter();
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(6000);
  refs::ResetResidents();
  refs::ResetDungeonEntities();
  c.world.SetWindowOrigin({0, 0, 0});
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  schedule::SetMinuteOverride(12 * 60);

  Fixture f;
  std::string why;
  auto leave = [&](refs::RefStore* st) {
    schedule::SetMinuteOverride(-1);
    if (st != nullptr) {
      refs::RefCtx rc{st, &c.mobs, &c.world, &c.stream.Store()};
      rc.phys = &c.phys;
      rc.debris = &c.debris;
      st->DeactivateAll(rc, refs::RefEvent::Reset);
    }
    refs::ResetResidents();
    refs::ResetDungeonEntities();
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
    std::error_code ec;
    fs::remove_all(f.root, ec);
  };
  if (!WriteAssets(f, why)) {
    fail(why);
    leave(nullptr);
    return out;
  }
  refs::RefStore st;
  st.LoadMap(f.root, "harness");
  st.BindChunkStore(&c.stream.Store());

  // ---- A. placement ----
  if (st.Dungeons().size() != 1 || !st.Dungeons()[0].resolved) {
    fail(Format("A: %zu placement(s), resolved %d", st.Dungeons().size(),
                st.Dungeons().empty() ? 0 : (int)st.Dungeons()[0].resolved));
    leave(&st);
    return out;
  }
  f.place = st.Dungeons()[0];
  const refs::Ref* lever = st.Find(kG + "lever_a");
  const refs::Ref* door = st.Find(kG + "door_c");
  if (lever == nullptr || door == nullptr) {
    fail("A: the fixture refs did not load under dungeon/entfix/ (the id re-prefix)");
    leave(&st);
    return out;
  }
  {
    // local (2, 0, 2) at yaw 90 about the anchor cell -> (AX + 2, G + 1, AZ - 2).
    const IVec3 want{kAX + 2, f.G + 1, kAZ - 2};
    if (lever->pos.x != want.x || lever->pos.y != want.y || lever->pos.z != want.z)
      fail(Format("A: lever at (%d,%d,%d), want (%d,%d,%d)", lever->pos.x, lever->pos.y, lever->pos.z,
                  want.x, want.y, want.z));
    if (door->yaw != 90) fail(Format("A: door yaw %d, want 90 (local 0 + the placement's 90)", door->yaw));
    // A rewrite of the group (a no-op Move) gives the file back byte for byte.
    std::ifstream a(f.root + "/dungeons/_entities_fixture/refs.json", std::ios::binary);
    const std::string before((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
    a.close();
    std::string err;
    if (!refs::Move(st, lever->id, lever->pos, lever->yaw, &err)) fail("A: rewrite refused: " + err);
    std::ifstream b(f.root + "/dungeons/_entities_fixture/refs.json", std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
    if (before != after) fail("A: writing the dungeon group back does not reproduce its local-frame file");
    lever = st.Find(kG + "lever_a");
    door = st.Find(kG + "door_c");
  }
  for (const std::string& w : st.Warnings())
    if (w.find("dungeon") != std::string::npos && w.find("entfix") != std::string::npos)
      fail("A: load warning: " + w);

  // ---- the schedule, the build ----
  {
    schedule::Schedule sc;
    std::vector<std::string> probs;
    if (!schedule::Parse(kSched, kSchedText, "(dungeon fixture)", sc, probs) || !probs.empty() ||
        sc.rows.size() != 1 || sc.rows[0].Route().size() != 3) {
      fail("the fixture patrol schedule did not parse: " + (probs.empty() ? std::string("?") : probs[0]));
      leave(&st);
      return out;
    }
    refs::Schedules().Add(sc);
  }
  MakeBuild(c, f);
  const IVec3 mid = f.place.ToWorld({24, 0, 24});
  uint32_t t = 21000;
  auto rig = std::make_unique<support::TickRig>(c, t, IVec3{mid.x >> 4, mid.y >> 4, mid.z >> 4});
  for (const std::vector<CellOp>& b : f.build)
    support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = b; });
  support::RunTicks(*rig, 4);
  // The refs join the tick only now, so the gate's IfAir build is not
  // shadowed by the pad's own clear ops in the same tick (keep-first).
  rig->Authority().refs = &st;
  auto tickN = [&](int n) { support::RunTicks(*rig, n); };
  tickN(6);

  const uint32_t iron = MatId(c, "iron"), stone = MatId(c, "stone"), gravel = MatId(c, "gravel");
  const std::string gateId = kG + "gate_a";
  IVec3 gLo, gHi;
  WorldBox(f.place, {6, 0, 8}, {15, 13, 8}, gLo, gHi);

  // ---- B. lever -> gate ----
  const int gateMax = (int)BaselineNumber("dungeon.gateMaxTicks", 60);
  const int barsBuilt = CountMat(c, gLo, gHi, iron);
  refs::GateStatus gs;
  if (!refs::GateStatusOf(gateId, gs) || !gs.built || barsBuilt == 0)
    fail(Format("B: the gate was not built (status %d, %d iron cells)", (int)refs::GateStatusOf(gateId, gs),
                barsBuilt));
  st.QueueDevUse(kG + "lever_a");
  int openTicks = 0;
  for (; openTicks < gateMax; openTicks++) {
    tickN(1);
    refs::GateStatusOf(gateId, gs);
    Mix(out.trace, gs.raised);
    if (gs.raised == gs.height && gs.height > 0) break;
  }
  tickN(2);
  const int barsOpen = CountMat(c, gLo, gHi, iron);
  if (gs.raised != gs.height || barsOpen != 0)
    fail(Format("B: after the pull the gate is raised %d/%d with %d iron cells left in its box (want 0)",
                gs.raised, gs.height, barsOpen));
  st.QueueDevUse(kG + "lever_a");
  int closeTicks = 0;
  for (; closeTicks < gateMax; closeTicks++) {
    tickN(1);
    refs::GateStatusOf(gateId, gs);
    Mix(out.trace, gs.raised);
    if (gs.raised == 0) break;
  }
  tickN(2);
  const int barsClosed = CountMat(c, gLo, gHi, iron);
  if (gs.raised != 0 || barsClosed != barsBuilt)
    fail(Format("B: pulled back, the gate is raised %d with %d iron cells (built %d)", gs.raised, barsClosed,
                barsBuilt));

  // ---- C. plate -> trap (collapse_floor) ----
  IVec3 rLo, rHi, pLo, pHi;
  WorldBox(f.place, {28, -4, 2}, {36, -1, 10}, rLo, rHi);
  WorldBox(f.place, {28, -10, 2}, {36, -1, 10}, pLo, pHi);
  const int stoneBefore = CountMat(c, rLo, rHi, stone);
  {
    // FIXTURE SETUP, not a phase call: the body that stands on the plate.
    const int def = c.mobs.FindDef("dummy");
    const IVec3 at = f.place.ToWorld({32, 0, 6});
    const Vec3 ws = def >= 0 ? c.mobs.Defs()[(size_t)def].worldSize : Vec3{};
    const uint64_t id =
        def >= 0 ? c.mobs.Spawn(def, {at.x - (int)(ws.x * 0.5f), at.y, at.z - (int)(ws.z * 0.5f)}) : 0;
    if (id == 0) fail("C: could not spawn the dummy onto the plate");
    else c.mobs.SetMobBehavior(id, "dummy");
  }
  const int collapseMax = (int)BaselineNumber("dungeon.collapseMaxTicks", 90);
  refs::TrapStatus ts;
  int collapseTicks = 0;
  for (; collapseTicks < collapseMax; collapseTicks++) {
    tickN(1);
    if (refs::TrapStatusOf(kG + "trap_b", ts) && ts.done) break;
  }
  tickN(20);   // let the gravel fall
  const int stoneAfter = CountMat(c, rLo, rHi, stone);
  const int gravelAfter = CountMat(c, pLo, pHi, gravel);
  const double frac = BaselineNumber("dungeon.collapsePowderFrac", 0.5);
  refs::TrapStatusOf(kG + "trap_b", ts);
  Mix(out.trace, ts.fires);
  Mix(out.trace, ts.ops);
  Mix(out.trace, ts.cells);
  if (ts.fires != 1 || !ts.done || ts.ops == 0 || stoneAfter != 0 ||
      gravelAfter < (int)(frac * (double)stoneBefore))
    fail(Format("C: trap fired %u, done %d, %u ops, %u converted; stone in the floor %d -> %d, gravel in "
                "region+pit %d (want >= %.0f%% of %d) [%s]",
                ts.fires, (int)ts.done, ts.ops, ts.cells, stoneBefore, stoneAfter, gravelAfter, frac * 100.0,
                stoneBefore, ts.note.c_str()));

  // ---- D. key -> door ----
  const std::string doorId = kG + "door_c", keyId = kG + "key_c";
  PlayerSession& me = rig->Session();
  const IVec3 stand = f.place.ToWorld({6, 0, 27});
  auto useWith = [&](const std::string& id) {
    me.player.pos = Vec3{(float)stand.x + 0.5f, (float)stand.y + Player::kHalfY, (float)stand.z + 0.5f};
    support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) {
      o.input.pressed = TB_USE;
      o.input.useRef = refs::RefHash(id);
    });
  };
  auto lastUse = [&](const std::string& id) {
    for (auto it = st.Uses().rbegin(); it != st.Uses().rend(); ++it)
      if (it->id == id) return it->message;
    return std::string("(no use recorded)");
  };
  useWith(doorId);
  tickN(5);
  refs::DoorStatus ds;
  const bool refusedLocked = lastUse(doorId) == "It is locked." &&
                             (!refs::DoorStatusOf(doorId, ds) || ds.phase == refs::DoorPhase::Closed);
  if (!refusedLocked) fail("D: the locked door did not refuse a body without its key: " + lastUse(doorId));
  useWith(keyId);
  bool haveKey = false;
  for (const ItemStack& s : me.kit().bag.slots) haveKey |= s.name == "iron_key";
  for (const ItemStack& s : me.kit().hotbar.slots) haveKey |= s.name == "iron_key";
  if (!haveKey) fail("D: the key was not taken into the kit: " + lastUse(keyId));
  useWith(doorId);
  const std::string openMsg = lastUse(doorId);
  int doorTicks = 0;
  for (; doorTicks < 90; doorTicks++) {
    tickN(1);
    if (refs::DoorStatusOf(doorId, ds) && ds.phase == refs::DoorPhase::Open) break;
  }
  const bool unlockedNow = !refs::DoorLockedNow(&st, *st.Find(doorId));
  Mix(out.trace, (int)ds.phase);
  if (ds.phase != refs::DoorPhase::Open || !unlockedNow)
    fail(Format("D: with the key the door is %s (unlocked %d): %s", refs::DoorPhaseName(ds.phase),
                (int)unlockedNow, openMsg.c_str()));

  // ---- E. loot ----
  st.QueueDevUse(kG + "chest_d");
  tickN(1);
  Bag got, want;
  refs::ContainerContents(st, *st.Find(kG + "chest_d"), &c.items, got);
  if (const loot::Table* tb = loot::Find("cave_cache")) loot::Roll(*tb, refs::RefHash(kG + "chest_d"), &c.items, want);
  std::string lootText;
  bool lootSame = true;
  for (int i = 0; i < Bag::kSlots; i++) {
    lootSame &= got.slots[i].name == want.slots[i].name && got.slots[i].count == want.slots[i].count;
    if (got.slots[i].Empty()) continue;
    lootText += (lootText.empty() ? "" : ", ") + got.slots[i].name +
                (got.slots[i].count > 1 ? Format(" x%d", got.slots[i].count) : std::string());
    MixStr(out.trace, got.slots[i].name);
    Mix(out.trace, got.slots[i].count);
  }
  if (lootText.empty() || !lootSame)
    fail("E: the chest rolled [" + lootText + "] -- empty, or not the direct roll of cave_cache by its id");

  // ---- F. spawner + patrol ----
  const std::string pSlot = kG + "spawner_p/s0";
  const int patrolMax = (int)BaselineNumber("dungeon.patrolMaxTicks", 900);
  const int minArr = (int)BaselineNumber("dungeon.patrolMinArrivals", 4);
  refs::ResidentStatus rs;
  int patrolTicks = 0;
  for (; patrolTicks < patrolMax; patrolTicks++) {
    tickN(1);
    if (refs::ResidentStatusOf(pSlot, rs) && (int)rs.patrolArrivals >= minArr) break;
  }
  std::string visits;
  bool order = rs.patrolVisits.size() >= 2;
  auto idx = [](const std::string& id) {
    const std::string n = id.substr(id.rfind('/') + 1);
    return n == "wn_a" ? 0 : n == "wn_b" ? 1 : n == "wn_c" ? 2 : -1;
  };
  for (size_t i = 0; i < rs.patrolVisits.size(); i++) {
    visits += (i ? " " : "") + rs.patrolVisits[i].substr(rs.patrolVisits[i].rfind('/') + 1);
    MixStr(out.trace, rs.patrolVisits[i]);
    if (i > 0) order &= idx(rs.patrolVisits[i]) == (idx(rs.patrolVisits[i - 1]) + 1) % 3;
  }
  if ((int)rs.patrolArrivals < minArr || !order)
    fail(Format("F: the patrol reached %u route nodes in %d ticks (want >= %d, in route order): [%s] %s",
                rs.patrolArrivals, patrolTicks, minArr, visits.c_str(), rs.note.c_str()));
  // The draw: both of spawner_f's slots, what and where.
  refs::SpawnerStatus ss;
  std::string spawnText;
  if (!refs::SpawnerStatusOf(kG + "spawner_f", ss) || ss.serial != 2) {
    fail(Format("F: spawner_f made %u spawns, want 2 (%s)", ss.serial, ss.note.c_str()));
  } else {
    for (size_t i = 0; i < ss.slots.size(); i++) {
      const Mob* m = c.mobs.FindMobByRef(ss.slots[i]);
      const refs::SpawnPlan plan = refs::PlanSpawn(*st.Find(kG + "spawner_f"), (uint32_t)i + 1);
      MixStr(out.trace, ss.bases[i]);
      spawnText += Format("%s%s", i ? ", " : "", ss.bases[i].c_str());
      if (m == nullptr || ss.bases[i] != plan.base) {
        fail("F: spawner_f slot " + std::to_string(i) + " is not its planned " + plan.base);
        continue;
      }
      const Vec3 o = m->Origin(), ws = m->Def()->worldSize;
      const float dx = o.x + ws.x * 0.5f - plan.foot.x, dz = o.z + ws.z * 0.5f - plan.foot.z;
      Mix(out.trace, (int64_t)std::floor(plan.foot.x * 4.0f));
      Mix(out.trace, (int64_t)std::floor(plan.foot.z * 4.0f));
      if (std::sqrt(dx * dx + dz * dz) > 4.0f)
        fail(Format("F: spawner_f slot %zu stands %.1f voxels from its planned spot", i, std::sqrt(dx * dx + dz * dz)));
    }
  }

  out.detail = Format(
      "gate %d bars, open in %d ticks, shut in %d | collapse %d stone -> %d (%d gravel) in %d ticks | door "
      "%s -> %s | loot [%s] | patrol %u nodes in %d ticks [%s] | spawner_f [%s]",
      barsBuilt, openTicks + 1, closeTicks + 1, stoneBefore, stoneAfter, gravelAfter, collapseTicks + 1,
      refusedLocked ? "refused" : "NOT refused", refs::DoorPhaseName(ds.phase), lootText.c_str(),
      rs.patrolArrivals, patrolTicks + 1, visits.c_str(), spawnText.c_str());
  RecordObserved("dungeon.patrolTicks", (double)(patrolTicks + 1));
  rig.reset();
  leave(&st);
  out.ok = out.fails.empty();
  return out;
}

Status GateDungeonEntities(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  const RunResult a = RunFixture(c);
  const RunResult b = RunFixture(c);
  const bool same = a.trace == b.trace;
  detail = a.detail + Format(" | twice-run %s", same ? "identical" : "DIFFERS");
  if (!a.ok) detail += " | FAIL: " + a.fails[0] + (a.fails.size() > 1 ? Format(" (+%zu more)", a.fails.size() - 1) : "");
  else if (!b.ok) detail += " | FAIL (second run): " + b.fails[0];
  else if (!same) detail += " | FAIL: the second run of the fixture differs";
  return a.ok && b.ok && same ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DungeonEntityGates() {
  static const std::vector<Gate> g = {
      {"dungeon-entities", "world", {}, false, GateDungeonEntities, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
