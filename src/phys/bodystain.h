#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"
#include "phys/physics.h"
#include "sim/voxload.h"

struct MicroBodySet;

// ---- BLOOD ON A BODY (DESIGN.md section 7) ----------------------------------
//
// The one place that knows how to smear a stain over a body's lattice, shared
// by the live-limb cut (Mob::StainWound), the blast crater
// (Mob::CarveLimbRadial) and the corpse cut (DebrisSystem::DamageBody), so a
// limb cut off and cut again is bloodied by the same rule both times.
//
// A body stain is the 16-bit COAT word on PrefabVoxel::stain /
// DebrisVoxel::stain (the MATERIAL on the voxel in bits 0..11, amount 0..15 in
// bits 12..15), mirrored into the micro brick's stain lattice
// (sim/microbody.h) for rendering -- which is the only consumer that narrows
// it back to the world's 3-bit palette slot. It is PRESENTATION AND GAMEPLAY
// STATE that never touches the hashed grid (rule 1): it decides what a
// creature looks like, travels with its voxels into every fragment, and IS
// SAVED -- both body savers write the lattices as PODs, so the coat rides
// along (this comment claimed the opposite until 2026-09-13, and the code
// never agreed with it).

// Whichever of a body's two lattices is authoritative: the skin when it is a
// separate, finer lattice, else the collider. Same rule BurnLimbView applies.
struct StainLattice {
  std::vector<PrefabVoxel>* skin = nullptr;
  std::vector<DebrisVoxel>* coll = nullptr;
  size_t Size() const { return skin ? skin->size() : coll->size(); }
  IVec3 At(size_t i) const {
    return skin ? IVec3{(*skin)[i].x, (*skin)[i].y, (*skin)[i].z}
                : IVec3{(*coll)[i].x, (*coll)[i].y, (*coll)[i].z};
  }
  uint32_t Mat(size_t i) const {
    return skin ? (uint32_t)((*skin)[i].material & 0xFFFu)
                : (uint32_t)((*coll)[i].payload & 0xFFFu);
  }
  uint16_t Stain(size_t i) const {
    return skin ? (*skin)[i].stain : (*coll)[i].stain;
  }
  void SetStain(size_t i, uint16_t s) const {
    if (skin) (*skin)[i].stain = s; else (*coll)[i].stain = s;
  }
  // The skin's BRUISE byte (voxload.h PrefabVoxel::bruise). Only the fine
  // skin carries one: the coarse lattice reads 0 and ignores a write.
  uint8_t Bruise(size_t i) const { return skin ? (*skin)[i].bruise : 0u; }
  void SetBruise(size_t i, uint8_t b) const {
    if (skin) (*skin)[i].bruise = b;
  }
};

// ---- WHICH OF TWO COATS OWNS A VOXEL (rule-unification W2-J2) ------------
//
// ONE precedence rule for the ground and the body: sim/coatrule.h
// stainPrecedence, whose WGSL twin common.wgsl stainStep calls (the
// `stainprec` mirror). Clean or the same coat: climb. A washer meeting a
// foreign coat: rinse it down. Anything else meeting a foreign coat: displace
// it only if strictly heavier, or -- with a PAID level, which every body coat
// is -- if it outranks it by class: a corrosive coat over one that is not, and
// anything over a washer's wetness. That is the rule the body used to spell as
// RaiseBodyStain's "strictly larger" plus MobSystem::CoatBeneath, and the
// ground as stainStep's "never paint over a foreign stain"; the ground lays one
// level at a time and pays for it only on absorbent ground, which is why the
// weight clause never fires there and the class clause fires only where the
// displacement is paid for in liquid.
//
// The CLASS of a coat material is derived data (materials.json `washes`, and
// "its rules rewrite body matter or it is hot"), published by the owner of the
// material tables on every load. Legitimately different between the two
// populations: the ground names a coat by its PALETTE SLOT (3 bits in the
// voxel word), a body by its MATERIAL id (12 bits in the coat word).
constexpr uint8_t kBodyCoatWashes = 1u;
constexpr uint8_t kBodyCoatCorrodes = 2u;
void SetBodyCoatClasses(std::vector<uint8_t> classes);
uint32_t BodyCoatClassOf(uint32_t mat);

