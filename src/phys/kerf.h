#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "math3d.h"
#include "sim/rng.h"

// THE SHAPE A BLADE TAKES OUT OF SOMETHING, once, for everything it can be
// taken out of.
//
// WHY THIS IS ITS OWN HEADER. There are two populations a sword can hit and
// they have nothing else in common: a LIVE LIMB (game/mob.cpp Mob::CutLimb),
// which is held in a joint chain and driven by an animation rig, and a DEBRIS
// BODY (phys/debris.cpp DebrisSystem::CutBody), which answers to nobody. The
// carve TAILS are rightly separate — one has to keep a rig's anchors agreeing
// with the new geometry and the other has to rebuild a Jolt compound — but the
// WOUND ITSELF must not be, and until 2026-09-19 it was not merely separate,
// it did not exist on the second population at all:
//
//   melee.cpp, before: "LIVE FLESH CARVES; DEBRIS MELTS ... a loose body has
//   no wound model to speak of, so EVERYTHING that can hurt melts it in one
//   call" — a SPHERE of the blade's own half-width, bored out of a corpse per
//   swing tick. That is the shape the live path was given a kerf to stop
//   using, and mob.h's BladeCut says why in as many words: "at any radius
//   large enough to feel like a sword, [it takes] most of the arm". The owner
//   reported it from the other end on 2026-09-19 — "swords shouldn't delete so
//   many voxels off of corpses" — which is the same sentence about the same
//   sphere, one population later.
//
// So the geometry lives here and both callers build it from the same four
// lines. A kerf is a SLOT: it enters at a point, travels the way the swing is
// going, is as long as the part of the edge that made contact, and narrows
// toward its bottom because a blade is a wedge.
//
// Everything in KerfCut is in WORLD voxels and world directions. The resolved
// KerfSlot is in the struck object's OWN frame, in world-voxel units still —
// the per-lattice scaling happens last, in KerfKeep, which is what lets one
// slot carve a fine skin and a coarse collider without either knowing about
// the other.

// ---- ONE EDGE BLOW ----------------------------------------------------------
//
// The three axes are independent and all three are MEASURED rather than tuned:
// `edgeAxis` comes from the item's authored edge segment through its live pose,
// `cutDir` from how far the tip actually travelled this tick, and `halfWidth`
// from the blade's own authored thickness. Only the DEPTH is a tuning
// question, because only the depth depends on how hard you swung.
struct KerfCut {
  Vec3 at{};          // contact point, world voxels
  Vec3 edgeAxis{};    // unit, along the blade's edge (the slot's long axis)
  Vec3 cutDir{};      // unit, the direction the edge is travelling
  float halfWidth = 0.25f;  // kerf half-thickness, world voxels
  float depth = 0.5f;       // how far in the edge bit, world voxels
  float length = 1.0f;      // half-length of the slot along the edge
  float power = 1.0f;       // 0..1 swing commitment, for the audio severity
  uint32_t seed = 0;        // ragged-rim / stain draw key. NOT a tick: the
                            // same stroke replayed must tear the same way.
};

// ---- THE SAME BLOW, IN THE STRUCK THING'S FRAME -----------------------------
struct KerfSlot {
  Vec3 c{};            // slot origin, object-local, world-voxel units
  Vec3 u{1, 0, 0};     // along the edge     (the slot's length)
  Vec3 v{0, 0, 1};     // the flat of the blade (the slot's thickness)
  Vec3 w{0, -1, 0};    // the way the edge is travelling (the slot's depth)
  float depth = 0.0f;
  float halfW = 0.0f;
  float halfL = 0.0f;
  float back = 0.0f;   // how far BEHIND the entry the slot starts
  float jitterScale = 1.0f;  // lattice the ragged rim is quantized onto
  uint32_t seed = 0;
};

