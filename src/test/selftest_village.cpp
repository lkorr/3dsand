// selftest_village.cpp — HARROWBY, the first hand-built place (docs/
// PLAN_world_editor.md P8, docs/EDITOR_GUIDE.md "Building a village").
//
//   village-harrowby  The CONTENT gate. It loads the game's own map (the
//                     `harrowby` refs group in assets/worldmap/<map>/refs/,
//                     the four structures, the five villagers) instead of the
//                     harness map every other gate uses, and asserts:
//                       A. the map and its refs load with NO warnings; the
//                          dialogue and schedule files every villager names
//                          load and validate (no errors); every schedule row
//                          of every villager resolves to an anchor, and every
//                          anchor is reachable from the villager's home over
//                          the waynode graph (no "walking straight");
//                       B. all five villagers spawn;
//                       C. over one FAST DAY -- the clock held at each
//                          schedule boundary (06:00, 07:00, ...) until
//                          everyone whose row changed has arrived, bounded by
//                          harrowby.arriveMaxTicks (tests/baseline.json) --
//                          each villager reaches each of its rows' anchors;
//                       D. every door any of them used is shut at the end of
//                          the day;
//                       E. the day is the PINNED day: its exact trace
//                          (every villager's body and every door's hinge,
//                          tick by tick) equals harrowby.dayTrace in
//                          tests/baseline.json -- the same day on every boot
//                          (why not an in-process second run: see the gate);
//                       F. nobody bleeds (a door leaf or a jostle that draws
//                          blood is a bug in the village);
//                       G. NO TREE IN A HOUSE: right after worldgen, not one
//                          voxel of a tree material (every assets/trees/*.json
//                          bark / leaf / autumnLeaf) inside any structure's
//                          stamped box or the 48 voxels of air above its roof
//                          -- the forest stands round the village's clearing
//                          (map.json kind "clearing"), its crowns included.
//                          Counted per structure.
//                     SANDVOX_HARROWBY_SHOTS=<prefix> also writes pictures
//                     from the first run: the village from above at midday,
//                     each interior, a villager at work
//                     (<prefix>_overview.bmp, ..._smithy.bmp, ...).
//
// The map is the game's map (worldmap::ActiveMapName of world.mapLayer, or
// SANDVOX_VILLAGE_MAP); the gate switches to it, regenerates the window
// around the village, and on the way out switches back, reloads the
// environment and regenerates the window at the origin it found -- the exit
// contract of the structure gates.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "game/bodyreg.h"
#include "game/dialogue.h"
#include "game/mob.h"
#include "game/schedule.h"
#include "measure/perfsuite.h"
#include "phys/debris.h"
#include "sim/biomes.h"
#include "sim/chunkstore.h"
#include "sim/microbody.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/worlddefaults.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "world/refs.h"
#include "world/refs_doors.h"
#include "world/refs_npc.h"
#include "world/structures.h"

#include <nlohmann/json.hpp>

using namespace sandvox;

