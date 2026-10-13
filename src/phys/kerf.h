#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

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
  // ---- HOW FAR THE BLOW COULD GO (KerfBite below) --------------------------
  // `edgeHalf` is half the length of the edge that was swung, world voxels:
  // how wide a cross-section this blade can part in one pass. 0 = `length`
  // (a hand-built cut that did not say). `cleave` is the blow's bite PAST the
  // ordinary chip, in world voxels^2 of skin-equivalent cross-section: what a
  // committed swing of a heavy blade has left over once the chip is paid for.
  // 0 = an ordinary blow, which can still part what the chip's own area
  // covers (the last strip of a neck grooved almost through).
  float edgeHalf = 0.0f;
  float cleave = 0.0f;
  // A STAB (2026-09-26): the point was driven in along the blade's own length
  // (melee.cpp BuildStrikeParts). The slot is then a BORE -- `cutDir` is the
  // thrust, `edgeAxis` runs ACROSS the blade, so the slot's length is the
  // blade's width and its depth is how far the point went in -- and it is
  // never priced by KerfBite: a stab makes a hole, it does not part a limb.
  bool stab = false;
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
  // The edge came out the other side (KerfBite): a CLEAN parting, full depth
  // and full width with no wedge and no ragged rim, so the lattice really is
  // in two pieces afterwards (KerfKeepAt floors its thickness per lattice).
  bool through = false;
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
    // DEGENERATE INPUT: the edge axis and the travel coincide. A real thrust
    // no longer arrives here -- BuildStrikeParts turns it into a stab and
    // hands over the blade's WIDTH as `uLocal` -- so this is only a
    // hand-built cut that did not say. Any perpendicular will do.
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
  //
  // ...BUT NEVER THINNER THAN ONE LATTICE COLUMN (2026-09-25). A stock edge's
  // core is a third of a skin cell across, so whenever the slot's plane sat
  // between two layers of cell centres the probe caught nothing, the snap
  // returned 0, and the slot started wherever the hit point was -- measured
  // as ~40% of blows on a fine-skinned neck, each of them boring from the
  // middle of the limb instead of from its surface.
  const float hw = std::max(s.halfW * sc * 0.35f, 0.5f);
  const float hl = std::max(s.halfL * sc * 0.35f, 0.5f);
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

// ---- WHAT THE BLOW CAN PAY FOR (2026-09-25) ----------------------------------
//
// THE BITE IS PAID FOR IN MATERIAL. A blow has a budget — an area of
// cross-section, in world voxels^2 of skin-equivalent matter — and the plane it
// is cutting along charges it cell by cell: flesh cheaply, bone at three times
// skin (materials.json `hardness` over gear.cutHardnessRef, the ratio the
// shell rule already uses). If the budget covers EVERYTHING left in that plane
// within the edge's reach, the edge comes out the other side and the cut is
// THROUGH: a clean parting, and CarveLimb's cut-through rule does the rest.
//
// This is what makes a decapitation depend on the right things, and nothing
// else: the SIZE of the blade (the budget scales with heft), HOW WELL IT HIT
// (speed x edge alignment — the budget is `power`-shaped — and a plane across a
// 10-cm neck costs a fraction of one through a skull, or of one running along
// the neck), and WHAT IS ALREADY CUT (a notch is cells no longer in the
// plane, so a neck chopped half through is half the price). There is no
// counter and no roll: the same blow on the same flesh always does the same
// thing, and a blow that falls a little short cuts deep instead.
//
// `forEach` walks the authoritative lattice calling back probe(x, y, z,
// cost), cost being the voxel's resistance relative to skin. `startDepth` is
// where the ordinary chip ends (world voxels from the entry-snapped origin);
// `extra` is marched from there, so a blow with no cleave in it keeps exactly
// the chip it always had.
struct KerfBiteResult {
  bool through = false;  // the whole plane within reach was paid for
  float exit = 0.0f;     // far side of the matter, world voxels along w
  float depth = 0.0f;    // where `extra` ran out, world voxels along w
  float planeCost = 0.0f;  // the whole plane's price, world voxels^2
  bool blocked = false;  // matter in the plane lies past the edge's reach
};

