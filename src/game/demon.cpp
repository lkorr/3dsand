// demon.cpp — summoning, the circle's hold, the goof. See demon.h.
#include "game/demon.h"
#include "game/demon_talk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "game/mob.h"
#include "game/session.h"
#include "game/spell.h"
#include "game/workpool.h"
#include "sim/materials.h"
#include "sim/scale.h"
#include "sim/world.h"

namespace sandvox {
std::string AssetDir();   // test/support.cpp: the one asset-path chokepoint
}

using json = nlohmann::json;

// ---- content ---------------------------------------------------------------------

bool DebugAllDemonNames() {
  static const bool on = [] {
    const char* e = std::getenv("SANDVOX_ALL_NAMES");
    return e != nullptr && e[0] != '\0' && e[0] != '0';
  }();
  return on;
}

bool LoadDemons(const std::string& dir, DemonLibrary& out, std::string& log) {
  out = DemonLibrary{};
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    log += "demons: no directory " + dir + "\n";
    return false;
  }
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());   // file order is load order: deterministic
  for (const auto& p : files) {
    if (demon::IsSealFile(p.stem().string())) {   // D3: seals.json, tells.json
      demon::LoadSealFile(p.string(), p.stem().string(), out.seals, log);
      continue;
    }
    // D5: the contract tariff and the stock pages (game/contract.h reads them).
    if (p.stem() == "contract_tariff" || p.stem() == "contracts") continue;
    json j;
    try {
      std::ifstream f(p);
      j = json::parse(f, nullptr, true, true);
    } catch (const std::exception& ex) {
      log += "demons: " + p.filename().string() + ": " + ex.what() + "\n";
      continue;
    }
    const std::string stem = p.stem().string();
    if (stem == "circle") {
      DemonCircleCfg& c = out.circle;
      c.material = j.value("material", c.material);
      c.radiusMaxM = std::clamp(j.value("radiusMaxM", c.radiusMaxM), 1.0f, 12.0f);
      c.slabBelow = std::clamp(j.value("slabBelow", c.slabBelow), 0, 3);
      c.slabAbove = std::clamp(j.value("slabAbove", c.slabAbove), 0, 3);
      c.recheckTicks = std::clamp(j.value("recheckTicks", c.recheckTicks), 1, 300);
      c.leadTicks = std::clamp(j.value("leadTicks", c.leadTicks), 1, 60);
      c.floorSearch = std::clamp(j.value("floorSearch", c.floorSearch), 1, 48);
      c.minEighths = std::clamp(j.value("minEighths", c.minEighths), 1, 8);
      continue;
    }
    DemonDef d;
    d.id = stem;
    d.name = j.value("name", stem);
    d.mob = j.value("mob", std::string());
    d.behavior = j.value("behavior", std::string());
    d.released = j.value("released", std::string());
    d.dialogue = j.value("dialogue", std::string());
    d.tier = std::clamp(j.value("tier", 1), 1, 3);
    d.power = std::max(0, j.value("power", 10));
    d.gaze = j.value("gaze", std::string("avert"));
    if (d.gaze != "hold" && d.gaze != "avert") {
      log += "demons: " + stem + ": gaze \"" + d.gaze + "\" is not hold | avert\n";
      d.gaze = "avert";
    }
    if (j.contains("resist") && j["resist"].is_object())
      for (auto it = j["resist"].begin(); it != j["resist"].end(); ++it)
        if (it.value().is_number()) d.resist.emplace_back(it.key(), it.value().get<int32_t>());
    for (const json& s : j.value("schemes", json::array()))
      if (s.is_string()) d.schemes.push_back(s.get<std::string>());
    if (d.mob.empty()) {
      log += "demons: " + stem + ": names no mob\n";
      continue;
    }
    out.defs.push_back(std::move(d));
  }
  return true;
}

const char* DemonStateName(DemonState s) {
  return s == DemonState::Contained ? "contained"
         : s == DemonState::Released  ? "released"
                                      : "unbound";
}

