// mob_stealth.cpp -- MobSystem's half of stealth and detection (game/stealth.h,
// docs/PLAN_stealth.md): what a body looks like to a watcher (the light on
// it, the noise it makes, its skill), the noise events every brain hears one
// tick later, the sneak-attack test, and the HUD's report of who is watching.
//
// The METER itself lives with the AI (ai_behavior.cpp UpdateAwareness): this
// file only measures the world for it, and it measures from the same stores
// the line of sight reads -- the chunk caches and the tick's day phase and
// rain word -- so a replay of the same ticks measures the same light.

#include <algorithm>
#include <climits>
#include <cmath>

#include "game/ai_behavior.h"
#include "game/mob.h"
#include "game/stealth.h"
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace {

// A material glows enough to LIGHT a body at this emission and up. Fainter
// self-lit matter (glowing moss at 18, a flower's sheen) is an art choice --
// it shows in the dark without lighting what stands beside it -- and a meadow
// of it summed would read as a bonfire.
constexpr uint32_t kLampEmission = 60;

// How often a body's light and worn metal are re-measured (ticks), staggered
// by id. Both change on the scale of a walk across a room, not of a tick.
constexpr uint32_t kLightEvery = 8;
constexpr uint32_t kMetalEvery = 30;

// Emitter chunks within this many chunks of a body are summed (horizontal,
// vertical): 4 x 16 = 64 voxels, a little over twice stealth.emitterRadius's
// default -- past that a cell contributes under a fifth of its glow.
constexpr int kEmitReachXZ = 4;
constexpr int kEmitReachY = 2;

// THE SKY OVER A POINT, ASKING NOTHING. MobSystem::OpenToSky's column walk
// (nothing but air and gas for kSkyProbeCells above), except that a chunk the
// cache does not hold reads OPEN and is NOT requested: this is a watcher's
// question about a body, asked for every creature every few ticks, and a
// fetch request per miss would queue in front of the ground probes and the
// dissolution scans that actually need the chunk (World::FetchSource, one
// FIFO, kFetchPerTick a tick).
constexpr int kSkyProbeCells = 48;
bool OpenSkyNoFetch(const World& world, const std::vector<MaterialGpu>& mats,
                    const Vec3& p) {
  const int x = ifloor(p.x), z = ifloor(p.z);
  const int y0 = ifloor(p.y) + 1;
  IVec3 memo{INT_MIN, INT_MIN, INT_MIN};
  const CachedChunk* cc = nullptr;
  for (int y = y0; y < y0 + kSkyProbeCells; y++) {
    const IVec3 c{x, y, z};
    if (!world.CellInWindow(c)) return true;
    const IVec3 wc{x >> 4, y >> 4, z >> 4};
    if (wc.x != memo.x || wc.y != memo.y || wc.z != memo.z) {
      memo = wc;
      cc = world.Cached(wc);
    }
    if (cc == nullptr || cc->voxels.size() != kChunkVol) {
      // Unknown: skip to the next chunk up rather than call it a roof.
      y = ((y >> 4) << 4) + 15;
      continue;
    }
    const uint32_t m = cc->voxels[((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) * kChunk +
                                  (uint32_t)(x & 15)] & 0xFFFu;
    if (m != 0 && m < mats.size() && mats[m].klass != CLASS_GAS) return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE TABLES (OnMaterialsReloaded)
//
// A FOOTSTEP'S LOUDNESS per material is DERIVED, not authored: from the sound
// set its footstep already plays (a material that crunches like dry branches
// is loud to a listener for the same reason it is loud to the player), and
// failing that from its class. Thief's rule -- moss quiet, metal loud -- falls
// out of the sound sets the materials already carry.
void MobSystem::BuildStealthTables(const std::vector<MaterialDef>& mats) {
  matStepNoise_.assign(mats.size(), 1.0f);
  matMetal_.assign(mats.size(), 0);
  matGlow_.assign(mats.size(), 0.0f);
  for (size_t i = 0; i < mats.size(); i++) {
    const MaterialDef& m = mats[i];
    bool metal = false;
    for (const std::string& t : m.tags) metal |= t == "metal";
    matMetal_[i] = metal ? 1 : 0;
    matGlow_[i] = m.gpu.emission >= kLampEmission ? (float)m.gpu.emission / 255.0f : 0.0f;
    const std::string& step = m.Sound("footstep");
    float k = 1.0f;
    if (step == "leaf") k = 0.7f;          // grass, moss, leaf litter
    else if (step == "branch") k = 1.5f;   // twigs snap
    else if (step == "metal") k = 1.4f;
    else if (step == "stone") k = 1.1f;
    else if (step == "path") k = 0.9f;     // packed earth
    else if (m.gpu.klass == CLASS_POWDER) k = 0.75f;   // sand, snow, ash
    else if (metal) k = 1.4f;
    matStepNoise_[i] = k;
  }
}

// ---------------------------------------------------------------------------
// NOISE EVENTS
void MobSystem::PostNoise(const stealth::Noise& n) {
  if (!(n.radius > 0.0f)) return;
  if (noiseNext_.size() >= kMaxNoises) {
    // FULL: the quietest goes, this one only if it is louder. Push order is
    // the tick's own deterministic order, so which one goes is too.
    auto q = std::min_element(noiseNext_.begin(), noiseNext_.end(),
                              [](const stealth::Noise& a, const stealth::Noise& b) {
                                return a.radius < b.radius;
                              });
    noiseRefused_++;
    if (q->radius < n.radius) *q = n;
    return;
  }
  noiseNext_.push_back(n);
}

// ---------------------------------------------------------------------------
// THE LIGHT ON A BODY
//
// Two terms, both from the tick:
//   SKY      five columns over the head (OpenSkyNoFetch: nothing but air and
//            gas for 48 cells -- a roof, a cave ceiling and a tree's canopy
//            are all shade; an unfetched chunk is open and NOT requested)
//            give an open fraction, and stealth::SkyLight turns
//            that and the day phase and the rain into a level. A body below
//            the ANALYTIC ground (World::TerrainHeight, worldgen's own height
//            contract) is underground whatever the caches say: a cave whose
//            ceiling was never fetched must not read as open sky.
//   GLOW     every glowing cell nearby, through per-chunk sums (EmitterOf),
//            each weighted 1 / (1 + (d / emitterRadius)^2). Not occluded: a
//            fire behind a wall still lights you, the price of not casting a
//            ray per cell. Unfetched chunks contribute nothing -- this asks
//            no fetches (a watcher's question about somebody else must not
//            queue a request every ground probe waits behind).
float MobSystem::LightAt(World& world, const Vec3& centre, float heightVox) {
  const stealth::Params& P = stealthP_;
  const Vec3 head{centre.x, centre.y + 0.4f * heightVox, centre.z};
  float open = 0.0f;
  const int ground =
      World::TerrainHeight(ifloor(head.x), ifloor(head.z), world.WorldSeed());
  if ((float)ground <= head.y + 4.0f) {
    static const int kOff[5][2] = {{0, 0}, {5, 0}, {-5, 0}, {0, 5}, {0, -5}};
    int n = 0;
    for (const auto& o : kOff)
      if (OpenSkyNoFetch(world, matGpu_, Vec3{head.x + (float)o[0], head.y, head.z + (float)o[1]}))
        n++;
    open = (float)n / 5.0f;
  }
  const float sky = stealth::SkyLight(P, DaylightStrengthCpu(dayPhase_), open,
                                      weatherRain_ & kRainAmountMask);
  float glow = 0.0f;
  if (!matGlow_.empty()) {
    const IVec3 c0{ifloor(centre.x) >> 4, ifloor(centre.y) >> 4, ifloor(centre.z) >> 4};
    const float R = std::max(1.0f, P.emitterRadius);
    for (int dz = -kEmitReachXZ; dz <= kEmitReachXZ; dz++)
      for (int dy = -kEmitReachY; dy <= kEmitReachY; dy++)
        for (int dx = -kEmitReachXZ; dx <= kEmitReachXZ; dx++) {
          const EmitterSum* es = EmitterOf(world, IVec3{c0.x + dx, c0.y + dy, c0.z + dz});
          if (es == nullptr || es->glow <= 0.0f) continue;
          const float d = (es->centroid - centre).len();
          glow += es->glow / (1.0f + (d / R) * (d / R));
        }
  }
  return std::clamp(sky + stealth::EmitterLight(P, glow), 0.0f, 1.0f);
}

const MobSystem::EmitterSum* MobSystem::EmitterOf(World& world, IVec3 chunk) {
  if (!world.ChunkInWindow(chunk)) return nullptr;
  const CachedChunk* cc = world.Cached(chunk);
  if (cc == nullptr || cc->voxels.size() != kChunkVol) return nullptr;
  const uint64_t key = World::PackChunkKey(chunk);
  auto it = emitters_.find(key);
  if (it != emitters_.end() && it->second.version == cc->version) return &it->second;
  // Bounded like the cache it shadows (rule 2): past 4096 summaries the whole
  // table goes and refills from what is asked next.
  if (it == emitters_.end() && emitters_.size() >= 4096) {
    emitters_.clear();
    it = emitters_.end();
  }
  EmitterSum es;
  es.version = cc->version;
  float g = 0.0f;
  Vec3 acc{};
  for (uint32_t z = 0; z < kChunk; z++)
    for (uint32_t y = 0; y < kChunk; y++)
      for (uint32_t x = 0; x < kChunk; x++) {
        const uint32_t m = cc->voxels[(z * kChunk + y) * kChunk + x] & 0xFFFu;
        if (m == 0 || m >= matGlow_.size()) continue;
        const float gl = matGlow_[m];
        if (gl <= 0.0f) continue;
        g += gl;
        acc += Vec3{(float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f} * gl;
      }
  es.glow = g;
  es.centroid = g > 0.0f
                    ? Vec3{(float)(chunk.x * (int)kChunk), (float)(chunk.y * (int)kChunk),
                           (float)(chunk.z * (int)kChunk)} +
                          acc * (1.0f / g)
                    : Vec3{};
  EmitterSum& slot = emitters_[key];
  slot = es;
  return &slot;
}

// ---------------------------------------------------------------------------
// WHO IS WHO
uint64_t MobSystem::ActorIdOf(const Mob& m) const {
  for (size_t i = 0; i < avatars_.size(); i++)
    if (avatars_[i] == &m) return ai::kPlayerActorBase + (uint64_t)i;
  return m.id_;
}

// The share of a body's dress that is METAL, 0..1, by PIECE: a piece is
// metal when most of its shells' voxels are, and a full harness is five of
// them (the stock iron set: helm, cuirass, gauntlets, greaves, sabatons).
// By piece rather than by voxel because a breastplate is louder than its
// voxel count says: it clanks against everything else.
float MobSystem::WornMetalOf(const Mob& m) const {
  if (matMetal_.empty()) return 0.0f;
  int pieces = 0;
  for (int p = 0; p < m.WornPieceCount(); p++) {
    uint64_t metal = 0, total = 0;
    for (int slot : m.WornSlotsAt(p)) {
      if (slot < 0 || slot >= (int)m.limbs_.size()) continue;
      const MobLimb& l = m.limbs_[(size_t)slot];
      auto count = [&](uint32_t mat) {
        total++;
        if (mat < matMetal_.size() && matMetal_[mat]) metal++;
      };
      if (l.HasFineSkin())
        for (const PrefabVoxel& v : l.skinVoxels) count(v.material);
      else
        for (const DebrisVoxel& v : l.voxels) count(v.payload & 0xFFFu);
    }
    if (total > 0 && metal * 2 > total) pieces++;
  }
  return std::clamp((float)pieces / 5.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// HOW HIDDEN A BODY IS (PreTick, beside PublishCombatant)
void MobSystem::PublishStealth(World& world, Mob& m, ai::Actor& a, uint32_t tick,
                               const PlayerActorDesc* desc) {
  stealth::Exposure& e = a.stealth;
  const stealth::Params& P = stealthP_;
  // WHO TAKES PART. A player (on foot: the session says so) always does. An
  // NPC does when its profile authors a `stealth` block: stealth is a trait
  // of creatures that have one, in both directions -- a body with no sneak
  // to it (a training dummy, a legacy fixture, a `duelist`) is a plain body
  // every watcher sees at once, exactly as before stealth existed.
  const ai::Profile* pr = behaviors_.At(m.ai_.profile);
  const bool takesPart = desc != nullptr ? desc->stealth
                                         : pr != nullptr && pr->stealth.enabled;
  if (!takesPart) {
    e = stealth::Exposure{};
    return;
  }
  e.modelled = true;
  // THE SKILL: the character's own, else (an NPC) its profile's or (a
  // player) the player default.
  if (m.def_ != nullptr && m.def_->stealthSneak >= 0.0f) e.sneak = m.def_->stealthSneak;
  else if (desc != nullptr) e.sneak = P.playerSneak;
  else e.sneak = pr->stealth.sneak;
  // THE STANCE AND THE WALK.
  float wading = 0.0f;
  if (desc != nullptr) {
    e.stance = desc->stance <= (uint8_t)stealth::Stance::Prone ? (stealth::Stance)desc->stance
                                                              : stealth::Stance::Stand;
    e.moveFrac = std::max(0.0f, desc->moveFrac);
    wading = std::clamp(desc->wading, 0.0f, 1.0f);
  } else {
    e.stance = m.aiCrouch_ ? stealth::Stance::Crouch : stealth::Stance::Stand;
    const float walk = m.def_ != nullptr ? std::max(m.def_->speed, 1.0f) : 40.0f;
    e.moveFrac = std::max(0.0f, m.speedNow_) / walk;
    // A swimmer IS in the water; its noise is its own business (a shark is
    // not splashing). A walker's legs in liquid are.
    const bool swimmer = m.def_ != nullptr && m.def_->swim.present;
    wading = swimmer ? 0.0f : std::clamp(a.immersion * 2.0f, 0.0f, 1.0f);
  }
  // THE LIGHT, on a cadence.
  if (!m.stealthLightValid_ || (tick + (uint32_t)m.id_) % kLightEvery == 0) {
    m.stealthLight_ = LightAt(world, a.centre, a.height);
    m.stealthLightValid_ = true;
    m.stealthLightAt_ = tick;
  }
  e.light = m.stealthLight_;
  float hp = 1.0f, burn = 0.0f;
  int lost = 0;
  m.BodyFacts(hp, burn, lost);
  e.burning = burn;
  // THE NOISE: its footsteps (and the clank of its dress).
  if (!m.wornMetalValid_ || (tick + (uint32_t)m.id_) % kMetalEvery == 0) {
    m.wornMetal_ = WornMetalOf(m);
    m.wornMetalValid_ = true;
    m.wornMetalAt_ = tick;
  }
  const float surface = m.lastFootMat_ != 0 && m.lastFootMat_ < matStepNoise_.size()
                            ? matStepNoise_[m.lastFootMat_]
                            : 1.0f;
  e.noise = stealth::MovementNoise(P, e.stance, e.moveFrac, m.wornMetal_, surface,
                                   wading, e.sneak);
  // ...and a LANDING, as an event: a thump heard once.
  if (m.landNoise_ > 0.0f) {
    stealth::Noise n;
    n.pos = n.point = a.centre;
    n.radius = P.landNoise * std::clamp(m.landNoise_ / std::max(1.0f, P.landRefSpeed), 0.0f, 3.0f) *
               (1.0f - P.sneakNoiseCut * std::clamp(e.sneak / 100.0f, 0.0f, 1.0f)) *
               (e.stance == stealth::Stance::Stand ? 1.0f : P.crouchNoise);
    n.source = a.id;
    n.faction = a.faction;
    n.kind = stealth::NoiseKind::Land;
    PostNoise(n);
    m.landNoise_ = 0.0f;
  }
}

const stealth::Exposure& MobSystem::PlayerExposure(size_t i) const {
  static const stealth::Exposure kNone{};
  return i < playerExposure_.size() ? playerExposure_[i] : kNone;
}

// ---------------------------------------------------------------------------
// SNEAK ATTACKS
float MobSystem::SneakAttackMul(const Mob& victim, uint64_t attackerActorId, bool ranged,
                                float itemMul) const {
  if (attackerActorId == 0 || !victim.alive_ || victim.def_ == nullptr) return 1.0f;
  if (victim.PlayerControlled()) return 1.0f;   // a player has no meter to catch
  if (attackerActorId == victim.id_) return 1.0f;
  if (ai::Detects(victim.ai_, behaviors_, attackerActorId)) return 1.0f;
  // THE ATTACKER, as the actor list published it: a body nobody models (a
  // gate's fixture) never sneak attacks, and with sneakNeedsCrouch neither
  // does one standing up.
  const ai::Actor* att = nullptr;
  for (const ai::Actor& a : actors_)
    if (a.id == attackerActorId) {
      att = &a;
      break;
    }
  if (att == nullptr || !att->stealth.modelled) return 1.0f;
  if (stealthP_.sneakNeedsCrouch && att->stealth.stance == stealth::Stance::Stand)
    return 1.0f;
  if (itemMul > 0.0f) return std::max(1.0f, itemMul);
  return std::max(1.0f, ranged ? stealthP_.sneakRanged : stealthP_.sneakMelee);
}

void MobSystem::NoteAttacked(Mob& victim, uint64_t attackerActorId, Vec3 attackerPos,
                             bool ranged, uint32_t tick) {
  if (attackerActorId == 0 || !victim.alive_ || victim.PlayerControlled()) return;
  Vec3 where = attackerPos;
  if (ranged) {
    // IT DID NOT SEE THE ARCHER: it knows roughly which way the arrow came
    // from. Blurred by a fifth of the distance (and a body or so), drawn from
    // (victim, tick) so a replay searches the same spot.
    const Vec3 vc = victim.origin_ + (victim.def_ ? victim.def_->worldSize * 0.5f : Vec3{});
    const float d = (attackerPos - vc).len();
    const float blur = 0.2f * d + 6.0f;
    const uint32_t h = rng::Hash3((uint32_t)victim.id_ ^ 0x5E4A7u, tick, 9u);
    where.x += blur * rng::SignedUnit(h);
    where.z += blur * rng::SignedUnit(h >> 16);
  }
  ai::NoteAttacked(victim.ai_, behaviors_, attackerActorId, where, ranged, tick, stealthP_);
}

// ---------------------------------------------------------------------------
// THE HUD'S REPORT
MobSystem::StealthReport MobSystem::StealthOf(uint64_t actorId) const {
  StealthReport r;
  const float caution = stealthP_.cautionAt;
  for (const Mob& m : mobs_) {
    if (!m.alive_ || m.def_ == nullptr || m.PlayerControlled()) continue;
    const ai::Brain& b = m.ai_;
    float level = ai::AwarenessOf(b, actorId);
    const bool fighting = b.hasTarget && b.targetId == actorId;
    if (fighting) {
      r.fighting++;
      level = std::max(level, b.visible ? 1.0f : level);
    }
    if (level <= 0.0f) continue;
    r.maxLevel = std::max(r.maxLevel, level);
    if (level < caution && !fighting) continue;
    r.watchers++;
    if ((int)b.alert > (int)r.alert) r.alert = b.alert;
    StealthWatcher w;
    w.pos = m.origin_ + m.def_->worldSize * 0.5f;
    w.level = level;
    w.alert = fighting ? stealth::Alert::Combat : b.alert;
    // The strongest few, kept sorted.
    int at = r.topN;
    while (at > 0 && r.top[at - 1].level < w.level) at--;
    if (at < StealthReport::kTop) {
      for (int k = std::min(r.topN, StealthReport::kTop - 1); k > at; k--) r.top[k] = r.top[k - 1];
      r.top[at] = w;
      r.topN = std::min(r.topN + 1, StealthReport::kTop);
    }
  }
  return r;
}