// Raise a voxel's coat toward `amt` of `mat`: the same material (or a clean
// voxel) keeps the larger amount; a DIFFERENT material displaces it by the
// precedence rule above (heavier, or outranking by class), so a splash of blood
// over a wet patch wins and a splash of water over blood does not repaint it
// (washing is a separate, subtractive rule). Returns the coat word.
uint16_t RaiseBodyStain(uint16_t cur, uint32_t mat, uint32_t amt);

// ---- ...AND A WASHING LIQUID ON A BODY ------------------------------------
//
// Water (materials.json `washes`) meeting a coat. Raise would leave blood
// where it is -- a splash of water is rarely heavier than the blood it lands
// on -- so a washer takes a different road: a FOREIGN coat is stepped down by
// `rinse`, and only a voxel that comes out clean (or was already clean, or
// already wet) takes the washer's own coat at `wetAmt`, keeping the larger of
// that and what is there. So washing and wetting are one rule: the water gets
// the blood off first and leaves the skin wet behind it. `wetAmt` 0 rinses
// without wetting. Returns the coat word.
uint16_t WashBodyStain(uint16_t cur, uint32_t washMat, uint32_t wetAmt,
                       uint32_t rinse);

// ---- ...AND THE OTHER WAY A COAT GETS DEEPER --------------------------------
//
// ADD `add` to what is already there, capped at `cap`. `RaiseBodyStain` above
// is a MAXIMUM and that is right for a splash -- being bled on twice does not
// make you twice as red, because the second splash is the same blood at the
// same strength. A BRUISE is the other shape entirely: it is an injury that
// ACCUMULATES, and asking Raise to express it would peg it at one blow's worth
// forever however many landed.
//
// The cross-material rule is Raise's (the precedence rule above): a different
// coat already in place is only repainted by a strictly larger amount or a
// coat that outranks it, so a bruise spreading under blood does not wash the
// blood off. Returns the coat word; `cur` when
// already at or past the cap, which is what makes the ceiling cheap to test.
uint16_t AddBodyStain(uint16_t cur, uint32_t mat, uint32_t add, uint32_t cap);

// ---- DISTANCE TO WHAT THE CARVE ACTUALLY TOOK -------------------------------
//
// A BALL ROUND A CENTROID IS NOT A CRATER. The blast crater's predicate removes
// with a chance that falls off to zero at the rim, so a GRAZE takes a scatter
// of voxels across the whole sphere rather than a clean bite out of one side.
// Its centroid is then somewhere in the middle of the limb and its RMS spread
// is most of the blast radius, so "soak a ball of that size" bloodies a quarter
// of the limb for eight lost voxels -- measured, `blast-stain`, 305 of 1344.
//
// What the owner asked for instead is blood "concentrated at areas where actual
// voxels are removed", and that is a distance to a SET, not to a point. This is
// that distance: a 3-4-5 chamfer over a box round the removed cells, two sweeps,
// O(cells), accurate to a few percent of true Euclidean at the two or three
// cells a wound rim actually uses. Distances are in LATTICE CELLS.
struct CellDist {
  IVec3 lo{}, dim{};
  std::vector<uint16_t> d;  // chamfer units, 3 per face step
  bool Empty() const { return d.empty(); }
  float At(int x, int y, int z) const {
    x -= lo.x;
    y -= lo.y;
    z -= lo.z;
    if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z)
      return 1e9f;
    return (float)d[((size_t)z * dim.y + y) * dim.x + x] * (1.0f / 3.0f);
  }
};
// `pad` cells of margin round the seeds' own bounds -- the field only has to
// cover the rim the caller means to stain, so it does not pay for the limb.
// Empty (and Empty() true) for no seeds or an absurd box.
CellDist BuildCellDist(const std::vector<IVec3>& seeds, int pad);