// ---- the stores ------------------------------------------------------------------
//
// The T-4 snapshot's 3x3x3 mirror first, then the on-demand fetch cache: the
// same two stores, in the same order, that a spell's strike reads
// (game/lightning.cpp StrikeChunkWords). Both are pure functions of the tick.

namespace {

struct StoreCtx {
  const World* world = nullptr;
  const std::vector<MaterialDef>* mats = nullptr;
};

const uint32_t* ChunkWords(const World& world, IVec3 wc) {
  const WorldSnapshot& s = world.Snap();
  if (s.valid) {
    const int cx = wc.x - s.mirrorBase.x, cy = wc.y - s.mirrorBase.y, cz = wc.z - s.mirrorBase.z;
    if (cx >= 0 && cy >= 0 && cz >= 0 && cx < 3 && cy < 3 && cz < 3) {
      const size_t base = (size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol;
      if (base + kChunkVol <= s.mirror.size()) return s.mirror.data() + base;
    }
  }
  const CachedChunk* cc = world.Cached(wc);
  if (cc != nullptr && cc->voxels.size() == kChunkVol) return cc->voxels.data();
  return nullptr;
}

bool InMirror(const World& world, IVec3 wc) {
  const WorldSnapshot& s = world.Snap();
  if (!s.valid) return false;
  const int cx = wc.x - s.mirrorBase.x, cy = wc.y - s.mirrorBase.y, cz = wc.z - s.mirrorBase.z;
  return cx >= 0 && cy >= 0 && cz >= 0 && cx < 3 && cy < 3 && cz < 3;
}

uint32_t StoreWordAt(void* ctx, int32_t x, int32_t y, int32_t z, bool& known) {
  const StoreCtx& c = *(const StoreCtx*)ctx;
  const uint32_t* w = ChunkWords(*c.world, IVec3{x >> 4, y >> 4, z >> 4});
  known = w != nullptr;
  if (!w) return 0;
  return w[(size_t)(((z & 15) * (int)kChunk + (y & 15)) * (int)kChunk + (x & 15))] & 0xFFFFu;
}

bool StorePassable(void* ctx, uint32_t m) {
  const StoreCtx& c = *(const StoreCtx*)ctx;
  if (m == 0) return true;
  return m < c.mats->size() && (*c.mats)[m].gpu.klass == CLASS_GAS;
}

uint32_t MatByName(const std::vector<MaterialDef>& mats, const std::string& n) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == n) return (uint32_t)i;
  return 0;
}

// Ask the fetch cache for every chunk in [lo, hi] the mirror does not hold,
// NEAREST THE MIDDLE FIRST, up to `cap` (a fill reads outward from its start,
// so the near chunks are the ones that decide). Coalesced by the world if
// already queued; refused if not resident.
void RequestMissing(World& world, IVec3 lo, IVec3 hi, int32_t cap) {
  const IVec3 mid{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
  std::vector<std::pair<int64_t, IVec3>> want;
  for (int cz = lo.z; cz <= hi.z; cz++)
    for (int cy = lo.y; cy <= hi.y; cy++)
      for (int cx = lo.x; cx <= hi.x; cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc) || InMirror(world, wc)) continue;
        const int64_t dx = cx - mid.x, dy = cy - mid.y, dz = cz - mid.z;
        want.push_back({dx * dx + dy * dy + dz * dz, wc});
      }
  // Distance, then coordinates: a total order, so the requests are too.
  std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    if (a.second.z != b.second.z) return a.second.z < b.second.z;
    if (a.second.y != b.second.y) return a.second.y < b.second.y;
    return a.second.x < b.second.x;
  });
  for (size_t i = 0; i < want.size() && (int32_t)i < cap; i++)
    world.RequestChunkFetch(want[i].second);
}

CircleParams ParamsOf(const DemonLibrary& lib, const std::vector<MaterialDef>& mats) {
  CircleParams p;
  p.saltMat = MatByName(mats, lib.circle.material);
  p.minEighths = (uint32_t)lib.circle.minEighths;
  p.radiusMax = std::max(1, (int32_t)std::lround(MetresToCells(lib.circle.radiusMaxM)));
  p.slabBelow = lib.circle.slabBelow;
  p.slabAbove = lib.circle.slabAbove;
  return p;
}