namespace selftest {
namespace {

constexpr const char* kGroup = "harrowby";

int FloorDiv16(int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }

void Mix(uint64_t& h, int64_t v) {
  for (int i = 0; i < 8; i++) {
    h ^= (uint64_t)((v >> (i * 8)) & 0xFF);
    h *= 1099511628211ull;
  }
}

std::string VillageMap() {
  if (const char* e = std::getenv("SANDVOX_VILLAGE_MAP"); e && *e) return e;
  return CurrentTuning().world.mapLayer.empty() ? std::string("default")
                                                : CurrentTuning().world.mapLayer;
}

// The village's refs, split by kind.
struct Village {
  std::vector<const refs::Ref*> npcs, doors, structures;
  IVec3 lo{1 << 30, 1 << 30, 1 << 30}, hi{-(1 << 30), -(1 << 30), -(1 << 30)};
};

Village Survey(const refs::RefStore& st) {
  Village v;
  for (const auto& [id, r] : st.All()) {
    if (id.rfind(std::string(kGroup) + "/", 0) != 0) continue;
    if (r.kind == "npc") v.npcs.push_back(&r);
    if (r.kind == "door") v.doors.push_back(&r);
    if (r.kind == "structure") v.structures.push_back(&r);
    v.lo = {std::min(v.lo.x, r.pos.x), std::min(v.lo.y, r.pos.y), std::min(v.lo.z, r.pos.z)};
    v.hi = {std::max(v.hi.x, r.pos.x), std::max(v.hi.y, r.pos.y), std::max(v.hi.z, r.pos.z)};
  }
  return v;
}

// ---- G. canopy intrusion -------------------------------------------------------------
// The materials worldgen's trees are made of: every species file's bark, leaf
// and autumn-leaf ramps (assets/trees/<name>.json), resolved by name. The
// species files are the truth the atlas is baked from, so a new species'
// materials join without an edit here.
std::set<uint32_t> TreeMaterials(const std::vector<MaterialDef>& mats) {
  std::set<std::string> names;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(AssetDir() + "/trees", ec)) {
    if (e.path().extension() != ".json") continue;
    std::ifstream f(e.path());
    nlohmann::json j;
    try { f >> j; } catch (...) { continue; }
    for (const char* k : {"bark", "leaf", "autumnLeaf"})
      if (j.contains(k) && j[k].is_array())
        for (const auto& n : j[k])
          if (n.is_string()) names.insert(n.get<std::string>());
  }
  std::set<uint32_t> ids;
  for (size_t i = 0; i < mats.size(); i++)
    if (names.count(mats[i].name)) ids.insert((uint32_t)i);
  return ids;
}

struct CanopyCount {
  std::string id;
  int inBox = 0, overhead = 0;
  long long cells = 0;
  IVec3 lo{}, hi{};
  IVec3 first{0, -1, 0};   // the first intruding cell, for the report
};

// Every structure ref's stamped box (structures::Frame::Box) plus the 48
// voxels above it, read back chunk by chunk, counting tree-material cells
// the HOUSE did not put there: a cell the asset authors is the asset's (a
// table of `wood` is furniture, not a branch), so only the asset's empty
// cells and the air above it are asked.
std::vector<CanopyCount> CountCanopy(Ctx& c, const std::vector<const refs::Ref*>& structs) {
  std::vector<CanopyCount> out;
  const std::set<uint32_t> tree = TreeMaterials(c.mats);
  std::map<uint32_t, std::vector<uint32_t>> chunks;
  auto mat = [&](IVec3 p) -> uint32_t {
    if (!c.world.CellInWindow(p)) return 0u;
    const uint32_t slot = World::SlotChunkIndex({p.x >> 4, p.y >> 4, p.z >> 4});
    auto it = chunks.find(slot);
    if (it == chunks.end()) {
      std::vector<uint32_t> w(kChunkVol);
      ReadVoxelsSync(c.ctx, c.world, slot, 1, w.data(), "village canopy");
      it = chunks.emplace(slot, std::move(w)).first;
    }
    return it->second[((uint32_t)(p.z & 15) * kChunk + (uint32_t)(p.y & 15)) * kChunk + (uint32_t)(p.x & 15)] & 0xFFFu;
  };
  for (const refs::Ref* r : structs) {
    CanopyCount cc;
    cc.id = r->id;
    structures::Asset a;
    std::string err;
    std::vector<std::string> aw;
    if (!structures::LoadAsset(AssetDir(), r->base, true, a, err, aw) || a.prefab.models.empty()) {
      cc.inBox = -1;
      out.push_back(cc);
      continue;
    }
    const structures::Frame f = structures::MakeFrame(a, r->pos, r->yaw);
    f.Box(cc.lo, cc.hi);
    std::set<std::tuple<int, int, int>> own;   // world cells the asset authors
    for (const PrefabVoxel& v : a.prefab.models[0].voxels) {
      const IVec3 w = f.Cell({v.x, v.y, v.z});
      own.insert({w.x, w.y, w.z});
    }
    for (int z = cc.lo.z; z <= cc.hi.z; z++)
      for (int y = cc.lo.y; y <= cc.hi.y + 48; y++)
        for (int x = cc.lo.x; x <= cc.hi.x; x++) {
          cc.cells++;
          if (!tree.count(mat({x, y, z})) || own.count({x, y, z})) continue;
          (y <= cc.hi.y ? cc.inBox : cc.overhead)++;
          if (cc.first.y < 0) cc.first = {x, y, z};
        }
    out.push_back(cc);
  }
  return out;
}

// ---- pictures (SANDVOX_HARROWBY_SHOTS) --------------------------------------------
// The --shot-mob recipe (bodies + micro bricks uploaded, four warm frames so
// the shadow cache and the irradiance grid catch up, the fourth grabbed),
// at noon whatever the schedule clock says.
void Shot(Ctx& c, Vec3 eye, Vec3 at, const std::string& path) {
  if (MicroBodySet* mbs = c.debris.MicroSet(); mbs != nullptr && mbs->dirty)
    c.sim.UploadMicroBodies(c.ctx.queue, *mbs);
  BodyRegistry reg(c.debris, c.mobs, nullptr);
  std::vector<BodyXformGpu> xf;
  reg.BuildXforms(xf);
  if (!xf.empty())
    c.ctx.queue.WriteBuffer(c.world.bodyXforms, 0, xf.data(), xf.size() * sizeof(BodyXformGpu));
  std::vector<MicroBodyInstGpu> microInsts;
  reg.BuildMicroInsts(microInsts);
  std::vector<BodyVoxInst> inst;
  reg.BuildInstances(inst);
  if (!inst.empty())
    c.ctx.queue.WriteBuffer(c.world.bodyInstances, 0, inst.data(), inst.size() * sizeof(BodyVoxInst));
  const uint32_t microCount = c.sim.UploadMicroBodyInsts(c.ctx.queue, microInsts);
  const Vec3 d = at - eye;
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  Camera cam;
  cam.yaw = std::atan2(d.z, d.x);
  cam.pitch = std::asin(std::clamp(d.y / std::max(len, 1e-3f), -1.0f, 1.0f));
  const uint32_t noon = (uint32_t)(0.5 * (double)TicksPerDay(CurrentTuning()));
  for (int f = 0; f < 4; f++) {
    WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)c.width / c.height, true, 11.7f,
                      kFarFogDensity, (float)c.height, noon);
    rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
    c.sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp =
        c.sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, c.width, c.height);
    c.sim.DrawWorld(rp);
    c.sim.DrawBodies(rp, (uint32_t)inst.size());
    c.sim.DrawMicroBodies(rp, microCount);
    rp.End();
    c.ctx.queue.Submit(enc.Finish());
  }
  c.ctx.WaitIdle();
  c.Grab(path.c_str());
}

