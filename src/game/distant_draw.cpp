// distant_draw.cpp -- the statue bake, the pose cache and the draw producer
// (game/distant_draw.h; docs/PLAN_distant_npcs.md §2.9, package D4a).
//
// THE BAKE, in four steps, all on the CPU and none of it touching the rig:
//
//   1. SOURCE. Every drawn limb's micro brick is read cell by cell into a list
//      of SOURCE VOXELS in the limb's own frame (world voxels from the brick's
//      min corner): material, resolved colour (art palette + the garment's
//      dye, as microbody.wgsl would shade it), body coat (blood), and whether
//      the voxel is EXPOSED (an empty 6-neighbour, or the brick's edge on a
//      face that is not a joint). The brick is what the player SEES -- carves,
//      char, blood, hair tucked under a hood -- which is why the statue is
//      read from it and not from the def. Interior voxels are subsampled at
//      the downsample ratio: they only decide the cells no exposed voxel
//      reaches, which are inside the body.
//   2. POSE. The creature's own skeleton is FK-posed by its own clips
//      (AnimSampleAndBlend + AnimFlatten, no Jolt, no pose pipeline): idle,
//      two walk frames (the walk clip's arm swing plus a procedural scissor on
//      the leg chains, which the runtime drives by IK and no clip carries), a
//      strike frame (the clip's farthest reach), and lying (a corpse's REAL
//      ragdoll pose when baked from a corpse; else idle tipped onto its back).
//      Each limb is placed by Mob::LimbTargetFor's arithmetic; a FOLLOWER (a
//      worn shell, a held item, an embedded arrow) keeps the rigid offset it
//      has from its host in the live rig.
//   3. MERGE + DOWNSAMPLE. Every posed source voxel is binned into ONE lattice
//      at kDistantCellsPerMetre (5 cm). A cell is SOLID IF ANY SOURCE VOXEL
//      LANDS IN IT -- the most-opaque rule, so a finger, a blade or a strand
//      of hair 1 cm thick still owns its 5 cm cell at 50 m -- and takes the
//      (material, colour) with the most weight, where an exposed voxel weighs
//      16 and an interior one 1: the cell shows its SKIN, not the meat behind
//      it. A coat survives when at least a quarter of the cell's exposed
//      voxels carry one.
//   4. GROUND. Each pose's lowest cell is put at the item's origin.y.
//
// THE PACK is lazy (step 5, Produce): a pose becomes a shared micro model
// (MicroBodyPack at scale kDistantStatueScale) the first frame it is drawn, in
// ONE art bank (a model has one), its colours mapped into that bank: an
// existing entry within kArtNear, else a new entry while the bank and the
// statues' allowance have room, else the nearest. Packed poses live in an LRU
// under the word budget.

#include "game/distant_draw.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/anim.h"
#include "game/dye.h"
#include "game/mob.h"
#include "sim/microbody.h"
#include "sim/voxload.h"

namespace {

DistantDrawRunStats g_run;

// ---- keys: 64-bit FNV-1a -------------------------------------------------------
constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
uint64_t FnvBytes(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * kFnvPrime;
  return h;
}
uint64_t FnvU64(uint64_t h, uint64_t v) { return FnvBytes(h, &v, sizeof(v)); }
uint64_t FnvStr(uint64_t h, const std::string& s) {
  h = FnvBytes(h, s.data(), s.size());
  return FnvU64(h, (uint64_t)s.size());   // "ab"+"c" != "a"+"bc"
}
uint64_t NonZero(uint64_t h) { return h ? h : 1u; }
uint64_t LookHash(uint64_t def, uint64_t gear, uint64_t damage) {
  uint64_t h = FnvU64(kFnvBasis, def);
  h = FnvU64(h, gear);
  h = FnvU64(h, damage);
  return NonZero(h);
}
DistantLookKey MakeKey(uint64_t def, uint64_t gear, uint64_t damage) {
  DistantLookKey k;
  k.def = def;
  k.full = LookHash(def, gear, damage);
  k.pristine = LookHash(def, gear, 0);
  k.bare = LookHash(def, 0, 0);
  return k;
}

// ---- the vote --------------------------------------------------------------
// An exposed voxel outvotes sixteen interior ones: the interior only decides
// a cell no exposed voxel reached (one inside the body, invisible anyway).
constexpr uint16_t kVoteExposed = 16;
constexpr uint16_t kVoteInterior = 1;

// ---- the art mapping --------------------------------------------------------
// A statue colour within this RGB distance^2 of an entry already in the bank
// takes that entry: ~10 levels a channel, finer than one 5 cm cell at 30 m can
// show. Past it, a new entry is added while the bank keeps kArtReserve free
// slots (pool humans merge their art at RUNTIME, MicroBodyMergeArt, and must
// find room) and the statues' allowance per palette lasts.
constexpr int64_t kArtNear = 300;
constexpr uint32_t kArtReserve = 32;
constexpr uint32_t kArtAddMax = 96;

int64_t RgbDist2(uint32_t a, uint32_t b) {
  const int dr = (int)((a >> 16) & 255u) - (int)((b >> 16) & 255u);
  const int dg = (int)((a >> 8) & 255u) - (int)((b >> 8) & 255u);
  const int db = (int)(a & 255u) - (int)(b & 255u);
  return (int64_t)dr * dr + (int64_t)dg * dg + (int64_t)db * db;
}

struct Xf {
  Vec3 pos{};
  Quat rot{};
  bool ok = false;
};

double MsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

int FindModelNamed(const Prefab& pf, const std::string& name) {
  for (size_t i = 0; i < pf.models.size(); i++)
    if (pf.models[i].name == name) return (int)i;
  return -1;
}

}  // namespace

const char* DistantPoseName(DistantPose p) {
  switch (p) {
    case DistantPose::Idle: return "idle";
    case DistantPose::WalkA: return "walkA";
    case DistantPose::WalkB: return "walkB";
    case DistantPose::Strike: return "strike";
    case DistantPose::Lying: return "lying";
    default: return "?";
  }
}

const DistantDrawRunStats& DistantDrawRunTotals() { return g_run; }

// ============================================================================
// THE SOURCE
// ============================================================================

struct DistantDraw::Source {
  struct Vox {
    Vec3 p;            // limb frame, world voxels (from the brick's min corner)
    uint16_t colour;   // index into `palette`, 0 = unpainted
    uint16_t coat;     // body coat word, 0 = clean
    uint8_t mat;
    uint8_t exposed;
  };
  struct Limb {
    int part = -1;                   // FK part (-1 = a follower)
    int host = -1;                   // follower: index into `limbs`
    Quat relRot{};                   // follower: its pose in the host's frame
    Vec3 relPos{};
    Vec3 anchorLimb{};               // FK: joint anchor in the limb's frame
    Vec3 livePos{};                  // the live rig's transform (world)
    Quat liveRot{};
    std::vector<Vox> vox;
  };
  const AnimSkeleton* skel = nullptr;
  std::vector<uint8_t> partAlive;    // empty = every part alive
  Vec3 worldSize{};
  Vec3 pivot{};                      // the yaw pivot, prefab frame
  Vec3 origin{};                     // Mob::origin_ (the live frame)
  float heading = 0.0f;
  bool dead = false;                 // Lying comes from the live pose
  int heldHand = -1;                 // the part a weapon hangs from
  std::string label;
  std::vector<Limb> limbs;
  std::vector<uint32_t> palette{0u};   // [0] = unpainted
  std::unordered_map<uint32_t, uint16_t> paletteIndex;
  uint32_t bankVotes[kArtBanks] = {};

  // The colour a painted voxel of `bank`'s entry `art` SHOWS, dye applied the
  // way microbody.wgsl applies it: a dyed instance's albedo is the dye times
  // the art's luminance over DYE_REF (game/dye.h). Deduplicated into `palette`.
  uint16_t ColourOf(const MicroBodySet& set, uint32_t bank, uint32_t art,
                    uint32_t dye) {
    const uint32_t flat = ArtFlat(bank, art);
    uint32_t rgb = flat >= 1 && flat - 1 < set.artColors.size()
                       ? set.artColors[flat - 1]
                       : 0u;
    if (DyeSet(dye)) {
      const float r = (float)((rgb >> 16) & 255u), g = (float)((rgb >> 8) & 255u),
                  b = (float)(rgb & 255u);
      const float lum = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f / kDyeRef;
      float d[3];
      DyeUnpack(dye, d);
      auto q = [&](float v) {
        return (uint32_t)std::clamp((int)std::lround(v * lum * 255.0f), 0, 255);
      };
      rgb = (rgb & 0xFF000000u) | (q(d[0]) << 16) | (q(d[1]) << 8) | q(d[2]);
    }
    auto it = paletteIndex.find(rgb);
    if (it != paletteIndex.end()) return it->second;
    if (palette.size() >= 0xFFFFu) return 0;   // absurd; unpainted
    const uint16_t id = (uint16_t)palette.size();
    palette.push_back(rgb);
    paletteIndex.emplace(rgb, id);
    return id;
  }