void EnsureLoaded(DemonWorld& d, bool reload) {
  if (d.loaded && !reload) return;
  std::string log;
  LoadDemons(sandvox::AssetDir() + "/demons", d.lib, log);
  d.loaded = true;
  if (!log.empty() && log != d.loadLog) std::fprintf(stderr, "%s", log.c_str());
  d.loadLog = log;
}

void Refund(std::span<SessionTick> players, int session, int32_t tariff) {
  if (tariff <= 0) return;
  for (SessionTick& p : players)
    if (p.s->index == session) {
      CasterState& m = p.s->caster.mana;
      m.mana = std::max(m.mana, (int32_t)std::min<int64_t>((int64_t)m.manaMax,
                                                           (int64_t)m.mana + tariff));
    }
}

// THE GOOF, AND THE BREAK: point the demon at the one who called it.
void AimAtSummoner(MobSystem& mobs, std::span<SessionTick> players, const LiveDemon& ld,
                   uint32_t tick) {
  ai::Brain* b = mobs.MobBrainMut(ld.mobId);
  if (b == nullptr) return;
  for (SessionTick& p : players)
    if (p.s->index == ld.session) {
      b->targetId = ai::kPlayerActorBase + (uint64_t)ld.session;
      b->hasTarget = true;
      b->targetPos = p.s->player.pos;
      b->lastSeenPos = p.s->player.pos;
      b->lastSeenTick = tick;
    }
}

// The fence MobSystem asks before a contained demon moves or strikes.
bool FenceAllow(void* ctx, uint64_t mobId, MobFence::Kind kind, float fromX, float fromZ,
                float toX, float toZ) {
  // SERIAL ONLY (fight64 M's work pool): MobSystem asks the fence from
  // DecideIntent / DriveLocomotion, which run in PreTick's serial creature
  // loop, never inside a workpool::ParallelFor task. This answer WRITES the
  // demon world's counters (moves/blows refused, a pact's blow tallies and
  // pending penalty), so a call from a task would be a race and an order the
  // replay cannot reproduce: refuse loudly rather than run one.
  if (workpool::InTask()) {
    std::fprintf(stderr, "demons: FATAL: the fence was asked from a work-pool task\n");
    std::abort();
  }
  DemonWorld& d = *(DemonWorld*)ctx;
  for (LiveDemon& ld : d.live) {
    if (ld.mobId != mobId) continue;
    // D5: a bound demon's blows answer to its contract (demon_talk.h).
    if (kind == MobFence::Blow && ld.pact && ld.state == DemonState::Released && d.fenced) {
      const ai::Brain* b = d.fenced->MobBrain(mobId);
      return demon::PactAllowBlow(d, ld, b && b->hasTarget ? b->targetId : 0);
    }
    if (ld.state != DemonState::Contained) return true;
    if (ld.circle.Inside(toX, toZ)) return true;
    // D3: a blow across the ring is the `touch` channel. Unsevered (not iron
    // enough for this demon in the band) it is a LOOPHOLE: the claws reach.
    if (kind == MobFence::Blow && !ld.bind.Severed(demon::Channel::Touch)) {
      ld.bind.blowsLoophole++;
      return true;
    }
    // Knocked out of its circle while contained (a blast, a shove): it may
    // walk back TOWARD the inside, never further out.
    if (kind == MobFence::Move && !ld.circle.Inside(fromX, fromZ)) {
      const float ax = fromX - ld.circle.cx, az = fromZ - ld.circle.cz;
      const float bx = toX - ld.circle.cx, bz = toZ - ld.circle.cz;
      if (bx * bx + bz * bz < ax * ax + az * az) return true;
    }
    if (kind == MobFence::Move) {
      ld.movesRefused++;
      d.stats.movesRefused++;
    } else {
      ld.blowsRefused++;
      d.stats.blowsRefused++;
    }
    return false;
  }
  return true;
}

}  // namespace