// A point in a placed structure's house frame, in the world.
Vec3 HousePoint(const refs::RefStore& st, const char* id, float hx, float hy, float hz) {
  const refs::Ref* r = st.Find(id);
  if (r == nullptr) return Vec3{0, 0, 0};
  const int q = ((r->yaw / 90) % 4 + 4) % 4;
  float x = hx, z = hz;
  switch (q) {
    case 1: x = hz; z = -hx; break;
    case 2: x = -hx; z = -hz; break;
    case 3: x = -hz; z = hx; break;
    default: break;
  }
  return Vec3{(float)r->pos.x + x + 0.5f, (float)r->pos.y + hy, (float)r->pos.z + z + 0.5f};
}

// ---- the day ------------------------------------------------------------------------

struct DayResult {
  bool ok = false;
  std::vector<std::string> why;
  uint64_t trace = 1469598103934665603ull;
  std::vector<uint64_t> perTick;
  // Every entity every tick (villagers, then doors), for naming the FIRST
  // thing that differs between the two runs rather than only the tick.
  struct Sample {
    std::string who;
    int fx = 0, fy = 0, fz = 0, row = -9, phase = -1;
    // The body's exact float bits (origin, heading): not in the pass/fail
    // trace (1/8 voxel, like npc-schedule's), but a divergence shows here
    // hundreds of ticks before it rounds into the trace.
    float ox = 0, oy = 0, oz = 0, hd = 0;
    bool operator==(const Sample& o) const {
      return fx == o.fx && fy == o.fy && fz == o.fz && row == o.row && phase == o.phase;
    }
    bool Exact(const Sample& o) const {
      return *this == o && ox == o.ox && oy == o.oy && oz == o.oz && hd == o.hd;
    }
  };
  std::vector<Sample> samples;
  int ticks = 0, segments = 0, worst = 0, spawned = 0;
  std::string worstWho;
  uint32_t opens = 0, closes = 0, arrivals = 0, replans = 0;
  int doorsLeftOpen = 0;
  std::string lines;   // per-villager summary
  std::vector<CanopyCount> canopy;   // G, read right after worldgen
};