  // Read one micro brick into `out`, limb frame.
  void GatherBrick(const MicroBodySet& set, uint32_t model, uint32_t dye,
                   std::vector<Vox>& out) {
    if (model >= set.models.size()) return;
    const MicroBodyModelGpu& m = set.models[model];
    const int dx = (int)(m.dims & 1023u), dy = (int)((m.dims >> 10) & 1023u),
              dz = (int)((m.dims >> 20) & 1023u);
    if (dx <= 0 || dy <= 0 || dz <= 0) return;
    const uint32_t s = std::max(1u, m.scale);
    const uint32_t bank = (m.cutFaces >> 8) & 0xFFu;
    const uint32_t cut = m.cutFaces & 0x3Fu;
    const size_t cells = (size_t)dx * dy * dz;
    const size_t base = m.base;
    const size_t payload = (cells + 1) / 2;
    if (base + payload > set.pool.size()) return;
    bool stain = (m.dims & kMicroBodyDimsStainBit) != 0;
    const size_t sbase = base + payload;
    if (stain && sbase + cells > set.pool.size()) stain = false;
    auto at = [&](int x, int y, int z) -> uint16_t {
      const size_t i = ((size_t)z * dy + y) * dx + x;
      return (uint16_t)(set.pool[base + i / 2] >> ((i & 1u) * 16u));
    };
    // Outside the brick is AIR, except across a JOINT face (cutFaces bit
    // axis * 2 + positive), where another limb is pressed against it.
    auto open = [&](int x, int y, int z, int face) -> bool {
      if (x < 0 || y < 0 || z < 0 || x >= dx || y >= dy || z >= dz)
        return (cut & (1u << face)) == 0;
      return (at(x, y, z) & 0xFFu) == 0;
    };
    // Interior subsample stride (a scale-8 skin into a scale-2 statue: every
    // 4th interior voxel per axis, about one per statue cell) and the splat
    // count for a brick COARSER than the statue (a scale-1 item: each voxel
    // covers two statue cells a side, so it is sampled at 2x2x2 points).
    const int stride = std::max(1, (int)(s / kDistantStatueScale));
    const int k = std::max(1, (int)((kDistantStatueScale + s - 1) / s));
    const float inv = 1.0f / (float)s;
    uint16_t artCache[256];
    bool artSeen[256] = {};
    for (int z = 0; z < dz; z++)
      for (int y = 0; y < dy; y++)
        for (int x = 0; x < dx; x++) {
          const uint16_t c = at(x, y, z);
          const uint8_t mat = (uint8_t)(c & 0xFFu);
          if (mat == 0) continue;
          const bool exposed = open(x - 1, y, z, 0) || open(x + 1, y, z, 1) ||
                               open(x, y - 1, z, 2) || open(x, y + 1, z, 3) ||
                               open(x, y, z - 1, 4) || open(x, y, z + 1, 5);
          if (!exposed && ((x % stride) | (y % stride) | (z % stride))) continue;
          const uint8_t art = (uint8_t)(c >> 8);
          uint16_t colour = 0;
          if (art) {
            if (!artSeen[art]) {
              artCache[art] = ColourOf(set, bank, art, dye);
              artSeen[art] = true;
            }
            colour = artCache[art];
            if (exposed && bank < kArtBanks) bankVotes[bank]++;
          }
          uint16_t coat = 0;
          if (stain) {
            const size_t i = ((size_t)z * dy + y) * dx + x;
            coat = (uint16_t)(set.pool[sbase + i] & 0xFFFFu);
          }
          for (int a = 0; a < k; a++)
            for (int b = 0; b < k; b++)
              for (int e = 0; e < k; e++) {
                Vox v;
                v.p = Vec3{((float)x + ((float)a + 0.5f) / (float)k) * inv,
                           ((float)y + ((float)b + 0.5f) / (float)k) * inv,
                           ((float)z + ((float)e + 0.5f) / (float)k) * inv};
                v.colour = colour;
                v.coat = coat;
                v.mat = mat;
                v.exposed = exposed ? 1 : 0;
                out.push_back(v);
              }
        }
  }
};

// ---- reading a live creature (Mob befriends DistantDraw) ----------------------

bool DistantDraw::SourceFromMob(const Mob& m, const MicroBodySet& set,
                                Source& src) {
  if (m.def_ == nullptr) return false;
  const MobDef& def = *m.def_;
  src.skel = &m.skel_;
  src.partAlive = m.anim_.partAlive;
  src.worldSize = def.worldSize;
  src.pivot = Vec3{def.worldSize.x * 0.5f, 0.0f, def.worldSize.z * 0.5f};
  src.origin = m.origin_;
  src.heading = m.heading_;
  src.dead = !m.alive_;
  src.label = "distant/" + def.name;
  // The weapon hand: the part a held slot hangs from (both hands; the first).
  for (const Mob::HeldHand& hh : m.held_)
    if (hh.slot >= 0 && hh.slot < (int)m.skel_.parts.size()) {
      src.heldHand = m.skel_.parts[(size_t)hh.slot].parent;
      break;
    }

  const size_t n = m.limbs_.size();
  std::vector<int> srcOf(n, -1);
  auto drawn = [&](size_t i) {
    const MobLimb& l = m.limbs_[i];
    if (!l.body || l.holdSeconds > 0) return false;   // gone, or coming off
    if (i < src.partAlive.size() && !src.partAlive[i]) return false;
    if (m.LimbHidden((int)i)) return false;
    // No brick, no statue cells. Every shipped creature's limbs are micro
    // bricks (debris.h: the cube path is plain debris and one test mob).
    return l.microModel >= 0 && (uint32_t)l.microModel < set.models.size();
  };
  auto follower = [&](size_t i) {
    const MobLimb& l = m.limbs_[i];
    return l.wornHost >= 0 || m.IsHeldSlot((int)i) || i >= def.limbs.size();
  };
  auto quatOf = [](const BodyTransform& xf) {
    return QuatNormalize(Quat{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]});
  };
  // FK limbs first, so every follower's host is already placed when the
  // follower is.
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < n; i++) {
      if (!drawn(i) || follower(i) != (pass == 1)) continue;
      const MobLimb& l = m.limbs_[i];
      Source::Limb sl;
      sl.livePos = l.xf.pos;
      sl.liveRot = quatOf(l.xf);
      if (pass == 0) {
        if (i >= m.skel_.parts.size()) continue;
        sl.part = (int)i;
        sl.anchorLimb = l.anchorLimb;
      } else {
        const int host = l.wornHost >= 0
                             ? l.wornHost
                             : (i < m.skel_.parts.size()
                                    ? m.skel_.parts[i].parent
                                    : -1);
        if (host < 0 || host >= (int)n || srcOf[(size_t)host] < 0) continue;
        sl.host = srcOf[(size_t)host];
        // The follower's pose in its host's frame, as the live rig holds it:
        // rigid by construction (DriveWornShells, the held-item socket).
        const MobLimb& h = m.limbs_[(size_t)host];
        const Quat hq = quatOf(h.xf);
        const Quat hinv = QuatConj(hq);
        sl.relRot = QuatNormalize(QuatMul(hinv, sl.liveRot));
        sl.relPos = QuatRotate(hinv, l.xf.pos - h.xf.pos);
      }
      src.GatherBrick(set, (uint32_t)l.microModel, l.dye, sl.vox);
      if (sl.vox.empty()) continue;
      srcOf[i] = (int)src.limbs.size();
      src.limbs.push_back(std::move(sl));
    }
  return !src.limbs.empty();
}