// Build the slot's orthonormal frame. `cLocal`, `uLocal` and `wLocal` are
// KerfCut::at / edgeAxis / cutDir already brought into the object's frame by
// the caller (only the caller knows its own rotation convention).
inline KerfSlot KerfFrame(Vec3 cLocal, Vec3 uLocal, Vec3 wLocal, float depth,
                          float halfW, float halfL, float jitterScale,
                          uint32_t seed) {
  KerfSlot s;
  const float ul = uLocal.len(), wl = wLocal.len();
  s.u = ul > 1e-4f ? uLocal * (1.0f / ul) : Vec3{1, 0, 0};
  s.w = wl > 1e-4f ? wLocal * (1.0f / wl) : Vec3{0, -1, 0};
  s.v = s.u.cross(s.w);
  if (s.v.len() < 0.15f) {
    // A THRUST, NOT A CUT: the edge is travelling along its own length, so
    // "the flat of the blade" is undefined and the cross product is noise.
    // Any perpendicular will do — the slot is then a round-ish bore, which is
    // what a thrust actually makes.
    s.v = s.u.cross(Vec3{0, 1, 0});
    if (s.v.len() < 0.15f) s.v = s.u.cross(Vec3{1, 0, 0});
  }
  s.v = s.v.normalized();
  // Re-derive the travel axis from the two exact ones, so the frame is
  // orthonormal by construction instead of by however square the inputs
  // happened to be.
  s.w = s.v.cross(s.u).normalized();
  s.c = cLocal;
  s.depth = std::max(depth, 0.0f);
  s.halfW = std::max(halfW, 1e-3f);
  s.halfL = std::max(halfL, 1e-3f);
  // HOW FAR BACK THE SLOT STARTS. `at` is a ray hit on the JOLT COLLIDER,
  // which is a greedy box merge inflated by a convex radius — it is near the
  // surface, not on it. Small, because the entry is SNAPPED TO MATTER by
  // KerfEntry below; it only has to cover the last fraction of a lattice cell.
  s.back = 0.15f * s.depth + 0.10f;
  s.jitterScale = std::max(jitterScale, 1.0f);
  s.seed = seed;
  return s;
}

// ---- WHERE THE EDGE MEETS MATTER --------------------------------------------
//
// THE SLOT STARTS AT THE SURFACE, NOT AT THE HIT POINT, and this is what makes
// "sustained hits dismember" work at all. A kerf of fixed depth placed at a
// fixed point SATURATES: the first blow empties the slot and every blow after
// it finds that space already gone. Measured on a live thigh before it existed
// — 46 identical cuts took 2.6% each and never severed, because they were all
// the same 2.6%.
//
// The physical statement is "the edge bites `depth` into whatever it first
// meets", so the entry plane is FOUND: the smallest projection onto the travel
// axis over the voxels inside the slot's own cross-section.
//
// `forEach` walks the object's authoritative lattice, calling back with each
// voxel's lattice coordinate. `latScale` is that lattice's cells per world
// voxel. Returns the offset along `w`, already divided back into world voxels,
// to add to the slot origin; 0 when nothing was found.
template <class ForEach>
inline float KerfEntry(const KerfSlot& s, float latScale, ForEach forEach) {
  const float sc = std::max(latScale, 1e-3f);
  const Vec3 c = s.c * sc;
  // THE CORE OF THE SLOT, NOT ALL OF IT. The edge is at the middle of the
  // kerf; the rest of the footprint is the wedge behind it, and the ragged rim
  // deliberately leaves stragglers out there. Probing the whole footprint lets
  // one surviving rim voxel pin the entry plane at the original surface, so
  // the groove stops advancing while the blade goes on shaving its own rim —
  // measured at 31 blows to part a thigh instead of four.
  const float hw = s.halfW * sc * 0.35f, hl = s.halfL * sc * 0.35f;
  float best = 1e30f;
  forEach([&](float x, float y, float z) {
    const Vec3 d{x + 0.5f - c.x, y + 0.5f - c.y, z + 0.5f - c.z};
    if (std::fabs(d.dot(s.u)) > hl || std::fabs(d.dot(s.v)) > hw) return;
    best = std::min(best, d.dot(s.w));
  });
  if (best > 1e29f) return 0.0f;
  const float e = best / sc;
  // CLAMPED, because "the nearest matter in this column" is not always "the
  // surface the blade met": a limb bent back on itself can put a hand's worth
  // of voxels a long way up the travel axis, and an unclamped snap would
  // teleport the slot there and cut something the edge never touched.
  const float lim = s.depth + 1.0f;
  return (e > -lim && e < lim) ? e : 0.0f;
}