DayResult RunDay(Ctx& c, const std::string& mapName, IVec3 centreChunk,
                 const std::string& shots, uint32_t tickBase) {
  DayResult out;
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(6000);   // both runs spawn the same ids (hash salts)
  refs::ResetResidents();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  // A FRESH store per run, straight from the files: a day leaves deltas
  // behind (who has spawned, which door is open), and the second run must
  // start from exactly what the first one did.
  refs::RefStore st;
  st.LoadMap(AssetDir(), mapName);
  st.BindChunkStore(&c.stream.Store());
  const Village v = Survey(st);
  // G. The generated village, before anyone moves: no tree in any house.
  out.canopy = CountCanopy(c, v.structures);

  const int maxArrive = (int)BaselineNumber("harrowby.arriveMaxTicks", 2400);
  // The boundaries of everyone's day, in order, from the 05:30 start (all
  // asleep) to the last bedtime.
  std::set<int> bounds;
  for (const refs::Ref* n : v.npcs)
    if (const schedule::Schedule* sc = refs::Schedules().Find(n->props.value("schedule", "")))
      for (const schedule::Row& rw : sc->rows) bounds.insert(rw.from);
  std::vector<int> order;
  for (int b : bounds)
    if (b >= 6 * 60) order.push_back(b);
  for (int b : bounds)
    if (b < 6 * 60) order.push_back(b + 1440);   // a row starting after midnight
  std::sort(order.begin(), order.end());

  struct Track {
    int lastRow = -2;
    uint32_t rowSince = 0;
    bool arrived = true;
    int rows = 0, reached = 0;
    // The resident's last few distinct notes ("no progress toward X", "door
    // ... is locked"): a failure names the leg that broke, not only the end.
    std::vector<std::string> notes;
  };
  std::map<std::string, Track> track;
  auto noteOf = [&](const std::string& id, const std::string& note, uint32_t tick) {
    if (note.empty()) return;
    std::vector<std::string>& n = track[id].notes;
    const std::string line = Format("@%u %s", tick, note.c_str());
    if (!n.empty() && n.back().substr(n.back().find(' ') + 1) == note) return;
    n.push_back(line);
    if (n.size() > 4) n.erase(n.begin());
  };
  uint32_t t = tickBase;
  auto rig = std::make_unique<support::TickRig>(c, t, centreChunk);
  // Thirty EMPTY ticks first (no refs, so nobody spawns): the chunk fetch
  // queue and the readback ring carry over from whatever ran before (the
  // previous gate), and a request left queued would delay the day's first
  // fetches -- the day must start from the same state whatever ran first.
  support::RunTicks(*rig, 30);
  rig->Authority().refs = &st;
  auto traceTick = [&]() {
    out.ticks++;
    Mix(out.trace, out.ticks);
    for (const refs::Ref* n : v.npcs) {
      refs::ResidentStatus rs;
      const bool have = refs::ResidentStatusOf(n->id, rs) && rs.mobId != 0;
      DayResult::Sample s;
      s.who = n->id;
      if (have) {
        s.fx = (int)std::floor(rs.foot.x * 8.0f);
        s.fy = (int)std::floor(rs.foot.y * 8.0f);
        s.fz = (int)std::floor(rs.foot.z * 8.0f);
        s.row = rs.row;
        s.phase = (int)rs.phase;
        noteOf(n->id, rs.note, (uint32_t)out.ticks);
        if (const Mob* m = c.mobs.FindMobByRef(n->id)) {
          s.ox = m->Origin().x;
          s.oy = m->Origin().y;
          s.oz = m->Origin().z;
          s.hd = m->Heading();
        }
      }
      Mix(out.trace, have ? 1 : 0);
      Mix(out.trace, s.fx);
      Mix(out.trace, s.fy);
      Mix(out.trace, s.fz);
      Mix(out.trace, s.row);
      Mix(out.trace, s.phase);
      out.samples.push_back(s);
    }
    for (const refs::Ref* d : v.doors) {
      refs::DoorStatus ds;
      DayResult::Sample s;
      s.who = d->id;
      s.phase = refs::DoorStatusOf(d->id, ds) ? (int)ds.phase : 0;
      s.hd = ds.angle;   // exact only: the hinge angle, radians
      Mix(out.trace, s.phase);
      out.samples.push_back(s);
    }
    out.perTick.push_back(out.trace);
  };
  // Has everyone whose row began since `since` got there? Updates `track`.
  auto settle = [&]() {
    bool all = true;
    for (const refs::Ref* n : v.npcs) {
      refs::ResidentStatus rs;
      if (!refs::ResidentStatusOf(n->id, rs) || rs.mobId == 0) {
        all = false;
        continue;
      }
      Track& tr = track[n->id];
      if (rs.row != tr.lastRow || rs.rowSince != tr.rowSince) {
        tr.lastRow = rs.row;
        tr.rowSince = rs.rowSince;
        tr.arrived = false;
        tr.rows++;
      }
      if (!tr.arrived && rs.arrivedTick != 0 && rs.phase == refs::ResidentPhase::AtAnchor) {
        tr.arrived = true;
        tr.reached++;
        const int took = (int)(rs.arrivedTick - rs.rowSince);
        if (took > out.worst) {
          out.worst = took;
          out.worstWho = n->id + " " + rs.activity;
        }
      }
      all &= tr.arrived;
    }
    return all;
  };

  // 05:30: everyone appears (catch-up puts them in bed) and lies down.
  schedule::SetMinuteOverride(5 * 60 + 30);
  int waited = 0;
  while (waited < 600) {
    support::RunTicks(*rig, 1);
    traceTick();
    waited++;
    int n = 0;
    for (const refs::Ref* r : v.npcs) n += c.mobs.FindMobByRef(r->id) != nullptr ? 1 : 0;
    out.spawned = n;
    if (n == (int)v.npcs.size() && settle()) break;
  }
  if (out.spawned != (int)v.npcs.size())
    out.why.push_back(Format("%d of %zu villagers spawned after %d ticks", out.spawned,
                             v.npcs.size(), waited));

  // The day, one boundary at a time.
  for (int b : order) {
    out.segments++;
    schedule::SetMinuteOverride(b % 1440);
    int k = 0;
    bool all = false;
    for (; k < maxArrive; k++) {
      support::RunTicks(*rig, 1);
      traceTick();
      if (k >= 2 && settle()) {
        all = true;
        break;
      }
    }
    // A few more ticks at the anchor: doors swing shut behind.
    for (int j = 0; j < 30; j++) {
      support::RunTicks(*rig, 1);
      traceTick();
    }
    if (!all)
      for (const refs::Ref* n : v.npcs) {
        const Track& tr = track[n->id];
        if (tr.arrived) continue;
        refs::ResidentStatus rs;
        refs::ResidentStatusOf(n->id, rs);
        // WHERE it stuck and WHICH way it was going, so the fix is one look
        // at the house plan, not another run.
        std::string route;
        for (size_t i = 0; i < rs.route.nodes.size(); i++)
          route += (i ? " > " : "") + (rs.route.nodes[i].empty() ? std::string("anchor") : rs.route.nodes[i]) +
                   (i == rs.cursor ? "*" : "");
        out.why.push_back(Format("%02d:%02d: %s never reached its '%s' anchor (%s at %.0f,%.0f,%.0f) "
                                 "in %d ticks: phase %s, feet at (%.1f,%.1f,%.1f), %.0f vox away%s%s "
                                 "[route %s] [notes %s]",
                                 (b % 1440) / 60, b % 60, n->id.c_str(), rs.activity.c_str(),
                                 rs.anchor.refId.c_str(), rs.anchor.foot.x, rs.anchor.foot.y,
                                 rs.anchor.foot.z, maxArrive, refs::ResidentPhaseName(rs.phase),
                                 rs.foot.x, rs.foot.y, rs.foot.z,
                                 std::sqrt((rs.foot.x - rs.anchor.foot.x) * (rs.foot.x - rs.anchor.foot.x) +
                                           (rs.foot.z - rs.anchor.foot.z) * (rs.foot.z - rs.anchor.foot.z)),
                                 rs.note.empty() ? "" : " -- ", rs.note.c_str(), route.c_str(),
                                 [&] {
                                   std::string s;
                                   for (const std::string& x : track[n->id].notes) s += (s.empty() ? "" : " / ") + x;
                                   return s;
                                 }().c_str()));
      }
    // THE PICTURES, from the first run, once everyone is where 13:00 puts
    // them (Osric at the anvil, Edric at the field edge, Wat in the yard).
    if (!shots.empty() && b % 1440 == 13 * 60) {
      RefillFarAround(c.ctx, c.world, c.sim, WindowCentreChunk(c.world));
      const Vec3 green = HousePoint(st, "harrowby/green", 0, 0, -8);   // the well
      Shot(c, green + Vec3{150.0f, 175.0f, 215.0f}, green + Vec3{-15.0f, 0.0f, -55.0f},
           shots + "_overview.bmp");
      Shot(c, green + Vec3{-40.0f, 30.0f, 95.0f}, green + Vec3{0.0f, 20.0f, -40.0f},
           shots + "_green.bmp");
      Shot(c, HousePoint(st, "harrowby/smithy", -25, 22, 62), HousePoint(st, "harrowby/smithy", -18, 6, 12),
           shots + "_osric_at_work.bmp");
      Shot(c, HousePoint(st, "harrowby/smithy", 3, 16, 24), HousePoint(st, "harrowby/smithy", -40, 5, -4),
           shots + "_smithy.bmp");
      Shot(c, HousePoint(st, "harrowby/longhouse", 15, 16, 22), HousePoint(st, "harrowby/longhouse", -25, 4, -6),
           shots + "_longhouse.bmp");
      Shot(c, HousePoint(st, "harrowby/longhouse", -36, 16, 20), HousePoint(st, "harrowby/longhouse", -62, 4, -16),
           shots + "_byre.bmp");
      Shot(c, HousePoint(st, "harrowby/alehouse", 38, 18, 22), HousePoint(st, "harrowby/alehouse", 0, 5, -18),
           shots + "_alehouse.bmp");
      Shot(c, HousePoint(st, "harrowby/field", 0, 22, -60), HousePoint(st, "harrowby/field", 0, 0, 10),
           shots + "_field.bmp");
    }
  }
  // D. every door shut at the end of the day.
  for (const refs::Ref* d : v.doors) {
    refs::DoorStatus ds;
    if (refs::DoorStatusOf(d->id, ds) && ds.phase != refs::DoorPhase::Closed) {
      out.doorsLeftOpen++;
      out.why.push_back(d->id + " ends the day " + refs::DoorPhaseName(ds.phase));
    }
  }
  for (const refs::Ref* n : v.npcs) {
    refs::ResidentStatus rs;
    refs::ResidentStatusOf(n->id, rs);
    out.opens += rs.doorOpens;
    out.closes += rs.doorCloses;
    out.arrivals += rs.arrivals;
    out.replans += rs.replans;
    const Track& tr = track[n->id];
    // A quiet day hurts nobody: a door leaf or a jostle that draws blood is a
    // bug in the village, not colour.
    const Mob* body = c.mobs.FindMobByRef(n->id);
    const float bled = body != nullptr ? body->BloodLost() : 0.0f;
    if (bled > 0.0f)
      out.why.push_back(Format("%s lost %.1f blood over an ordinary day", n->id.c_str(), bled));
    out.lines += Format("%s%s %d/%d rows, %u doors%s", out.lines.empty() ? "" : "; ",
                        n->id.substr(n->id.find('/') + 1).c_str(), tr.reached, tr.rows, rs.doorOpens,
                        bled > 0.0f ? Format(", BLED %.1f", bled).c_str() : "");
  }
  rig.reset();
  refs::RefCtx rc{&st, &c.mobs, &c.world, &c.stream.Store()};
  rc.phys = &c.phys;
  rc.debris = &c.debris;
  st.DeactivateAll(rc, refs::RefEvent::Reset);
  refs::ResetResidents();
  out.ok = out.why.empty();
  return out;
}