void DemonUnbind(TickAuthorityCtx& w, std::span<SessionTick> players, LiveDemon& ld,
                 uint32_t tick, const std::string& why) {
  ld.state = DemonState::Unbound;
  ld.stateTick = tick;
  ld.why = why;
  ld.pact.reset();   // D5: loose is loose -- the contract is void, the upkeep freed
  // A released demon going loose gets its hostile profile back.
  if (const DemonDef* def = w.demons ? w.demons->lib.Find(ld.demon) : nullptr)
    if (!def->behavior.empty()) w.mobs.SetMobBehavior(ld.mobId, def->behavior);
  AimAtSummoner(w.mobs, players, ld, tick);
  std::printf("demons: %s unbound at tick %u: %s\n", ld.name.c_str(), tick, why.c_str());
}

DemonWorld::~DemonWorld() {
  if (fenced != nullptr && fenced->Fence().ctx == this) fenced->SetFence(MobFence{});
}

DemonWorld& Demons(TickAuthorityCtx& w) {
  if (!w.demons) w.demons = std::make_shared<DemonWorld>();
  return *w.demons;
}

void DemonQueueSummon(TickAuthorityCtx& w, std::span<SessionTick> players, int session,
                      const SpellSummon& su, const GlyphLibrary& glyphs, uint32_t tick) {
  DemonWorld& d = Demons(w);
  // Read the library again at every cast: an edit is live on the next one.
  EnsureLoaded(d, true);
  const GlyphDef* g = glyphs.At(su.glyph);
  const DemonDef* def = g && g->summon.has ? d.lib.Find(g->summon.demon) : nullptr;
  if (def == nullptr || d.pending.size() >= DemonWorld::kMaxPending) {
    d.stats.refused++;
    Refund(players, session, su.tariff);
    std::fprintf(stderr, "demons: summon refused (%s)\n",
                 def == nullptr ? "no such demon" : "too many in flight");
    return;
  }
  PendingSummon p;
  p.at = IVec3{su.x, su.y, su.z};
  p.glyph = su.glyph;
  p.demon = def->id;
  p.session = session;
  p.tariff = su.tariff;
  p.castTick = tick;
  p.dueTick = tick + (uint32_t)d.lib.circle.leadTicks;
  d.pending.push_back(p);
  // Everything the arrival may read: the floor search under the impact and
  // the fill's whole reach round it.
  const int32_t R = std::max(1, (int32_t)std::lround(MetresToCells(d.lib.circle.radiusMaxM))) + 1;
  RequestMissing(w.world,
                 IVec3{(su.x - R) >> 4, (su.y - d.lib.circle.floorSearch - 2) >> 4, (su.z - R) >> 4},
                 IVec3{(su.x + R) >> 4, (su.y + 3) >> 4, (su.z + R) >> 4},
                 DemonWorld::kMaxFetchPerSummon);
}