namespace {

// A def's REST pose as a source: every limb's shared (pristine) brick, placed
// exactly as Mob::BuildRig places it (restOffset from the .vox model's offset,
// anchorLimb = the rig's anchorLocal minus it). No gear, no damage.
bool SourceFromDef(const MobDef& def, const MicroBodySet& set,
                   DistantDraw::Source& src) {
  src.skel = &def.skel;
  src.worldSize = def.worldSize;
  src.pivot = Vec3{def.worldSize.x * 0.5f, 0.0f, def.worldSize.z * 0.5f};
  src.label = "distant/" + def.name + "/bare";
  const float inv = 1.0f / (float)std::max(1u, def.skinScale);
  for (size_t i = 0; i < def.limbs.size() && i < def.skel.parts.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    if (ld.microModel < 0 || (uint32_t)ld.microModel >= set.models.size())
      continue;
    const int mi = FindModelNamed(def.prefab, ld.name);
    if (mi < 0) continue;
    const PrefabModel& pm = def.prefab.models[(size_t)mi];
    const Vec3 restOffset = Vec3{(float)pm.offset.x, (float)pm.offset.y,
                                 (float)pm.offset.z} * inv;
    DistantDraw::Source::Limb sl;
    sl.part = (int)i;
    sl.anchorLimb = def.skel.parts[i].anchorLocal - restOffset;
    sl.livePos = restOffset;
    src.GatherBrick(set, (uint32_t)ld.microModel, 0u, sl.vox);
    if (sl.vox.empty()) continue;
    src.limbs.push_back(std::move(sl));
  }
  return !src.limbs.empty();
}

// ---- posing --------------------------------------------------------------------

struct ClipAt {
  int clip = -1;
  float tMs = 0.0f;
};

// Every source limb's transform, PREFAB frame, from a flattened pose:
// Mob::LimbTargetFor with the body at the prefab origin and unrotated.
void LimbXfs(const DistantDraw::Source& src, const AnimState& st,
             std::vector<Xf>& xf) {
  xf.assign(src.limbs.size(), Xf{});
  for (size_t i = 0; i < src.limbs.size(); i++) {
    const DistantDraw::Source::Limb& l = src.limbs[i];
    if (l.part < 0 || (size_t)l.part >= st.model.size()) continue;
    const Transform& t = st.model[(size_t)l.part];
    const Quat r = QuatNormalize(t.rot);
    xf[i] = Xf{t.pos - QuatRotate(r, l.anchorLimb), r, true};
  }
  for (size_t i = 0; i < src.limbs.size(); i++) {
    const DistantDraw::Source::Limb& l = src.limbs[i];
    if (l.part >= 0 || l.host < 0 || !xf[(size_t)l.host].ok) continue;
    const Xf& h = xf[(size_t)l.host];
    xf[i] = Xf{h.pos + QuatRotate(h.rot, l.relPos),
               QuatNormalize(QuatMul(h.rot, l.relRot)), true};
  }
}

// Which leg of a chain leads in WalkA: the gait's own grouping (a biped's two
// singleton groups, a quadruped's diagonal pairs), else chain order among the
// leg chains. One rule for SwingLegs and the arm counter-swing check.
int LegPhase(const AnimSkeleton& sk, const IkChain& ch, int legIdx) {
  int group = -1;
  for (size_t g = 0; g < sk.gait.groups.size() && group < 0; g++)
    for (int gp : sk.gait.groups[g])
      if (gp == ch.effector ||
          std::find(ch.parts.begin(), ch.parts.end(), gp) != ch.parts.end())
        group = (int)g;
  return (group >= 0 ? group : legIdx) & 1;
}

// THE WALK'S LEGS. The runtime walks every leg by IK (the gait plants feet;
// no clip carries a leg), so a statue's stride is made here: a scissor on
// each leg chain's hip and knee, in the parent's frame (+X is the rig's
// lateral axis; a negative angle swings a hanging bone forward toward +Z).
// Which legs lead is the gait's own grouping (a biped's two singletons, a
// quadruped's diagonal pairs), else chain order.
void SwingLegs(const AnimSkeleton& sk, AnimState& st, int leadPhase) {
  constexpr float kHipFwd = 0.42f, kHipBack = 0.32f;
  constexpr float kKneeFwd = 0.12f, kKneeBack = 0.55f;
  int legIdx = 0;
  for (const IkChain& ch : sk.chains) {
    if (ch.tag != "leg" || ch.parts.size() < 2) continue;
    bool alive = true;
    for (int p : ch.parts)
      if (p < 0 || (size_t)p >= st.local.size() ||
          ((size_t)p < st.partAlive.size() && !st.partAlive[(size_t)p]))
        alive = false;
    const int phase = LegPhase(sk, ch, legIdx);
    legIdx++;
    if (!alive) continue;
    const bool fwd = phase == leadPhase;
    Transform& hip = st.local[(size_t)ch.parts[0]];
    Transform& knee = st.local[(size_t)ch.parts[1]];
    hip.rot = QuatNormalize(
        QuatMul(QuatAxisAngle(Vec3{1, 0, 0}, fwd ? -kHipFwd : kHipBack), hip.rot));
    knee.rot = QuatNormalize(
        QuatMul(QuatAxisAngle(Vec3{1, 0, 0}, fwd ? kKneeFwd : kKneeBack), knee.rot));
  }
}

void PoseClips(const DistantDraw::Source& src, const ClipAt* clips, int nClips,
               int legLead, AnimState& st) {
  const AnimSkeleton& sk = *src.skel;
  st.clips.clear();
  for (int c = 0; c < nClips; c++) {
    if (clips[c].clip < 0 || clips[c].clip >= (int)sk.clips.size()) continue;
    ClipInstance ci;
    ci.clip = clips[c].clip;
    ci.timeMs = clips[c].tMs;
    // Long past every blend-in: the sample is the clip's own pose at tMs.
    ci.ageMs = 1.0e7f;
    ci.weight = 1.0f;
    ci.fade = 1.0f;
    st.clips.push_back(ci);
  }
  st.partAlive = src.partAlive;
  AnimSampleAndBlend(sk, st, 0.0f);
  if (legLead >= 0) SwingLegs(sk, st, legLead);
  AnimFlatten(sk, st);
}

// A one-shot clip's usable span: its blend-out has it at less than full
// weight at the end, so the sample stays before that.
float ClipSpanMs(const AnimClip& c) {
  float d = (float)std::max(c.durationMs, 1);
  if (!c.loop && !c.holdEnd && c.blendOutMs > 0)
    d = std::max(1.0f, d - (float)c.blendOutMs);
  return d;
}

int PickStrikeClip(const AnimSkeleton& sk, int heldHand) {
  // A weapon swings from its hand: of the two punches, the one that keys that
  // hand's arm. Otherwise the first strike the rig has.
  if (heldHand >= 0) {
    const int forearm = (size_t)heldHand < sk.parts.size()
                            ? sk.parts[(size_t)heldHand].parent
                            : -1;
    for (const char* name : {"punch_r", "punch_l"}) {
      const int ci = sk.FindClip(name);
      if (ci < 0) continue;
      for (const AnimTrack& tr : sk.clips[(size_t)ci].tracks)
        if (tr.part == heldHand || (forearm >= 0 && tr.part == forearm))
          return ci;
    }
  }
  for (const char* name : {"punch_r", "punch_l", "bite", "lunge", "throw", "cast"}) {
    const int ci = sk.FindClip(name);
    if (ci >= 0) return ci;
  }
  return -1;
}

// The strike frame: the moment of the clip's farthest reach forward (+Z).
float StrikeTimeMs(const DistantDraw::Source& src, int clip, AnimState& st) {
  const AnimClip& c = src.skel->clips[(size_t)clip];
  const float span = ClipSpanMs(c);
  float bestT = span * 0.5f, bestZ = -1e30f;
  for (int k = 1; k <= 9; k++) {
    const float t = span * (float)k / 10.0f;
    const ClipAt at{clip, t};
    PoseClips(src, &at, 1, -1, st);
    float z = -1e30f;
    for (const Transform& tr : st.model) z = std::max(z, tr.pos.z);
    if (z > bestZ) { bestZ = z; bestT = t; }
  }
  return bestT;
}

// ---- binning ------------------------------------------------------------------

struct BinEntry {
  uint64_t sort;   // (cell << 32) | vote key
  int32_t x, y, z;
  uint32_t key;    // material | look colour << 8
  uint16_t w;
  uint16_t coat;
};

}  // namespace

// ============================================================================
// THE CACHE
// ============================================================================

DistantDraw::DistantDraw(uint32_t wordBudget) : budget_(wordBudget) {}
DistantDraw::~DistantDraw() = default;