// ---- THE SLOT ITSELF, AT ONE LATTICE'S RESOLUTION ---------------------------
//
// A functor rather than a lambda so both carve families can hold it: the limb
// path wants bool(int,int,int) and the debris path bool(float,float,float),
// and one call operator taking floats serves both.
struct KerfKeep {
  Vec3 c, u, v, w;
  float dep, hw, hl, bk, toSkin;
  uint32_t seed;
  // true = KEEP this voxel.
  bool operator()(float x, float y, float z) const {
    const Vec3 p{x + 0.5f, y + 0.5f, z + 0.5f};
    const Vec3 d = p - c;
    const float dw = d.dot(w);
    if (dw < -bk || dw > dep) return true;
    const float du = d.dot(u), dv = d.dot(v);
    // A BLADE IS A WEDGE. The slot narrows toward its bottom, in both of the
    // axes that are not the travel direction: that is what makes a shallow
    // contact a chip and a deep one a gash, from one shape, with no second
    // case. `t` is 0 at the entry face.
    const float t = dep > 1e-4f ? std::clamp(dw / dep, 0.0f, 1.0f) : 0.0f;
    const float wAt = hw * (1.0f - 0.55f * t);
    const float lAt = hl * (1.0f - 0.35f * t);
    const float fu = std::fabs(du), fv = std::fabs(dv);
    if (fu > lAt || fv > wAt) return true;
    // RAGGED RIM: certain removal in the core of the slot, thinning to nothing
    // at its edge, quantised onto the SKIN lattice so both passes tear the
    // same way and the wound's shape is a property of the art rather than of
    // whichever collider resolution the object happened to derive.
    //
    // CPU gameplay state — neither limbs nor bodies are in the hashed domain —
    // so a float hash is fine here; rule 1 governs the grid, and everything a
    // cut puts INTO the grid goes through the ordinary ParticleSpawn/BrushOp
    // streams.
    const float e =
        std::max(fu / std::max(lAt, 1e-3f), std::max(fv / std::max(wAt, 1e-3f), t));
    const float chance = 1.0f - e * e;
    const int sx = (int)std::floor(x * toSkin);
    const int sy = (int)std::floor(y * toSkin);
    const int sz = (int)std::floor(z * toSkin);
    const uint32_t h = rng::Hash3(seed, (uint32_t)sx * 73856093u,
                                  (uint32_t)sy * 19349663u ^
                                      (uint32_t)sz * 83492791u);
    return (float)(h & 0xFFFFu) / 65535.0f >= chance;
  }
};

// ONE world-space slot, re-expressed at `scale` lattice cells per world voxel.
// Every consumer builds one of these per lattice it tests, so a fine-skinned
// object loses the same physical volume from both of its lattices.
inline KerfKeep KerfKeepAt(const KerfSlot& s, float scale) {
  KerfKeep k;
  k.c = s.c * scale;
  k.u = s.u;
  k.v = s.v;
  k.w = s.w;
  k.dep = s.depth * scale;
  k.hw = s.halfW * scale;
  k.hl = s.halfL * scale;
  k.bk = s.back * scale;
  k.toSkin = s.jitterScale / scale;
  k.seed = s.seed;
  return k;
}