template <class ForEach>
inline KerfBiteResult KerfBite(const KerfSlot& s, float latScale,
                               float edgeHalf, float budget, float startDepth,
                               float extra, ForEach forEach) {
  KerfBiteResult r;
  const float sc = std::max(latScale, 1e-3f);
  const Vec3 c = s.c * sc;
  const float back = s.back * sc;
  const float eh = std::max(edgeHalf, s.halfL) * sc;
  // ONE CELL THICK: the plane the edge travels in. |dv| <= 0.5 holds on
  // average exactly one cell per (u, w) column at any orientation, so the
  // cell count is the plane's area in cells whichever way the blade was held.
  std::vector<float> bins;
  bins.reserve(64);
  forEach([&](float x, float y, float z, float cost) {
    const Vec3 d{x + 0.5f - c.x, y + 0.5f - c.y, z + 0.5f - c.z};
    if (std::fabs(d.dot(s.v)) > 0.5f) return;
    const float dw = d.dot(s.w);
    if (dw < -back) return;
    if (std::fabs(d.dot(s.u)) > eh) {
      // Past the end of the edge: this pass cannot part the plane, whatever
      // the budget. It can still cut as deep as it pays for.
      r.blocked = true;
      return;
    }
    const size_t k = (size_t)std::floor(dw + back);
    if (k >= 4096) return;  // a bent limb folded far down the travel axis
    if (k >= bins.size()) bins.resize(k + 1, 0.0f);
    bins[k] += std::max(cost, 0.0f);
  });
  const float cellArea = 1.0f / (sc * sc);
  float total = 0.0f;
  size_t last = 0;
  for (size_t k = 0; k < bins.size(); k++)
    if (bins[k] > 0.0f) {
      total += bins[k];
      last = k + 1;
    }
  r.planeCost = total * cellArea;
  r.exit = ((float)last - back) / sc;
  r.through = !r.blocked && last > 0 && budget > 0.0f && r.planeCost <= budget;
  // How far `extra` reaches past the chip, charged bin by bin.
  r.depth = startDepth;
  float left = std::max(extra, 0.0f) / cellArea;
  size_t k = (size_t)std::max(0.0f, std::floor(startDepth * sc + back));
  float at = startDepth * sc + back;
  while (left > 0.0f && k < bins.size()) {
    const float frac = std::clamp((float)(k + 1) - at, 0.0f, 1.0f);
    const float price = bins[k] * frac;
    if (price > left) {
      at += frac * (left / price);
      left = 0.0f;
      break;
    }
    left -= price;
    at = (float)(k + 1);
    k++;
  }
  // Out of matter with budget to spare: the depth is the far side.
  r.depth = std::max(startDepth, (at - back) / sc);
  return r;
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
  bool through = false;
  // true = KEEP this voxel.
  bool operator()(float x, float y, float z) const {
    const Vec3 p{x + 0.5f, y + 0.5f, z + 0.5f};
    const Vec3 d = p - c;
    const float dw = d.dot(w);
    if (dw < -bk || dw > dep) return true;
    const float du = d.dot(u), dv = d.dot(v);
    // A CLEAN PARTING: the edge went all the way, so there is no wedge bottom
    // and no ragged rim to leave a bridge of flesh across the cut.
    if (through) return std::fabs(du) > hl || std::fabs(dv) > hw;
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
  // A THROUGH CUT MUST PART EVERY LATTICE IT IS TESTED ON. A blade is a
  // tenth of a voxel thick and a collider cell can be half a voxel, so the
  // real kerf falls between the coarse lattice's cell centres and physics
  // would go on holding the head on by a slab the art shows cut. Half a cell
  // plus a hair is the thinnest slab no 6-connected path can step across.
  k.through = s.through;
  if (s.through) k.hw = std::max(k.hw, 0.51f);
  return k;
}

// ============================================================================
// A POINT DRIVEN THROUGH (2026-10-12; docs/PLAN_projectiles.md package B)
// ============================================================================
//
// The kerf above is an EDGE: a slot that travels across the thing it cuts and
// is priced by the PLANE it parts. A penetrator is the other blow a blade-shaped
// thing can deliver: a POINT that travels along its own axis and is priced by
// the LENGTH of matter it pushes through. An arrow, a thrown spear, a bolt of
// iron out of a spell -- one model for all of them, and it is the same pricing
// KerfBite already uses (materials.json `hardness` over gear.cutHardnessRef),
// so a body is as hard to shoot through as it is to cut through, by what it is
// made of, with no per-race rule anywhere: an automaton's brass and a sylvan's
// bark stop an arrow that goes through a man because brass and bark are harder
// than skin, and for no other reason.
//
// THE CHANNEL IS A THIN CYLINDER ON THE STRUCK THING'S FINE LATTICE (the
// owner's decision 2): an arrow is a centimetre across, so it takes a tube of
// skin cells out of a man, not a world voxel. It walks the axis cell by cell
// in order of depth, paying for each one:
//
//   cost(cell) = gore.pierceCellJ * (cell.hardness / gear.cutHardnessRef)
//              * soft(cell.hardness / tip.hardness) / max(sharpness, floor)
//              * (cell volume / fine cell volume)
//
//   soft(r) = r^gore.pierceSoftExp when r > 1 (the point is SOFTER than what
//   it meets: a wooden point on iron), 1 otherwise.
//
// Energy left when the axis runs out of matter -> EXITED, and the flight goes
// on with the residual speed (through an arm into the torso, through one man
// into the next). Energy gone inside -> EMBEDDED at that depth. Not enough to
// pay for the first fine cell of matter at all -> GLANCED: nothing is carved
// and the caller falls back to trauma (pebbles bruise, arrows skip off plate).
//
// Everything that FOLLOWS -- the hp the lost tissue costs, the brain charge,
// the bleed, the gore particles, the wound soak -- is the ordinary carve path
// (Mob::CarveLimb, DebrisSystem::DamageBody). This header only says which
// cells, and what the flight has left.

// The FINE lattice a tip is measured on: the human art's 80 cells per metre
// (1.25 cm). A tip radius is stated in these whatever lattice the struck thing
// is on, and a cell's price is scaled by its volume against one of these, so
// the energy per unit length of channel is a property of the matter and the
// point, never of the struck art's resolution.
inline constexpr float kPierceRefCellsPerMetre = 80.0f;
// The thinnest channel a lattice can carve without the axis slipping between
// cell centres: half a cell's space diagonal. A tip thinner than this on a
// coarse lattice is WIDENED to it and its per-cell price scaled down by the
// area ratio, so it still pays the physical energy per unit length.
inline constexpr float kPierceMinRadiusCells = 0.8661f;

// ---- ONE PROJECTILE, AS PHYSICS ---------------------------------------------
// What the struck thing needs to know about what hit it. Everything a flight
// knows about speed and aim stays with the flight.
struct Penetrator {
  float massKg = 0.03f;     // a war arrow is 25..60 g
  float speedMps = 0.0f;    // at the moment of contact
  float tipRadius = 1.0f;   // in FINE cells (1/kPierceRefCellsPerMetre m)
  // How hard the point is (materials.json `hardness` scale). 0 = take it
  // from `material`; 0 there too = harder than anything (no soft penalty).
  float hardness = 0.0f;
  float sharpness = 1.0f;   // 0..1: a bodkin 1, a broadhead ~0.8, a stone ~0.05
  uint32_t material = 0;    // the projectile's matter, 0 = not named
  // A flat hp charge on top of the tissue the channel destroys -- a weapon
  // stat, the way StrikeProfile::cut is melee's. 0 = the wound alone, which
  // is what the `pierce` gate measures.
  float hp = 0.0f;
  // Draw key for the wound soak. NOT a tick: the same shot replayed against
  // the same body must bleed the same way (KerfCut::seed).
  uint32_t seed = 0;
  float EnergyJ() const {
    return 0.5f * std::max(massKg, 0.0f) * speedMps * speedMps;
  }
  float SpeedFor(float energyJ) const {
    return massKg > 1e-6f ? std::sqrt(2.0f * std::max(energyJ, 0.0f) / massKg)
                          : 0.0f;
  }
};

enum class PierceOutcome : uint8_t { Glanced = 0, Embedded, Exited };

inline const char* PierceOutcomeName(PierceOutcome o) {
  switch (o) {
    case PierceOutcome::Glanced: return "glanced";
    case PierceOutcome::Embedded: return "embedded";
    case PierceOutcome::Exited: return "exited";
  }
  return "?";
}

// Cells the channel took, and what they cost, per material.
struct PierceMatCells {
  uint32_t mat = 0;
  uint32_t cells = 0;
  float joules = 0.0f;
};

// ---- WHAT ONE BODY DID TO ONE PROJECTILE ------------------------------------
struct PierceResult {
  bool hit = false;            // the handle was a body this caller owns
  // ...but no matter of it lay on the axis: the ray met its collider (a box
  // merge's convex radius, or a body rebuilt under a new handle by a carve
  // the same flight just made) and passed it by. Outcome Exited, nothing paid.
  bool missed = false;
  PierceOutcome outcome = PierceOutcome::Glanced;
  float energyIn = 0.0f;       // J at the entry face
  float energyLeft = 0.0f;     // J past the exit (Exited only)
  float exitSpeedMps = 0.0f;   // ...as a speed, for the flight to continue at
  float firstLayerJ = 0.0f;    // what the first fine cell of matter would cost
  float depth = 0.0f;          // world voxels of channel, entry face -> stop/exit
  Vec3 entryWorld{}, stopWorld{};      // where it went in, where it stopped/left
  Vec3 entryLocal{}, stopLocal{}, dirLocal{};  // the same, in the body's frame
  uint32_t cells = 0;          // lattice cells the channel took
  uint32_t carved = 0;         // ...of which the carve really removed
  uint32_t stopMat = 0;        // what it could not pay for (Embedded/Glanced)
  std::vector<PierceMatCells> byMat;
  float hpCharged = 0.0f;      // creature path: hp the wound cost (all causes)
  bool killed = false;         // creature path: alive before, dead after
  bool severed = false;        // the struck slot left the creature
  bool loose = false;          // the struck thing was a loose body, not a limb
};

// ---- THE PRICE OF ONE CELL ---------------------------------------------------
// The four numbers, so this header stays free of the tuning struct (and of its
// rebuild fan-out); PierceCostOf reads them out of any Tuning.
struct PierceCost {
  float cellJ = 1.0f;       // gore.pierceCellJ
  float hardRef = 8.0f;     // gear.cutHardnessRef
  float softExp = 2.0f;     // gore.pierceSoftExp
  float sharpFloor = 0.05f; // gore.pierceSharpFloor
};
template <class TuningT>
inline PierceCost PierceCostOf(const TuningT& t) {
  PierceCost c;
  c.cellJ = t.gore.pierceCellJ;
  c.hardRef = t.gear.cutHardnessRef;
  c.softExp = t.gore.pierceSoftExp;
  c.sharpFloor = t.gore.pierceSharpFloor;
  return c;
}
// ---- A GLANCE IS A THROWN THING'S BLOW ---------------------------------------
// What a point that could not get in delivers instead: trauma, by the CONTACT
// rows (gore.contactImpulseMin / contactHpPerImpulse / contactMaxHp) -- the rule
// MobSystem::ApplyContactDamage bills a thrown rock with, impulse in kg m/s.
// Nothing new decides how much a stone hurts. `power` is hp over the cap, the
// contact path's bruise scale.
struct PierceGlance {
  float hp = 0.0f;
  float power = 0.0f;
};
template <class TuningT>
inline PierceGlance PierceGlanceOf(const TuningT& t, const Penetrator& pen) {
  const auto& g = t.gore;
  const float impulse =
      std::max(pen.massKg, 0.0f) * std::max(pen.speedMps, 0.0f);
  PierceGlance r;
  const float hp = (impulse - g.contactImpulseMin) * g.contactHpPerImpulse;
  r.hp = hp > 0.0f ? std::min(hp, std::max(g.contactMaxHp, 0.0f)) : 0.0f;
  r.power = g.contactMaxHp > 0.0f ? std::clamp(r.hp / g.contactMaxHp, 0.0f, 1.0f)
                                  : 1.0f;
  return r;
}
// J to drive a point of `tipHard` through one cell of `cellHard` matter.
// `volScale` is the cell's volume against a fine cell, times the area
// correction PierceLatticeFor computed. Unknown matter (hardness 0) is
// charged as skin, the convention KerfBite's resistance table keeps.
inline float PierceCellJ(const PierceCost& c, float cellHard, float tipHard,
                         float sharpness, float volScale) {
  const float rel = (cellHard > 0.0f && c.hardRef > 0.0f) ? cellHard / c.hardRef
                                                          : 1.0f;
  float soft = 1.0f;
  if (tipHard > 0.0f && cellHard > tipHard && c.softExp > 0.0f)
    soft = std::pow(cellHard / tipHard, c.softExp);
  const float sharp = std::max(std::clamp(sharpness, 0.0f, 1.0f),
                               std::max(c.sharpFloor, 1e-3f));
  return std::max(c.cellJ, 0.0f) * rel * soft / sharp *
         std::max(volScale, 0.0f);
}

// ---- THE TIP, ON ONE LATTICE -------------------------------------------------
// `latScale` = the struck lattice's cells per world voxel; `voxelMeters` =
// the world voxel (sim/world.h kVoxelMeters). `rCells` is the channel radius
// in that lattice's cells; `volScale` the per-cell price factor.
struct PierceLattice {
  float rCells = 1.0f;
  float volScale = 1.0f;
};
inline PierceLattice PierceLatticeFor(float tipRadiusFine, float latScale,
                                      float voxelMeters) {
  PierceLattice L;
  const float ref = kPierceRefCellsPerMetre * voxelMeters;  // fine per voxel
  const float s = std::max(latScale, 1e-3f);
  // A tip thinner than a quarter of a fine cell is a needle the lattice
  // cannot price: held at that, so a zero radius is not free passage.
  const float rPhys = std::max(tipRadiusFine, 0.25f) * s / ref;
  L.rCells = std::max(rPhys, kPierceMinRadiusCells);
  const float cell = ref / s;  // this lattice's cell edge, in fine cells
  L.volScale = cell * cell * cell * (rPhys * rPhys) / (L.rCells * L.rCells);
  return L;
}

// ---- THE AXIS TEST, SHARED BY THE CHANNEL AND ITS CARVE ----------------------
// ONE expression, so the cells the channel PAID for and the cells the carve
// REMOVES are the same set to the last ulp on the authoritative lattice.
// `o` is the origin in lattice units, `d` the unit axis.
inline bool PierceInAxis(float x, float y, float z, const Vec3& o, const Vec3& d,
                         float r2, float& t) {
  const Vec3 p{x + 0.5f - o.x, y + 0.5f - o.y, z + 0.5f - o.z};
  t = p.dot(d);
  if (t < 0.0f) return false;
  return p.dot(p) - t * t <= r2;
}

struct PierceChannelResult {
  bool any = false;      // there was matter on the axis at all
  bool glanced = false;  // ...and the point could not pay for its first cell
  bool exited = false;   // paid for everything on the axis
  float tEntry = 0.0f;   // world voxels along the axis from the origin
  float tStop = 0.0f;    // the channel ends here (world voxels)
  float tExit = 0.0f;    // far side of the last cell paid (exited)
  // THE CARVE'S BOUND, EXACT: the channel is every cell on the axis whose
  // DEPTH BIN, floor(t - t0Lat) in lattice units, is below binStop (INT_MAX
  // when it exited). PierceKeepAt must be handed these, not a world-voxel
  // round trip of them: t/s*s is not t.
  float t0Lat = 0.0f;
  int binStop = 0;
  float firstLayerJ = 0.0f;
  float spentJ = 0.0f, leftJ = 0.0f;
  uint32_t cells = 0;
  uint32_t stopMat = 0;  // commonest matter of the depth it could not pay for
  std::vector<PierceMatCells> byMat;
};

// Walk the axis through ONE lattice. `oLocal`/`dLocal` are the axis in the
// body's frame (world-voxel units, `dLocal` unit); `rCells` the channel radius
// in this lattice's cells. `forEach` calls back probe(x, y, z, material) for
// every non-empty cell of the lattice; `costOf(material)` is ONE FULL CELL of
// that matter's price in J (PierceCellJ).
//
// PAID BY THE DEPTH, NOT BY THE CELL COUNT. The cells whose centres fall within
// `rCells` of an axis are 2 to 5 per cell of depth for a radius of one cell,
// depending only on where the axis crosses the lattice: priced per cell, the
// same arrow shifted half a centimetre paid more than twice as much (measured
// in the first `pierce` run: 2-cell slices through one head, 5-cell slices
// through the next). So the axis is cut into DEPTH BINS one lattice cell long
// from the first matter, and each bin is charged its occupied cells' prices
// scaled by (pi r^2 / the bin's GEOMETRIC cell count) -- the cells a full bin
// of the cylinder holds, matter or not. A full bin of one matter then costs
// pi r^2 cells' worth whatever the alignment; a bin half in air (the curve of
// a limb's surface) costs half. A bin is paid whole or not at all, so the
// channel is exactly the bins paid and the carve (PierceKeepAt) takes exactly
// that.
//
// GLANCE: the first bin -- the first lattice cell of depth of matter -- is
// what the point must be able to pay for to get in at all.
template <class ForEach, class CostOf>
inline PierceChannelResult PierceChannel(Vec3 oLocal, Vec3 dLocal,
                                         float latScale, float rCells,
                                         float energyJ, ForEach forEach,
                                         CostOf costOf) {
  PierceChannelResult r;
  const float s = std::max(latScale, 1e-3f);
  const Vec3 o = oLocal * s;
  const float dl = dLocal.len();
  const Vec3 d = dl > 1e-6f ? dLocal * (1.0f / dl) : Vec3{0, 0, 1};
  const float r2 = rCells * rCells;
  struct Cell {
    float t;
    int16_t x, y, z;
    uint32_t mat;
  };
  std::vector<Cell> cells;
  cells.reserve(256);
  forEach([&](float x, float y, float z, uint32_t mat) {
    float t = 0.0f;
    if (!PierceInAxis(x, y, z, o, d, r2, t)) return;
    cells.push_back({t, (int16_t)x, (int16_t)y, (int16_t)z, mat});
  });
  r.leftJ = std::max(energyJ, 0.0f);
  r.binStop = INT_MAX;
  if (cells.empty()) {
    r.exited = true;  // nothing of this lattice on the axis: passed it by
    return r;
  }
  // A total order, so equal depths cannot reorder by however the lattice
  // happened to be listed.
  std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
    if (a.t != b.t) return a.t < b.t;
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
  });
  r.any = true;
  const float t0 = cells.front().t;
  r.t0Lat = t0;
  r.tEntry = t0 / s;
  auto binOf = [t0](float t) { return (int)std::floor(t - t0); };
  // A body folded far down its own axis is not a channel anyone can fly:
  // the bins are bounded (rule 2), and matter past the bound is not reached.
  constexpr int kMaxBins = 4096;
  const int nBins = std::min(binOf(cells.back().t) + 1, kMaxBins);
  // The cylinder's GEOMETRIC cell count in bin k: every lattice point inside
  // it, matter or not. One small box per bin paid.
  auto geomCount = [&](int k) {
    const Vec3 a = o + d * (t0 + (float)k);
    const Vec3 b = o + d * (t0 + (float)k + 1.0f);
    const int lo[3] = {(int)std::floor(std::min(a.x, b.x) - rCells) - 1,
                       (int)std::floor(std::min(a.y, b.y) - rCells) - 1,
                       (int)std::floor(std::min(a.z, b.z) - rCells) - 1};
    const int hi[3] = {(int)std::ceil(std::max(a.x, b.x) + rCells) + 1,
                       (int)std::ceil(std::max(a.y, b.y) + rCells) + 1,
                       (int)std::ceil(std::max(a.z, b.z) + rCells) + 1};
    int n = 0;
    for (int z = lo[2]; z <= hi[2]; z++)
      for (int y = lo[1]; y <= hi[1]; y++)
        for (int x = lo[0]; x <= hi[0]; x++) {
          float t = 0.0f;
          if (PierceInAxis((float)x, (float)y, (float)z, o, d, r2, t) &&
              binOf(t) == k)
            n++;
        }
    return n;
  };
  constexpr float kPi = 3.14159265f;
  const float area = kPi * r2;
  auto tally = [&](uint32_t mat, float j) {
    for (PierceMatCells& m : r.byMat)
      if (m.mat == mat) {
        m.cells++;
        m.joules += j;
        return;
      }
    r.byMat.push_back({mat, 1u, j});
  };
  size_t i = 0;
  for (int k = 0; k < nBins && i < cells.size(); k++) {
    size_t j = i;
    float price = 0.0f;
    while (j < cells.size() && binOf(cells[j].t) <= k) {
      price += costOf(cells[j].mat);
      j++;
    }
    if (j == i) continue;  // a gap in the matter: nothing to pay
    const float norm = area / (float)std::max(geomCount(k), 1);
    price *= norm;
    if (k == 0) r.firstLayerJ = price;
    if (price > r.leftJ) {
      r.glanced = k == 0;
      r.binStop = k;
      r.tStop = (t0 + (float)k) / s;
      // What stopped it: the commonest matter of the bin it could not pay.
      uint32_t best = cells[i].mat, bestN = 0;
      for (size_t a = i; a < j; a++) {
        uint32_t n = 0;
        for (size_t b = i; b < j; b++) n += cells[b].mat == cells[a].mat;
        if (n > bestN) {
          bestN = n;
          best = cells[a].mat;
        }
      }
      r.stopMat = best;
      return r;
    }
    r.leftJ -= price;
    r.spentJ += price;
    for (size_t a = i; a < j; a++) {
      tally(cells[a].mat, costOf(cells[a].mat) * norm);
      r.cells++;
    }
    i = j;
  }
  if (i < cells.size()) {
    // Past the bin bound: it stops there, as if at a wall.
    r.binStop = nBins;
    r.tStop = (t0 + (float)nBins) / s;
    r.stopMat = cells[i].mat;
    return r;
  }
  r.exited = true;
  r.tExit = (cells.back().t + 0.5f) / s;
  r.tStop = r.tExit;
  r.binStop = INT_MAX;
  return r;
}