void DistantDraw::CheckEpoch(MicroBodySet& set) {
  if (epoch_ == set.epoch) return;
  if (epoch_ != 0) {
    // THE SET WAS REPLACED (a materials reload: `mbSet = MicroBodySet{}` and
    // LoadMobDefs again). Every packed pose's model index is a position in a
    // table that no longer exists -- freeing one now would free somebody
    // else's brick in the NEW table, which is the aliasing bug the epoch was
    // added for (MicroBodySet::epoch). Drop them unfreed. The CPU looks go
    // too: an R reload can renumber materials, and a cell's material id is
    // baked into it. A look comes back at the next Bake, or as its def's
    // bare look on demand (Produce with `defs`).
    DropAll(false, nullptr);
    stats_.epochDrops++;
    g_run.epochDrops++;
  }
  epoch_ = set.epoch;
  artAddedThisEpoch_ = 0;
}

void DistantDraw::DropAll(bool freeModels, MicroBodySet* set) {
  if (freeModels && set != nullptr)
    for (const Packed& p : packed_) {
      const int m = looks_[p.look].pose[p.pose].model;
      if (m >= 0) MicroBodyReleaseShared(*set, (uint32_t)m);
    }
  packed_.clear();
  looks_.clear();
  freeLooks_.clear();
  index_.clear();
  resident_ = 0;
  RefreshState();
}

void DistantDraw::Reset(MicroBodySet& set) {
  CheckEpoch(set);
  DropAll(true, &set);
}

void DistantDraw::RefreshState() {
  stats_.looks = (uint32_t)(looks_.size() - freeLooks_.size());
  stats_.packedPoses = (uint32_t)packed_.size();
  stats_.residentWords = resident_;
  g_run.residentWordsPeak = std::max(g_run.residentWordsPeak, resident_);
  g_run.packedPosesPeak = std::max(g_run.packedPosesPeak, stats_.packedPoses);
  g_run.looksPeak = std::max(g_run.looksPeak, stats_.looks);
}

DistantDraw::Look* DistantDraw::Find(uint64_t key) {
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : &looks_[it->second];
}
const DistantDraw::Look* DistantDraw::Find(uint64_t key) const {
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : &looks_[it->second];
}

uint32_t DistantDraw::NewLook(uint64_t key, MicroBodySet& set) {
  const uint32_t live = (uint32_t)(looks_.size() - freeLooks_.size());
  if (live >= kDistantLooksMax) {
    // The least recently used look nobody has drawn or baked this frame. A
    // look still named by an item comes back as its fallback, or re-bakes.
    uint32_t victim = UINT32_MAX;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < (uint32_t)looks_.size(); i++) {
      const Look& l = looks_[i];
      if (l.key == 0 || l.lastUsed >= frame_) continue;
      if (l.lastUsed < oldest) { oldest = l.lastUsed; victim = i; }
    }
    if (victim != UINT32_MAX) {
      for (int p = 0; p < kDistantPoseCount; p++)
        if (looks_[victim].pose[p].model >= 0) Evict(set, victim, p);
      index_.erase(looks_[victim].key);
      looks_[victim] = Look{};
      freeLooks_.push_back(victim);
      stats_.lookEvictions++;
      g_run.lookEvictions++;
    }
  }
  uint32_t idx;
  if (!freeLooks_.empty()) {
    idx = freeLooks_.back();
    freeLooks_.pop_back();
    looks_[idx] = Look{};
  } else {
    idx = (uint32_t)looks_.size();
    looks_.emplace_back();
  }
  looks_[idx].key = key;
  looks_[idx].lastUsed = frame_;
  index_[key] = idx;
  return idx;
}

void DistantDraw::Evict(MicroBodySet& set, uint32_t li, int pi) {
  Pose& p = looks_[li].pose[pi];
  if (p.model < 0) return;
  MicroBodyReleaseShared(set, (uint32_t)p.model);
  resident_ -= std::min(resident_, p.words);
  p.model = -1;
  for (size_t k = 0; k < packed_.size(); k++)
    if (packed_[k].look == li && packed_[k].pose == (uint8_t)pi) {
      packed_[k] = packed_.back();
      packed_.pop_back();
      break;
    }
  stats_.evictions++;
  g_run.evictions++;
}

bool DistantDraw::MakeRoom(MicroBodySet& set, uint32_t words) {
  if (words > budget_) return false;
  while (resident_ + words > budget_ || packed_.size() >= kDistantPoseModelsMax) {
    // Least recently DRAWN first; a pose drawn this frame is never a victim
    // (it is in this frame's list, and the GPU is about to read it).
    size_t victim = SIZE_MAX;
    uint64_t oldest = UINT64_MAX;
    for (size_t k = 0; k < packed_.size(); k++) {
      const Pose& p = looks_[packed_[k].look].pose[packed_[k].pose];
      if (p.lastUsed >= frame_) continue;
      if (p.lastUsed < oldest) { oldest = p.lastUsed; victim = k; }
    }
    if (victim == SIZE_MAX) return false;
    Evict(set, packed_[victim].look, packed_[victim].pose);
  }
  return true;
}

// A model has ONE art bank (cutFaces bits 8..15), and a statue's paint can
// come from several -- the cast's bank and the wardrobe's at least, plus dyed
// colours no bank holds. So the look's palette is mapped into the bank most of
// its painted voxels came from: an entry already within kArtNear, else a new
// entry while the bank keeps kArtReserve slots free and the statues'
// allowance (kArtAddMax per palette) lasts, else the nearest. Counted both
// ways (artAdded, artNearest): a statue whose colours drift is a number.
void DistantDraw::ResolveArt(MicroBodySet& set, Look& lk) {
  lk.art.assign(lk.palette.size(), 0);
  const uint32_t bank = std::min(lk.bank, kArtBanks - 1u);
  const size_t b0 = (size_t)bank * kArtBankSlots;
  for (size_t i = 1; i < lk.palette.size(); i++) {
    const uint32_t rgb = lk.palette[i];
    const uint32_t fill = set.artBankFill[bank];
    int best = -1;
    int64_t bestD = INT64_MAX;
    for (uint32_t j = 0; j < fill && b0 + j < set.artColors.size(); j++) {
      const uint32_t c = set.artColors[b0 + j];
      const int64_t d = RgbDist2(c, rgb) +
                        ((c >> 24) != (rgb >> 24) ? ((int64_t)1 << 40) : 0);
      if (d < bestD) { bestD = d; best = (int)j; }
    }
    uint8_t art = 0;
    if (best >= 0 && bestD <= kArtNear) {
      art = (uint8_t)(best + 1);
    } else if (fill + kArtReserve < kArtBankSlots &&
               artAddedThisEpoch_ < kArtAddMax) {
      art = MicroBodyArtInBank(set, bank, rgb);
      artAddedThisEpoch_++;
      stats_.artAdded++;
      g_run.artAdded++;
      set.dirty = true;   // the palette rides the next micro upload
    } else if (best >= 0) {
      art = (uint8_t)(best + 1);
      stats_.artNearest++;
      g_run.artNearest++;
    }
    lk.art[i] = art;
  }
  lk.artResolved = true;
}

