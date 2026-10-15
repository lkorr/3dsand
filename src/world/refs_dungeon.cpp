// refs_dungeon.cpp — see refs_dungeon.h.

#include "world/refs_dungeon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "game/dye.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/mob_archery.h"  // ArmArcher: an archer profile arms itself
#include "game/session.h"
#include "phys/damagecause.h"
#include "sim/bytestream.h"
#include "sim/materials.h"
#include "sim/rng.h"
#include "sim/world.h"
#include "world/refs_doors.h"
#include "world/refs_npc.h"

namespace refs {

namespace {

constexpr uint32_t kTickHz = 30;        // the fixed sim rate (support.h kTickDt)
constexpr uint32_t kFmt = 1;            // every delta here: u32 fmt, then u32 words
constexpr uint32_t kRefetchTicks = 15;  // re-ask the fetch cache this often
// A collapse converts at most this many cells per two ticks (one clear tick,
// one fill tick), and no region is scanned past kMaxRegionCells.
constexpr size_t kCollapseBatch = 8192;
constexpr size_t kMaxRegionCells = 65536;
constexpr size_t kMaxFloodCells = 16384;
constexpr int kMaxSpawnerCount = 16;
// A spawned creature not found for this long while its spawner is active is
// counted dead (despawned, eaten, released to debris): 20 s, long enough for
// a parked one to be unparked first.
constexpr uint32_t kSpawnMissingTicks = 600;
// Draws a spawner makes for an OPEN spot before it falls back to its own pos
// (SpawnInto's ground rule).
constexpr uint32_t kSpawnTries = 8;

int FloorDiv(int v, int d) { return (v >= 0) ? v / d : -((-v + d - 1) / d); }
IVec3 ChunkOfCell(IVec3 c) { return {FloorDiv(c.x, 16), FloorDiv(c.y, 16), FloorDiv(c.z, 16)}; }

// Heading unit vector of a whole-90 yaw (0 = +Z, 90 = +X).
IVec3 HeadingAxis(int yaw) {
  switch ((((yaw % 360) + 360) % 360) / 90) {
    case 0: return {0, 0, 1};
    case 1: return {1, 0, 0};
    case 2: return {0, 0, -1};
    default: return {-1, 0, 0};
  }
}

std::string PropStr(const Ref& r, const char* k, const std::string& def = std::string()) {
  if (r.props.contains(k) && r.props[k].is_string()) return r.props[k].get<std::string>();
  return def;
}
int PropInt(const Ref& r, const char* k, int def) {
  if (r.props.contains(k) && r.props[k].is_number()) return (int)std::lround(r.props[k].get<double>());
  return def;
}
float PropFloat(const Ref& r, const char* k, float def) {
  if (r.props.contains(k) && r.props[k].is_number()) return r.props[k].get<float>();
  return def;
}
bool PropBool(const Ref& r, const char* k, bool def) {
  if (r.props.contains(k) && r.props[k].is_boolean()) return r.props[k].get<bool>();
  return def;
}
std::string LastPart(const std::string& id) {
  const size_t s = id.rfind('/');
  return s == std::string::npos ? id : id.substr(s + 1);
}
uint32_t Secs(float s) { return (uint32_t)std::lround(std::max(0.0f, s) * (float)kTickHz); }

uint32_t MatId(const RefCtx& c, const std::string& name) {
  if (c.mats == nullptr) return 0;
  for (size_t i = 0; i < c.mats->size(); i++)
    if ((*c.mats)[i].name == name) return (uint32_t)i;
  return 0;
}
uint32_t ClassOf(const RefCtx& c, uint32_t mat) {
  if (c.mats == nullptr || mat >= c.mats->size()) return CLASS_SOLID;
  return (*c.mats)[mat].gpu.klass;
}

// ---- deltas: u32 fmt, then u32 words ----------------------------------------------
std::vector<uint32_t> ReadWords(RefStore* s, const Ref& r) {
  std::vector<uint32_t> w;
  if (s == nullptr) return w;
  const RefDelta* d = s->Delta(r.id);
  if (d == nullptr || d->kind != r.kind || d->bytes.size() < 4) return w;
  ByteReader rd{d->bytes.data(), d->bytes.size()};
  uint32_t fmt = 0;
  rd.U32(fmt);
  if (fmt != kFmt) return w;
  while (rd.ok && rd.off + 4 <= rd.n) {
    uint32_t v = 0;
    rd.U32(v);
    w.push_back(v);
  }
  return w;
}
void WriteWords(RefStore* s, const Ref& r, const std::vector<uint32_t>& words) {
  if (s == nullptr) return;
  std::vector<uint8_t> b;
  ByteWriter wr{b};
  wr.U32(kFmt);
  for (uint32_t v : words) wr.U32(v);
  s->SetDelta(r.id, r.kind, 1, std::move(b));
}

// ---- the fetch cache (the door's rule: refs_doors.cpp CacheFresh) ------------------
bool CacheFresh(World& world, IVec3 lo, IVec3 hi, uint32_t since, uint32_t tick, uint32_t& fetchAt) {
  bool fresh = true;
  const IVec3 a = ChunkOfCell(lo), b = ChunkOfCell(hi);
  const bool ask = fetchAt == 0 || tick - fetchAt >= kRefetchTicks;
  for (int z = a.z; z <= b.z; z++)
    for (int y = a.y; y <= b.y; y++)
      for (int x = a.x; x <= b.x; x++) {
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
bool BoxInWindow(const World* w, IVec3 lo, IVec3 hi) {
  if (w == nullptr) return false;
  const IVec3 a = ChunkOfCell(lo), b = ChunkOfCell(hi);
  for (int z = a.z; z <= b.z; z++)
    for (int y = a.y; y <= b.y; y++)
      for (int x = a.x; x <= b.x; x++)
        if (!w->ChunkInWindow({x, y, z})) return false;
  return true;
}
size_t BoxCells(IVec3 lo, IVec3 hi) {
  return (size_t)(hi.x - lo.x + 1) * (size_t)(hi.y - lo.y + 1) * (size_t)(hi.z - lo.z + 1);
}

// ---- bodies ------------------------------------------------------------------------
struct BodyRef {
  Vec3 foot{};
  float halfXZ = 3.0f, height = 17.0f;
  Mob* mob = nullptr;      // null for a player without an avatar
  bool player = false;
};
std::vector<BodyRef> Bodies(RefCtx& c, bool players, bool mobs) {
  std::vector<BodyRef> out;
  if (players && c.players != nullptr)
    for (const RefCtx::Body& b : *c.players) {
      if (!b.alive) continue;
      out.push_back({b.foot, b.halfXZ, b.height, b.mob, true});
    }
  if (mobs && c.mobs != nullptr)
    for (uint32_t i = 0; i < c.mobs->MobCount(); i++) {
      const Mob* m = c.mobs->MobAt(i);
      if (m == nullptr || !m->Alive() || m->Def() == nullptr) continue;
      const Vec3 o = m->Origin(), ws = m->Def()->worldSize;
      BodyRef b;
      b.foot = Vec3{o.x + ws.x * 0.5f, o.y, o.z + ws.z * 0.5f};
      b.halfXZ = 0.5f * std::max(ws.x, ws.z);
      b.height = ws.y;
      b.mob = c.mobs->FindMobById(m->Id());
      out.push_back(b);
    }
  return out;
}
// Feet in the box: inside it across, and from two below its floor to two
// above its top (standing ON a plate's cells or in a region's air).
bool FeetIn(const Vec3& f, IVec3 lo, IVec3 hi) {
  return f.x >= (float)lo.x && f.x < (float)hi.x + 1.0f && f.z >= (float)lo.z &&
         f.z < (float)hi.z + 1.0f && f.y >= (float)lo.y - 2.0f && f.y <= (float)hi.y + 2.0f;
}

const Ref* FindNamed(const RefStore& s, const Ref& from, const std::string& name) {
  if (const Ref* r = s.Find(name)) return r;
  const size_t cut = from.id.rfind('/');
  if (cut != std::string::npos)
    if (const Ref* r = s.Find(from.id.substr(0, cut) + "/" + name)) return r;
  return s.Find(from.group + "/" + name);
}

IVec3 SizeProp(const Ref& r, const char* k, IVec3 def) {
  if (!r.props.contains(k)) return def;
  const Json& j = r.props[k];
  if (!j.is_array() || j.size() != 3) return def;
  for (const Json& v : j)
    if (!v.is_number()) return def;
  return {std::max(1, (int)j[0].get<double>()), std::max(1, (int)j[1].get<double>()),
          std::max(1, (int)j[2].get<double>())};
}
bool SizeValid(const Ref& r, const char* k, std::vector<std::string>& p) {
  if (!r.props.contains(k)) return true;
  const Json& j = r.props[k];
  bool ok = j.is_array() && j.size() == 3;
  if (ok)
    for (const Json& v : j) ok &= v.is_number() && v.get<double>() >= 1.0;
  if (!ok) p.push_back(std::string("props.") + k + ": [sx, sy, sz] in cells, each >= 1");
  return ok;
}

std::string SignalOf(const Ref& r) { return PropStr(r, "signal", LastPart(r.id)); }

// ---- live state ----------------------------------------------------------------------
struct GateLive {
  int raised = 0;
  bool built = false;
  uint32_t lastStep = 0;
  uint32_t steps = 0;
  bool wantOpen = false;
};

enum class TrapPhase : uint8_t { Idle, Fetch, Clear, Fill, Active };
struct TrapLive {
  bool lastLevel = false;
  uint32_t fires = 0;
  bool done = false;
  TrapPhase phase = TrapPhase::Idle;
  uint32_t reqTick = 0, fetchAt = 0, until = 0, armAt = 0;
  // collapse: (cell, the material it held, the twin word to write)
  struct Conv {
    IVec3 cell;
    uint32_t mat;
    uint32_t twin;
  };
  std::vector<Conv> conv;
  size_t cursor = 0;
  std::vector<std::pair<IVec3, uint32_t>> placed;   // spikes: cells to retract
  uint32_t cells = 0, ops = 0, hits = 0;
  float wallT = -1.0f;   // dart: how far the last bolt flew before a wall (voxels; -1 = never fired)
  std::string note;
};

struct SpawnSlot {
  uint32_t serial = 0;   // which spawn put the current creature here (0 = none)
  uint32_t state = 0;    // 0 empty, 1 out, 2 dead
  uint32_t deadTick = 0;
  uint32_t missing = 0;  // ticks not found while out (not saved)
  std::string base;
};
struct SpawnerLive {
  uint32_t serial = 0;
  bool triggered = false;
  std::vector<SpawnSlot> slots;
  uint32_t askedAt = 0;  // the ground wait (0 = not asking)
  uint32_t fetchAt = 0;
  std::string note;
};

struct PlateLive {
  bool pressed = false;
};
struct LeverLive {
  uint32_t pulseUntil = 0;
};

struct DungeonWorld {
  std::map<std::string, GateLive> gates;
  std::map<std::string, TrapLive> traps;
  std::map<std::string, SpawnerLive> spawners;
  std::map<std::string, PlateLive> plates;
  std::map<std::string, LeverLive> levers;
  std::set<std::string> active;   // every active ref of the kinds here
  // per store revision: the emitters of each "group|signal", the doors a
  // signal works
  const RefStore* idxFor = nullptr;
  uint64_t idxRev = ~0ull;
  std::map<std::string, std::vector<std::string>> emitters;
  std::vector<std::string> signalDoors;
};
DungeonWorld& DW() {
  static DungeonWorld d;
  return d;
}

void StoreGone(RefStore* s) {
  if (DW().idxFor == s) {
    DW().idxFor = nullptr;
    DW().idxRev = ~0ull;
  }
}

void Reindex(const RefStore& s) {
  DungeonWorld& D = DW();
  if (D.idxFor == &s && D.idxRev == s.Revision()) return;
  D.idxFor = &s;
  D.idxRev = s.Revision();
  D.emitters.clear();
  D.signalDoors.clear();
  for (const auto& [id, r] : s.All()) {
    if (r.kind == "lever" || r.kind == "plate") D.emitters[r.group + "|" + SignalOf(r)].push_back(id);
    if (r.kind == "door" && r.props.contains("on") && r.props["on"].is_string())
      D.signalDoors.push_back(id);
  }
}

bool LeverOn(RefStore& s, const Ref& r) {
  const std::vector<uint32_t> w = ReadWords(&s, r);
  return w.empty() ? PropBool(r, "startOn", false) : w[0] != 0;
}
bool PlateLatched(RefStore& s, const Ref& r) {
  const std::vector<uint32_t> w = ReadWords(&s, r);
  return !w.empty() && w[0] != 0;
}

bool ReceiverLevel(RefStore& s, const Ref& r) {
  const std::string on = PropStr(r, "on");
  const bool lvl = !on.empty() && SignalLevel(s, r.group, on);
  return lvl != PropBool(r, "invert", false);
}

// ---- the gate (portcullis) -----------------------------------------------------------
struct GateGeom {
  bool ok = false;
  std::string why;
  int width = 20, height = 30, thick = 1, spacing = 3, speed = 2;
  bool solid = false;
  IVec3 pos{}, along{}, face{};
  std::string material;
  IVec3 Cell(int i, int j, int k) const {
    return {pos.x + i * along.x - k * face.x, pos.y + j, pos.z + i * along.z - k * face.z};
  }
  // The authored pattern at gate row j (0 = the bottom row of the bars).
  bool Pattern(int i, int j) const {
    if (solid) return true;
    if (i % spacing == 0 || i == width - 1) return true;   // the uprights
    if (j == height - 1) return true;                      // the top rail
    return j % (spacing * 4) == spacing * 2;               // the cross bars
  }
  // Cell (i, j) with the bars lifted `raised` rows: the bottom rows are air.
  bool Solid(int raised, int i, int j) const { return j >= raised && Pattern(i, j - raised); }
  void Box(IVec3& lo, IVec3& hi) const {
    const IVec3 a = Cell(0, 0, 0), b = Cell(width - 1, height - 1, thick - 1);
    lo = {std::min(a.x, b.x), a.y, std::min(a.z, b.z)};
    hi = {std::max(a.x, b.x), b.y, std::max(a.z, b.z)};
  }
};
GateGeom GateGeometry(const Ref& r) {
  GateGeom g;
  if (r.yaw % 90 != 0) {
    g.why = "yaw: a gate faces along an axis; use 0, 90, 180 or 270";
    return g;
  }
  g.width = PropInt(r, "width", 20);
  g.height = PropInt(r, "height", 30);
  g.thick = PropInt(r, "thickness", 1);
  g.spacing = std::max(2, PropInt(r, "spacing", 3));
  g.speed = std::max(1, PropInt(r, "speed", 2));
  if (g.width < 1 || g.width > 200 || g.height < 2 || g.height > 200 || g.thick < 1 || g.thick > 8) {
    g.why = "props.width/height/thickness: 1..200 cells (height >= 2, thickness 1..8)";
    return g;
  }
  const std::string pat = PropStr(r, "pattern", "bars");
  if (pat != "bars" && pat != "solid") {
    g.why = "props.pattern: \"bars\" or \"solid\"";
    return g;
  }
  g.solid = pat == "solid";
  g.material = PropStr(r, "material", "iron");
  g.pos = r.pos;
  g.face = HeadingAxis(r.yaw);
  g.along = HeadingAxis(r.yaw + 90);
  g.ok = true;
  return g;
}

// Emit the cells that change between two row offsets: a conditional clear
// where bars leave, an IfAir write where they arrive.
uint32_t GateDiff(RefCtx& c, const GateGeom& g, uint32_t word, uint32_t mat, int from, int to) {
  if (c.ops == nullptr) return 0;
  uint32_t n = 0;
  for (int i = 0; i < g.width; i++)
    for (int j = 0; j < g.height; j++) {
      const bool a = from < 0 ? false : g.Solid(from, i, j);
      const bool b = g.Solid(to, i, j);
      if (a == b) continue;
      for (int k = 0; k < g.thick; k++) {
        const IVec3 cell = g.Cell(i, j, k);
        if (c.world != nullptr && !c.world->CellInWindow(cell)) continue;
        c.ops->cells.push_back(
            {World::SlotCellIndex(cell), b ? (word | kCellOpIfAir) : CellOpClearIfMat(mat)});
        n++;
      }
    }
  return n;
}

void TickGate(RefCtx& c, const Ref& r, GateLive& L) {
  const GateGeom g = GateGeometry(r);
  if (!g.ok) return;
  L.wantOpen = ReceiverLevel(*c.refs, r);
  const int target = L.wantOpen ? g.height : 0;
  if (L.raised == target || c.tick - L.lastStep < (uint32_t)g.speed) return;
  IVec3 lo, hi;
  g.Box(lo, hi);
  if (!BoxInWindow(c.world, lo, hi) || c.ops == nullptr) return;
  const uint32_t mat = MatId(c, g.material);
  if (mat == 0) return;
  const int next = L.raised + (target > L.raised ? 1 : -1);
  GateDiff(c, g, PackVoxNew(mat, 0), mat, L.raised, next);
  L.raised = next;
  L.lastStep = c.tick;
  L.steps++;
  WriteWords(c.refs, r, {(uint32_t)L.raised, L.built ? 1u : 0u});
}

// ---- traps -----------------------------------------------------------------------------
bool OnceByDefault(const std::string& type) { return type == "collapse_floor" || type == "flood"; }

void SaveTrap(RefCtx& c, const Ref& r, const TrapLive& L) {
  WriteWords(c.refs, r, {L.fires, L.done ? 1u : 0u});
}

void DamageBodies(RefCtx& c, TrapLive& L, IVec3 lo, IVec3 hi, float dmg) {
  for (BodyRef& b : Bodies(c, true, true)) {
    if (!FeetIn(b.foot, lo, hi) || b.mob == nullptr) continue;
    b.mob->SpendHp(dmg, DamageCtx(DamageCause::Blade));
    L.hits++;
  }
}

void Fire(RefCtx& c, const Ref& r, TrapLive& L) {
  const std::string type = PropStr(r, "type");
  L.fires++;
  const bool once = PropBool(r, "once", OnceByDefault(type));
  L.armAt = c.tick + std::max(1u, Secs(PropFloat(r, "cooldown", 3.0f)));
  IVec3 lo{}, hi{};
  std::string why;
  const bool haveRegion = RegionOf(*c.refs, r, lo, hi, &why);
  if (type != "dart" && !haveRegion) {
    L.note = "fired, but " + why;
    if (once) L.done = true;
    SaveTrap(c, r, L);
    return;
  }
  if (type == "collapse_floor") {
    L.phase = TrapPhase::Fetch;
    L.reqTick = c.tick;
    L.fetchAt = 0;
    L.conv.clear();
    L.cursor = 0;
    L.note = "collapsing: reading the floor";
  } else if (type == "flood" || type == "fire_vent") {
    const bool flood = type == "flood";
    const uint32_t mat = MatId(c, PropStr(r, "material", flood ? "water" : "fire"));
    if (mat == 0 || c.ops == nullptr) {
      L.note = "no material \"" + PropStr(r, "material", flood ? "water" : "fire") + "\"";
    } else if (flood) {
      // A BOUNDED VOLUME, once: the region's air filled, never a source.
      const uint32_t full = ClassOf(c, mat) == CLASS_LIQUID ? 8u : 0u;
      size_t n = 0;
      for (int y = lo.y; y <= hi.y && n < kMaxFloodCells; y++)
        for (int z = lo.z; z <= hi.z && n < kMaxFloodCells; z++)
          for (int x = lo.x; x <= hi.x && n < kMaxFloodCells; x++) {
            const IVec3 cell{x, y, z};
            if (c.world != nullptr && !c.world->CellInWindow(cell)) continue;
            c.ops->cells.push_back({World::SlotCellIndex(cell), PackVoxNew(mat, full) | kCellOpIfAir});
            n++;
          }
      L.cells += (uint32_t)n;
      L.ops += (uint32_t)n;
      L.note = "flooded";
    } else {
      L.phase = TrapPhase::Active;
      L.until = c.tick + std::max(1u, Secs(PropFloat(r, "duration", 2.0f)));
      L.note = "venting";
    }
  } else if (type == "spikes") {
    DamageBodies(c, L, lo, hi, PropFloat(r, "damage", 25.0f));
    const uint32_t mat = MatId(c, PropStr(r, "material", "iron"));
    L.placed.clear();
    if (mat != 0 && c.ops != nullptr) {
      const int h = std::min(hi.y - lo.y + 1, std::max(1, PropInt(r, "spikeHeight", 4)));
      for (int z = lo.z; z <= hi.z; z++)
        for (int x = lo.x; x <= hi.x; x++) {
          if (((x - lo.x) % 3) != 1 || ((z - lo.z) % 3) != 1) continue;
          for (int y = lo.y; y < lo.y + h; y++) {
            const IVec3 cell{x, y, z};
            if (c.world != nullptr && !c.world->CellInWindow(cell)) continue;
            c.ops->cells.push_back({World::SlotCellIndex(cell), PackVoxNew(mat, 0) | kCellOpIfAir});
            L.placed.push_back({cell, mat});
            L.ops++;
          }
        }
    }
    L.cells += (uint32_t)L.placed.size();
    L.phase = TrapPhase::Active;
    L.until = c.tick + std::max(1u, Secs(PropFloat(r, "duration", 1.0f)));
    L.note = "spikes up";
  } else if (type == "dart") {
    const IVec3 f = HeadingAxis(r.yaw);
    const Vec3 o{(float)r.pos.x + 0.5f, (float)r.pos.y + 0.5f, (float)r.pos.z + 0.5f};
    const float range = PropFloat(r, "range", 120.0f);
    // THE BOLT STOPS AT THE FIRST WALL (D9; D3 hit any body on the line, so a
    // dart in one room struck a player in the next through the rock). The
    // line is axis-aligned, so it is walked cell by cell from the cell in
    // front of the launcher: the first SOLID cell caps the range for both the
    // hit test and the particle. Read from the CPU mirror (World::KindAt,
    // collision classes: passable vegetation is air), the same contract every
    // body's collision has; an Unknown cell (outside the mirror) is open, as
    // it is for projectiles -- so a dart fired far from every player can
    // still pass a wall the CPU cannot see (a limitation, not a hit through a
    // wall anyone is standing at).
    float wallT = range;
    if (c.world != nullptr && c.mats != nullptr) {
      static thread_local const std::vector<MaterialDef>* clsFor = nullptr;
      static thread_local size_t clsN = 0;
      static thread_local std::vector<uint32_t> cls;
      if (clsFor != c.mats || clsN != c.mats->size()) {
        cls = BuildCollisionClasses(*c.mats);
        clsFor = c.mats;
        clsN = c.mats->size();
      }
      // A launcher is usually SET INTO its wall ("dart_wall"): the solid cells
      // it starts in -- up to kDartBore of them -- are its own housing, and
      // the bolt is stopped only by a solid cell after open air.
      constexpr int kDartBore = 8;
      const int steps = (int)std::ceil(range);
      bool open = false;
      for (int t = 0; t <= steps; t++) {
        const IVec3 cell{r.pos.x + f.x * t, r.pos.y, r.pos.z + f.z * t};
        const bool solid = c.world->KindAt(cell, cls) == CellKind::Solid;
        if (!solid) { open = true; continue; }
        if (open || t >= kDartBore) {
          wallT = std::max(0.0f, (float)t - 0.5f);   // the wall's near face
          break;
        }
      }
    }
    L.wallT = wallT;
    Mob* best = nullptr;
    float bestT = wallT;
    for (BodyRef& b : Bodies(c, true, true)) {
      const float dx = b.foot.x - o.x, dz = b.foot.z - o.z;
      const float t = dx * (float)f.x + dz * (float)f.z;
      const float perp = std::fabs(dx * (float)f.z - dz * (float)f.x);
      if (t < 0.0f || t > bestT || perp > b.halfXZ + 1.0f) continue;
      if (o.y < b.foot.y - 1.0f || o.y > b.foot.y + b.height) continue;
      best = b.mob;
      bestT = t;
    }
    if (best != nullptr) {
      best->SpendHp(PropFloat(r, "damage", 15.0f), DamageCtx(DamageCause::Blade));
      L.hits++;
    }
    // What you see: a fast grain of the bolt's material along the line.
    if (c.ops != nullptr) {
      const uint32_t mat = MatId(c, PropStr(r, "material", "iron"));
      if (mat != 0) {
        const float speed = 8.0f;   // voxels per tick
        ParticleSpawn p{};
        p.px = (int32_t)std::lround(o.x * 256.0f);
        p.py = (int32_t)std::lround(o.y * 256.0f);
        p.pz = (int32_t)std::lround(o.z * 256.0f);
        p.vx = (int32_t)std::lround((float)f.x * speed * 256.0f);
        p.vy = 0;
        p.vz = (int32_t)std::lround((float)f.z * speed * 256.0f);
        p.payload = mat;
        p.flags = kPFlagMicro | ParticleMicroBits(4, (int)std::min(255.0f, wallT / speed + 1.0f));
        c.ops->spawns.push_back(p);
      }
    }
    L.note = best != nullptr ? "a dart hit" : (wallT < range ? "a dart struck a wall" : "a dart flew");
  } else {
    L.note = "unknown trap type \"" + type + "\"";
  }
  if (once && type != "collapse_floor") L.done = true;
  SaveTrap(c, r, L);
}

void TickTrap(RefCtx& c, const Ref& r, TrapLive& L) {
  const bool lvl = ReceiverLevel(*c.refs, r);
  const bool rising = lvl && !L.lastLevel;
  L.lastLevel = lvl;
  if (rising && !L.done && L.phase == TrapPhase::Idle && c.tick >= L.armAt) Fire(c, r, L);
  const std::string type = PropStr(r, "type");
  switch (L.phase) {
    case TrapPhase::Idle:
      return;
    case TrapPhase::Fetch: {
      IVec3 lo, hi;
      if (!RegionOf(*c.refs, r, lo, hi) || c.world == nullptr) {
        L.phase = TrapPhase::Idle;
        return;
      }
      if (!BoxInWindow(c.world, lo, hi)) {
        L.note = "collapsing: waiting for the floor to be in the window";
        return;
      }
      if (!CacheFresh(*c.world, lo, hi, L.reqTick, c.tick, L.fetchAt)) return;
      // CAPTURE: every static cell with a powder twin.
      const std::string from = PropStr(r, "from");
      const std::string to = PropStr(r, "to");
      const uint32_t toId = to.empty() ? 0 : MatId(c, to);
      size_t scanned = 0;
      for (int y = lo.y; y <= hi.y; y++)
        for (int z = lo.z; z <= hi.z; z++)
          for (int x = lo.x; x <= hi.x; x++) {
            if (++scanned > kMaxRegionCells) break;
            const IVec3 cell{x, y, z};
            const uint32_t w = CachedWord(*c.world, cell);
            const uint32_t m = w & 0xFFFu;
            if (m == 0 || ClassOf(c, m) != CLASS_SOLID || c.mats == nullptr) continue;
            if (!from.empty() && (*c.mats)[m].name != from) continue;
            uint32_t twin = toId;
            if (twin == 0 && !(*c.mats)[m].rubble.empty()) twin = MatId(c, (*c.mats)[m].rubble);
            if (twin == 0 || twin == m || ClassOf(c, twin) != CLASS_POWDER) continue;
            L.conv.push_back({cell, m, PackVoxNew(twin, (w >> 12) & 0xFu)});
          }
      L.cursor = 0;
      L.phase = L.conv.empty() ? TrapPhase::Idle : TrapPhase::Clear;
      if (L.conv.empty()) {
        L.note = "collapsed nothing: no static floor with a powder twin in the region";
        L.done = PropBool(r, "once", true);
        SaveTrap(c, r, L);
      }
      return;
    }
    case TrapPhase::Clear: {
      if (c.ops == nullptr) return;
      const size_t end = std::min(L.conv.size(), L.cursor + kCollapseBatch);
      for (size_t i = L.cursor; i < end; i++) {
        if (c.world != nullptr && !c.world->CellInWindow(L.conv[i].cell)) continue;
        c.ops->cells.push_back({World::SlotCellIndex(L.conv[i].cell), CellOpClearIfMat(L.conv[i].mat)});
        L.ops++;
      }
      L.phase = TrapPhase::Fill;
      return;
    }
    case TrapPhase::Fill: {
      if (c.ops == nullptr) return;
      const size_t end = std::min(L.conv.size(), L.cursor + kCollapseBatch);
      for (size_t i = L.cursor; i < end; i++) {
        if (c.world != nullptr && !c.world->CellInWindow(L.conv[i].cell)) continue;
        c.ops->cells.push_back({World::SlotCellIndex(L.conv[i].cell), L.conv[i].twin | kCellOpIfAir});
        L.ops++;
        L.cells++;
      }
      L.cursor = end;
      if (L.cursor < L.conv.size()) {
        L.phase = TrapPhase::Clear;
        return;
      }
      L.phase = TrapPhase::Idle;
      L.conv.clear();
      L.conv.shrink_to_fit();
      L.note = "collapsed";
      if (PropBool(r, "once", true)) L.done = true;
      SaveTrap(c, r, L);
      return;
    }
    case TrapPhase::Active: {
      if (type == "fire_vent" && c.ops != nullptr && c.tick < L.until && (c.tick % 3) == 0) {
        IVec3 lo, hi;
        const uint32_t mat = MatId(c, PropStr(r, "material", "fire"));
        if (mat != 0 && RegionOf(*c.refs, r, lo, hi)) {
          const size_t n = BoxCells(lo, hi);
          const int k = (int)std::min<size_t>(64, std::max<size_t>(1, n / 8));
          const IVec3 d{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
          for (int i = 0; i < k; i++) {
            const uint32_t h = rng::Hash3(RefHash(r.id), c.tick, (uint32_t)i);
            const IVec3 cell{lo.x + (int)(h % (uint32_t)d.x), lo.y + (int)((h >> 8) % (uint32_t)d.y),
                             lo.z + (int)((h >> 16) % (uint32_t)d.z)};
            if (c.world != nullptr && !c.world->CellInWindow(cell)) continue;
            c.ops->cells.push_back({World::SlotCellIndex(cell), PackVoxNew(mat, 0) | kCellOpIfAir});
            L.ops++;
            L.cells++;
          }
        }
      }
      if (c.tick < L.until) return;
      // Spikes go back down (conditional: what replaced one stays).
      if (c.ops != nullptr)
        for (const auto& [cell, mat] : L.placed) {
          if (c.world != nullptr && !c.world->CellInWindow(cell)) continue;
          c.ops->cells.push_back({World::SlotCellIndex(cell), CellOpClearIfMat(mat)});
          L.ops++;
        }
      L.placed.clear();
      L.phase = TrapPhase::Idle;
      L.note = type == "spikes" ? "spikes down" : "quiet";
      return;
    }
  }
}

// ---- spawners -------------------------------------------------------------------------
std::string SlotId(const Ref& r, size_t i) { return r.id + "/s" + std::to_string(i); }
int SpawnerCount(const Ref& r) { return std::clamp(PropInt(r, "count", 1), 1, kMaxSpawnerCount); }

// "never" -> -1; "onLeave" -> -2; "ticks:N" -> N.
int RespawnRule(const Ref& r) {
  const std::string s = PropStr(r, "respawn", "never");
  if (s == "onLeave") return -2;
  if (s.compare(0, 6, "ticks:") == 0) {
    const int n = std::atoi(s.c_str() + 6);
    return n > 0 ? n : -1;
  }
  return -1;
}
bool RespawnValid(const std::string& s) {
  if (s == "never" || s == "onLeave") return true;
  return s.compare(0, 6, "ticks:") == 0 && std::atoi(s.c_str() + 6) > 0;
}
// "always" -> ""; "onEnter(x)" / "onEnter:x" -> "x"; anything else -> "?".
std::string TriggerRegion(const Ref& r) {
  const std::string t = PropStr(r, "trigger", "always");
  if (t == "always") return std::string();
  if (t.compare(0, 8, "onEnter(") == 0 && t.size() > 9 && t.back() == ')')
    return t.substr(8, t.size() - 9);
  if (t.compare(0, 8, "onEnter:") == 0 && t.size() > 8) return t.substr(8);
  return "?";
}

void SaveSpawner(RefCtx& c, const Ref& r, const SpawnerLive& L) {
  std::vector<uint32_t> w = {L.serial, L.triggered ? 1u : 0u, (uint32_t)L.slots.size()};
  for (const SpawnSlot& s : L.slots) {
    w.push_back(s.serial);
    w.push_back(s.state);
    w.push_back(s.deadTick);
  }
  WriteWords(c.refs, r, w);
}

// The stand-in npc ref a spawned creature's schedule runs on (refs_npc.h).
Ref StandIn(const Ref& spawner, const std::string& slotId) {
  Ref s;
  s.id = slotId;
  s.kind = "npc";
  s.group = spawner.group;
  s.pos = spawner.pos;
  s.yaw = spawner.yaw;
  s.props = spawner.props;
  s.hash = RefHash(slotId);
  return s;
}

void Dress(RefCtx& c, const Ref& r, uint64_t id) {
  if (c.items == nullptr || c.mobs == nullptr) return;
  if (r.props.contains("outfit") && r.props["outfit"].is_array())
    for (const Json& e : r.props["outfit"]) {
      if (!e.is_string()) continue;
      std::string nm = e.get<std::string>();
      uint32_t dye = 0;
      if (const size_t h = nm.find('#'); h != std::string::npos) {
        dye = DyeParseHex(nm.substr(h));
        nm = nm.substr(0, h);
      }
      const ItemDef* it = c.items->Named(nm);
      const int slot = it != nullptr && ItemKindIsWorn(it->kind) ? EquipSlotFor(it->kind, Equipment{}) : -1;
      if (it == nullptr || slot < 0 || !c.mobs->WearItem(id, it, slot, dye))
        c.refs->Warn("refs: " + r.id + ": props.outfit: \"" + nm + "\" is not a wearable item");
    }
  const std::string weapon = PropStr(r, "weapon");
  if (!weapon.empty()) {
    const ItemDef* it = c.items->Named(weapon);
    if (it == nullptr || !c.mobs->EquipItem(id, it))
      c.refs->Warn("refs: " + r.id + ": props.weapon: no item \"" + weapon + "\" to hold");
  }
  // An archer profile arms itself (game/mob_archery.h ArmArcher): its bow and
  // a quiver. Every other profile is untouched.
  ArmArcher(*c.mobs, id, *c.items);
}

// One spawn into slot `si`. False = not now (ground unknown, crowd full).
bool SpawnInto(RefCtx& c, const Ref& r, SpawnerLive& L, size_t si) {
  MobSystem& mobs = *c.mobs;
  const SpawnPlan plan = PlanSpawn(r, L.serial + 1);
  if (!plan.ok) {
    L.note = "no mob to spawn (props.mobs / base)";
    return false;
  }
  const int def = mobs.FindDef(plan.base);
  if (def < 0) {
    L.note = "no mob def \"" + plan.base + "\"";
    return false;
  }
  const Vec3 ws = mobs.Defs()[(size_t)def].worldSize;
  // THE GROUND RULE (the npc kind's): place a body only onto ground the
  // fetch cache has answered for since this spawner first asked -- and only
  // where that ground is OPEN. The radius is a disc at the spawner's own y and
  // knows nothing of the room's walls, and the mob ground probe climbs at most
  // 1.2 m out of matter, so a point drawn into the host rock is a creature
  // entombed for good. Each draw is tested against the cached words: the body
  // column above the foot cell clear of solid and powder (the foot cell itself
  // may be the floor), with something to stand on at or below the foot. A
  // closed draw takes the next attempt (PlanSpawn's own salt, so the answer is
  // still the serial's and the cache's, never the tick's); after kSpawnTries
  // the spawner's own pos, which its author put in the room.
  Vec3 foot = plan.foot;
  if (c.world != nullptr) {
    if (L.askedAt == 0) L.askedAt = c.tick;
    const int h = std::max(2, (int)std::ceil(ws.y));
    bool placed = false;
    for (uint32_t k = 0; k <= kSpawnTries && !placed; k++) {
      const Vec3 f = k == 0             ? plan.foot
                     : k < kSpawnTries ? PlanSpawn(r, L.serial + 1, k).foot
                                       : Vec3{(float)r.pos.x + 0.5f, (float)r.pos.y, (float)r.pos.z + 0.5f};
      const IVec3 cell{(int)std::floor(f.x), (int)std::floor(f.y), (int)std::floor(f.z)};
      const IVec3 lo{cell.x, cell.y - 16, cell.z}, hi{cell.x, cell.y + h - 1, cell.z};
      if (!CacheFresh(*c.world, lo, hi, L.askedAt, c.tick, L.fetchAt)) {
        L.note = "waiting for the ground";
        return false;
      }
      auto blocks = [&](uint32_t m) {
        if (m == 0) return false;
        const uint32_t cls = ClassOf(c, m);
        return cls == CLASS_SOLID || cls == CLASS_POWDER;
      };
      bool open = true;
      for (int y = cell.y + 1; y <= hi.y && open; y++)
        open = !blocks(CachedWord(*c.world, {cell.x, y, cell.z}) & 0xFFFu);
      bool support = false;
      for (int y = cell.y; y >= lo.y && open && !support; y--) {
        const uint32_t m = CachedWord(*c.world, {cell.x, y, cell.z}) & 0xFFFu;
        support = m != 0 && (blocks(m) || ClassOf(c, m) == CLASS_LIQUID);
      }
      if ((open && support) || k == kSpawnTries) {
        foot = f;
        placed = true;
      }
    }
  }
  if (!mobs.HasRoomToSpawn()) {
    L.note = "the crowd is full";
    return false;
  }
  const IVec3 origin{(int)std::floor(foot.x - ws.x * 0.5f), (int)std::floor(foot.y),
                     (int)std::floor(foot.z - ws.z * 0.5f)};
  const uint64_t id = mobs.Spawn(def, origin);
  if (id == 0) return false;
  L.askedAt = 0;
  const std::string sid = SlotId(r, si);
  if (Mob* m = mobs.FindMobById(id)) m->SetRefId(sid);
  mobs.SetHeading(id, (float)r.yaw * 3.14159265f / 180.0f);
  const std::string beh = PropStr(r, "behavior");
  if (!beh.empty() && !mobs.SetMobBehavior(id, beh))
    c.refs->Warn("refs: " + r.id + ": props.behavior: no behaviour profile \"" + beh + "\"");
  Dress(c, r, id);
  L.serial++;
  SpawnSlot& s = L.slots[si];
  s.serial = L.serial;
  s.state = 1;
  s.missing = 0;
  s.base = plan.base;
  ForgetResident(sid);
  L.note = "spawned " + plan.base + " into slot " + std::to_string(si);
  return true;
}

void TickSpawner(RefCtx& c, const Ref& r, SpawnerLive& L) {
  if (c.mobs == nullptr) return;
  bool dirty = false;
  if (!L.triggered) {
    const std::string reg = TriggerRegion(r);
    if (reg.empty()) {
      L.triggered = true;
    } else if (const Ref* t = FindNamed(*c.refs, r, reg); t != nullptr) {
      IVec3 lo, hi;
      if (RegionOf(*c.refs, *t, lo, hi))
        for (const BodyRef& b : Bodies(c, true, false))
          if (FeetIn(b.foot, lo, hi)) L.triggered = true;
    }
    if (!L.triggered) return;
    dirty = true;
  }
  const int rule = RespawnRule(r);
  int spawnSlot = -1;
  const bool schedule = !PropStr(r, "schedule").empty();
  for (size_t i = 0; i < L.slots.size(); i++) {
    SpawnSlot& s = L.slots[i];
    const std::string sid = SlotId(r, i);
    Mob* m = c.mobs->FindMobByRef(sid);
    if (s.state == 1) {
      if (m != nullptr && !m->Alive()) {
        s.state = 2;
        s.deadTick = c.tick;
        dirty = true;
        ForgetResident(sid);
      } else if (m == nullptr) {
        if (++s.missing > kSpawnMissingTicks) {
          s.state = 2;
          s.deadTick = c.tick;
          dirty = true;
          ForgetResident(sid);
        }
      } else {
        s.missing = 0;
        if (schedule) StepScheduled(c, StandIn(r, sid));
      }
    } else if (s.state == 2) {
      if (rule > 0 && c.tick - s.deadTick >= (uint32_t)rule && spawnSlot < 0) spawnSlot = (int)i;
    } else if (spawnSlot < 0) {
      spawnSlot = (int)i;
    }
  }
  // ONE spawn per spawner per tick: bounded however many slots are empty.
  if (spawnSlot >= 0 && SpawnInto(c, r, L, (size_t)spawnSlot)) dirty = true;
  if (dirty) SaveSpawner(c, r, L);
}

// ---- the one tick -----------------------------------------------------------------------
void TickDungeon(RefCtx& c) {
  if (c.refs == nullptr) return;
  DungeonWorld& D = DW();
  Reindex(*c.refs);
  if (D.active.empty() && D.signalDoors.empty()) return;   // rule 2: nothing here
  // 1. Emitters: plates feel their bodies, pulsed levers spring back.
  for (auto& [id, P] : D.plates) {
    const Ref* r = c.refs->Find(id);
    if (r == nullptr) continue;
    IVec3 lo, hi;
    BoxOfRef(*r, SizeProp(*r, "size", {8, 2, 8}), lo, hi);
    const std::string who = PropStr(*r, "who", "any");
    bool pressed = false;
    for (const BodyRef& b : Bodies(c, who != "mob", who != "player"))
      if (FeetIn(b.foot, lo, hi)) pressed = true;
    P.pressed = pressed;
    if (pressed && PropBool(*r, "latch", false) && !PlateLatched(*c.refs, *r)) WriteWords(c.refs, *r, {1u});
  }
  for (auto& [id, V] : D.levers) {
    if (V.pulseUntil == 0 || c.tick < V.pulseUntil) continue;
    V.pulseUntil = 0;
    if (const Ref* r = c.refs->Find(id)) WriteWords(c.refs, *r, {0u});
  }
  // 2. Receivers, in id order: doors, gates, traps.
  for (const std::string& id : D.signalDoors) {
    if (!c.refs->IsActive(id)) continue;
    if (const Ref* r = c.refs->Find(id)) DoorDrive(c, *r, ReceiverLevel(*c.refs, *r));
  }
  for (auto& [id, G] : D.gates)
    if (const Ref* r = c.refs->Find(id)) TickGate(c, *r, G);
  for (auto& [id, T] : D.traps)
    if (const Ref* r = c.refs->Find(id)) TickTrap(c, *r, T);
  // 3. Spawners (and their creatures' schedules).
  for (auto& [id, S] : D.spawners)
    if (const Ref* r = c.refs->Find(id)) TickSpawner(c, *r, S);
}

void Deact(const std::string& id) {
  DungeonWorld& D = DW();
  D.active.erase(id);
  D.gates.erase(id);
  D.traps.erase(id);
  D.plates.erase(id);
  D.levers.erase(id);
}

}  // namespace

// ---- public helpers ---------------------------------------------------------------------

void BoxOfRef(const Ref& r, IVec3 size, IVec3& lo, IVec3& hi) {
  const IVec3 right = HeadingAxis(r.yaw + 90), fwd = HeadingAxis(r.yaw);
  const IVec3 a = r.pos;
  const IVec3 b{r.pos.x + (size.x - 1) * right.x + (size.z - 1) * fwd.x, r.pos.y + size.y - 1,
                r.pos.z + (size.x - 1) * right.z + (size.z - 1) * fwd.z};
  lo = {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
  hi = {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

bool RegionOf(const RefStore& s, const Ref& r, IVec3& lo, IVec3& hi, std::string* why) {
  if (r.kind == "region" || r.kind == "plate") {
    BoxOfRef(r, SizeProp(r, "size", r.kind == "plate" ? IVec3{8, 2, 8} : IVec3{10, 10, 10}), lo, hi);
    return true;
  }
  if (!r.props.contains("region")) {
    if (why) *why = "props.region is missing";
    return false;
  }
  const Json& g = r.props["region"];
  if (g.is_array()) {
    BoxOfRef(r, SizeProp(r, "region", {1, 1, 1}), lo, hi);
    return true;
  }
  if (g.is_string()) {
    const Ref* t = FindNamed(s, r, g.get<std::string>());
    if (t == nullptr || (t->kind != "region" && t->kind != "plate")) {
      if (why) *why = "props.region: \"" + g.get<std::string>() + "\" is not a region or plate ref";
      return false;
    }
    BoxOfRef(*t, SizeProp(*t, "size", t->kind == "plate" ? IVec3{8, 2, 8} : IVec3{10, 10, 10}), lo,
             hi);
    return true;
  }
  if (why) *why = "props.region: a [sx, sy, sz] box or the name of a region / plate ref";
  return false;
}

bool SignalLevel(RefStore& s, const std::string& group, const std::string& signal) {
  Reindex(s);
  auto it = DW().emitters.find(group + "|" + signal);
  if (it == DW().emitters.end()) return false;
  for (const std::string& id : it->second) {
    const Ref* r = s.Find(id);
    if (r == nullptr) continue;
    if (r->kind == "lever" && LeverOn(s, *r)) return true;
    if (r->kind == "plate") {
      if (PlateLatched(s, *r)) return true;
      auto p = DW().plates.find(id);
      if (p != DW().plates.end() && p->second.pressed) return true;
    }
  }
  return false;
}

SpawnPlan PlanSpawn(const Ref& r, uint32_t serial, uint32_t attempt) {
  SpawnPlan p;
  struct Opt {
    std::string base;
    uint32_t w;
  };
  std::vector<Opt> opts;
  if (r.props.contains("mobs") && r.props["mobs"].is_array())
    for (const Json& e : r.props["mobs"]) {
      if (!e.is_object() || !e.contains("base") || !e["base"].is_string()) continue;
      const double w = e.contains("weight") && e["weight"].is_number() ? e["weight"].get<double>() : 1.0;
      if (w <= 0.0) continue;
      opts.push_back({e["base"].get<std::string>(), std::max(1u, (uint32_t)std::lround(w * 1000.0))});
    }
  if (opts.empty()) opts.push_back({r.base.empty() ? std::string("human") : r.base, 1000u});
  uint64_t total = 0;
  for (const Opt& o : opts) total += o.w;
  const uint32_t key = RefHash(r.id);
  uint64_t pick = rng::Hash3(key, serial, 0x5A1u) % total;
  size_t k = 0;
  for (; k + 1 < opts.size(); k++) {
    if (pick < opts[k].w) break;
    pick -= opts[k].w;
  }
  p.base = opts[k].base;
  // WHERE: an integer point within `radius` (metres) of pos, hashed on the
  // serial -- never the tick, so a respawn lands where its number says.
  // Attempt k > 0 is a fresh draw on its own salt (SpawnInto's open test).
  const int R = std::max(0, (int)std::lround(PropFloat(r, "radius", 1.5f) * 10.0f));
  int dx = 0, dz = 0;
  if (R > 0) {
    const uint32_t h = rng::Hash3(key, serial, 0x5A2u + 0x100u * attempt);
    dx = (int)(h % (uint32_t)(2 * R + 1)) - R;
    dz = (int)((h >> 16) % (uint32_t)(2 * R + 1)) - R;
    while (dx * dx + dz * dz > R * R) {
      dx /= 2;
      dz /= 2;
    }
  }
  p.foot = Vec3{(float)(r.pos.x + dx) + 0.5f, (float)r.pos.y, (float)(r.pos.z + dz) + 0.5f};
  p.ok = true;
  return p;
}

bool GateStatusOf(const std::string& id, GateStatus& out) {
  auto it = DW().gates.find(id);
  if (it == DW().gates.end()) return false;
  out.raised = it->second.raised;
  out.wantOpen = it->second.wantOpen;
  out.built = it->second.built;
  out.steps = it->second.steps;
  out.height = 0;
  if (DW().idxFor != nullptr)
    if (const Ref* r = DW().idxFor->Find(id)) out.height = GateGeometry(*r).height;
  return true;
}

bool TrapStatusOf(const std::string& id, TrapStatus& out) {
  auto it = DW().traps.find(id);
  if (it == DW().traps.end()) return false;
  const TrapLive& L = it->second;
  out.fires = L.fires;
  out.done = L.done;
  out.cells = L.cells;
  out.ops = L.ops;
  out.hits = L.hits;
  out.wallT = L.wallT;
  out.note = L.note;
  out.type.clear();
  if (DW().idxFor != nullptr)
    if (const Ref* r = DW().idxFor->Find(id)) out.type = PropStr(*r, "type");
  return true;
}

bool SpawnerStatusOf(const std::string& id, SpawnerStatus& out) {
  auto it = DW().spawners.find(id);
  if (it == DW().spawners.end()) return false;
  const SpawnerLive& L = it->second;
  out = SpawnerStatus{};
  out.serial = L.serial;
  out.triggered = L.triggered;
  out.note = L.note;
  for (size_t i = 0; i < L.slots.size(); i++) {
    out.slots.push_back(id + "/s" + std::to_string(i));
    out.bases.push_back(L.slots[i].base);

  }
  return true;
}

void ResetDungeonEntities() {
  DungeonWorld& D = DW();
  D.gates.clear();
  D.traps.clear();
  D.spawners.clear();
  D.plates.clear();
  D.levers.clear();
  D.active.clear();
  D.idxFor = nullptr;
  D.idxRev = ~0ull;
}

// ---- the kinds ----------------------------------------------------------------------------

void RegisterDungeonKinds() {
  AddStoreGoneHook(&StoreGone);

  // ---- lever ----
  RefKind lever;
  lever.name = "lever";
  lever.validate = [](const Ref& r, std::vector<std::string>& p) {
    for (const char* k : {"signal", "label"})
      if (r.props.contains(k) && !r.props[k].is_string()) p.push_back(std::string("props.") + k + ": text");
    if (r.props.contains("startOn") && !r.props["startOn"].is_boolean()) p.push_back("props.startOn: true or false");
    if (r.props.contains("pulse") && (!r.props["pulse"].is_number() || r.props["pulse"].get<double>() < 0.0))
      p.push_back("props.pulse: seconds >= 0 (0 = a toggle)");
  };
  lever.activate = [](RefCtx&, const Ref& r) {
    DW().active.insert(r.id);
    DW().levers[r.id] = LeverLive{};
    return Activation::Done;
  };
  lever.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    Deact(r.id);
    if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  lever.usePrompt = [](RefCtx&, const Ref& r) { return PropStr(r, "label", "Pull lever"); };
  lever.onUse = [](RefCtx& c, const Ref& r, RefUse& u) {
    if (c.refs == nullptr) return;
    const float pulse = PropFloat(r, "pulse", 0.0f);
    const bool now = pulse > 0.0f ? true : !LeverOn(*c.refs, r);
    WriteWords(c.refs, r, {now ? 1u : 0u});
    if (pulse > 0.0f) DW().levers[r.id].pulseUntil = c.tick + std::max(1u, Secs(pulse));
    u.message = now ? "The lever goes down with a clank." : "The lever goes back up.";
  };
  lever.useRadius = 5.0f;
  Kinds().Register(std::move(lever));

  // ---- plate ----
  RefKind plate;
  plate.name = "plate";
  plate.validate = [](const Ref& r, std::vector<std::string>& p) {
    SizeValid(r, "size", p);
    if (r.props.contains("signal") && !r.props["signal"].is_string()) p.push_back("props.signal: text");
    if (r.props.contains("latch") && !r.props["latch"].is_boolean()) p.push_back("props.latch: true or false");
    const std::string who = PropStr(r, "who", "any");
    if (who != "any" && who != "player" && who != "mob") p.push_back("props.who: \"any\", \"player\" or \"mob\"");
  };
  plate.activate = [](RefCtx&, const Ref& r) {
    DW().active.insert(r.id);
    DW().plates[r.id] = PlateLive{};
    return Activation::Done;
  };
  plate.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    Deact(r.id);
    if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  Kinds().Register(std::move(plate));

  // ---- region ----
  RefKind region;
  region.name = "region";
  region.validate = [](const Ref& r, std::vector<std::string>& p) { SizeValid(r, "size", p); };
  Kinds().Register(std::move(region));

  // ---- gate (the one tick of every kind here rides on it) ----
  RefKind gate;
  gate.name = "gate";
  gate.validate = [](const Ref& r, std::vector<std::string>& p) {
    const GateGeom g = GateGeometry(r);
    if (!g.ok) p.push_back(g.why);
    if (r.props.contains("on") && !r.props["on"].is_string()) p.push_back("props.on: the signal that opens it (text)");
    if (r.props.contains("invert") && !r.props["invert"].is_boolean()) p.push_back("props.invert: true or false");
  };
  gate.activate = [](RefCtx& c, const Ref& r) -> Activation {
    const GateGeom g = GateGeometry(r);
    if (!g.ok) return Activation::Done;
    GateLive L;
    const std::vector<uint32_t> w = ReadWords(c.refs, r);
    if (w.size() >= 2) {
      L.raised = std::clamp((int)w[0], 0, g.height);
      L.built = w[1] != 0;
    }
    if (!L.built && c.ops != nullptr && c.world != nullptr) {
      // BUILT ONCE, the first time it is ever in the window: IfAir, so it
      // never overwrites what is there.
      IVec3 lo, hi;
      g.Box(lo, hi);
      if (!BoxInWindow(c.world, lo, hi)) return Activation::Retry;
      const uint32_t mat = MatId(c, g.material);
      if (mat == 0) {
        c.refs->Warn("refs: " + r.id + ": props.material: no material \"" + g.material + "\"");
        return Activation::Done;
      }
      GateDiff(c, g, PackVoxNew(mat, 0), mat, -1, L.raised);
      L.built = true;
      WriteWords(c.refs, r, {(uint32_t)L.raised, 1u});
    }
    L.lastStep = c.tick;
    DW().active.insert(r.id);
    DW().gates[r.id] = L;
    return Activation::Done;
  };
  gate.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    Deact(r.id);
    if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  gate.tick = [](RefCtx& c) { TickDungeon(c); };
  Kinds().Register(std::move(gate));

  // ---- trap ----
  RefKind trap;
  trap.name = "trap";
  trap.validate = [](const Ref& r, std::vector<std::string>& p) {
    const std::string t = PropStr(r, "type");
    if (t != "collapse_floor" && t != "spikes" && t != "dart" && t != "flood" && t != "fire_vent")
      p.push_back("props.type: collapse_floor, spikes, dart, flood or fire_vent");
    if (!r.props.contains("on") || !r.props["on"].is_string())
      p.push_back("props.on: the signal (or a plate's name) that springs it");
    if (t != "dart" && !r.props.contains("region"))
      p.push_back("props.region: a [sx, sy, sz] box or the name of a region / plate ref");
    if (r.props.contains("region") && r.props["region"].is_array()) SizeValid(r, "region", p);
    if (r.yaw % 90 != 0) p.push_back("yaw: use 0, 90, 180 or 270");
  };
  trap.activate = [](RefCtx& c, const Ref& r) {
    TrapLive L;
    const std::vector<uint32_t> w = ReadWords(c.refs, r);
    if (w.size() >= 2) {
      L.fires = w[0];
      L.done = w[1] != 0;
    }
    // A collapse that fired and did not finish (the window left) finishes now.
    if (L.fires > 0 && !L.done && PropStr(r, "type") == "collapse_floor") {
      L.phase = TrapPhase::Fetch;
      L.reqTick = c.tick;
    }
    if (c.refs != nullptr) L.lastLevel = ReceiverLevel(*c.refs, r);   // no fire on arrival
    DW().active.insert(r.id);
    DW().traps[r.id] = std::move(L);
    return Activation::Done;
  };
  trap.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    auto it = DW().traps.find(r.id);
    // Spikes caught mid-thrust go back down (the ops are this tick's).
    if (it != DW().traps.end() && c.ops != nullptr && why != RefEvent::Reset)
      for (const auto& [cell, mat] : it->second.placed)
        if (c.world != nullptr && c.world->CellInWindow(cell))
          c.ops->cells.push_back({World::SlotCellIndex(cell), CellOpClearIfMat(mat)});
    Deact(r.id);
    if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  Kinds().Register(std::move(trap));

  // ---- key ----
  RefKind key;
  key.name = "key";
  key.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (!r.props.contains("item") && !r.props.contains("keyId"))
      p.push_back("props.item (a key item id) or props.keyId (the lock it opens)");
    for (const char* k : {"item", "keyId", "label"})
      if (r.props.contains(k) && !r.props[k].is_string()) p.push_back(std::string("props.") + k + ": text");
  };
  key.activate = [](RefCtx&, const Ref& r) {
    DW().active.insert(r.id);
    return Activation::Done;
  };
  key.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    Deact(r.id);
    if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  auto keyItem = [](RefCtx& c, const Ref& r) -> const ItemDef* {
    if (c.items == nullptr) return nullptr;
    const std::string item = PropStr(r, "item");
    if (!item.empty()) return c.items->Named(item);
    const std::string kid = PropStr(r, "keyId");
    for (const ItemDef& d : c.items->items)
      if (d.kind == ItemKind::Key && d.keyId == kid) return &d;
    return nullptr;
  };
  auto taken = [](RefCtx& c, const Ref& r) {
    const std::vector<uint32_t> w = ReadWords(c.refs, r);
    return !w.empty() && w[0] != 0;
  };
  key.usePrompt = [keyItem, taken](RefCtx& c, const Ref& r) -> std::string {
    if (taken(c, r)) return "";
    const std::string label = PropStr(r, "label");
    if (!label.empty()) return label;
    const ItemDef* d = keyItem(c, r);
    std::string n = d != nullptr ? d->name : PropStr(r, "item", "key");
    std::replace(n.begin(), n.end(), '_', ' ');
    return "Take " + n;
  };
  key.onUse = [keyItem, taken](RefCtx& c, const Ref& r, RefUse& u) {
    if (taken(c, r)) {
      u.message = "There is nothing there now.";
      return;
    }
    const ItemDef* d = keyItem(c, r);
    if (d == nullptr) {
      u.message = "The key is not a thing this world knows (props.item / keyId).";
      return;
    }
    if (u.session != nullptr && c.items != nullptr) {
      Kit& kit = u.session->kit();
      ItemInstance it;
      it.name = d->name;
      it.count = 1;
      const ItemStack got = KitStackFrom(it, *c.items);
      if (kit.bag.Add(got) < 0 && kit.hotbar.Add(got) < 0) {
        u.message = "You have no room for the " + d->name + ".";
        return;
      }
    }
    WriteWords(c.refs, r, {1u});
    std::string n = d->name;
    std::replace(n.begin(), n.end(), '_', ' ');
    u.message = "You take the " + n + ".";
  };
  key.useRadius = 5.0f;
  Kinds().Register(std::move(key));

  // ---- spawner ----
  RefKind sp;
  sp.name = "spawner";
  sp.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (r.props.contains("mobs")) {
      const Json& m = r.props["mobs"];
      bool ok = m.is_array() && !m.empty();
      if (ok)
        for (const Json& e : m)
          ok &= e.is_object() && e.contains("base") && e["base"].is_string() &&
                (!e.contains("weight") || (e["weight"].is_number() && e["weight"].get<double>() > 0.0));
      if (!ok) p.push_back("props.mobs: a list like [{\"base\": \"zombie\", \"weight\": 2}]");
    }
    if (r.props.contains("count") &&
        (!r.props["count"].is_number_integer() || r.props["count"].get<int>() < 1 ||
         r.props["count"].get<int>() > kMaxSpawnerCount))
      p.push_back("props.count: 1..16 creatures alive at once");
    if (r.props.contains("radius") && (!r.props["radius"].is_number() || r.props["radius"].get<double>() < 0.0))
      p.push_back("props.radius: metres >= 0");
    if (r.props.contains("respawn") && (!r.props["respawn"].is_string() || !RespawnValid(r.props["respawn"].get<std::string>())))
      p.push_back("props.respawn: \"never\", \"onLeave\" or \"ticks:N\"");
    if (TriggerRegion(r) == "?") p.push_back("props.trigger: \"always\" or \"onEnter(<region ref>)\"");
    for (const char* k : {"schedule", "behavior", "weapon"})
      if (r.props.contains(k) && !r.props[k].is_string()) p.push_back(std::string("props.") + k + ": text");
  };
  sp.activate = [](RefCtx& c, const Ref& r) {
    SpawnerLive L;
    L.slots.resize((size_t)SpawnerCount(r));
    const std::vector<uint32_t> w = ReadWords(c.refs, r);
    if (w.size() >= 3) {
      L.serial = w[0];
      L.triggered = w[1] != 0;
      const size_t n = std::min<size_t>(w[2], L.slots.size());
      for (size_t i = 0; i < n && 3 + i * 3 + 2 < w.size(); i++) {
        L.slots[i].serial = w[3 + i * 3];
        L.slots[i].state = w[3 + i * 3 + 1];
        L.slots[i].deadTick = w[3 + i * 3 + 2];
        L.slots[i].base = PlanSpawn(r, L.slots[i].serial).base;
      }
    }
    // onLeave: the dead come back when the player does.
    if (RespawnRule(r) == -2)
      for (SpawnSlot& s : L.slots)
        if (s.state == 2) s.state = 0;
    DW().active.insert(r.id);
    DW().spawners[r.id] = std::move(L);
    return Activation::Done;
  };
  sp.deactivate = [](RefCtx& c, const Ref& r, RefEvent why) {
    auto it = DW().spawners.find(r.id);
    if (it != DW().spawners.end())
      for (size_t i = 0; i < it->second.slots.size(); i++) {
        const std::string sid = SlotId(r, i);
        ForgetResident(sid);
        // The author changed or removed the spawner: its creatures go too
        // (the npc kind's rule), and the new line starts from nothing.
        if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.mobs != nullptr)
          if (Mob* m = c.mobs->FindMobByRef(sid)) c.mobs->RemoveMob(m->Id());
      }
    DW().spawners.erase(r.id);
    Deact(r.id);
    if ((why == RefEvent::Edited || why == RefEvent::Deleted) && c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  Kinds().Register(std::move(sp));
}

}  // namespace refs