// The soak round a cut. `centre` and `radius` are in the LATTICE's own units.
//
// Every voxel in range takes a stain, bone included: an EXPOSED voxel (one
// with an empty 6-neighbour in this lattice, i.e. the walls of the hole and
// the skin round its mouth) takes `amountExposed` at the centre tapering to
// the rim with a per-voxel jitter; a buried one takes `amountBuried` with
// `buriedChance`. `boneMin` floors the exposed amount on any voxel that is
// NOT tissue (`tissue[mat]` false; an empty table means everything is
// tissue), so bone is always shown bloodied to some degree. Amounts are the
// 0..15 world scale, `mat` the MATERIAL doing the staining (this body's blood).
//
// `micro`/`model`: when the model is OWNED, every stained voxel is also poked
// into the brick's stain lattice, brick-local coordinates being the lattice's
// (the caller guarantees the brick was last rebased to the same frame -- a
// re-skin after the carve, before this). Pass model -1 to write the lattice
// only, e.g. when a re-skin is about to rewrite the whole brick anyway.
//
// Keyed on `seed` and the lattice position, so a replay stains the same
// voxels. Returns the number of voxels whose stain changed.
struct CutSoak {
  uint32_t mat = 0;  // the staining material (this body's blood), 0 = no soak
  float radius = 0.0f;
  int amountExposed = 15;
  int amountBuried = 6;
  float buriedChance = 0.35f;
  int boneMin = 5;
  const std::vector<uint8_t>* tissue = nullptr;
  // When set, the taper is measured from the nearest cell of `from` instead of
  // from `centre`, and `radius` is how far past it the smear reaches. See the
  // note on CellDist: a crater is a set, not a point.
  const CellDist* from = nullptr;
};
uint32_t SoakCut(const StainLattice& L, Vec3 centre, const CutSoak& p,
                 uint32_t seed, MicroBodySet* micro, int model);


// ---- A BLOW THAT DOES NOT BREAK THE SKIN -----------------------------------
//
// THE BRUISE LADDER, and it is three rungs: a cell DARKENS toward a ceiling, a
// cell already at that ceiling can SPLIT and bleed, and a split cell is PULPED
// and will crumble (and bleeds again whenever it is struck). All of it is
// per-voxel arithmetic over one lattice, which is why it belongs beside the
// cut's soak rather than inside the creature that used to own it.
//
// THE BRUISE IS THE SKIN, NOT A COAT (2026-09-26). The ladder used to live in
// the COAT word, which made a bruise something water rinsed off, something a
// splash of mud replaced, and something that could not wear blood over it;
// and since "pulped" was "wears blood at gore.pulpAmt", washing a beaten limb
// reset the beating. It now lives in PrefabVoxel::bruise, a level on the
// tissue itself (kBruiseBroken = split), and only the BLOOD a split cell
// sheds is a coat. Fine skins only -- see StainLattice::Bruise.
//
// WHY IT MOVED (2026-09-20). It was `Mob::BruiseLimb`, so a mace marked a
// living body and did nothing whatever to a corpse: the loose-matter path had
// only an instant crater, which is the shape the living path was given a
// bruise to stop using. A corpse is the same tissue one function call later —
// what it lacks is hp and a voice, not the capacity of flesh to discolour.
// One implementation, two callers (Mob::BruiseLimb, DebrisSystem::BruiseBody).
//
// Everything here is in LATTICE units and pre-resolved: the caller has already
// applied its own unarmed overrides, its power ramp and its scale, because
// only the caller knows which of those it has.
struct BruiseSoak {
  Vec3 centre{};            // lattice units
  float radius = 0.0f;      // lattice units
  uint32_t bloodMat = 0;    // what a broken bruise becomes (0 = never breaks)
  float step = 0.0f;        // coat added at the contact, before the taper
  uint32_t cap = 0;         // gore.bruiseMax, the global ceiling (<= 14)
  uint32_t bleedFrom = 0;   // bruise level at which a cell may split
  float bleedChance = 0.0f; // ...and how often it does
  float blowScale = 1.0f;   // how hard this blow was, 0..1
  const std::vector<uint8_t>* tissue = nullptr;  // bone does not bruise
  uint32_t seed = 0;
};
// What the blow found and what it left. `core`/`pulped` are the reading rung 3
// is scored against, taken BEFORE this blow changes anything — a blow must be
// scored against the damage it ARRIVED at, or the first blow that breaks the
// skin would also be the first that carves.
struct BruiseTally {
  uint32_t marked = 0;   // voxels whose bruise (or bleeding) changed
  uint32_t core = 0;     // exposed voxels in the inner half-radius
  uint32_t pulped = 0;   // ...of those, already split (kBruiseBroken)
  float Ripeness() const {
    return core == 0 ? 0.0f : (float)pulped / (float)core;
  }
};
uint32_t SoakBruise(const StainLattice& L, const BruiseSoak& p, BruiseTally* out,
                    MicroBodySet* micro, int model);
