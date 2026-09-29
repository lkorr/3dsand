// refs_doors.cpp — see refs_doors.h.

#include "world/refs_doors.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "game/equipment.h"
#include "game/item.h"
#include "game/iteminstance.h"
#include "game/session.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/bytestream.h"
#include "sim/materials.h"
#include "sim/world.h"

namespace refs {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kTickHz = 30;            // the fixed sim rate (support.h kTickDt)
constexpr uint32_t kDoorOpen = 1, kDoorBroken = 2;
constexpr uint32_t kRefetchTicks = 15;      // re-ask the cache this often while waiting
// "Home": the hinge within ~3.4 degrees of shut, turning slower than ~17 deg/s.
constexpr float kHomeRad = 0.06f;
constexpr float kHomeSpin = 0.3f;
// Motor torque per kg of leaf, N*m: a 100 kg oak leaf gets 2500 N*m, which
// swings it through 95 degrees in about a third of a second and still stops
// dead against a man standing in the doorway (his capsule is kinematic).
constexpr float kMotorPerKg = 25.0f;
// Only these bits of a word are the voxel's durable state (world.h layout):
// the tick stamp and excite scratch (16..23) are the sim's, bit 31 the op flag.
constexpr uint32_t kDurableBits = 0x7F00FFFFu;

int FloorDiv(int v, int d) { return (v >= 0) ? v / d : -((-v + d - 1) / d); }
IVec3 ChunkOfCell(IVec3 c) { return {FloorDiv(c.x, 16), FloorDiv(c.y, 16), FloorDiv(c.z, 16)}; }

// Heading unit vector of a whole-90 yaw (0 = +Z, 90 = +X).
IVec3 HeadingAxis(int yaw) {
  switch (((yaw % 360) + 360) % 360) {
    case 0: return {0, 0, 1};
    case 90: return {1, 0, 0};
    case 180: return {0, 0, -1};
    default: return {-1, 0, 0};
  }
}

// Rotate (x, z) about +Y by `rad`, right-handed (+Z turns toward +X).
Vec3 RotY(Vec3 v, float rad) {
  const float c = std::cos(rad), s = std::sin(rad);
  return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

int PropInt(const Ref& r, const char* k, int def) {
  if (r.props.contains(k) && r.props[k].is_number()) return r.props[k].get<int>();
  return def;
}
float PropFloat(const Ref& r, const char* k, float def) {
  if (r.props.contains(k) && r.props[k].is_number()) return r.props[k].get<float>();
  return def;
}

// ---- the live doors ---------------------------------------------------------------
struct DoorLive {
  DoorPhase phase = DoorPhase::Closed;
  uint32_t reqTick = 0;    // Opening/Settling: the cache must be at least this new
  uint32_t fetchAt = 0;    // the last fetch request
  uint32_t openedAt = 0;   // for autoClose
  uint32_t blocked = 0;    // Settling: ticks refused
  std::vector<std::pair<IVec3, uint32_t>> cells;   // the captured leaf: cell, word
  uint64_t body = 0, joint = 0;
  float target = 0.0f;
  float angle = 0.0f;      // the hinge angle last read (radians)
  std::string note;
};
struct DoorWorld {
  std::map<std::string, DoorLive> live;   // doors in any phase but a quiet Closed
  std::map<std::string, DoorStatus> last; // what a door that went quiet ended as
  // Write-backs asked for outside a tick (R's reload deleting an open door):
  // pushed into the next tick's ops by the kind's tick.
  std::vector<CellOp> pending;
};
DoorWorld& Doors() {
  static DoorWorld d;
  return d;
}

// Every chunk of the door's box, fetch-cached no older than `since`? Asks for
// the ones that are not (at most every kRefetchTicks, from `fetchAt`).
bool CacheFresh(World& world, const DoorGeom& g, uint32_t since, uint32_t tick,
                uint32_t& fetchAt) {
  bool fresh = true;
  const IVec3 lo = ChunkOfCell(g.lo), hi = ChunkOfCell(g.hi);
  const bool ask = fetchAt == 0 || tick - fetchAt >= kRefetchTicks;
  for (int z = lo.z; z <= hi.z; z++)
    for (int y = lo.y; y <= hi.y; y++)
      for (int x = lo.x; x <= hi.x; x++) {
        const CachedChunk* cc = world.Cached({x, y, z});
        if (cc != nullptr && cc->voxels.size() == kChunkVol && cc->version >= since) continue;
        fresh = false;
        if (ask) world.RequestChunkFetch({x, y, z});
      }
  if (!fresh && ask) fetchAt = tick;
  return fresh;
}

uint32_t CachedWord(World& world, IVec3 c) {
  const CachedChunk* cc = world.Cached(ChunkOfCell(c));
  if (cc == nullptr || cc->voxels.size() != kChunkVol) return 0;
  return cc->voxels[(size_t)(((c.z & 15) * 16 + (c.y & 15)) * 16 + (c.x & 15))];
}

uint32_t ClassOf(const RefCtx& c, uint32_t mat) {
  if (c.mats == nullptr || mat >= c.mats->size()) return CLASS_SOLID;
  return (*c.mats)[mat].gpu.klass;
}

void EncodeOpen(const DoorLive& L, std::vector<uint8_t>& b) {
  ByteWriter w{b};
  w.U32(kDoorOpen);
  w.U32((uint32_t)L.cells.size());
  for (const auto& [c, word] : L.cells) {
    w.Pod(c);
    w.U32(word);
  }
}

void SetOpenDelta(RefCtx& c, const Ref& r, const DoorLive& L) {
  std::vector<uint8_t> b;
  EncodeOpen(L, b);
  c.refs->SetDelta(r.id, "door", 1, std::move(b));
}

// The saved state: 0 = closed/none, kDoorOpen (+cells), kDoorBroken.
uint32_t DecodeDelta(RefStore* s, const std::string& id,
                     std::vector<std::pair<IVec3, uint32_t>>* cells) {
  if (s == nullptr) return 0;
  const RefDelta* d = s->Delta(id);
  if (d == nullptr || d->bytes.size() < 4) return 0;
  ByteReader rd{d->bytes.data(), d->bytes.size()};
  uint32_t state = 0;
  rd.U32(state);
  if (state == kDoorOpen && cells != nullptr) {
    uint32_t n = 0;
    rd.U32(n);
    cells->clear();
    for (uint32_t i = 0; i < n && rd.ok; i++) {
      IVec3 cell{};
      uint32_t word = 0;
      rd.Pod(cell);
      rd.U32(word);
      if (rd.ok) cells->push_back({cell, word});
    }
    if (!rd.ok) return 0;
  }
  return state;
}

// HANG THE LEAF: the captured cells become one body at the closed pose, on a
// motored hinge to the world; then (a re-activation of an open door) turned
// to `startRad`. False when physics refused it.
bool HangLeaf(RefCtx& c, const DoorGeom& g, DoorLive& L, float startRad) {
  if (c.phys == nullptr || c.debris == nullptr || L.cells.empty()) return false;
  IVec3 mn{INT32_MAX, INT32_MAX, INT32_MAX};
  for (const auto& [cell, word] : L.cells) {
    mn.x = std::min(mn.x, cell.x);
    mn.y = std::min(mn.y, cell.y);
    mn.z = std::min(mn.z, cell.z);
  }
  std::vector<DebrisVoxel> vox;
  vox.reserve(L.cells.size());
  for (const auto& [cell, word] : L.cells) {
    DebrisVoxel v{};
    v.x = (int8_t)(cell.x - mn.x);
    v.y = (int8_t)(cell.y - mn.y);
    v.z = (int8_t)(cell.z - mn.z);
    v.color = 0;
    v.payload = (uint16_t)(word & 0xFFFFu);
    v.stain = 0;
    vox.push_back(v);
  }
  Physics& ph = *c.phys;
  const uint64_t h = ph.CreateDebrisBody(vox, mn, c.debris->DensityOf());
  if (h == 0) return false;
  ph.SetBodyRole(h, Physics::BodyRole::Door);
  BodyTransform xf{};
  xf.pos = Vec3{(float)mn.x, (float)mn.y, (float)mn.z};
  xf.quat[3] = 1.0f;
  c.debris->AdoptBody(h, vox, xf);
  c.debris->SetBodyFixture(h, true);

  Physics::JointDesc d;
  d.type = Physics::JointType::Hinge;
  d.anchorVoxel = Vec3{g.hinge.x, g.hinge.y + 0.5f * (float)g.height, g.hinge.z};
  d.axis = Vec3{0, 1, 0};
  d.minAngle = std::min(0.0f, L.target) - 0.03f;
  d.maxAngle = std::max(0.0f, L.target) + 0.03f;
  d.friction = 0.0f;
  d.motorTorque = std::max(50.0f, kMotorPerKg * ph.BodyMass(h));
  d.motorFreq = 2.0f;
  const uint64_t j = ph.CreateJoint(0, h, d);
  if (j == 0) {
    c.debris->DestroyBody(h);
    return false;
  }
  if (startRad != 0.0f) {
    BodyTransform now{};
    if (ph.GetTransform(h, now)) {
      const Vec3 hingeAt{g.hinge.x, now.pos.y, g.hinge.z};
      const Vec3 p = hingeAt + RotY(now.pos - hingeAt, startRad);
      const float q[4] = {0.0f, std::sin(0.5f * startRad), 0.0f, std::cos(0.5f * startRad)};
      ph.SetBodyTransform(h, p, q);
    }
  }
  L.body = h;
  L.joint = j;
  return true;
}

// The leaf is no longer what was hung: drop the hinge and let the pieces be
// debris. The door says so from now on (delta Broken).
void Break(RefCtx& c, const Ref& r, DoorLive& L, const std::string& why) {
  if (c.phys != nullptr && L.joint != 0) c.phys->DestroyJoint(L.joint);
  if (c.debris != nullptr && L.body != 0 && c.debris->HasBody(L.body)) {
    c.debris->SetBodyFixture(L.body, false);
    if (c.phys != nullptr) c.phys->SetBodyRole(L.body, Physics::BodyRole::Debris);
  }
  L.body = L.joint = 0;
  L.cells.clear();
  L.phase = DoorPhase::Broken;
  L.note = why;
  if (c.refs != nullptr) {
    std::vector<uint8_t> b;
    ByteWriter w{b};
    w.U32(kDoorBroken);
    c.refs->SetDelta(r.id, "door", 1, std::move(b));
    c.refs->Warn("refs: " + r.id + ": " + why + "; it is not a door any more");
  }
}

// The body we hung, or its successor if damage REPLACED the handle and the
// replacement is still our fixture. 0 = gone.
uint64_t LiveBody(RefCtx& c, DoorLive& L) {
  if (c.debris == nullptr || L.body == 0) return 0;
  if (c.debris->HasBody(L.body)) return c.debris->IsFixture(L.body) ? L.body : 0;
  uint64_t h = L.body;
  for (int i = 0; i < 4 && c.phys != nullptr; i++) {
    h = c.phys->Successor(h);
    if (h == 0) return 0;
    if (c.debris->HasBody(h)) return c.debris->IsFixture(h) ? h : 0;
  }
  return 0;
}

// Does what the body holds still match what was hung? Same voxel count and
// the same material histogram (a burn rewrites payloads; a cut removes some).
bool LeafIntact(RefCtx& c, const DoorLive& L) {
  const std::vector<DebrisVoxel>* v = c.debris ? c.debris->BodyVoxelsOf(L.body) : nullptr;
  if (v == nullptr || v->size() != L.cells.size()) return false;
  std::map<uint32_t, int> hist;
  for (const auto& [cell, word] : L.cells) hist[word & 0xFFFu]++;
  for (const DebrisVoxel& d : *v) hist[(uint32_t)d.payload & 0xFFFu]--;
  for (const auto& [m, n] : hist)
    if (n != 0) return false;
  return true;
}

// WRITE THE LEAF BACK, unchecked except by kCellOpIfAir: an edit/delete
// undoing an open door (the careful close is TickDoor's Settling).
void WriteBack(RefCtx& c, const DoorLive& L) {
  if (c.world == nullptr) return;
  std::vector<CellOp>& out = c.ops != nullptr ? c.ops->cells : Doors().pending;
  for (const auto& [cell, word] : L.cells) {
    if (!c.world->CellInWindow(cell)) continue;
    out.push_back({World::SlotCellIndex(cell), (word & kDurableBits) | kCellOpIfAir});
  }
}

void StartOpen(RefCtx& c, const Ref& r, DoorLive& L, const DoorGeom& g) {
  L.phase = DoorPhase::Opening;
  L.reqTick = c.tick;
  L.fetchAt = 0;
  L.target = g.openSign * g.openRad;
  L.note = "opening";
  if (c.world != nullptr) CacheFresh(*c.world, g, L.reqTick, c.tick, L.fetchAt);
  (void)r;
}

void StartClose(RefCtx& c, DoorLive& L) {
  L.phase = DoorPhase::Closing;
  L.note = "closing";
  if (c.phys != nullptr && L.joint != 0) c.phys->SetJointMotorTarget(L.joint, 0.0f);
}

// ---- one door, one tick ---------------------------------------------------------
void TickDoor(RefCtx& c, const Ref& r, DoorLive& L) {
  const DoorGeom g = DoorGeometry(r);
  if (!g.ok) return;
  if (c.world == nullptr || c.phys == nullptr || c.debris == nullptr) return;
  World& world = *c.world;

  if (L.phase == DoorPhase::Opening) {
    for (const IVec3& cell : g.Cells())
      if (!world.CellInWindow(cell)) {
        L.note = "waiting: the leaf is not all inside the window";
        return;
      }
    if (!CacheFresh(world, g, L.reqTick, c.tick, L.fetchAt)) return;
    // CAPTURE: every SOLID cell of the box is the leaf, word for word.
    L.cells.clear();
    bool hingeCol = false;
    const std::vector<IVec3> box = g.Cells();
    for (size_t k = 0; k < box.size(); k++) {
      const uint32_t w = CachedWord(world, box[k]);
      const uint32_t mat = w & 0xFFFu;
      if (mat == 0 || ClassOf(c, mat) != CLASS_SOLID) continue;
      L.cells.push_back({box[k], w & kDurableBits});
      // The HINGE COLUMN (i == 0) must hold something to hang.
      const IVec3 d{box[k].x - r.pos.x, 0, box[k].z - r.pos.z};
      const int i = d.x * g.along.x + d.z * g.along.z;
      if (i == 0) hingeCol = true;
    }
    if (L.cells.empty() || !hingeCol) {
      Break(c, r, L, L.cells.empty() ? "the doorway is empty (no leaf in its box)"
                                     : "nothing of the leaf is left on the hinge side");
      return;
    }
    if (!HangLeaf(c, g, L, 0.0f)) {
      L.note = "waiting: physics refused the leaf body";
      L.cells.clear();
      L.reqTick = c.tick;
      return;
    }
    // Out of the grid, conditionally: a cell that changed material since the
    // capture (fire, a chisel) keeps what is there now.
    if (c.ops != nullptr)
      for (const auto& [cell, word] : L.cells)
        c.ops->cells.push_back({World::SlotCellIndex(cell), CellOpClearIfMat(word & 0xFFFu)});
    c.phys->SetJointMotorTarget(L.joint, L.target);
    L.phase = DoorPhase::Open;
    L.openedAt = c.tick;
    L.note = "open";
    SetOpenDelta(c, r, L);
    return;
  }

  // Open / Closing / Settling: the body must still be ours and whole.
  const uint64_t body = LiveBody(c, L);
  if (body == 0) {
    L.body = 0;
    Break(c, r, L, "the open leaf was destroyed");
    return;
  }
  L.body = body;
  if (!c.phys->JointAlive(L.joint)) {
    Break(c, r, L, "the leaf came off its hinge");
    return;
  }

  c.phys->JointHingeAngle(L.joint, L.angle);
  if (L.phase == DoorPhase::Open) {
    if (g.autoCloseSec > 0.0f &&
        c.tick - L.openedAt >= (uint32_t)std::lround(g.autoCloseSec * (float)kTickHz))
      StartClose(c, L);
    return;
  }

  float ang = 0.0f;
  c.phys->JointHingeAngle(L.joint, ang);
  Vec3 lin{}, spin{};
  c.phys->GetBodyVelocities(L.body, lin, spin);
  const bool home = std::fabs(ang) < kHomeRad && std::fabs(spin.y) < kHomeSpin;

  if (L.phase == DoorPhase::Closing) {
    if (!home) {
      L.note = "closing";
      return;
    }
    L.phase = DoorPhase::Settling;
    L.reqTick = c.tick;
    L.fetchAt = 0;
    L.blocked = 0;
  }
  // SETTLING: home. The write-back needs the box clear in the grid.
  if (!home) {
    L.phase = DoorPhase::Closing;
    return;
  }
  if (!CacheFresh(world, g, L.reqTick, c.tick, L.fetchAt)) return;
  if (!LeafIntact(c, L)) {
    Break(c, r, L, "the leaf burned or was cut while it stood open");
    return;
  }
  std::vector<CellOp> ops;
  ops.reserve(L.cells.size());
  std::string blockedBy;
  for (const auto& [cell, word] : L.cells) {
    if (!world.CellInWindow(cell)) {
      blockedBy = "the doorway left the window";
      break;
    }
    const uint32_t now = CachedWord(world, cell);
    const uint32_t mat = now & 0xFFFu;
    if (mat == 0) {
      ops.push_back({World::SlotCellIndex(cell), word | kCellOpIfAir});
    } else if (ClassOf(c, mat) == CLASS_GAS) {
      ops.push_back({World::SlotCellIndex(cell), word});   // smoke gives way
    } else {
      char buf[160];
      const CachedChunk* cc = world.Cached(ChunkOfCell(cell));
      std::snprintf(buf, sizeof buf,
                    "something is in the doorway at %d %d %d (word %08x, read at tick %u "
                    "off a copy of tick %u)",
                    cell.x, cell.y, cell.z, now, c.tick, cc ? cc->version : 0u);
      blockedBy = buf;
      break;
    }
  }
  if (!blockedBy.empty()) {
    // WAIT AND RETRY: look again when the cache next refreshes.
    L.blocked++;
    L.note = "waiting: " + blockedBy;
    if (c.tick - L.reqTick >= kRefetchTicks) {
      L.reqTick = c.tick;
      L.fetchAt = 0;
    }
    return;
  }
  // HOME: the body goes, the exact words come back.
  if (c.ops != nullptr) c.ops->cells.insert(c.ops->cells.end(), ops.begin(), ops.end());
  c.debris->DestroyBody(L.body);   // the hinge dies with it
  L.body = L.joint = 0;
  L.cells.clear();
  L.phase = DoorPhase::Closed;
  L.note = "closed";
  if (c.refs != nullptr) c.refs->ClearDelta(r.id);
}

DoorStatus StatusOf(RefCtx* c, const DoorLive& L) {
  DoorStatus s;
  s.phase = L.phase;
  s.body = L.body;
  s.joint = L.joint;
  s.leafCells = (uint32_t)L.cells.size();
  s.blockedTicks = L.blocked;
  s.note = L.note;
  s.angle = L.angle;
  if (c != nullptr && c->phys != nullptr && L.joint != 0) c->phys->JointHingeAngle(L.joint, s.angle);
  return s;
}

// ---- containers -------------------------------------------------------------------
std::string Lower(std::string s) {
  for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

void DecodeBag(const RefDelta& d, Bag& out) {
  out = Bag{};
  ByteReader rd{d.bytes.data(), d.bytes.size()};
  uint32_t n = 0;
  rd.U32(n);
  for (uint32_t i = 0; i < n && rd.ok; i++) {
    uint32_t slot = 0;
    rd.U32(slot);
    ItemInstance it;
    if (!ReadItemInstance(rd, it, kItemFmtStopper)) break;
    if (slot < (uint32_t)Bag::kSlots) out.slots[slot] = it;
  }
}

}  // namespace

// ---- geometry -----------------------------------------------------------------------
std::vector<IVec3> DoorGeom::Cells() const {
  std::vector<IVec3> out;
  if (!ok) return out;
  out.reserve((size_t)width * height * thickness);
  // i along the leaf, j up, k behind the front face.
  const IVec3 base = pos;
  for (int i = 0; i < width; i++)
    for (int j = 0; j < height; j++)
      for (int k = 0; k < thickness; k++)
        out.push_back({base.x + i * along.x - k * face.x, base.y + j,
                       base.z + i * along.z - k * face.z});
  return out;
}

Vec3 DoorGeom::Center() const {
  return Vec3{0.5f * (float)(lo.x + hi.x + 1), 0.5f * (float)(lo.y + hi.y + 1),
              0.5f * (float)(lo.z + hi.z + 1)};
}

Vec3 DoorGeom::LatchAt(float rad) const {
  const Vec3 a{(float)along.x * (float)width, 0.0f, (float)along.z * (float)width};
  const Vec3 e = RotY(a, rad);
  return Vec3{hinge.x + e.x, hinge.y + 0.5f * (float)height, hinge.z + e.z};
}

DoorGeom DoorGeometry(const Ref& r) {
  DoorGeom g;
  if (r.yaw % 90 != 0) {
    g.why = "yaw: a door opens along an axis; use 0, 90, 180 or 270";
    return g;
  }
  g.width = PropInt(r, "width", 9);
  g.height = PropInt(r, "height", 20);
  g.thickness = PropInt(r, "thickness", 1);
  if (g.width < 1 || g.width > 100 || g.height < 1 || g.height > 100 || g.thickness < 1 ||
      g.thickness > 8) {
    g.why = "props.width/height/thickness: 1..100 cells (thickness 1..8)";
    return g;
  }
  std::string hinge = "left";
  if (r.props.contains("hinge") && r.props["hinge"].is_string())
    hinge = r.props["hinge"].get<std::string>();
  if (hinge != "left" && hinge != "right") {
    g.why = "props.hinge: \"left\" or \"right\"";
    return g;
  }
  const float deg = PropFloat(r, "openAngle", 95.0f);
  if (!(deg >= 5.0f && deg <= 175.0f)) {
    g.why = "props.openAngle: 5..175 degrees";
    return g;
  }
  g.openRad = deg * kPi / 180.0f;
  g.face = HeadingAxis(r.yaw);
  // The door-facer's right hand, facing -face: (cos y, 0, -sin y).
  // heading(y+90) = (sin(y+90), 0, cos(y+90)) = (cos y, 0, -sin y): exactly it.
  const IVec3 right = HeadingAxis(r.yaw + 90);
  g.along = hinge == "left" ? right : IVec3{-right.x, 0, -right.z};
  // d/dtheta of RotY(along) at 0 is (along.z, -along.x); toward `face`?
  const int s = g.along.z * g.face.x - g.along.x * g.face.z;
  g.openSign = s >= 0 ? 1.0f : -1.0f;
  // The hinge line: the pos cell's edge AWAY from `along`, on the FRONT face.
  g.hinge = Vec3{(float)r.pos.x + 0.5f - 0.5f * (float)g.along.x + 0.5f * (float)g.face.x,
                 (float)r.pos.y,
                 (float)r.pos.z + 0.5f - 0.5f * (float)g.along.z + 0.5f * (float)g.face.z};
  g.pos = r.pos;
  IVec3 lo{INT32_MAX, r.pos.y, INT32_MAX}, hi{INT32_MIN, r.pos.y + g.height - 1, INT32_MIN};
  for (int i : {0, g.width - 1})
    for (int k : {0, g.thickness - 1}) {
      const int x = r.pos.x + i * g.along.x - k * g.face.x;
      const int z = r.pos.z + i * g.along.z - k * g.face.z;
      lo.x = std::min(lo.x, x);
      lo.z = std::min(lo.z, z);
      hi.x = std::max(hi.x, x);
      hi.z = std::max(hi.z, z);
    }
  g.lo = lo;
  g.hi = hi;
  g.locked = r.props.contains("locked") && r.props["locked"].is_boolean() &&
             r.props["locked"].get<bool>();
  g.autoCloseSec = std::max(0.0f, PropFloat(r, "autoClose", 0.0f));
  g.ok = true;
  return g;
}

const char* DoorPhaseName(DoorPhase p) {
  switch (p) {
    case DoorPhase::Closed: return "closed";
    case DoorPhase::Opening: return "opening";
    case DoorPhase::Open: return "open";
    case DoorPhase::Closing: return "closing";
    case DoorPhase::Settling: return "settling";
    case DoorPhase::Broken: return "broken";
  }
  return "?";
}

bool DoorStatusOf(const std::string& id, DoorStatus& out) {
  auto it = Doors().live.find(id);
  if (it != Doors().live.end()) {
    out = StatusOf(nullptr, it->second);   // the angle as last read by the tick
    return true;
  }
  auto jt = Doors().last.find(id);
  if (jt == Doors().last.end()) return false;
  out = jt->second;
  return true;
}

// ---- containers ----------------------------------------------------------------------
std::string ContainerTitle(const Ref& r) {
  if (r.props.contains("title") && r.props["title"].is_string())
    return r.props["title"].get<std::string>();
  return "chest";
}

void ContainerContents(RefStore& s, const Ref& r, const ItemLibrary* lib, Bag& out) {
  out = Bag{};
  if (const RefDelta* d = s.Delta(r.id); d != nullptr && d->kind == "container") {
    DecodeBag(*d, out);
    return;
  }
  if (!r.props.contains("items") || !r.props["items"].is_array()) return;
  for (const Json& e : r.props["items"]) {
    if (!e.is_object() || !e.contains("item") || !e["item"].is_string()) continue;
    ItemInstance it;
    it.name = e["item"].get<std::string>();
    it.count = e.contains("count") && e["count"].is_number_integer() ? e["count"].get<int>() : 1;
    if (it.count <= 0) continue;
    if (lib != nullptr && lib->Find(it.name) < 0) continue;
    out.Add(it);
  }
}

void ContainerSetContents(RefStore& s, const Ref& r, const Bag& bag) {
  std::vector<uint8_t> b;
  ByteWriter w{b};
  w.U32((uint32_t)bag.Count());
  for (int i = 0; i < Bag::kSlots; i++) {
    if (bag.slots[i].Empty()) continue;
    w.U32((uint32_t)i);
    WriteItemInstance(w, bag.slots[i], kItemFmtStopper);
  }
  s.SetDelta(r.id, "container", 1, std::move(b));
}

bool ContainerTake(RefStore& s, const std::string& id, int n, Kit& kit,
                   const ItemLibrary& lib, std::string* msg) {
  const Ref* r = s.Find(id);
  if (r == nullptr || r->kind != "container") {
    if (msg) *msg = "there is no chest there";
    return false;
  }
  Bag bag;
  ContainerContents(s, *r, &lib, bag);
  const int slot = bag.NthUsed(n);
  if (slot < 0) {
    if (msg) *msg = "there is nothing there";
    return false;
  }
  const ItemStack got = KitStackFrom(bag.slots[slot], lib);
  if (got.Empty()) {
    if (msg) *msg = "that is not a thing any more";
    bag.slots[slot] = ItemStack{};
    ContainerSetContents(s, *r, bag);
    return false;
  }
  // Room first, so a refusal moves nothing (the corpse-loot rule, game/
  // corpses.h TakeCorpseLoot): a free bag slot or a stack it merges into,
  // else the hotbar the same way.
  bool room = kit.bag.FirstFree() >= 0;
  for (const ItemStack& st : kit.bag.slots)
    if (!st.Empty() && st.StacksWith(got)) room = true;
  if (!room)
    for (const ItemStack& st : kit.hotbar.slots)
      if (st.Empty() || st.StacksWith(got)) room = true;
  if (!room) {
    if (msg) *msg = "you have no room for that";
    return false;
  }
  if (kit.bag.Add(got) < 0) kit.hotbar.Add(got);
  bag.slots[slot] = ItemStack{};
  ContainerSetContents(s, *r, bag);
  if (msg) *msg = "took " + got.name + (got.count > 1 ? " x" + std::to_string(got.count) : "");
  return true;
}

bool ContainerPut(RefStore& s, const std::string& id, ItemInstance& stack, std::string* msg) {
  const Ref* r = s.Find(id);
  if (r == nullptr || r->kind != "container" || stack.Empty()) {
    if (msg) *msg = "there is nothing to put";
    return false;
  }
  Bag bag;
  ContainerContents(s, *r, nullptr, bag);
  if (bag.Add(stack) < 0) {
    if (msg) *msg = "the " + ContainerTitle(*r) + " is full";
    return false;
  }
  ContainerSetContents(s, *r, bag);
  if (msg) *msg = "put " + stack.name + " in the " + ContainerTitle(*r);
  stack = ItemInstance{};
  stack.count = 0;
  return true;
}

// ---- beds ------------------------------------------------------------------------------
bool BedAnchorOf(const Ref& r, BedAnchor& out) {
  if (r.yaw % 90 != 0) return false;
  const int len = PropInt(r, "length", 18);
  if (len < 1) return false;
  const IVec3 f = HeadingAxis(r.yaw);
  out.head = Vec3{(float)r.pos.x + 0.5f, (float)r.pos.y + 0.5f, (float)r.pos.z + 0.5f};
  out.foot = Vec3{out.head.x + (float)(f.x * (len - 1)), out.head.y,
                  out.head.z + (float)(f.z * (len - 1))};
  out.headingRad = (float)r.yaw * kPi / 180.0f;
  return true;
}

// ---- the kinds -------------------------------------------------------------------------
void RegisterDoorKinds() {
  // ---- door ----
  RefKind door;
  door.name = "door";
  door.validate = [](const Ref& r, std::vector<std::string>& p) {
    const DoorGeom g = DoorGeometry(r);
    if (!g.ok) p.push_back(g.why);
    if (r.props.contains("locked") && !r.props["locked"].is_boolean())
      p.push_back("props.locked: true or false");
    if (r.props.contains("autoClose") && !r.props["autoClose"].is_number())
      p.push_back("props.autoClose: seconds (0 = never)");
  };
  door.activate = [](RefCtx& c, const Ref& r) -> Activation {
    DoorWorld& D = Doors();
    D.live.erase(r.id);
    D.last.erase(r.id);
    std::vector<std::pair<IVec3, uint32_t>> cells;
    const uint32_t state = DecodeDelta(c.refs, r.id, &cells);
    if (state == kDoorBroken) {
      DoorLive L;
      L.phase = DoorPhase::Broken;
      L.note = "broken";
      D.last[r.id] = StatusOf(&c, L);
      return Activation::Done;
    }
    if (state != kDoorOpen) {
      D.last[r.id] = DoorStatus{};
      return Activation::Done;
    }
    // OPEN IN THE SAVE: the grid holds air where the leaf was; hang the saved
    // words again, already at the open angle.
    if (c.phys == nullptr || c.debris == nullptr || c.world == nullptr) return Activation::Done;
    const DoorGeom g = DoorGeometry(r);
    if (!g.ok) return Activation::Done;
    for (const auto& [cell, word] : cells)
      if (!c.world->CellInWindow(cell)) return Activation::Retry;
    DoorLive L;
    L.cells = std::move(cells);
    L.target = g.openSign * g.openRad;
    if (!HangLeaf(c, g, L, L.target)) return Activation::Retry;
    c.phys->SetJointMotorTarget(L.joint, L.target);
    L.phase = DoorPhase::Open;
    L.openedAt = c.tick;
    L.note = "open (restored)";
    D.live[r.id] = std::move(L);
    return Activation::Done;
  };
  door.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    DoorWorld& D = Doors();
    DoorLive L;
    auto it = D.live.find(r.id);
    const bool live = it != D.live.end();
    if (live) {
      L = std::move(it->second);
      D.live.erase(it);
    }
    D.last.erase(r.id);
    if (why == RefEvent::Reset) return;   // a load: debris was reset under us
    const bool hung = live && !L.cells.empty() && L.phase != DoorPhase::Opening &&
                      L.phase != DoorPhase::Broken;
    if (hung && c.debris != nullptr) {
      const uint64_t b = LiveBody(c, L);
      if (b != 0) c.debris->DestroyBody(b);   // the hinge dies with it
    }
    if (why == RefEvent::WindowLeft) return;   // the delta keeps it open
    // EDITED / DELETED: undo, so the new line starts from a closed door. An
    // open leaf goes back into the wall now -- hung, or only in the delta.
    if (!hung && c.refs != nullptr && DecodeDelta(c.refs, r.id, &L.cells) != kDoorOpen)
      L.cells.clear();
    WriteBack(c, L);
    if (c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  door.usePoint = [](RefCtx& c, const Ref& r, Vec3& at) {
    const DoorGeom g = DoorGeometry(r);
    if (!g.ok) return false;
    at = g.Center();
    auto it = Doors().live.find(r.id);
    if (it != Doors().live.end() && it->second.joint != 0 && c.phys != nullptr) {
      float a = 0.0f;
      if (c.phys->JointHingeAngle(it->second.joint, a)) {
        const Vec3 h{g.hinge.x, at.y, g.hinge.z};
        at = h + RotY(at - h, a);
      }
    }
    return true;
  };
  door.usePrompt = [](RefCtx& c, const Ref& r) -> std::string {
    const DoorGeom g = DoorGeometry(r);
    if (!g.ok) return "";
    auto it = Doors().live.find(r.id);
    if (it == Doors().live.end()) {
      if (DecodeDelta(c.refs, r.id, nullptr) == kDoorBroken) return "Broken door";
      return g.locked ? "Locked" : "Open door";
    }
    switch (it->second.phase) {
      case DoorPhase::Opening: return "";
      case DoorPhase::Open: return "Close door";
      case DoorPhase::Closing:
      case DoorPhase::Settling: return "Open door";
      case DoorPhase::Broken: return "Broken door";
      default: return g.locked ? "Locked" : "Open door";
    }
  };
  door.onUse = [](RefCtx& c, const Ref& r, RefUse& u) {
    const DoorGeom g = DoorGeometry(r);
    if (!g.ok) {
      u.message = "this door is misconfigured (" + g.why + ")";
      return;
    }
    DoorWorld& D = Doors();
    auto it = D.live.find(r.id);
    const bool broken = it != D.live.end() ? it->second.phase == DoorPhase::Broken
                                            : DecodeDelta(c.refs, r.id, nullptr) == kDoorBroken;
    if (broken) {
      u.message = "The door is broken off its hinges.";
      return;
    }
    if (it == D.live.end() || it->second.phase == DoorPhase::Closed) {
      if (g.locked) {
        u.message = "It is locked.";
        return;
      }
      DoorLive& L = D.live[r.id];
      L = DoorLive{};
      D.last.erase(r.id);
      StartOpen(c, r, L, g);
      return;
    }
    DoorLive& L = it->second;
    switch (L.phase) {
      case DoorPhase::Open: StartClose(c, L); break;
      case DoorPhase::Closing:
      case DoorPhase::Settling:
        // Changed our mind: swing it back open.
        L.phase = DoorPhase::Open;
        L.openedAt = c.tick;
        L.note = "open";
        if (c.phys != nullptr) c.phys->SetJointMotorTarget(L.joint, L.target);
        break;
      default: break;
    }
  };
  door.tick = [](RefCtx& c) {
    DoorWorld& D = Doors();
    if (!D.pending.empty() && c.ops != nullptr) {
      c.ops->cells.insert(c.ops->cells.end(), D.pending.begin(), D.pending.end());
      D.pending.clear();
    }
    if (D.live.empty()) return;
    for (auto it = D.live.begin(); it != D.live.end();) {
      const Ref* r = c.refs != nullptr ? c.refs->Find(it->first) : nullptr;
      if (r == nullptr || !c.refs->IsActive(it->first) || r->kind != "door") {
        it = D.live.erase(it);   // a stale entry from another store / world
        continue;
      }
      TickDoor(c, *r, it->second);
      if (it->second.phase == DoorPhase::Closed || it->second.phase == DoorPhase::Broken) {
        D.last[it->first] = StatusOf(&c, it->second);
        it = D.live.erase(it);
      } else {
        ++it;
      }
    }
  };
  door.useRadius = 9.0f;
  Kinds().Register(std::move(door));

  // ---- container ----
  RefKind box;
  box.name = "container";
  box.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (r.props.contains("items")) {
      const Json& a = r.props["items"];
      bool ok = a.is_array();
      if (ok)
        for (const Json& e : a)
          ok &= e.is_object() && e.contains("item") && e["item"].is_string() &&
                (!e.contains("count") || e["count"].is_number_integer());
      if (!ok)
        p.push_back("props.items: a list like [{\"item\": \"bread\", \"count\": 3}]");
    }
    if (r.props.contains("title") && !r.props["title"].is_string())
      p.push_back("props.title: text");
    if (r.props.contains("locked") && !r.props["locked"].is_boolean())
      p.push_back("props.locked: true or false");
  };
  // props.locked: the door rule for a chest (P8, Osric's strongbox). A locked
  // chest says so and does not open -- no keys yet, exactly like a door.
  auto chestLocked = [](const Ref& r) {
    return r.props.contains("locked") && r.props["locked"].is_boolean() &&
           r.props["locked"].get<bool>();
  };
  box.usePrompt = [chestLocked](RefCtx&, const Ref& r) {
    return chestLocked(r) ? "Locked " + Lower(ContainerTitle(r))
                          : "Search " + Lower(ContainerTitle(r));
  };
  box.onUse = [chestLocked](RefCtx&, const Ref& r, RefUse& u) {
    if (chestLocked(r)) {
      u.message = "The " + Lower(ContainerTitle(r)) + " is locked.";
      return;
    }
    u.openContainer = r.id;
  };
  box.useRadius = 5.0f;
  Kinds().Register(std::move(box));

  // ---- bed ----
  RefKind bed;
  bed.name = "bed";
  bed.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (r.yaw % 90 != 0) p.push_back("yaw: a bed lies along an axis; use 0, 90, 180 or 270");
    if (r.props.contains("length") &&
        (!r.props["length"].is_number_integer() || r.props["length"].get<int>() < 1))
      p.push_back("props.length: cells from head to foot (default 18)");
  };
  bed.usePoint = [](RefCtx&, const Ref& r, Vec3& at) {
    BedAnchor a;
    if (!BedAnchorOf(r, a)) return false;
    at = (a.head + a.foot) * 0.5f;
    return true;
  };
  bed.usePrompt = [](RefCtx&, const Ref&) { return std::string("Rest"); };
  bed.onUse = [](RefCtx&, const Ref&, RefUse& u) {
    u.message = "You lie down for a moment. (Sleeping through the night is not in yet.)";
  };
  bed.useRadius = 8.0f;
  Kinds().Register(std::move(bed));
}

}  // namespace refs
