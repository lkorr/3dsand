// refs_npc.cpp — see refs_npc.h.

#include "world/refs_npc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>

#include "game/ai_behavior.h"
#include "game/dialogue.h"
#include "game/dye.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "sim/bytestream.h"
#include "sim/rng.h"
#include "sim/world.h"
#include "test/support.h"
#include "world/refs_doors.h"

namespace refs {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kNpcSpawned = 1;
// The walk: a fraction of the def's own speed. `human` is 31.5 vox/s (a
// jog); half of it is an unhurried 1.6 m/s.
constexpr float kWalkSpeed = 0.5f;
// A waypoint (not the anchor) counts as reached within this, planar voxels.
constexpr float kNodeRadius = 3.5f;
// No progress toward the current point for this long = the leg failed.
constexpr uint32_t kStuckTicks = 150;
// Re-plans after failed legs before the villager gives up on this row.
constexpr int kMaxReplans = 4;
// A door that will not stand open within this long = the edge failed.
constexpr uint32_t kDoorTimeoutTicks = 240;
// The use marker in RefStore::Uses for a villager's door use.
constexpr uint32_t kNpcUser = 0xFFFFFFFEu;

int FloorDiv(int v, int d) { return (v >= 0) ? v / d : -((-v + d - 1) / d); }
int IFloor(float v) { return (int)std::floor(v); }

float Planar(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

std::string PropStr(const Ref& r, const char* k) {
  if (r.props.contains(k) && r.props[k].is_string()) return r.props[k].get<std::string>();
  return std::string();
}

std::string NpcName(const Ref& r) {
  const std::string n = PropStr(r, "name");
  if (!n.empty()) return n;
  const size_t s = r.id.rfind('/');
  return s == std::string::npos ? r.id : r.id.substr(s + 1);
}

std::string NpcDefName(const Ref& r) { return r.base.empty() ? "human" : r.base; }

Vec3 FootOfRef(const Ref& r) {
  return Vec3{(float)r.pos.x + 0.5f, (float)r.pos.y, (float)r.pos.z + 0.5f};
}

float YawRad(int yawDeg) { return (float)yawDeg * kPi / 180.0f; }

// ---- the resident layer's process state ----------------------------------------
struct Resident {
  uint64_t mobId = 0;
  ResidentStatus st;
  std::string rowKey;
  std::set<std::pair<int, int>> bad;   // graph edges this villager failed to walk
  int replansThisRow = 0;
  float bestDist = 1e9f;
  uint32_t bestTick = 0;
  // DoorWait
  bool doorAsked = false;
  uint32_t doorAskTick = 0, doorOpenSeen = 0, doorWaitSince = 0, doorFetchAt = 0;
  int doorSide = 0;        // which side of the door plane it waits on (+1 = the
                           // side the leaf swings toward)
  // A door this villager opened and must close behind it.
  std::string closeDoor;
  int closeSide = 0;
  bool closePassed = false;
  // wander
  bool wanderGoing = false;
  Vec3 wanderGoal{};
  uint32_t wanderUntil = 0, wanderSeq = 0;
};

struct ResidentsWorld {
  schedule::Library schedules;
  bool loaded = false;
  std::map<std::string, Resident> npcs;
  // The ground wait for a first spawn: ref id -> tick of the first ask.
  std::map<std::string, uint32_t> askedAt;
  // The store the last tick ran against, for the unpark placer (which runs
  // after the tick, from MobParking). Nulled when that store is destroyed.
  RefStore* store = nullptr;
  uint32_t lastTick = 0;
  // Graph cache.
  const RefStore* graphFor = nullptr;
  uint64_t graphRev = ~0ull;
  WayGraph graph;
};
ResidentsWorld& W() {
  static ResidentsWorld w;
  return w;
}

void StoreGone(RefStore* s) {
  ResidentsWorld& w = W();
  if (w.store == s) {
    w.store = nullptr;
    w.npcs.clear();
    w.askedAt.clear();
  }
  if (w.graphFor == s) {
    w.graphFor = nullptr;
    w.graphRev = ~0ull;
    w.graph = WayGraph{};
  }
}

// The MobParking ground rule (persist.h): place a body only onto ground the
// fetch cache has answered for SINCE this ref first asked.
bool GroundKnown(World& world, const std::string& id, IVec3 cell, uint32_t tick) {
  auto it = W().askedAt.find(id);
  const bool first = it == W().askedAt.end();
  if (first) it = W().askedAt.emplace(id, tick).first;
  bool known = !first;
  const int cx = FloorDiv(cell.x, (int)kChunk), cz = FloorDiv(cell.z, (int)kChunk);
  const int cy = FloorDiv(cell.y, (int)kChunk);
  for (int dy = 0; dy >= -1; dy--) {
    const IVec3 wc{cx, cy + dy, cz};
    const CachedChunk* cc = world.Cached(wc);
    if (first || cc == nullptr || cc->voxels.size() != kChunkVol || cc->version < it->second) {
      world.RequestChunkFetch(wc, World::FetchSource::Mob);
      known = false;
    }
  }
  return known;
}

// ---- anchors ---------------------------------------------------------------------

bool HasTag(const Ref& r, const std::string& tag) {
  if (!r.props.contains("tags") || !r.props["tags"].is_array()) return false;
  for (const Json& t : r.props["tags"])
    if (t.is_string() && t.get<std::string>() == tag) return true;
  return false;
}

void AnchorOfRef(const RefStore& s, const Ref& t, Anchor& a) {
  a.ok = true;
  a.refId = t.id;
  if (t.kind == "bed") {
    BedAnchor b;
    if (BedAnchorOf(t, b)) {
      // Stand (and then lie) over the middle of the mattress, facing from
      // the foot toward the head: the sleep pose pitches the body forward,
      // so its head goes where it faces.
      a.foot = Vec3{0.5f * (b.head.x + b.foot.x), b.head.y - 0.5f, 0.5f * (b.head.z + b.foot.z)};
      a.haveHeading = true;
      a.heading = b.headingRad + kPi;
      a.bed = true;
      return;
    }
  }
  // A structure (or anything with children): its hearth, else its first
  // waynode, else its own pos.
  const std::vector<const Ref*> kids = s.ChildrenOf(t.id);
  if (!kids.empty() && t.kind != "marker" && t.kind != "waynode") {
    const Ref* pick = nullptr;
    for (const Ref* k : kids)
      if (pick == nullptr && (k->id == t.id + "/hearth" || HasTag(*k, "hearth"))) pick = k;
    for (const Ref* k : kids)
      if (pick == nullptr && k->kind == "waynode") pick = k;
    if (pick != nullptr) {
      a.refId = pick->id;
      a.foot = FootOfRef(*pick);
      a.haveHeading = true;
      a.heading = YawRad(pick->yaw);
      return;
    }
  }
  a.foot = FootOfRef(t);
  a.haveHeading = true;
  a.heading = YawRad(t.yaw);
}

// A WayGraph node's foot point.
Vec3 NodeFoot(const Ref& r) { return FootOfRef(r); }

// Does segment a->b (planar) pass through the door's leaf box?
bool CrossesDoor(const Vec3& a, const Vec3& b, const DoorGeom& g) {
  const float y = std::min(a.y, b.y);
  if (y < (float)g.lo.y - 6.0f || y > (float)g.hi.y) return false;
  const float lx = (float)g.lo.x - 0.5f, hx = (float)g.hi.x + 1.5f;
  const float lz = (float)g.lo.z - 0.5f, hz = (float)g.hi.z + 1.5f;
  float t0 = 0.0f, t1 = 1.0f;
  const float d[2] = {b.x - a.x, b.z - a.z};
  const float p0[2] = {a.x, a.z};
  const float lo[2] = {lx, lz}, hi[2] = {hx, hz};
  for (int k = 0; k < 2; k++) {
    if (std::fabs(d[k]) < 1e-6f) {
      if (p0[k] < lo[k] || p0[k] > hi[k]) return false;
      continue;
    }
    float ta = (lo[k] - p0[k]) / d[k], tb = (hi[k] - p0[k]) / d[k];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

void BuildGraph(const RefStore& s, WayGraph& g) {
  g = WayGraph{};
  for (const auto& [id, r] : s.All())
    if (r.kind == "waynode") g.nodes.push_back({id, NodeFoot(r)});
  g.adj.assign(g.nodes.size(), {});
  std::set<std::pair<int, int>> have;
  std::vector<std::pair<std::string, DoorGeom>> doors;
  for (const auto& [id, r] : s.All())
    if (r.kind == "door") {
      DoorGeom dg = DoorGeometry(r);
      if (dg.ok) doors.push_back({id, dg});
    }
  auto addEdge = [&](int a, int b, bool autoLinked) {
    if (a == b) return;
    const std::pair<int, int> k{std::min(a, b), std::max(a, b)};
    if (!have.insert(k).second) return;
    WayGraph::Edge e;
    e.a = k.first;
    e.b = k.second;
    const Vec3 pa = g.nodes[(size_t)e.a].p, pb = g.nodes[(size_t)e.b].p;
    const float dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
    e.cost = std::sqrt(dx * dx + dy * dy + dz * dz);
    e.autoLinked = autoLinked;
    for (const auto& [did, dg] : doors)
      if (CrossesDoor(pa, pb, dg)) {
        e.door = did;
        break;
      }
    g.adj[(size_t)e.a].push_back((int)g.edges.size());
    g.adj[(size_t)e.b].push_back((int)g.edges.size());
    g.edges.push_back(std::move(e));
  };
  for (size_t i = 0; i < g.nodes.size(); i++) {
    const Ref* r = s.Find(g.nodes[i].id);
    if (r == nullptr) continue;
    if (r->props.contains("links") && r->props["links"].is_array()) {
      const size_t cut = r->id.rfind('/');
      const std::string parent = cut == std::string::npos ? std::string() : r->id.substr(0, cut);
      for (const Json& l : r->props["links"]) {
        if (!l.is_string()) continue;
        const std::string name = l.get<std::string>();
        int j = g.Find(name);
        if (j < 0 && !parent.empty()) j = g.Find(parent + "/" + name);
        if (j < 0) j = g.Find(r->group + "/" + name);
        if (j < 0) {
          g.problems.push_back("refs/" + r->group + ".json: " + r->id + ": props.links: \"" + name +
                               "\" is not a waynode (tried the id, a sibling slot and " +
                               r->group + "/" + name + ")");
          continue;
        }
        addEdge((int)i, j, false);
      }
    }
    if (r->props.contains("autoLink") && r->props["autoLink"].is_number()) {
      const float reach = r->props["autoLink"].get<float>() * 10.0f;   // metres -> voxels
      if (reach > 0.0f)
        for (size_t j = 0; j < g.nodes.size(); j++) {
          if (j == i) continue;
          const Vec3 a = g.nodes[i].p, b = g.nodes[j].p;
          const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
          if (std::fabs(dy) <= 20.0f && std::sqrt(dx * dx + dy * dy + dz * dz) <= reach)
            addEdge((int)i, (int)j, true);
        }
    }
  }
}

int NearestNode(const WayGraph& g, const Vec3& p) {
  int best = -1;
  float bestD = 1e30f;
  for (size_t i = 0; i < g.nodes.size(); i++) {
    const Vec3& q = g.nodes[i].p;
    // Height counts triple: the node on the floor you stand on, not the one
    // through the ceiling.
    const float dx = q.x - p.x, dy = (q.y - p.y) * 3.0f, dz = q.z - p.z;
    const float d = dx * dx + dy * dy + dz * dz;
    if (d < bestD) {
      bestD = d;
      best = (int)i;
    }
  }
  return best;
}

const WayGraph::Edge* EdgeBetween(const WayGraph& g, int a, int b) {
  for (int ei : g.adj[(size_t)a]) {
    const WayGraph::Edge& e = g.edges[(size_t)ei];
    if ((e.a == a && e.b == b) || (e.a == b && e.b == a)) return &e;
  }
  return nullptr;
}

// ---- doors -------------------------------------------------------------------------

// The door kind's own onUse, as the player's use runs it (refs_game.cpp
// TickRefs), with no session: a villager opening or closing a door. Recorded
// in RefStore::Uses like every use.
void UseDoorAsNpc(RefCtx& c, const Ref& door, const std::string& who) {
  const RefKind* k = Kinds().Find(door.kind);
  UseRecord rec;
  rec.tick = c.tick;
  rec.player = kNpcUser;
  rec.id = door.id;
  if (k == nullptr || !k->onUse || c.refs == nullptr || !c.refs->IsActive(door.id)) {
    rec.message = "npc " + who + ": the door is not usable";
  } else {
    RefUse u;
    u.player = kNpcUser;
    k->onUse(c, door, u);
    rec.used = true;
    rec.message = "npc " + who + (u.message.empty() ? std::string(": used") : ": " + u.message);
  }
  if (c.refs != nullptr) c.refs->NoteUse(std::move(rec));
}

DoorPhase PhaseOfDoor(const std::string& id, DoorStatus* out = nullptr) {
  DoorStatus ds;
  if (!DoorStatusOf(id, ds)) ds.phase = DoorPhase::Closed;
  if (out) *out = ds;
  return ds.phase;
}

// Which side of the door's plane `p` is on: +1 = the side the leaf swings
// toward (the door's yaw), -1 = behind; `along` gets the distance.
int DoorSideOf(const DoorGeom& g, const Vec3& p, float* along = nullptr) {
  const Vec3 c = g.Center();
  const float d = (p.x - c.x) * (float)g.face.x + (p.z - c.z) * (float)g.face.z;
  if (along) *along = std::fabs(d);
  return d >= 0.0f ? 1 : -1;
}

// Where to wait for a door, on `side`: straight out from the doorway, clear
// of the leaf's swing when the leaf swings toward us.
Vec3 DoorWaitPoint(const DoorGeom& g, int side, float floorY) {
  const Vec3 c = g.Center();
  const float out = side > 0 ? (float)g.width + 5.0f : 6.0f;
  return Vec3{c.x + (float)g.face.x * out * (float)side, floorY,
              c.z + (float)g.face.z * out * (float)side};
}

bool DoorCacheFresh(World* world, const DoorGeom& g, uint32_t since, uint32_t tick,
                    uint32_t& fetchAt) {
  if (world == nullptr) return true;
  bool fresh = true;
  const bool ask = fetchAt == 0 || tick - fetchAt >= 10;
  for (int z = FloorDiv(g.lo.z, 16); z <= FloorDiv(g.hi.z, 16); z++)
    for (int y = FloorDiv(g.lo.y, 16); y <= FloorDiv(g.hi.y, 16); y++)
      for (int x = FloorDiv(g.lo.x, 16); x <= FloorDiv(g.hi.x, 16); x++) {
        const CachedChunk* cc = world->Cached({x, y, z});
        if (cc != nullptr && cc->voxels.size() == kChunkVol && cc->version > since) continue;
        fresh = false;
        if (ask) world->RequestChunkFetch({x, y, z}, World::FetchSource::Mob);
      }
  if (!fresh && ask) fetchAt = tick;
  return fresh;
}

// ---- one villager's tick -------------------------------------------------------------

float ArriveRadius(const std::string& act) {
  if (act == "sleep") return 4.0f;
  if (act == "socialize") return 6.0f;
  if (act == "wander") return 5.0f;
  if (act == "goto") return 4.0f;
  return 3.0f;
}

void Plan(RefCtx& c, Resident& R, const Vec3& foot) {
  R.st.route = Route{};
  R.st.cursor = 0;
  R.bestDist = 1e9f;
  R.bestTick = c.tick;
  if (!R.st.anchor.ok) {
    R.st.phase = ResidentPhase::NoRoute;
    R.st.note = R.st.anchor.why;
    return;
  }
  R.st.route = PlanRoute(*c.refs, foot, R.st.anchor.foot, &R.bad);
  R.st.phase = ResidentPhase::Travel;
  if (!R.st.route.note.empty()) R.st.note = R.st.route.note;
}

void Replan(RefCtx& c, Resident& R, const Vec3& foot, const std::string& why) {
  const WayGraph& g = Graph(*c.refs);
  const Route& rt = R.st.route;
  if (R.st.cursor > 0 && R.st.cursor < rt.pts.size()) {
    const int a = g.Find(rt.nodes[R.st.cursor - 1]), b = g.Find(rt.nodes[R.st.cursor]);
    if (a >= 0 && b >= 0) R.bad.insert({std::min(a, b), std::max(a, b)});
  }
  R.st.replans++;
  R.replansThisRow++;
  R.st.note = why;
  if (R.replansThisRow > kMaxReplans) {
    R.st.phase = ResidentPhase::NoRoute;
    R.bestDist = 1e9f;   // the straight walk gets its own stuck clock
    R.bestTick = c.tick;
    R.st.note = why + " -- gave up on this row's route after " +
                std::to_string(kMaxReplans) + " tries; walking straight at the anchor";
    return;
  }
  Plan(c, R, foot);
  R.st.note = why;
}

void CheckCloseBehind(RefCtx& c, Resident& R, const Vec3& foot, const std::string& who) {
  const Ref* d = c.refs->Find(R.closeDoor);
  if (d == nullptr) {
    R.closeDoor.clear();
    return;
  }
  const DoorGeom g = DoorGeometry(*d);
  if (!g.ok) {
    R.closeDoor.clear();
    return;
  }
  float off = 0.0f;
  const int side = DoorSideOf(g, foot, &off);
  if (side != R.closeSide && off > 3.0f) R.closePassed = true;
  const bool waitingHere = R.st.phase == ResidentPhase::DoorWait && R.st.door == R.closeDoor;
  if (waitingHere) return;
  bool clear = false;
  if (R.closePassed) {
    // Clear of the swing: behind the plane by a pace, or -- on the side the
    // leaf swings toward -- further from the hinge than the leaf is long.
    const float hx = foot.x - g.hinge.x, hz = foot.z - g.hinge.z;
    clear = side > 0 ? std::sqrt(hx * hx + hz * hz) > (float)g.width + 4.0f : off > 4.0f;
  } else {
    // Opened it and never went through (the row changed): close it once well
    // away and it is no longer on the route.
    bool onRoute = false;
    for (size_t i = R.st.cursor; i < R.st.route.doorBefore.size(); i++)
      onRoute |= R.st.route.doorBefore[i] == R.closeDoor;
    clear = !onRoute && Planar(foot, g.Center()) > (float)g.width + 8.0f;
  }
  if (!clear) return;
  const DoorPhase ph = PhaseOfDoor(R.closeDoor);
  if (ph == DoorPhase::Open) {
    UseDoorAsNpc(c, *d, who);
    R.st.doorCloses++;
  }
  R.closeDoor.clear();
  R.closePassed = false;
}

void BeginDoorWait(RefCtx& c, Resident& R, const std::string& door, const Vec3& foot) {
  R.st.phase = ResidentPhase::DoorWait;
  R.st.door = door;
  R.doorAsked = false;
  R.doorAskTick = 0;
  R.doorOpenSeen = 0;
  R.doorFetchAt = 0;
  R.doorWaitSince = c.tick;
  const Ref* d = c.refs->Find(door);
  R.doorSide = 1;
  if (d != nullptr) {
    const DoorGeom g = DoorGeometry(*d);
    if (g.ok) R.doorSide = DoorSideOf(g, foot);
  }
}

// DoorWait. True = the door stands open (or is no obstacle): walk on.
bool DoorStep(RefCtx& c, Resident& R, const Vec3& foot, ai::Routine& rt, const std::string& who) {
  const Ref* d = c.refs->Find(R.st.door);
  if (d == nullptr || !c.refs->IsActive(R.st.door)) return true;
  const DoorGeom g = DoorGeometry(*d);
  if (!g.ok) return true;
  DoorStatus ds;
  const DoorPhase ph = PhaseOfDoor(R.st.door, &ds);
  if (ph == DoorPhase::Broken) return true;
  if (ph == DoorPhase::Closed && g.locked) {
    Replan(c, R, foot, "door " + R.st.door + " is locked");
    return false;
  }
  if (c.tick - R.doorWaitSince > kDoorTimeoutTicks) {
    Replan(c, R, foot, "door " + R.st.door + " would not open (" + DoorPhaseName(ph) + ")");
    return false;
  }
  if (ph == DoorPhase::Closed && (!R.doorAsked || c.tick - R.doorAskTick > 60)) {
    UseDoorAsNpc(c, *d, who);
    if (!R.doorAsked) R.st.doorOpens++;
    R.doorAsked = true;
    R.doorAskTick = c.tick;
    R.closeDoor = R.st.door;
    R.closeSide = R.doorSide;
    R.closePassed = false;
  } else if ((ph == DoorPhase::Closing || ph == DoorPhase::Settling) &&
             c.tick - R.doorAskTick > 20) {
    // Somebody is shutting it in our face: open it again.
    UseDoorAsNpc(c, *d, who);
    R.doorAsked = true;
    R.doorAskTick = c.tick;
    if (R.closeDoor.empty()) {
      R.closeDoor = R.st.door;
      R.closeSide = R.doorSide;
      R.closePassed = false;
    }
  }
  bool ready = false;
  if (ph == DoorPhase::Open) {
    if (R.doorOpenSeen == 0) R.doorOpenSeen = c.tick;
    // The leaf's cells left the grid by ops; the fetch cache the body's
    // probes read must have caught up before the doorway reads as open.
    const bool fresh = DoorCacheFresh(c.world, g, R.doorOpenSeen, c.tick, R.doorFetchAt);
    ready = fresh && std::fabs(ds.angle) >= 0.7f * g.openRad;
  }
  if (ready) return true;
  // Stand clear of the swing and face the doorway.
  rt.goal = DoorWaitPoint(g, R.doorSide, foot.y);
  rt.final = true;
  rt.arriveRadius = 2.5f;
  rt.facePointSet = true;
  rt.facePoint = g.Center();
  return false;
}

void StepResident(RefCtx& c, const Ref& r) {
  MobSystem& mobs = *c.mobs;
  Mob* m = mobs.FindMobByRef(r.id);
  ResidentsWorld& w = W();
  if (m == nullptr || !m->Alive() || m->Def() == nullptr) {
    auto it = w.npcs.find(r.id);
    if (it != w.npcs.end()) {
      it->second.st.phase = ResidentPhase::None;
      it->second.st.mobId = 0;
      it->second.mobId = 0;
    }
    return;
  }
  ai::Brain* br = mobs.MobBrainMut(m->Id());
  if (br == nullptr) return;
  Resident& R = w.npcs[r.id];
  if (R.mobId != m->Id()) {
    R = Resident{};
    R.mobId = m->Id();
  }
  R.st.refId = r.id;
  R.st.mobId = m->Id();
  const Vec3 ws = m->Def()->worldSize;
  const Vec3 o = m->Origin();
  const Vec3 foot{o.x + ws.x * 0.5f, o.y, o.z + ws.z * 0.5f};
  R.st.foot = foot;
  ai::Routine& rt = br->routine;
  const std::string who = r.id;

  // A door opened on the way is closed behind, whatever else happens.
  if (!R.closeDoor.empty()) CheckCloseBehind(c, R, foot, who);

  const std::string schedName = PropStr(r, "schedule");
  R.st.schedule = schedName;
  const schedule::Schedule* sc = Schedules().Find(schedName);
  if (sc == nullptr) {
    rt.active = false;
    m->SetActivity("");
    R.st.phase = ResidentPhase::None;
    R.st.activity.clear();
    R.st.note = schedName.empty() ? "no props.schedule" : "no schedule named '" + schedName + "'";
    return;
  }
  const int minute = schedule::MinuteNow(c.tick);
  R.st.minute = minute;
  const int row = sc->RowAt(minute);
  schedule::Row gap;
  gap.act = "goto";
  const schedule::Row& rw = row >= 0 ? sc->rows[(size_t)row] : gap;
  R.st.nextRow = sc->NextRow(minute, &R.st.nextFrom);
  const std::string key = sc->name + "|" + std::to_string(row) + "|" + rw.act + "|" + rw.at +
                          "|" + std::to_string(c.refs->Revision()) + "|" +
                          std::to_string(Schedules().Revision());
  if (key != R.rowKey) {
    R.rowKey = key;
    const bool sameRow = R.st.row == row && R.st.activity == rw.act;
    R.st.row = row;
    R.st.activity = rw.act;
    R.st.anchor = ResolveAnchor(*c.refs, r, rw.at);
    if (!sameRow) {
      R.st.rowSince = c.tick;
      R.st.arrivedTick = 0;
      R.bad.clear();
      R.replansThisRow = 0;
      R.wanderGoing = false;
      R.wanderUntil = 0;
    }
    R.st.note.clear();
    Plan(c, R, foot);
    m->SetActivity("");
  }

  const ai::Intent verb = ai::IntentForActivity(rw.act);
  rt.active = true;
  rt.verb = verb == ai::Intent::Count ? ai::Intent::Goto : verb;
  rt.speed = kWalkSpeed;
  rt.hold = false;
  rt.facePointSet = false;
  rt.faceHeadingSet = false;
  rt.final = false;
  rt.arriveRadius = kNodeRadius;
  rt.goal = foot;

  R.st.talking = false;
  // ---- a player is talking to it: hold, face them ----
  if (c.talkers != nullptr)
    for (const RefCtx::Talker& t : *c.talkers)
      if (t.mobId == m->Id()) {
        rt.hold = true;
        rt.facePointSet = true;
        rt.facePoint = t.eye;
        m->SetActivity("");
        R.bestTick = c.tick;
        R.st.talking = true;
        return;   // phase kept: the day resumes where it stood
      }
  // ---- the arbiter chose something else (a fight, a flight) ----
  const bool busy = (int)br->intent < (int)ai::Intent::Sleep &&
                    (br->hasTarget || br->intent == ai::Intent::Flee);
  R.st.busy = busy;
  if (busy) {
    R.bestTick = c.tick;   // no stuck clock while fighting or running
    m->SetActivity("");
  }

  const Anchor& A = R.st.anchor;
  const float radius = ArriveRadius(rw.act);

  if (R.st.phase == ResidentPhase::NoRoute) {
    if (A.ok) {
      rt.goal = A.foot;
      rt.final = true;
      rt.arriveRadius = radius;
      const float d = Planar(foot, A.foot);
      if (d <= radius) {
        R.st.phase = ResidentPhase::AtAnchor;
        if (R.st.arrivedTick == 0) {
          R.st.arrivedTick = c.tick;
          R.st.arrivals++;
        }
      } else if (!busy) {
        // GIVING UP IS NOT FOREVER (P8, Harrowby). Four failed legs in a row
        // are usually ANOTHER VILLAGER in a doorway or on a stair -- two of
        // the Cotters climbing to the loft at ten -- and walking straight at
        // the anchor from the foot of the stair gets nowhere all night. When
        // the straight walk stalls too, forget the bad legs and try the
        // waynodes again from where it now stands.
        if (d < R.bestDist - 1.0f) {
          R.bestDist = d;
          R.bestTick = c.tick;
        } else if (c.tick - R.bestTick > kStuckTicks) {
          R.bad.clear();
          R.replansThisRow = 0;
          Plan(c, R, foot);
          R.st.note = "walking straight got nowhere either: trying the waynodes again";
          return;
        }
      }
    } else {
      rt.hold = true;
    }
    return;
  }

  // ---- DoorWait ----
  if (R.st.phase == ResidentPhase::DoorWait) {
    if (!DoorStep(c, R, foot, rt, who)) return;
    if (R.st.phase == ResidentPhase::DoorWait) R.st.phase = ResidentPhase::Travel;
    R.bestDist = 1e9f;
    R.bestTick = c.tick;
  }

  // ---- Travel ----
  if (R.st.phase == ResidentPhase::Travel) {
    Route& route = R.st.route;
    while (R.st.cursor < route.pts.size()) {
      const bool last = R.st.cursor + 1 == route.pts.size();
      const Vec3 goal = route.pts[R.st.cursor];
      const float d = Planar(foot, goal);
      const bool reached = last ? (d <= radius || br->routine.arrived)
                                : (d <= kNodeRadius && std::fabs(goal.y - foot.y) <= 12.0f);
      if (!reached) break;
      R.st.cursor++;
      R.bestDist = 1e9f;
      R.bestTick = c.tick;
      if (last) break;
      if (!route.doorBefore[R.st.cursor].empty()) {
        BeginDoorWait(c, R, route.doorBefore[R.st.cursor], foot);
        if (!DoorStep(c, R, foot, rt, who)) return;
        R.st.phase = ResidentPhase::Travel;
      }
    }
    if (R.st.cursor >= route.pts.size()) {
      R.st.phase = ResidentPhase::AtAnchor;
      if (R.st.arrivedTick == 0) {
        R.st.arrivedTick = c.tick;
        R.st.arrivals++;
      }
    } else {
      const Vec3 goal = route.pts[R.st.cursor];
      const bool last = R.st.cursor + 1 == route.pts.size();
      rt.goal = goal;
      rt.final = last;
      rt.arriveRadius = last ? radius : kNodeRadius;
      if (last && A.haveHeading) {
        rt.faceHeadingSet = true;
        rt.faceHeading = A.heading;
      }
      const float d = Planar(foot, goal);
      if (!busy) {
        if (d < R.bestDist - 1.0f) {
          R.bestDist = d;
          R.bestTick = c.tick;
        } else if (c.tick - R.bestTick > kStuckTicks) {
          Replan(c, R, foot, "no progress toward " +
                                 (route.nodes[R.st.cursor].empty() ? std::string("the anchor")
                                                                    : route.nodes[R.st.cursor]));
        }
      }
      return;
    }
  }

  // ---- AtAnchor: the activity ----
  if (R.st.phase != ResidentPhase::AtAnchor) return;
  rt.goal = A.foot;
  rt.final = true;
  rt.arriveRadius = radius;
  if (A.haveHeading) {
    rt.faceHeadingSet = true;
    rt.faceHeading = A.heading;
  }
  const float fromAnchor = Planar(foot, A.foot);
  const float strayMax =
      rw.act == "wander" ? std::max(40.0f, rw.Radius() * 10.0f + 20.0f) : 24.0f;
  if (!busy && fromAnchor > strayMax) {
    // Knocked, chased or pushed well off its spot: walk back.
    m->SetActivity("");
    Plan(c, R, foot);
    return;
  }
  if (rw.act == "sleep") {
    if (!busy && fromAnchor <= radius + 2.0f) m->SetActivity("sleep");
    return;
  }
  m->SetActivity("");
  if (rw.act == "socialize") {
    // Face the nearest other villager in reach, else the anchor's yaw.
    float best = rw.Radius() * 10.0f + 30.0f;
    for (const auto& [oid, other] : w.npcs) {
      if (oid == r.id || other.mobId == 0) continue;
      const Mob* om = mobs.FindMobById(other.mobId);
      if (om == nullptr || !om->Alive() || om->Def() == nullptr) continue;
      const Vec3 os = om->Def()->worldSize, oo = om->Origin();
      const Vec3 oc{oo.x + os.x * 0.5f, oo.y + os.y * 0.8f, oo.z + os.z * 0.5f};
      const float d = Planar(foot, oc);
      if (d < best) {
        best = d;
        rt.facePointSet = true;
        rt.facePoint = oc;
      }
    }
    return;
  }
  if (rw.act == "wander") {
    rt.faceHeadingSet = false;
    if (!R.wanderGoing && c.tick >= R.wanderUntil) {
      const uint32_t h = rng::Hash3(RefHash(r.id), c.tick, R.wanderSeq++);
      const float ang = rng::Unit01(h) * 2.0f * kPi;
      const float rad = (0.3f + 0.7f * rng::Unit01(rng::Pcg(h))) * rw.Radius() * 10.0f;
      R.wanderGoal = Vec3{A.foot.x + std::sin(ang) * rad, A.foot.y, A.foot.z + std::cos(ang) * rad};
      R.wanderGoing = true;
      R.bestDist = 1e9f;
      R.bestTick = c.tick;
    }
    if (R.wanderGoing) {
      rt.goal = R.wanderGoal;
      rt.arriveRadius = 3.0f;
      const float d = Planar(foot, R.wanderGoal);
      if (d <= 3.0f || br->routine.arrived) {
        R.wanderGoing = false;
        R.wanderUntil = c.tick + 60 + rng::Hash3(RefHash(r.id), c.tick, 77) % 120;
      } else if (d < R.bestDist - 1.0f) {
        R.bestDist = d;
        R.bestTick = c.tick;
      } else if (!busy && c.tick - R.bestTick > 90) {
        R.wanderGoing = false;   // could not get there: pick another
        R.wanderUntil = c.tick + 30;
      }
    } else {
      rt.hold = true;
    }
  }
}

}  // namespace

// ---- public ------------------------------------------------------------------------

schedule::Library& Schedules() {
  ResidentsWorld& w = W();
  if (!w.loaded) {
    w.loaded = true;
    w.schedules.Load(sandvox::AssetDir() + "/schedules");
  }
  return w.schedules;
}

void ReloadSchedules() {
  W().loaded = true;
  W().schedules.Load(sandvox::AssetDir() + "/schedules");
}

int WayGraph::Find(const std::string& id) const {
  // nodes are in id order (built from RefStore::All)
  auto it = std::lower_bound(nodes.begin(), nodes.end(), id,
                             [](const Node& n, const std::string& k) { return n.id < k; });
  return it != nodes.end() && it->id == id ? (int)(it - nodes.begin()) : -1;
}

const WayGraph& Graph(const RefStore& s) {
  ResidentsWorld& w = W();
  if (w.graphFor != &s || w.graphRev != s.Revision()) {
    BuildGraph(s, w.graph);
    w.graphFor = &s;
    w.graphRev = s.Revision();
  }
  return w.graph;
}

Anchor ResolveAnchor(const RefStore& s, const Ref& npc, const std::string& atIn) {
  Anchor a;
  const std::string at = atIn.empty() ? std::string("home") : atIn;
  std::string target;
  if (npc.props.contains(at) && npc.props[at].is_string()) {
    target = npc.props[at].get<std::string>();
    a.how = "role " + at;
  } else if (at.find('/') != std::string::npos) {
    target = at;
    a.how = "ref";
  } else if (at == "home") {
    a.ok = true;
    a.how = "home (no props.home: the npc's own pos)";
    a.refId = npc.id;
    a.foot = FootOfRef(npc);
    a.haveHeading = true;
    a.heading = YawRad(npc.yaw);
    return a;
  } else {
    // A TAG: the nearest ref wearing it, to the villager's authored pos.
    const Ref* best = nullptr;
    int64_t bestD = std::numeric_limits<int64_t>::max();
    for (const auto& [id, r] : s.All()) {
      if (!HasTag(r, at)) continue;
      const int64_t dx = r.pos.x - npc.pos.x, dy = r.pos.y - npc.pos.y, dz = r.pos.z - npc.pos.z;
      const int64_t d = dx * dx + dy * dy + dz * dz;
      if (d < bestD) {
        bestD = d;
        best = &r;
      }
    }
    if (best == nullptr) {
      a.why = "'" + at + "' is not a prop on " + npc.id + ", not a ref id and no ref has it as a tag";
      return a;
    }
    target = best->id;
    a.how = "tag " + at;
  }
  const Ref* t = s.Find(target);
  if (t == nullptr) {
    a.why = a.how + ": no ref named \"" + target + "\"";
    return a;
  }
  AnchorOfRef(s, *t, a);
  return a;
}

Route PlanRoute(const RefStore& s, Vec3 from, Vec3 to, const std::set<std::pair<int, int>>* bad) {
  Route out;
  auto direct = [&](const std::string& note) {
    out = Route{};
    out.pts = {to};
    out.nodes = {""};
    out.doorBefore = {""};
    out.note = note;
    return out;
  };
  const WayGraph& g = Graph(s);
  if (g.nodes.empty()) return direct("");
  const int sn = NearestNode(g, from), en = NearestNode(g, to);
  if (sn < 0 || en < 0 || sn == en) return direct("");
  // Dijkstra, O(n^2): a village has tens of nodes. Ties to the lower index.
  const size_t n = g.nodes.size();
  std::vector<float> dist(n, 1e30f);
  std::vector<int> prev(n, -1);
  std::vector<char> done(n, 0);
  dist[(size_t)sn] = 0.0f;
  for (size_t it = 0; it < n; it++) {
    int u = -1;
    for (size_t i = 0; i < n; i++)
      if (!done[i] && dist[i] < 1e29f && (u < 0 || dist[i] < dist[(size_t)u])) u = (int)i;
    if (u < 0 || u == en) break;
    done[(size_t)u] = 1;
    for (int ei : g.adj[(size_t)u]) {
      const WayGraph::Edge& e = g.edges[(size_t)ei];
      if (bad != nullptr && bad->count({e.a, e.b})) continue;
      const int v = e.a == u ? e.b : e.a;
      const float nd = dist[(size_t)u] + e.cost;
      if (nd < dist[(size_t)v]) {
        dist[(size_t)v] = nd;
        prev[(size_t)v] = u;
      }
    }
  }
  if (prev[(size_t)en] < 0)
    return direct("no waynode path from " + g.nodes[(size_t)sn].id + " to " +
                  g.nodes[(size_t)en].id + ": walking straight");
  std::vector<int> path;
  for (int v = en; v >= 0; v = prev[(size_t)v]) path.push_back(v);
  std::reverse(path.begin(), path.end());
  auto doorOf = [&](int a, int b) {
    const WayGraph::Edge* e = EdgeBetween(g, a, b);
    return e != nullptr ? e->door : std::string();
  };
  // Skip the first node when we already stand past it toward the second
  // (and the leg between them is not a door); the same for the last node
  // toward the anchor.
  if (path.size() >= 2 && doorOf(path[0], path[1]).empty() &&
      Planar(from, g.nodes[(size_t)path[1]].p) <
          Planar(g.nodes[(size_t)path[0]].p, g.nodes[(size_t)path[1]].p))
    path.erase(path.begin());
  if (path.size() >= 2) {
    const size_t k = path.size() - 1;
    if (doorOf(path[k - 1], path[k]).empty() &&
        Planar(to, g.nodes[(size_t)path[k - 1]].p) <
            Planar(g.nodes[(size_t)path[k]].p, g.nodes[(size_t)path[k - 1]].p))
      path.pop_back();
  }
  out.viaGraph = true;
  for (size_t i = 0; i < path.size(); i++) {
    out.pts.push_back(g.nodes[(size_t)path[i]].p);
    out.nodes.push_back(g.nodes[(size_t)path[i]].id);
    out.doorBefore.push_back(i == 0 ? std::string() : doorOf(path[i - 1], path[i]));
  }
  out.pts.push_back(to);
  out.nodes.push_back("");
  out.doorBefore.push_back("");
  return out;
}

const char* ResidentPhaseName(ResidentPhase p) {
  switch (p) {
    case ResidentPhase::None: return "none";
    case ResidentPhase::Travel: return "travel";
    case ResidentPhase::DoorWait: return "door";
    case ResidentPhase::AtAnchor: return "at anchor";
    case ResidentPhase::NoRoute: return "no route";
  }
  return "?";
}

bool ResidentStatusOf(const std::string& refId, ResidentStatus& out) {
  auto it = W().npcs.find(refId);
  if (it == W().npcs.end()) return false;
  out = it->second.st;
  return true;
}

std::vector<std::string> Residents() {
  std::vector<std::string> v;
  for (const auto& [id, r] : W().npcs) v.push_back(id);
  return v;
}

std::string ResidentActivity(const std::string& refId) {
  auto it = W().npcs.find(refId);
  if (it == W().npcs.end() || it->second.mobId == 0) return std::string();
  return it->second.st.activity;
}

bool ScheduledPlace(const RefStore& s, const Ref& npc, uint32_t tick, Vec3& foot,
                    float& heading) {
  const schedule::Schedule* sc = Schedules().Find(PropStr(npc, "schedule"));
  if (sc == nullptr) return false;
  const int row = sc->RowAt(schedule::MinuteNow(tick));
  const Anchor a = ResolveAnchor(s, npc, row >= 0 ? sc->rows[(size_t)row].at : std::string());
  if (!a.ok) return false;
  foot = a.foot;
  heading = a.haveHeading ? a.heading : YawRad(npc.yaw);
  return true;
}

void ResetResidents() {
  W().npcs.clear();
  W().askedAt.clear();
}

// ---- the kinds -------------------------------------------------------------------------

void RegisterResidentKinds() {
  AddStoreGoneHook(&StoreGone);

  // ---- waynode ----
  RefKind wn;
  wn.name = "waynode";
  wn.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (r.props.contains("links")) {
      const Json& l = r.props["links"];
      bool ok = l.is_array();
      if (ok)
        for (const Json& v : l) ok &= v.is_string();
      if (!ok)
        p.push_back("props.links: a list of waynode ids (or sibling slot names), e.g. "
                    "[\"harrowby/smithy/waynode_front_0_out\"]");
    }
    if (r.props.contains("autoLink") &&
        (!r.props["autoLink"].is_number() || r.props["autoLink"].get<float>() < 0.0f))
      p.push_back("props.autoLink: metres (0 = off)");
  };
  Kinds().Register(std::move(wn));

  // ---- npc (replaces P1's temporary one) ----
  RefKind npc;
  npc.name = "npc";
  npc.validate = [](const Ref& r, std::vector<std::string>& p) {
    for (const char* k : {"name", "schedule", "dialogue", "home", "bed", "work", "weapon",
                          "behavior"})
      if (r.props.contains(k) && !r.props[k].is_string())
        p.push_back(std::string("props.") + k + ": must be text");
    if (r.props.contains("outfit")) {
      const Json& o = r.props["outfit"];
      bool ok = o.is_array();
      if (ok)
        for (const Json& v : o) ok &= v.is_string();
      if (!ok)
        p.push_back("props.outfit: a list of item names, e.g. [\"tunic#B4472A\", \"breeches\"]");
    }
    // An unknown schedule NAME is not a load problem (schedules are content
    // that may be written after the ref): the References page's npc panel
    // says "no schedule named ..." and the villager stands at its pos.
  };
  npc.activate = [](RefCtx& c, const Ref& r) -> Activation {
    if (c.mobs == nullptr || c.refs == nullptr) return Activation::Done;   // store-only
    MobSystem& mobs = *c.mobs;
    ResidentsWorld& w = W();
    w.store = c.refs;
    w.lastTick = c.tick;
    if (!mobs.HasUnparkPlacer())
      mobs.SetUnparkPlacer([](const std::string& refId, Vec3& foot, float& heading) {
        RefStore* s = W().store;
        if (s == nullptr) return false;
        const Ref* nr = s->Find(refId);
        if (nr == nullptr || nr->kind != "npc") return false;
        if (!ScheduledPlace(*s, *nr, W().lastTick, foot, heading)) return false;
        W().npcs.erase(refId);   // re-plan from where it now stands
        return true;
      });
    // Already standing (a load or an unpark brought it back): nothing to do.
    if (mobs.FindMobByRef(r.id) != nullptr) {
      w.askedAt.erase(r.id);
      return Activation::Done;
    }
    // Spawned before and not standing: parked (the unpark brings it back, at
    // its schedule's place), saved in a bucket, or dead. The world owns it.
    if (const RefDelta* d = c.refs->Delta(r.id); d != nullptr && d->bytes.size() >= 4) {
      ByteReader rd{d->bytes.data(), d->bytes.size()};
      uint32_t state = 0;
      rd.U32(state);
      if (state == kNpcSpawned) {
        w.askedAt.erase(r.id);
        return Activation::Done;
      }
    }
    const int def = mobs.FindDef(NpcDefName(r));
    if (def < 0) {
      c.refs->Warn("refs: " + r.id + ": base: no mob def named \"" + NpcDefName(r) +
                   "\" (assets/mobs); the npc is not spawned");
      return Activation::Done;
    }
    // CATCH-UP: first appearance at the place its day says, not where it was
    // authored (which is only the fallback).
    Vec3 foot = FootOfRef(r);
    float heading = YawRad(r.yaw);
    ScheduledPlace(*c.refs, r, c.tick, foot, heading);
    const IVec3 cell{IFloor(foot.x), IFloor(foot.y), IFloor(foot.z)};
    if (c.world != nullptr && !GroundKnown(*c.world, r.id, cell, c.tick)) return Activation::Retry;
    if (!mobs.HasRoomToSpawn()) return Activation::Retry;
    const Vec3 ws = mobs.Defs()[(size_t)def].worldSize;
    const IVec3 origin{IFloor(foot.x - ws.x * 0.5f), IFloor(foot.y), IFloor(foot.z - ws.z * 0.5f)};
    const uint64_t id = mobs.Spawn(def, origin);
    if (id == 0) return Activation::Retry;
    w.askedAt.erase(r.id);
    if (Mob* m = mobs.FindMobById(id)) m->SetRefId(r.id);
    mobs.SetHeading(id, heading);
    // Character: the behaviour profile (a villager unless the ref says).
    std::string beh = PropStr(r, "behavior");
    if (beh.empty()) beh = "villager";
    if (!mobs.SetMobBehavior(id, beh))
      c.refs->Warn("refs: " + r.id + ": props.behavior: no behaviour profile \"" + beh +
                   "\" in assets/mobs/behaviors.json");
    // Dress and arm it through the player's own paths (Mob::WearItem /
    // EquipItem), so its kit says what it wears.
    if (c.items != nullptr) {
      if (r.props.contains("outfit") && r.props["outfit"].is_array())
        for (const Json& e : r.props["outfit"]) {
          if (!e.is_string()) continue;
          std::string nm = e.get<std::string>();
          uint32_t dye = 0;
          if (const size_t hash = nm.find('#'); hash != std::string::npos) {
            dye = DyeParseHex(nm.substr(hash));
            nm = nm.substr(0, hash);
          }
          const ItemDef* it = c.items->Named(nm);
          const int slot = it != nullptr && ItemKindIsWorn(it->kind) ? EquipSlotFor(it->kind, Equipment{}) : -1;
          if (it == nullptr || slot < 0 || !mobs.WearItem(id, it, slot, dye))
            c.refs->Warn("refs: " + r.id + ": props.outfit: \"" + nm +
                         "\" is not a wearable item (or would not go on)");
        }
      const std::string weapon = PropStr(r, "weapon");
      if (!weapon.empty()) {
        const ItemDef* it = c.items->Named(weapon);
        if (it == nullptr || !mobs.EquipItem(id, it))
          c.refs->Warn("refs: " + r.id + ": props.weapon: no item \"" + weapon + "\" to hold");
      }
    }
    std::vector<uint8_t> b;
    ByteWriter wr{b};
    wr.U32(kNpcSpawned);
    c.refs->SetDelta(r.id, "npc", 1, std::move(b));
    return Activation::Done;
  };
  npc.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    W().askedAt.erase(r.id);
    W().npcs.erase(r.id);
    if (why != RefEvent::Edited && why != RefEvent::Deleted) return;
    // The author changed or removed the villager: take the old body away so
    // the new line can spawn fresh (Deleted: nothing comes back).
    if (c.mobs != nullptr) {
      if (Mob* m = c.mobs->FindMobByRef(r.id)) c.mobs->RemoveMob(m->Id());
    }
    if (c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  npc.usePoint = [](RefCtx& c, const Ref& r, Vec3& at) {
    if (c.mobs == nullptr) return false;
    const Mob* m = c.mobs->FindMobByRef(r.id);
    if (m == nullptr || !m->Alive() || m->Def() == nullptr) return false;
    const Vec3 o = m->Origin();
    const Vec3 s = m->Def()->worldSize;
    at = Vec3{o.x + s.x * 0.5f, o.y + s.y * 0.6f, o.z + s.z * 0.5f};
    return true;
  };
  npc.usePrompt = [](RefCtx&, const Ref& r) { return "Talk to " + NpcName(r); };
  npc.onUse = [](RefCtx& c, const Ref& r, RefUse& u) {
    const std::string dlg = PropStr(r, "dialogue");
    const Mob* m = c.mobs != nullptr ? c.mobs->FindMobByRef(r.id) : nullptr;
    if (m == nullptr || !m->Alive()) {
      u.message = NpcName(r) + " does not answer.";
      return;
    }
    if (dlg.empty()) {
      u.message = NpcName(r) + " nods, and has nothing to say.";
      return;
    }
    if (c.talk == nullptr || u.session == nullptr) {
      u.message = NpcName(r) + " would talk, but no conversations are loaded here.";
      return;
    }
    dialogue::Speaker spk;
    spk.mobId = m->Id();
    spk.refId = r.id;
    spk.name = NpcName(r);
    std::string why;
    if (!dialogue::Begin(*c.talk, *u.session, spk, dlg, c.tick, &why))
      u.message = NpcName(r) + ": " + why;
  };
  npc.useRadius = 7.0f;
  // THE DAY: every active villager with a body, in id order. Costs a map
  // lookup and a few distances per villager per tick; nothing when none.
  npc.tick = [](RefCtx& c) {
    if (c.refs == nullptr || c.mobs == nullptr) return;
    ResidentsWorld& w = W();
    w.store = c.refs;
    w.lastTick = c.tick;
    for (const auto& [id, r] : c.refs->All()) {
      if (r.kind != "npc" || !c.refs->IsActive(id)) continue;
      StepResident(c, r);
    }
  };
  Kinds().Register(std::move(npc));
}

}  // namespace refs