// ---- the gate ----------------------------------------------------------------------

Status GateVillageHarrowby(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& w) {
    if (!ok && why.size() < 12) why.push_back(w);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint32_t ticksAtEntry = c.world.TicksEncoded();   // 0: no gate ticked before this one
  const uint64_t idWas = c.mobs.NextIdCounter();
  const std::string prevMap = worldmap::ActiveMapName(CurrentTuning().world.mapLayer);
  const std::string mapName = VillageMap();
  const std::string ad = AssetDir();

  // ---- A. the map, the refs, the content -------------------------------------------
  worldmap::SetMapOverride(mapName);
  biomes::EnvironmentStamp stamp;
  std::string log;
  auto restore = [&]() {
    schedule::SetMinuteOverride(-1);
    refs::ResetResidents();
    c.debris.Reset();
    c.mobs.Reset();
    c.mobs.SetParkFn(nullptr);
    c.mobs.SetUnparkPlacer(nullptr);
    c.mobs.ClearPlayerActors();
    c.mobs.SetNextIdCounter(idWas);
    c.stream.Store().Clear();
    worldmap::SetMapOverride(prevMap);
    biomes::EnvironmentStamp s2;
    std::string l2;
    ReloadEnvironment(c.ctx, c.sim, c.mats, s2, l2);
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    structures::TakeReapply();
  };
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
    restore();
    detail = "map '" + mapName + "' did not load: " + log;
    return Status::Fail;
  }
  int mapWarn = 0;
  for (const std::string& w : worldmap::CurrentWorldMap().warnings) {
    mapWarn++;
    check(false, "map warning: " + w);
  }
  refs::RefStore st;
  st.LoadMap(ad, mapName);
  const Village v = Survey(st);
  int refWarn = 0;
  for (const std::string& w : st.Warnings()) {
    refWarn++;
    check(false, "refs warning: " + w);
  }
  // Harrowby was built with five; a house and a villager added later only
  // move the pinned trace (--rebaseline), they do not fail the count.
  check(v.npcs.size() >= 5, Format("%zu villagers in refs/%s.json, want at least the five", v.npcs.size(), kGroup));
  check(v.structures.size() >= 3, Format("%zu structures, want >= 3", v.structures.size()));

  // The content every villager names: its conversation and its day.
  dialogue::Library lib;
  std::vector<dialogue::Problem> dprobs;
  lib.Load(ad + "/dialogue", &c.items, dprobs);
  lib.Validate(&c.items, dprobs);
  int dErr = 0, dWarn = 0;
  std::set<std::string> ours;
  for (const refs::Ref* n : v.npcs) ours.insert(n->props.value("dialogue", "") + ".json");
  for (const dialogue::Problem& p : dprobs) {
    bool mine = false;
    for (const std::string& f : ours) mine |= p.file.find(f) != std::string::npos;
    if (!mine) continue;
    if (p.error) {
      dErr++;
      check(false, "dialogue: " + p.Line());
    } else {
      dWarn++;
    }
  }
  refs::ReloadSchedules();
  int rows = 0, badAnchors = 0, straight = 0;
  for (const refs::Ref* n : v.npcs) {
    const std::string dn = n->props.value("dialogue", "");
    check(!dn.empty() && lib.Find(dn) != nullptr, n->id + ": dialogue '" + dn + "' does not load");
    const schedule::Schedule* sc = refs::Schedules().Find(n->props.value("schedule", ""));
    if (!check(sc != nullptr, n->id + ": schedule '" + n->props.value("schedule", "") + "' does not load"))
      continue;
    const refs::Anchor home = refs::ResolveAnchor(st, *n, "home");
    for (const schedule::Row& rw : sc->rows) {
      rows++;
      const refs::Anchor a = refs::ResolveAnchor(st, *n, rw.at);
      if (!a.ok) {
        badAnchors++;
        check(false, n->id + ": row " + rw.act + " at '" + rw.at + "': " + a.why);
        continue;
      }
      // Reachable from home over the graph (the anchor and home share a
      // nearest node when they are in the same room: that is fine too).
      const refs::Route r = refs::PlanRoute(st, home.foot, a.foot, nullptr);
      if (!r.note.empty()) {
        straight++;
        check(false, n->id + ": home -> '" + rw.at + "': " + r.note);
      }
    }
  }

  // ---- B-E. the day, twice -----------------------------------------------------------
  // The window centred on the village (x/z), ground in the lower half.
  const int cx = (v.lo.x + v.hi.x) / 2, cz = (v.lo.z + v.hi.z) / 2;
  const IVec3 origin{FloorDiv16(cx - (int)kWorldN / 2), 0, FloorDiv16(cz - (int)kWorldN / 2)};
  const IVec3 centreChunk{FloorDiv16(cx), FloorDiv16(v.lo.y), FloorDiv16(cz)};
  c.stream.OnRegen();
  c.world.SetWindowOrigin(origin);
  const char* shotEnv = std::getenv("SANDVOX_HARROWBY_SHOTS");
  // E. DETERMINISM, BOOT TO BOOT. The day is run ONCE and its trace -- every
  // villager's exact body position and heading and every door's phase and
  // hinge angle, every tick -- is pinned in tests/baseline.json
  // (harrowby.dayTrace), the determinismHash pattern: the same content gives
  // the same day on every boot, and a content edit moves it (a notification:
  // `--selftest --gate village-harrowby --rebaseline`).
  //
  // Why not npc-schedule's in-process twice-run: measured 2026-09-29, a second
  // day in the same process matched the first bit for bit in every villager
  // until the first door opened, and the longhouse back door's hinge angle
  // then differed in the fifth decimal on its first swing (tick 470; with a
  // fresh Jolt world per run as well, tick 474) -- physics state that outlives
  // the harness's resets, not this village. Each run was itself identical on
  // every boot, which is the claim that matters and the one pinned here.
  //
  // THE SAME STATE LEAKS ACROSS GATES: after npc-schedule, or after the three
  // gates of a --verify list, the day comes out a tick shorter with another
  // trace (measured: three scopes, three traces, each stable boot to boot).
  // So the pin is compared only when this gate is the FIRST to tick the world
  // in its process (`--gate village-harrowby`, or first in a --verify list);
  // anywhere else the trace is reported and not judged (CLAUDE.md rule 7: a
  // subset is not the suite, and the suite is not a subset).
  const bool firstToTick = ticksAtEntry == 0;
  DayResult a;
  if (!v.npcs.empty()) a = RunDay(c, mapName, centreChunk, shotEnv ? shotEnv : "", 26000);
  uint64_t exact = 1469598103934665603ull;
  for (const DayResult::Sample& s : a.samples) {
    uint32_t bits[4];
    std::memcpy(&bits[0], &s.ox, 4);
    std::memcpy(&bits[1], &s.oy, 4);
    std::memcpy(&bits[2], &s.oz, 4);
    std::memcpy(&bits[3], &s.hd, 4);
    for (uint32_t b : bits) Mix(exact, b);
    Mix(exact, s.row);
    Mix(exact, s.phase);
  }
  const std::string traceHex = Format("%016llx:%d", (unsigned long long)exact, a.ticks);
  if (firstToTick) RecordObserved("harrowby.dayTrace", traceHex);   // only the comparable scope
  RecordObserved("harrowby.arriveWorstTicks", (double)a.worst);
  const std::string* pinned = BaselineValue("harrowby.dayTrace");
  const bool pinOk = !firstToTick || pinned == nullptr || pinned->empty() || *pinned == traceHex;
  for (const std::string& w : a.why) check(false, w);
  // G. canopy intrusion, per structure.
  std::string canopyLine;
  int canopyTotal = 0;
  for (const CanopyCount& cc : a.canopy) {
    const std::string name = cc.id.substr(cc.id.find('/') + 1);
    if (cc.inBox < 0) {
      check(false, cc.id + ": its asset did not load for the canopy count");
      continue;
    }
    canopyTotal += cc.inBox + cc.overhead;
    canopyLine += Format("%s%s %d+%d", canopyLine.empty() ? "" : ", ", name.c_str(), cc.inBox, cc.overhead);
    if (cc.inBox + cc.overhead > 0)
      check(false, Format("%s: %d tree-material voxels inside its stamped box (%d,%d,%d)..(%d,%d,%d) and %d in "
                          "the 48 above it, the first at (%d,%d,%d): the forest's crowns reach into the "
                          "village -- widen the map's clearing (map.json kind \"clearing\") over it",
                          cc.id.c_str(), cc.inBox, cc.lo.x, cc.lo.y, cc.lo.z, cc.hi.x, cc.hi.y, cc.hi.z,
                          cc.overhead, cc.first.x, cc.first.y, cc.first.z));
  }
  RecordObserved("harrowby.canopyVoxels", (double)canopyTotal);
  if (!pinOk && why.empty()) {
    MarkPinnedOnly();
    check(false, "the day's trace " + traceHex + " is not the pinned " + *pinned +
                     " (a village, schedule or engine edit moves it: --rebaseline; if nothing "
                     "changed, the day is not reproducible)");
  }
  restore();

  detail = Format(
      "map '%s': %d map + %d refs warnings | %zu villagers, %zu structures, %zu doors | dialogue "
      "%d errors (%d warnings) | %d schedule rows, %d unresolved, %d unrouted | day: %d spawned, "
      "%d segments in %d ticks, worst arrival %d ticks (%s, bound %d), doors opened %u closed %u, "
      "%d left open, re-plans %u [%s] | canopy in houses (box+above) %s | trace %s (%s)",
      mapName.c_str(), mapWarn, refWarn, v.npcs.size(), v.structures.size(), v.doors.size(), dErr,
      dWarn, rows, badAnchors, straight, a.spawned, a.segments, a.ticks, a.worst,
      a.worstWho.c_str(), (int)BaselineNumber("harrowby.arriveMaxTicks", 2400), a.opens, a.closes,
      a.doorsLeftOpen, a.replans, a.lines.c_str(), canopyLine.c_str(), traceHex.c_str(),
      !firstToTick ? Format("not compared: %u ticks ran before this gate in this process", ticksAtEntry).c_str()
      : pinned == nullptr || pinned->empty() ? "not pinned yet"
      : (pinOk ? "= pinned" : "DIFFERS from the pin"));
  if (!why.empty()) {
    detail += " | FAIL: " + why[0];
    for (size_t i = 1; i < why.size() && i < 6; i++) detail += " | " + why[i];
  }
  return why.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& VillageGates() {
  static const std::vector<Gate> g = {
      {"village-harrowby", "world", {}, false, GateVillageHarrowby, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