uint64_t DemonArrive(TickAuthorityCtx& w, std::span<SessionTick> players,
                     const PendingSummon& p, uint32_t tick) {
  DemonWorld& d = Demons(w);
  EnsureLoaded(d, false);
  const DemonDef* def = d.lib.Find(p.demon);
  const int defIndex = def ? w.mobs.FindDef(def->mob) : -1;
  if (def == nullptr || defIndex < 0 || d.live.size() >= DemonWorld::kMaxLive) {
    d.stats.refused++;
    Refund(players, p.session, p.tariff);
    std::fprintf(stderr, "demons: arrival refused (%s)\n",
                 def == nullptr ? "no such demon" : defIndex < 0 ? "no such mob" : "too many");
    return 0;
  }
  StoreCtx sc{&w.world, &w.mats};
  CircleProbe probe{&StoreWordAt, &sc, &StorePassable};
  int32_t feetY = p.at.y;
  bool known = true;
  const bool floor = CircleFindFeet(probe, p.at, d.lib.circle.floorSearch, feetY, known);
  CircleShape circle;
  if (floor) {
    circle = ScanCircle(probe, ParamsOf(d.lib, w.mats), p.at.x, feetY, p.at.z);
  } else {
    circle.verdict = CircleVerdict::Unknown;
    feetY = p.at.y;
  }
  const MobDef& md = w.mobs.Defs()[(size_t)defIndex];
  const uint64_t id = w.mobs.Spawn(defIndex, IVec3{p.at.x - (int)(md.worldSize.x * 0.5f), feetY,
                                                   p.at.z - (int)(md.worldSize.z * 0.5f)});
  if (id == 0) {
    d.stats.refused++;
    Refund(players, p.session, p.tariff);
    std::fprintf(stderr, "demons: arrival refused (spawn failed)\n");
    return 0;
  }
  if (!def->behavior.empty()) w.mobs.SetMobBehavior(id, def->behavior);
  LiveDemon ld;
  ld.mobId = id;
  ld.demon = def->id;
  ld.name = def->name;
  ld.session = p.session;
  ld.spawnTick = ld.stateTick = ld.lastCheck = tick;
  ld.circle = std::move(circle);
  d.stats.summons++;
  if (ld.circle.Closed()) {
    ld.state = DemonState::Contained;
    ld.why = "called into a closed salt circle";
    d.stats.contained++;
  } else {
    ld.state = DemonState::Unbound;
    ld.why = !floor ? "no floor under the name"
                    : std::string("called loose: the circle is ") +
                          CircleVerdictName(ld.circle.verdict);
    d.stats.loose++;
    AimAtSummoner(w.mobs, players, ld, tick);
  }
  std::printf("demons: %s arrives at (%d,%d,%d) %s (%s; %d inside, %d salt, %d visited)\n",
              def->name.c_str(), p.at.x, feetY, p.at.z, DemonStateName(ld.state),
              CircleVerdictName(ld.circle.verdict), ld.circle.cells, ld.circle.ringCells,
              ld.circle.visited);
  if (ld.circle.verdict == CircleVerdict::Unknown) {
    // WHY it could not see: the first unseen cell and what the stores held.
    const WorldSnapshot& s = w.world.Snap();
    const IVec3 u = ld.circle.unknownAt;
    std::printf("demons:   unseen cell (%d,%d,%d) chunk (%d,%d,%d); snapshot %s tick %u, mirror "
                "base (%d,%d,%d), %zu words; cached %s\n",
                u.x, u.y, u.z, u.x >> 4, u.y >> 4, u.z >> 4, s.valid ? "valid" : "INVALID", s.tick,
                s.mirrorBase.x, s.mirrorBase.y, s.mirrorBase.z, s.mirror.size(),
                w.world.Cached(IVec3{u.x >> 4, u.y >> 4, u.z >> 4}) ? "yes" : "no");
  }
  d.live.push_back(std::move(ld));
  return id;
}

void DemonTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick) {
  if (!w.demons) {
    // Nothing was ever summoned in this world: nothing to do, no fence.
    return;
  }
  DemonWorld& d = *w.demons;
  // ---- arrivals, in cast order ------------------------------------------------
  if (!d.pending.empty()) {
    std::vector<PendingSummon> due;
    size_t keep = 0;
    for (size_t i = 0; i < d.pending.size(); i++) {
      if (d.pending[i].dueTick <= tick) due.push_back(d.pending[i]);
      else d.pending[keep++] = d.pending[i];
    }
    d.pending.resize(keep);
    for (const PendingSummon& p : due) DemonArrive(w, players, p, tick);
  }
  // ---- the living: gone, broken, held ------------------------------------------
  const WorldSnapshot& snap = w.world.Snap();
  StoreCtx sc{&w.world, &w.mats};
  CircleProbe probe{&StoreWordAt, &sc, &StorePassable};
  bool anyContained = false;
  size_t keep = 0;
  for (size_t i = 0; i < d.live.size(); i++) {
    LiveDemon& ld = d.live[i];
    if (!w.mobs.IsAlive(ld.mobId)) continue;   // dead or gone: forget it
    if (ld.state == DemonState::Contained) {
      // RE-READ when a chunk the circle overlaps is awake in the snapshot, or
      // on the cadence. Never twice in a tick.
      bool due = tick >= ld.lastCheck + (uint32_t)d.lib.circle.recheckTicks;
      const IVec3 lo = ld.circle.ChunkLo(), hi = ld.circle.ChunkHi();
      if (!due && tick > ld.lastCheck && snap.valid)
        for (int cz = lo.z; cz <= hi.z && !due; cz++)
          for (int cy = lo.y; cy <= hi.y && !due; cy++)
            for (int cx = lo.x; cx <= hi.x && !due; cx++) {
              const IVec3 wc{cx, cy, cz};
              if (!w.world.ChunkInWindow(wc) || !InMirror(w.world, wc)) continue;
              const uint32_t si = World::SlotChunkIndex(wc);
              due = si < snap.dirtyFlags.size() && snap.dirtyFlags[si] != 0;
            }
      if (due) {
        ld.lastCheck = tick;
        d.stats.rechecks++;
        // From where the demon stands if that is inside, else from where it
        // arrived: the question is whether ITS circle is still a loop.
        int32_t sx = ld.circle.startX, sz = ld.circle.startZ;
        if (const Mob* m = w.mobs.FindMobById(ld.mobId)) {
          const Vec3 o = m->Origin();
          const MobDef* md = m->Def();
          const float fx = o.x + (md ? md->worldSize.x * 0.5f : 0.0f);
          const float fz = o.z + (md ? md->worldSize.z * 0.5f : 0.0f);
          if (ld.circle.Inside(fx, fz)) {
            sx = (int32_t)std::floor(fx);
            sz = (int32_t)std::floor(fz);
          }
        }
        CircleShape c = ScanCircle(probe, ParamsOf(d.lib, w.mats), sx, ld.circle.feetY, sz);
        if (c.verdict == CircleVerdict::Unknown) {
          d.stats.held++;   // cannot see it: HOLD, and ask for the chunks again
          RequestMissing(w.world, lo, hi, DemonWorld::kMaxFetchPerSummon);
        } else if (c.Closed()) {
          ld.circle = std::move(c);
        } else {
          // BROKEN: unbound at once, and it remembers who called it.
          d.stats.broken++;
          DemonUnbind(w, players, ld, tick,
                      std::string("the circle broke (") + CircleVerdictName(c.verdict) + ")");
        }
      }
    }
    if (keep != i) d.live[keep] = std::move(ld);
    keep++;
  }
  d.live.resize(keep);
  // ---- D3: release presses, the seal band, the move loophole, gaze, strength --------
  demon::SealsTick(w, players, tick, probe);
  for (const LiveDemon& ld : d.live)
    anyContained = anyContained || ld.state == DemonState::Contained ||
                   (ld.pact && ld.state == DemonState::Released);   // D5: a contract fences blows
  // ---- the fence -------------------------------------------------------------------
  if (anyContained) {
    MobFence f;
    f.allow = &FenceAllow;
    f.ctx = &d;
    w.mobs.SetFence(f);
    d.fenced = &w.mobs;
  } else if (d.fenced != nullptr) {
    if (d.fenced->Fence().ctx == &d) d.fenced->SetFence(MobFence{});
    d.fenced = nullptr;
  }
  // ---- the HUD line: each summoner's most recent demon ------------------------------
  for (SessionTick& p : players) {
    UIState& ui = p.s->sink ? p.s->sink->ui : w.ui;
    ui.demonState = 0;
    ui.demonName.clear();
    for (auto it = d.live.rbegin(); it != d.live.rend(); ++it)
      if (it->session == p.s->index) {
        ui.demonState = it->state == DemonState::Contained  ? 1
                        : it->state == DemonState::Released ? 3
                                                             : 2;
        ui.demonName = it->name;
        ui.demonRadiusM = it->state == DemonState::Contained
                              ? it->circle.radius / (float)kVoxelsPerMetre
                              : 0.0f;
        demon::FillHud(ui, *it);
        break;
      }
  }
}