// The carve that follows a channel, at `scale` cells per world voxel: true =
// KEEP. On the channel's own lattice (`scale == latScale`) the test is the
// channel's own -- PierceInAxis, then the depth bin -- with the channel's own
// numbers, so it removes exactly the cells that were paid; on another lattice
// (a fine limb's collider, re-derived from the skin anyway) it is the same
// cylinder, converted.
struct PierceKeep {
  Vec3 o, d;
  float r2 = 0.0f, t0 = 0.0f, binLen = 1.0f;
  int binStop = 0;
  bool operator()(float x, float y, float z) const {
    float t = 0.0f;
    if (!PierceInAxis(x, y, z, o, d, r2, t)) return true;
    if (binStop == INT_MAX) return false;
    const int bin = binLen == 1.0f ? (int)std::floor(t - t0)
                                   : (int)std::floor((t - t0) / binLen);
    return bin >= binStop;
  }
};
// `t0Lat` / `binStop` are PierceChannelResult's (the channel lattice's units).
inline PierceKeep PierceKeepAt(Vec3 oLocal, Vec3 dLocal, float latScale,
                               float rCells, float t0Lat, int binStop,
                               float scale) {
  PierceKeep k;
  const float dl = dLocal.len();
  k.d = dl > 1e-6f ? dLocal * (1.0f / dl) : Vec3{0, 0, 1};
  k.binStop = binStop;
  const float s = std::max(latScale, 1e-3f);
  if (scale == s) {
    // The channel's own expressions, verbatim.
    k.o = oLocal * s;
    k.r2 = rCells * rCells;
    k.t0 = t0Lat;
    k.binLen = 1.0f;
    return k;
  }
  const float kk = scale / s;
  k.o = oLocal * scale;
  k.r2 = (rCells * kk) * (rCells * kk);
  k.t0 = t0Lat * kk;
  k.binLen = kk;
  return k;
}