int DistantDraw::EnsurePacked(MicroBodySet& set, uint32_t li, int pi,
                              bool allowPack, bool* noFit) {
  Look& lk = looks_[li];
  Pose& p = lk.pose[pi];
  if (!p.present) return -1;
  if (p.model >= 0) {
    p.lastUsed = frame_;
    lk.lastUsed = frame_;
    return p.model;
  }
  if (!allowPack || packsThisFrame_ >= kDistantPacksPerFrame) return -1;
  if (p.refusedUntil > frame_) {
    if (noFit) *noFit = true;
    return -1;
  }
  if (!MakeRoom(set, p.words)) {
    if (noFit) *noFit = true;
    return -1;
  }
  // THE POOL AND THE TABLE KEEP THEIR RESERVES: a statue is the thing that
  // goes without, never a wound (MicroBodyClone) that cannot show. Checked
  // here rather than left to MicroBodyPack, whose refusal message is about
  // damaged bodies and would be a false alarm for a statue.
  uint32_t largest = 0;
  const uint32_t freeWords = MicroBodyFreeWords(set, &largest);
  const size_t freeRecords =
      set.freeModels.size() + (set.models.size() < kMaxMicroBodyModels
                                   ? kMaxMicroBodyModels - set.models.size()
                                   : 0u);
  if (largest < p.words || freeWords < p.words + kDistantPoolReserveWords ||
      freeRecords <= kDistantModelReserve) {
    p.refusedUntil = frame_ + 60;
    if (noFit) *noFit = true;
    return -1;
  }
  if (!lk.artResolved) ResolveArt(set, lk);
  static thread_local std::vector<PrefabVoxel> vox;
  vox.clear();
  vox.reserve(p.cells.size());
  for (const Cell& c : p.cells) {
    PrefabVoxel v{};
    v.x = (int16_t)c.x;
    v.y = (int16_t)c.y;
    v.z = (int16_t)c.z;
    v.material = c.mat;
    v.color = c.colour < lk.art.size() ? lk.art[c.colour] : (uint8_t)0;
    v.bruise = 0;
    v.stain = c.coat;
    vox.push_back(v);
  }
  std::string log;
  const int m = MicroBodyPack(set, vox, p.dims, kDistantStatueScale, lk.label,
                              log, 0u);
  if (m < 0) {
    p.refusedUntil = frame_ + 120;
    stats_.packRefused++;
    g_run.packRefused++;
    if (noFit) *noFit = true;
    return -1;
  }
  MicroBodySetArtBank(set, (uint32_t)m, lk.bank);
  p.model = m;
  if ((size_t)m < set.blockWords.size()) p.words = set.blockWords[(size_t)m];
  resident_ += p.words;
  packed_.push_back(Packed{li, (uint8_t)pi});
  p.lastUsed = frame_;
  lk.lastUsed = frame_;
  packsThisFrame_++;
  stats_.packs++;
  g_run.packs++;
  return m;
}

// ============================================================================
// THE BAKE
// ============================================================================

void DistantDraw::BakeInto(MicroBodySet& set, uint32_t li, const Source& src) {
  if (src.skel == nullptr) return;
  // What to bake: every pose the look lacks, and Lying again when this source
  // is a real corpse and the look's Lying was only an idle tipped over.
  bool want[kDistantPoseCount];
  bool any = false;
  for (int p = 0; p < kDistantPoseCount; p++) {
    want[p] = !looks_[li].pose[p].present;
    any = any || want[p];
  }
  const int kLie = (int)DistantPose::Lying;
  if (src.dead && !looks_[li].pose[kLie].fromCorpse) want[kLie] = any = true;
  if (!any) return;
  if (want[kLie] && looks_[li].pose[kLie].model >= 0) Evict(set, li, kLie);
  Look& lk = looks_[li];

  // The source palette onto the look's, by colour.
  const bool fresh = lk.label.empty();
  if (lk.palette.empty()) lk.palette.push_back(0u);
  std::unordered_map<uint32_t, uint16_t> have;
  for (size_t i = 1; i < lk.palette.size(); i++)
    have.emplace(lk.palette[i], (uint16_t)i);
  std::vector<uint16_t> remap(src.palette.size(), 0);
  for (size_t i = 1; i < src.palette.size(); i++) {
    auto it = have.find(src.palette[i]);
    if (it != have.end()) { remap[i] = it->second; continue; }
    if (lk.palette.size() >= 0xFFFFu) continue;
    const uint16_t id = (uint16_t)lk.palette.size();
    lk.palette.push_back(src.palette[i]);
    have.emplace(src.palette[i], id);
    remap[i] = id;
    lk.artResolved = false;   // a new colour: map again at the next pack
  }
  if (fresh) {
    // A new look: the bank most of its painted SURFACE came from.
    uint32_t bestBank = 0, bestVotes = 0;
    for (uint32_t b = 0; b < kArtBanks; b++)
      if (src.bankVotes[b] > bestVotes) { bestVotes = src.bankVotes[b]; bestBank = b; }
    lk.bank = bestBank;
    lk.pivot = src.pivot;
    lk.label = src.label;
  }

  static thread_local std::vector<BinEntry> ent;
  const float S = (float)kDistantStatueScale;
  // ---- MERGE + DOWNSAMPLE one posed body into `out`, grounded ----------------
  auto bin = [&](const std::vector<Xf>& xfs, Pose& out) -> bool {
    ent.clear();
    int mn[3] = {INT_MAX, INT_MAX, INT_MAX}, mx[3] = {INT_MIN, INT_MIN, INT_MIN};
    for (size_t i = 0; i < src.limbs.size() && i < xfs.size(); i++) {
      if (!xfs[i].ok) continue;
      const Source::Limb& l = src.limbs[i];
      const Vec3 ax = QuatRotate(xfs[i].rot, Vec3{1, 0, 0});
      const Vec3 ay = QuatRotate(xfs[i].rot, Vec3{0, 1, 0});
      const Vec3 az = QuatRotate(xfs[i].rot, Vec3{0, 0, 1});
      const Vec3 o = xfs[i].pos;
      for (const Source::Vox& v : l.vox) {
        const Vec3 p = o + ax * v.p.x + ay * v.p.y + az * v.p.z;
        BinEntry e;
        e.sort = 0;
        e.x = ifloor(p.x * S);
        e.y = ifloor(p.y * S);
        e.z = ifloor(p.z * S);
        e.key = (uint32_t)v.mat |
                ((uint32_t)(v.colour < remap.size() ? remap[v.colour] : 0u) << 8);
        e.w = v.exposed ? kVoteExposed : kVoteInterior;
        e.coat = v.coat;
        mn[0] = std::min(mn[0], e.x); mx[0] = std::max(mx[0], e.x);
        mn[1] = std::min(mn[1], e.y); mx[1] = std::max(mx[1], e.y);
        mn[2] = std::min(mn[2], e.z); mx[2] = std::max(mx[2], e.z);
        ent.push_back(e);
      }
    }
    if (ent.empty()) return false;
    const int dx = mx[0] - mn[0] + 1, dy = mx[1] - mn[1] + 1, dz = mx[2] - mn[2] + 1;
    if (dx > 1023 || dy > 1023 || dz > 1023) return false;   // MicroBodyPack's ceiling
    for (BinEntry& e : ent) {
      const uint64_t lin = ((uint64_t)(e.z - mn[2]) * (uint64_t)dy +
                            (uint64_t)(e.y - mn[1])) * (uint64_t)dx +
                           (uint64_t)(e.x - mn[0]);
      e.sort = (lin << 32) | e.key;
    }
    std::sort(ent.begin(), ent.end(),
              [](const BinEntry& a, const BinEntry& b) { return a.sort < b.sort; });
    out.cells.clear();
    out.anyCoat = false;
    const size_t n = ent.size();
    size_t i = 0;
    while (i < n) {
      const uint64_t cell = ent[i].sort >> 32;
      uint32_t bestKey = 0, bestW = 0, exposedN = 0, coatedN = 0, bestAmt = 0;
      uint16_t bestCoat = 0;
      size_t j = i;
      while (j < n && (ent[j].sort >> 32) == cell) {
        const uint64_t run = ent[j].sort;
        uint32_t w = 0;
        while (j < n && ent[j].sort == run) {
          const BinEntry& e = ent[j];
          w += e.w;
          if (e.w == kVoteExposed) {
            exposedN++;
            const uint32_t amt = BodyStainAmt(e.coat);
            if (amt) {
              coatedN++;
              if (amt > bestAmt || (amt == bestAmt && e.coat < bestCoat)) {
                bestAmt = amt;
                bestCoat = e.coat;
              }
            }
          }
          j++;
        }
        // Strictly greater: a tie keeps the lower key (sorted first), so the
        // vote is a pure function of the voxels, not of their order.
        if (w > bestW) { bestW = w; bestKey = (uint32_t)run; }
      }
      Cell c;
      c.x = (uint16_t)(cell % (uint64_t)dx);
      c.y = (uint16_t)((cell / (uint64_t)dx) % (uint64_t)dy);
      c.z = (uint16_t)(cell / ((uint64_t)dx * (uint64_t)dy));
      c.mat = (uint8_t)(bestKey & 0xFFu);
      c.colour = (uint16_t)(bestKey >> 8);
      // A coat shows when a quarter of the cell's skin carries one.
      c.coat = (exposedN && coatedN * 4 >= exposedN) ? bestCoat : (uint16_t)0;
      if (c.coat) out.anyCoat = true;
      out.cells.push_back(c);
      i = j;
    }
    out.present = true;
    out.dims = IVec3{dx, dy, dz};
    // GROUNDED: the lowest cell sits at the item's origin.y.
    out.minCorner = Vec3{(float)mn[0] / S, 0.0f, (float)mn[2] / S};
    const Vec3 ext{(float)dx / S, (float)dy / S, (float)dz / S};
    out.centre = out.minCorner + ext * 0.5f;
    out.radius = ext.len() * 0.5f;
    const size_t cells = (size_t)dx * dy * dz;
    out.words = (uint32_t)((cells + 1) / 2 + (out.anyCoat ? cells : 0));
    out.model = -1;
    out.refusedUntil = 0;
    return true;
  };

  const AnimSkeleton& sk = *src.skel;
  AnimState st;
  std::vector<Xf> xf, idleXf;
  auto span = [&](int c) {
    return c >= 0 ? ClipSpanMs(sk.clips[(size_t)c]) : 0.0f;
  };
  // IDLE: the idle clip a quarter in (an additive breath on the rest pose),
  // or the rest pose itself.
  const int idleC = sk.FindClip("idle");
  const ClipAt idleAt{idleC, span(idleC) * 0.25f};
  PoseClips(src, &idleAt, 1, -1, st);
  LimbXfs(src, st, idleXf);
  if (want[(int)DistantPose::Idle]) bin(idleXf, lk.pose[(int)DistantPose::Idle]);

  // WALK A / B: the walk clip's arm swing at a quarter and three quarters,
  // the legs scissored by SwingLegs. The arm that swings forward is the one
  // on the side of the leg that swings BACK; if the clip's quarter puts it
  // the other way round, the two halves trade places.
  if (want[(int)DistantPose::WalkA] || want[(int)DistantPose::WalkB]) {
    const int walkC = sk.FindClip("walk");
    float tA = span(walkC) * 0.25f, tB = span(walkC) * 0.75f;
    if (walkC >= 0) {
      const ClipAt a{walkC, tA};
      PoseClips(src, &a, 1, -1, st);
      float armZ[2] = {0, 0};
      bool haveArm[2] = {false, false};
      int legSide = -1, legIdx = 0;
      for (const IkChain& ch : sk.chains) {
        if (ch.parts.empty() || ch.parts[0] < 0 ||
            (size_t)ch.parts[0] >= st.model.size())
          continue;
        const int side = st.model[(size_t)ch.parts[0]].pos.x > src.pivot.x ? 1 : 0;
        if (ch.tag == "arm" && ch.effector >= 0 &&
            (size_t)ch.effector < st.model.size()) {
          armZ[side] = st.model[(size_t)ch.effector].pos.z -
                       st.model[(size_t)ch.parts[0]].pos.z;
          haveArm[side] = true;
        } else if (ch.tag == "leg" && ch.parts.size() >= 2) {
          if (legSide < 0 && LegPhase(sk, ch, legIdx) == 0) legSide = side;
          legIdx++;
        }
      }
      if (haveArm[0] && haveArm[1] && legSide >= 0) {
        const int armSide = armZ[1] > armZ[0] ? 1 : 0;
        if (armSide == legSide) std::swap(tA, tB);
      }
    }
    const ClipAt a{walkC, tA}, b{walkC, tB};
    if (want[(int)DistantPose::WalkA]) {
      PoseClips(src, &a, 1, 0, st);
      LimbXfs(src, st, xf);
      bin(xf, lk.pose[(int)DistantPose::WalkA]);
    }
    if (want[(int)DistantPose::WalkB]) {
      PoseClips(src, &b, 1, 1, st);
      LimbXfs(src, st, xf);
      bin(xf, lk.pose[(int)DistantPose::WalkB]);
    }
  }

  // STRIKE: the strike clip at its farthest reach; the idle when the rig has
  // no strike clip at all.
  if (want[(int)DistantPose::Strike]) {
    const int sc = PickStrikeClip(sk, src.heldHand);
    if (sc >= 0) {
      const ClipAt s{sc, StrikeTimeMs(src, sc, st)};
      PoseClips(src, &s, 1, -1, st);
      LimbXfs(src, st, xf);
      bin(xf, lk.pose[(int)DistantPose::Strike]);
    } else {
      bin(idleXf, lk.pose[(int)DistantPose::Strike]);
    }
  }

  // LYING: a corpse as it really lies (its live ragdoll pose, brought into the
  // creature's own frame), else the idle tipped onto its back about the body's
  // middle -- front up, head toward -Z.
  if (want[kLie]) {
    Pose& lp = lk.pose[kLie];
    if (src.dead) {
      const Quat inv = QuatConj(QuatAxisAngle(Vec3{0, 1, 0}, src.heading));
      xf.assign(src.limbs.size(), Xf{});
      for (size_t i = 0; i < src.limbs.size(); i++) {
        const Source::Limb& l = src.limbs[i];
        xf[i] = Xf{src.pivot + QuatRotate(inv, l.livePos - src.origin - src.pivot),
                   QuatNormalize(QuatMul(inv, l.liveRot)), true};
      }
      if (bin(xf, lp)) lp.fromCorpse = true;
    } else {
      const Quat g = QuatAxisAngle(Vec3{1, 0, 0}, -1.57079633f);
      const Vec3 c{src.pivot.x, src.worldSize.y * 0.5f, src.pivot.z};
      xf = idleXf;
      for (Xf& t : xf) {
        if (!t.ok) continue;
        t.pos = c + QuatRotate(g, t.pos - c);
        t.rot = QuatNormalize(QuatMul(g, t.rot));
      }
      if (bin(xf, lp)) lp.fromCorpse = false;
    }
  }
  lk.lastUsed = frame_;
}

void DistantDraw::KeyPartsOf(const Mob& m, const MicroBodySet& set,
                             uint64_t& def, uint64_t& gear, uint64_t& damage) {
  def = m.def_ ? NonZero(FnvStr(kFnvBasis, m.def_->name)) : 0;
  // GEAR: what is worn (with its dye) and what is held, order-free.
  std::vector<std::string> parts;
  for (const Mob::WornPiece& w : m.worn_) {
    uint32_t dye = 0;
    for (int s : w.slots)
      if (s >= 0 && s < (int)m.limbs_.size()) { dye = m.limbs_[(size_t)s].dye; break; }
    parts.push_back(w.item + "#" + std::to_string(dye));
  }
  int h = 0;
  for (const Mob::HeldHand& hh : m.held_) {
    if (!hh.item.empty()) parts.push_back("held" + std::to_string(h) + ":" + hh.item);
    h++;
  }
  std::sort(parts.begin(), parts.end());
  gear = 0;
  if (!parts.empty()) {
    uint64_t g = kFnvBasis;
    for (const std::string& s : parts) g = FnvStr(g, s);
    gear = NonZero(g);
  }
  // DAMAGE, COARSELY: per limb, gone; or its brick is its own (carved, burnt,
  // bloodied, rotted) with how much of it is left (4 buckets) and how much it
  // has been rewritten (4); and how coated it is (4). Coarse on purpose: two
  // creatures beaten about the same share one statue, and a clean one shares
  // the def's. Zero exactly when every limb is present, unowned and clean.
  const size_t baseLimbs = m.def_ ? m.def_->limbs.size() : 0;
  bool any = false;
  uint64_t d = kFnvBasis;
  for (size_t i = 0; i < m.limbs_.size(); i++) {
    const MobLimb& l = m.limbs_[i];
    const bool alive = i >= m.anim_.partAlive.size() || m.anim_.partAlive[i];
    const bool present = l.body != 0 && l.holdSeconds <= 0 && alive;
    if (!present) {
      if (i < baseLimbs) {
        any = true;
        d = FnvU64(d, ((uint64_t)i << 8) | 0xFFu);
      }
      continue;
    }
    uint32_t bits = 0;
    const bool owned = l.microModel >= 0 &&
                       (size_t)l.microModel < set.owned.size() &&
                       set.owned[(size_t)l.microModel] != 0;
    if (owned) {
      const size_t now = l.HasFineSkin() ? l.skinVoxels.size() : l.voxels.size();
      const float frac = l.voxelsAtSpawn ? (float)now / (float)l.voxelsAtSpawn : 1.0f;
      const uint32_t loss = frac >= 0.95f ? 0u : frac >= 0.75f ? 1u : frac >= 0.5f ? 2u : 3u;
      const uint32_t gen = MicroBodyEditGen(set, (uint32_t)l.microModel);
      const uint32_t edits = gen == 0 ? 0u : gen < 16 ? 1u : gen < 256 ? 2u : 3u;
      bits |= 1u | (loss << 1) | (edits << 3);
    }
    const float cf = l.coat.Frac();
    const uint32_t coat = cf <= 0.0f ? 0u : cf < 0.1f ? 1u : cf < 0.3f ? 2u : 3u;
    bits |= coat << 5;
    if (bits) {
      any = true;
      d = FnvU64(d, ((uint64_t)i << 8) | bits);
    }
  }
  damage = any ? NonZero(d) : 0;
}

DistantLookKey DistantDraw::KeyOf(const Mob& m, const MicroBodySet& set) const {
  uint64_t d = 0, g = 0, dmg = 0;
  KeyPartsOf(m, set, d, g, dmg);
  return MakeKey(d, g, dmg);
}

DistantLookKey DistantDraw::KeyOfDef(const MobDef& def) {
  return MakeKey(NonZero(FnvStr(kFnvBasis, def.name)), 0, 0);
}

DistantBake DistantDraw::Bake(const Mob& m, MicroBodySet& set) {
  DistantBake out;
  CheckEpoch(set);
  if (m.def_ == nullptr) return out;
  const auto t0 = std::chrono::steady_clock::now();
  out.key = KeyOf(m, set);
  out.restY = m.origin_.y;
  const Look* existing = Find(out.key.full);
  bool need = existing == nullptr ||
              (!m.alive_ && !existing->pose[(int)DistantPose::Lying].fromCorpse);
  if (existing)
    for (int p = 0; p < kDistantPoseCount; p++)
      need = need || !existing->pose[p].present;
  if (need || !m.alive_) {
    Source src;
    if (SourceFromMob(m, set, src)) {
      if (!m.alive_) {
        // Where the corpse's lowest cell is, on the grid the grounded Lying
        // pose was binned on (prefab y = world y - origin.y; the yaw does not
        // move y).
        float minY = 1e30f;
        for (const Source::Limb& l : src.limbs) {
          const Vec3 ax = QuatRotate(l.liveRot, Vec3{1, 0, 0});
          const Vec3 ay = QuatRotate(l.liveRot, Vec3{0, 1, 0});
          const Vec3 az = QuatRotate(l.liveRot, Vec3{0, 0, 1});
          for (const Source::Vox& v : l.vox)
            minY = std::min(minY, l.livePos.y + ax.y * v.p.x + ay.y * v.p.y +
                                      az.y * v.p.z);
        }
        if (minY < 1e29f) {
          const float S = (float)kDistantStatueScale;
          out.restY = m.origin_.y + (float)ifloor((minY - m.origin_.y) * S) / S;
        }
      }
      if (need) {
        auto it = index_.find(out.key.full);
        const uint32_t li =
            it != index_.end() ? it->second : NewLook(out.key.full, set);
        looks_[li].def = out.key.def;
        BakeInto(set, li, src);
        stats_.bakes++;
        g_run.bakes++;
      }
    }
  }
  // The def's bare look, so every look has a fallback no live creature is
  // needed for (and D1's statues survive their own looks' eviction).
  if (!HasLook(out.key.bare)) BakeDef(*m.def_, set);
  out.baked = HasLook(out.key.full);
  if (Look* lk = Find(out.key.full)) lk->lastUsed = frame_;
  const double ms = MsSince(t0);
  stats_.bakeMsMax = std::max(stats_.bakeMsMax, ms);
  g_run.bakeMsMax = std::max(g_run.bakeMsMax, ms);
  RefreshState();
  return out;
}

DistantBake DistantDraw::BakeDef(const MobDef& def, MicroBodySet& set) {
  DistantBake out;
  CheckEpoch(set);
  out.key = KeyOfDef(def);
  if (const Look* lk = Find(out.key.full)) {
    bool all = true;
    for (int p = 0; p < kDistantPoseCount; p++) all = all && lk->pose[p].present;
    if (all) {
      out.baked = true;
      return out;
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  Source src;
  if (!SourceFromDef(def, set, src)) return out;
  auto it = index_.find(out.key.full);
  const uint32_t li = it != index_.end() ? it->second : NewLook(out.key.full, set);
  looks_[li].def = out.key.def;
  BakeInto(set, li, src);
  stats_.bakes++;
  g_run.bakes++;
  out.baked = HasLook(out.key.full);
  const double ms = MsSince(t0);
  stats_.bakeMsMax = std::max(stats_.bakeMsMax, ms);
  g_run.bakeMsMax = std::max(g_run.bakeMsMax, ms);
  RefreshState();
  return out;
}

bool DistantDraw::PoseOf(uint64_t key, DistantPose pose, PoseInfo& out) const {
  out = PoseInfo{};
  const Look* lk = Find(key);
  if (lk == nullptr || (int)pose >= kDistantPoseCount) return false;
  const Pose& p = lk->pose[(int)pose];
  out.present = p.present;
  out.fromCorpse = p.fromCorpse;
  out.cells = (uint32_t)p.cells.size();
  out.dims = p.dims;
  out.words = p.words;
  out.model = p.model;
  return p.present;
}

// ============================================================================
// THE PRODUCER
// ============================================================================

void DistantDraw::Produce(MicroBodySet& set, const std::vector<MobDef>* defs,
                          const DistantView& view, uint32_t slotsFree,
                          std::vector<RenderOnlyBody>& out,
                          std::vector<uint32_t>* outItems) {
  out.clear();
  if (outItems) outItems->clear();
  CheckEpoch(set);
  frame_++;
  packsThisFrame_ = 0;
  stats_.items = (uint32_t)items_.size();
  stats_.drawn = stats_.culled = stats_.starved = stats_.unbaked = 0;
  stats_.deferred = stats_.noFit = stats_.fallbacks = 0;
  stats_.slotsFree = slotsFree;

  if (defs != nullptr &&
      (defIndexOf_ != (const void*)defs || defIndexFor_ != defs->size())) {
    defIndex_.clear();
    for (size_t i = 0; i < defs->size(); i++)
      defIndex_[KeyOfDef((*defs)[i]).def] = (uint32_t)i;
    defIndexOf_ = defs;
    defIndexFor_ = defs->size();
  }
  uint32_t defBakes = 0;

  // The flipbook: dead lies, a strike strikes, a walk alternates on the gait
  // phase; a pose the look lacks falls back to its idle.
  auto poseFor = [](const DistantDrawItem& it) {
    if (it.dead) return (int)DistantPose::Lying;
    if (it.state == DistantDrawState::Strike) return (int)DistantPose::Strike;
    if (it.state == DistantDrawState::Walk) {
      const float f = it.gaitPhase - std::floor(it.gaitPhase);
      return f < 0.5f ? (int)DistantPose::WalkA : (int)DistantPose::WalkB;
    }
    return (int)DistantPose::Idle;
  };
  auto poseIn = [](const Look& lk, int pose) {
    if (lk.pose[pose].present) return pose;
    if (lk.pose[(int)DistantPose::Idle].present) return (int)DistantPose::Idle;
    for (int p = 0; p < kDistantPoseCount; p++)
      if (lk.pose[p].present) return p;
    return -1;
  };
  // Mob::LimbTargetFor's placement of a prefab-frame point: yaw about the
  // footprint's centre column, then the origin.
  auto place = [](const DistantDrawItem& it, const Look& lk, const Vec3& local,
                  const Quat& q) {
    return it.origin + lk.pivot + QuatRotate(q, local - lk.pivot);
  };

  const float ty = view.tanHalfFovY, tx = view.tanHalfFovY * view.aspect;
  const float kx = std::sqrt(1.0f + tx * tx), ky = std::sqrt(1.0f + ty * ty);
  order_.clear();
  for (uint32_t i = 0; i < (uint32_t)items_.size(); i++) {
    const DistantDrawItem& it = items_[i];
    const uint64_t keys[3] = {it.look.full, it.look.pristine, it.look.bare};
    const int want = poseFor(it);
    uint32_t li = UINT32_MAX;
    uint8_t which = 0;
    for (uint8_t w = 0; w < 3 && li == UINT32_MAX; w++) {
      if (keys[w] == 0) continue;
      auto f = index_.find(keys[w]);
      if (f == index_.end() || poseIn(looks_[f->second], want) < 0) continue;
      li = f->second;
      which = w;
    }
    // Nothing baked under any of the three: the def's rest pose, on demand.
    if (li == UINT32_MAX && defs != nullptr && it.look.def != 0 &&
        defBakes < kDistantDefBakesPerFrame) {
      auto d = defIndex_.find(it.look.def);
      if (d != defIndex_.end()) {
        BakeDef((*defs)[d->second], set);
        defBakes++;
        auto f = index_.find(it.look.bare);
        if (f != index_.end() && poseIn(looks_[f->second], want) >= 0) {
          li = f->second;
          which = 2;
        }
      }
    }
    if (li == UINT32_MAX) {
      stats_.unbaked++;
      continue;
    }
    // Touched this frame: neither LRU may take it while this list holds it.
    looks_[li].lastUsed = frame_;
    const Look& lk = looks_[li];
    const int pose = poseIn(lk, want);
    const Pose& p = lk.pose[pose];
    const Quat q = QuatAxisAngle(Vec3{0, 1, 0}, it.heading);
    const Vec3 c = place(it, lk, p.centre, q);
    const Vec3 d = c - view.eye;
    if (view.cull) {
      // The body cull's sphere test (bodyreg.cpp CullBodyRanges), +1 voxel.
      const float rad = p.radius + 1.0f;
      const float z = d.dot(view.fwd), xr = d.dot(view.right), yu = d.dot(view.up);
      if (z < -rad || std::fabs(xr) - z * tx > rad * kx ||
          std::fabs(yu) - z * ty > rad * ky) {
        stats_.culled++;
        continue;
      }
    }
    const float d2 = d.dot(d);
    uint32_t bits;
    std::memcpy(&bits, &d2, sizeof(bits));
    // The living before the dead, then nearest first (a non-negative float's
    // bits order as the float does), ties by item index.
    order_.push_back(Order{((uint64_t)(it.dead ? 1u : 0u) << 32) | bits, i, li,
                           (uint8_t)pose, which});
  }
  std::sort(order_.begin(), order_.end(), [](const Order& a, const Order& b) {
    return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.item < b.item;
  });

  for (size_t k = 0; k < order_.size(); k++) {
    // THE SLOT BUDGET. Everything past it is starved, never drawn over a real
    // body: the caller's `slotsFree` is what the real walks and the arrows
    // left, and BodyRegistry walks this list after both.
    if ((uint32_t)out.size() >= slotsFree) {
      stats_.starved += (uint32_t)(order_.size() - k);
      break;
    }
    const Order& o = order_[k];
    const DistantDrawItem& it = items_[o.item];
    bool noFit = false;
    uint32_t li = o.look;
    int pose = o.pose;
    uint8_t which = o.which;
    int model = EnsurePacked(set, li, pose, true, &noFit);
    if (model < 0) {
      // A fallback that is ALREADY packed: the same gear undamaged, then the
      // def alone. No new pack for it -- the budget just said no.
      const uint64_t keys[3] = {it.look.full, it.look.pristine, it.look.bare};
      for (uint8_t w = (uint8_t)(o.which + 1); w < 3 && model < 0; w++) {
        if (keys[w] == 0 || keys[w] == keys[o.which]) continue;
        auto f = index_.find(keys[w]);
        if (f == index_.end()) continue;
        const int p2 = poseIn(looks_[f->second], poseFor(it));
        if (p2 < 0) continue;
        const int m2 = EnsurePacked(set, f->second, p2, false, nullptr);
        if (m2 >= 0) {
          model = m2;
          li = f->second;
          pose = p2;
          which = w;
        }
      }
    }
    if (model < 0) {
      if (noFit) stats_.noFit++;
      else stats_.deferred++;
      continue;
    }
    if (which != 0) stats_.fallbacks++;
    const Look& lk = looks_[li];
    const Pose& p = lk.pose[pose];
    const Quat q = QuatAxisAngle(Vec3{0, 1, 0}, it.heading);
    RenderOnlyBody b;
    b.pos = place(it, lk, p.minCorner, q);
    b.quat[0] = q.x;
    b.quat[1] = q.y;
    b.quat[2] = q.z;
    b.quat[3] = q.w;
    b.model = (uint32_t)model;
    out.push_back(b);
    if (outItems) outItems->push_back(o.item);
  }
  stats_.drawn = (uint32_t)out.size();
  RefreshState();

  g_run.produces++;
  g_run.drawnTotal += stats_.drawn;
  g_run.culledTotal += stats_.culled;
  g_run.starvedTotal += stats_.starved;
  g_run.unbakedTotal += stats_.unbaked;
  g_run.deferredTotal += stats_.deferred;
  g_run.noFitTotal += stats_.noFit;
  g_run.fallbacksTotal += stats_.fallbacks;
  g_run.drawnPeak = std::max(g_run.drawnPeak, stats_.drawn);
}

// ============================================================================
// THE TEST HOOK
// ============================================================================

DistantDrawTestHook::DistantDrawTestHook() {
  const char* e = std::getenv("SANDVOX_DISTANT_DRAW_TEST");
  if (e == nullptr || *e == 0) return;
  const std::string s = e;
  const size_t colon = s.find(':');
  count_ = std::clamp(std::atoi(s.substr(0, colon).c_str()), 0, 4096);
  if (colon != std::string::npos) {
    size_t a = colon + 1;
    while (a <= s.size()) {
      size_t b = s.find(',', a);
      if (b == std::string::npos) b = s.size();
      if (b > a) names_.push_back(s.substr(a, b - a));
      a = b + 1;
    }
  }
  if (names_.empty()) names_.push_back("human");
  if (count_ > 0)
    std::printf("distant draw test: %d statue(s) of %zu def(s) in a ring "
                "30..80 m around the player\n",
                count_, names_.size());
}

void DistantDrawTestHook::Fill(DistantDraw& dd, const std::vector<MobDef>& defs,
                               MicroBodySet& set, const Vec3& playerPos,
                               double timeSec, uint32_t seed) {
  if (!Enabled()) return;
  if (defOf_.size() != names_.size()) {
    defOf_.assign(names_.size(), -1);
    keys_.assign(names_.size(), DistantLookKey{});
  }
  for (size_t n = 0; n < names_.size(); n++) {
    int& d = defOf_[n];
    if (d < 0 || d >= (int)defs.size() || defs[(size_t)d].name != names_[n]) {
      d = -1;
      for (size_t k = 0; k < defs.size(); k++)
        if (defs[k].name == names_[n]) d = (int)k;
    }
    if (d < 0) continue;
    keys_[n] = DistantDraw::KeyOfDef(defs[(size_t)d]);
    // After a materials reload the cache has dropped everything: bake again.
    if (!dd.HasLook(keys_[n].full)) dd.BakeDef(defs[(size_t)d], set);
  }
  if (!haveCentre_) {
    haveCentre_ = true;
    centre_ = playerPos;
    spots_.clear();
    for (int i = 0; i < count_; i++) {
      // A golden-angle spiral over the 30..80 m band: even, and the same
      // every run.
      const float a = (float)i * 2.39996323f;
      const float rM = 30.0f + std::fmod((float)i * 13.7f, 50.0f);
      const float r = rM * (float)kVoxelsPerMetre;
      const float x = centre_.x + std::cos(a) * r;
      const float z = centre_.z + std::sin(a) * r;
      // TerrainHeight is the top SOLID cell; feet stand on the one above.
      const int y = World::TerrainHeight(ifloor(x), ifloor(z), seed) + 1;
      spots_.push_back(Vec3{x, (float)y, z});
    }
  }
  std::vector<DistantDrawItem>& items = dd.Items();
  items.clear();
  for (int i = 0; i < count_ && i < (int)spots_.size(); i++) {
    const size_t n = (size_t)i % names_.size();
    if (defOf_[n] < 0) continue;
    const MobDef& def = defs[(size_t)defOf_[n]];
    const Vec3 s = spots_[(size_t)i];
    DistantDrawItem it;
    it.look = keys_[n];
    it.origin = Vec3{s.x - def.worldSize.x * 0.5f, s.y, s.z - def.worldSize.z * 0.5f};
    // Facing the ring's centre (heading 0 = +Z: the facing is (sin h, cos h)).
    it.heading = std::atan2(centre_.x - s.x, centre_.z - s.z);
    const int kind = i % 5;
    it.dead = kind == 4;
    it.state = kind == 3 ? DistantDrawState::Strike
               : (kind == 1 || kind == 2) ? DistantDrawState::Walk
                                          : DistantDrawState::Idle;
    it.gaitPhase = (float)(timeSec * 1.6 + (double)i * 0.37);
    items.push_back(it);
  }
}

