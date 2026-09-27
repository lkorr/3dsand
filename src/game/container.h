#pragma once
// VESSELS: a flask scoops liquid out of the grid, a pouch scoops powder, and
// both pour it back out (ItemKind::Container, items.json's `container` block).
//
// ONE KIND, NOT TWO. What a vessel takes up is data (`holds`: a set of
// material CLASSES), so a flask and a pouch are two rows of items.json and a
// bucket or a sack of salt would be a third and fourth with no code.
//
// THE MATTER IS CONSERVED, through the MutationQueue both ways (rule 3):
//   * a scoop is a list of CONDITIONAL CLEARS (world.h CellOpClearIfMat), one
//     per cell, decided off the snapshot mirror. The snapshot is
//     kSnapshotLatency ticks old and liquid moves, so the op names the
//     material it expects and the GPU refuses it if something else has flowed
//     in -- nothing that was not what you scooped is ever deleted.
//   * AND THE VESSEL IS CREDITED WITH WHAT THE GPU TOOK, NOT WHAT THE CPU
//     ASKED FOR. Measured before this: a flask held over a levelling pool was
//     credited 125 eighths while the grid lost 113, because the water kept
//     running into the holes during the snapshot's four ticks and every cell
//     was paid for at its stale fullness -- scoop, pour back, repeat, and
//     water is minted. So a scoop files a CLAIM; sim_mutate adds what each
//     clear really removed to the scoop ledger (world.h kPageFaultScoop*); and
//     when that tick's snapshot arrives, ContainerSettle pays min(claim,
//     removed). Four ticks late, and exact.
//   * a pour is ordinary GRID PARTICLES (not micro spray): each one is a whole
//     cell, flies the kernel's own ballistic arc and reinserts where it lands,
//     so poured water is water on the ground and poured sand is a heap.
//
// THE STREAM IS ALSO A SPLATTER (mob.h SplatterEvent). GPU particles cannot
// see a limb, so the CPU records the same arc and MobSystem::StainLimbs
// replays it against every body in reach -- living, corpse or the player. That
// is the whole of "pour blood on somebody and they are stained where it hit":
// the same road a severed artery's spray already takes.
//
// Everything here is pure (no GPU, no physics, no MobSystem) so the gate can
// drive it against a hand-built grid.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/item.h"
#include "math3d.h"
#include "phys/fillview.h"
#include "sim/materials.h"
#include "sim/world.h"

struct SplatterEvent;
class Physics;
class DebrisSystem;
class WorldItems;

// The raw voxel word at a world cell, false when the cell is not known (outside
// the snapshot mirror). The live game binds this to World::Snap().mirror; the
// gate binds it to a map.
using ContainerWordFn = std::function<bool(IVec3, uint32_t&)>;

// The raw word from the snapshot's 3x3x3 mirror around the player.
bool ContainerSnapWord(const World& world, IVec3 c, uint32_t& word);

// Eighths a cell of this word is worth to a vessel: a liquid's fullness (state
// code 0..7 = 1..8 eighths), a powder's mass (world.h POWDER MASS), a whole 8
// for anything else. MUST agree with sim_mutate.wgsl's scoop ledger credit.
int ContainerCellUnits(uint32_t word, const MaterialDef& m);

// Can this vessel take up `mat` at all, and does its current fill allow it?
// `why` receives the refusal the HUD shows.
bool ContainerAccepts(const ItemDef& def, const ItemStack& st, uint32_t mat,
                      const std::vector<MaterialDef>& mats, const char** why);

// CELLS ALREADY TAKEN, still visible in the snapshot. World::Snap() is
// kSnapshotLatency ticks old, so a cell scooped this tick is still water in
// the next four snapshots -- and a held button would credit it four more
// times while the GPU (correctly) refuses every clear after the first. That is
// matter from nothing, so the scooper remembers what it took until the
// snapshot has caught up. World coordinates, not slot indices: a window shift
// renumbers slots.
// THE SCOOP LEDGER, READ ONCE PER SNAPSHOT FOR EVERY SCOOPER. The GPU keeps
// ONE counter (world.h kPageFaultScoopEighths) for every conditional clear in
// the world, so the delta between two consecutive snapshots is what ALL the
// scoops landing on that tick removed together. It used to be read per
// PlayerSession, each with its own "last seen" value: two local players whose
// scoops landed on the same tick were each paid the tick's whole delta, and
// the pair of flasks was credited twice what the grid lost. One reader per
// world, and each tick's delta is a pot the landing claims draw down in a
// fixed order (session order, then claim order), so the sum paid never
// exceeds what the GPU removed.
struct ContainerScoopLedger {
  bool have = false;       // a snapshot has been observed
  uint32_t tick = 0;       // the snapshot tick last observed
  uint32_t eighths = 0;    // its ledger value
  bool known = false;      // `left` speaks for `tick` (the delta is exact)
  int left = 0;            // eighths `tick` removed, not yet paid out
  int landing = 0;         // claims landing on `tick`, not yet settled
};
// Observe one snapshot: `ledger` its scoop counter, `landingClaims` the number
// of claims (across every memo that will settle this tick) whose tick is
// `snapTick`. Idempotent for a snapshot already observed.
void ContainerLedgerObserve(ContainerScoopLedger& L, uint32_t snapTick,
                            uint32_t ledger, int landingClaims);

struct ContainerScoopMemo {
  struct Taken {
    IVec3 c;
    uint32_t tick;
  };
  std::vector<Taken> cells;
  // CLAIMS NOT YET PAID: eighths asked for on the tick the ops LAND, waiting
  // for that tick's snapshot (ContainerSettle). One per tick.
  struct Claim {
    uint32_t tick;
    uint16_t mat;
    int units;
  };
  std::vector<Claim> claims;
  // The ledger this memo reads when it is settled ALONE (the single-memo
  // ContainerSettle overload: the gates). The live tick settles every
  // session's memo against ONE shared ContainerScoopLedger instead -- see
  // there for why a ledger per memo over-pays.
  ContainerScoopLedger ledger;
  int Pending() const {
    int n = 0;
    for (const Claim& c : claims) n += c.units;
    return n;
  }
  uint16_t PendingMat() const { return claims.empty() ? 0 : claims.front().mat; }
  void Prune(uint32_t tick) {
    size_t w = 0;
    for (const Taken& t : cells)
      if (tick <= t.tick + World::kSnapshotLatency + 1) cells[w++] = t;
    cells.resize(w);
  }
  bool Has(IVec3 c) const {
    for (const Taken& t : cells)
      if (t.c.x == c.x && t.c.y == c.y && t.c.z == c.z) return true;
    return false;
  }
};

// SCOOP: lift up to `spec.scoopPerTick` cells of ONE material from around
// `hit` (the cell the pick ray struck) into `st`. Nearest-first, ties broken
// top-down then by index, so a pond is skimmed from the surface rather than
// drilled. Returns the cells taken; the conditional clears are appended to
// `ops`. `why` names the refusal when nothing was taken.
int ContainerScoop(const ItemDef& def, ItemStack& st, IVec3 hit,
                   const ContainerWordFn& wordAt, const World& window,
                   const std::vector<MaterialDef>& mats,
                   std::vector<CellOp>& ops, const char** why,
                   ContainerScoopMemo* memo = nullptr, uint32_t tick = 0);

// PAY THE CLAIMS a snapshot has caught up with. `snapTick` is the tick the
// snapshot shows, `ledger` its scoop ledger (WorldSnapshot::scoopEighths).
// Each claim for that tick is paid min(claim, what the ledger says the tick
// removed); a claim whose tick was skipped, or whose tick the ledger cannot
// speak for (a reset between snapshots), is paid in full -- the one fallback,
// and it can only happen across a load or a regen. Credits go into `st` when
// it can take them; returns the eighths paid.
//
// THE PAYMENT GOES WHERE `deposit` PUTS IT. The live tick passes a deposit
// that tries the held vessel, then every other vessel on the hotbar and in the
// pack that can take it (ContainerDeposit), and returns what fit. What the GPU
// removed but no vessel could take -- the flask was thrown or set down while
// its scoop was in flight, or the claim was paid into a vessel with less room
// -- is NOT dropped: it is appended to `unpaid` as (material, eighths) for the
// caller to put back into the world (session.cpp spills it as a
// ContainerSpill). Matter the GPU removed is always either in a vessel or on
// its way back to the grid.
struct ContainerUnpaid {
  uint16_t mat = 0;
  int units = 0;
};
int ContainerSettle(ContainerScoopLedger& L, ContainerScoopMemo& memo,
                    uint32_t snapTick,
                    const std::function<int(uint16_t mat, int units)>& deposit,
                    std::vector<ContainerUnpaid>* unpaid);
// The single-memo form (the gates): observes `memo.ledger` itself and deposits
// into `st` alone. Unpaid eighths are returned through `unpaid` when given.
int ContainerSettle(ContainerScoopMemo& memo, uint32_t snapTick, uint32_t ledger,
                    const ItemDef* def, ItemStack* st,
                    std::vector<ContainerUnpaid>* unpaid = nullptr);

// Put up to `units` eighths of `mat` into ONE vessel. Refuses (returns 0) a
// stack of more than one -- a fill is one object's (ItemInstance::StacksWith) and
// paying a stack of three would triple it -- and a vessel that already holds
// kMaxSubstances OTHER things. Returns what fit, capped by its room. A vessel
// holding something else takes it as another portion: contents mix.
int ContainerDeposit(const ItemDef& def, ItemStack& st, uint16_t mat, int units);

// POUR: up to `spec.pourPerTick` cells out of `mouth` (the vessel in the hand),
// charging `st`. Two ways, and the difference is the owner's report that the
// first version "flies off at a weird angle instead of appearing slightly in
// front of the character where they're looking":
//   * AIMED -- `target` given and within `spec.pourRange`: the stream is
//     solved to come down ON it (a limb, a cup, the ground at your feet).
//   * TIPPED -- anything else: out along `fwd` at the vessel's gentle
//     `pourSpeed` and down under gravity, landing a short way in front. It
//     used to be solved toward a point `pourRange` along the look line, which
//     looking at the horizon is a point in mid-AIR -- the stream was lobbed
//     through it and came down far off, long enough in flight for the wind
//     (sim.windMode drags every particle) to carry it sideways.
// Particles are appended to `spawns`; `splat`, when given and anything left
// the vessel, is filled with the matching SplatterEvent (the caller queues it
// and sets sourceMob). `partGravity` is sim.partGravity (24.8 fixed
// voxels/tick^2), the number the kernel integrates with, so the arc solved
// here is the arc it flies.
//
// A GRID PARTICLE CARRIES EXACTLY WHAT IT WAS CHARGED (kPFlagMeasured). Each
// one is charged min(8, fill) eighths, and the kernel used to reinsert EVERY
// liquid particle as a full cell -- so the last eighth of a flask of lava came
// back as eight, and "scoop one eighth, pour, re-scoop eight" minted lava
// without limit. A liquid particle now carries its charge as its fullness code
// and the MEASURED flag tells sim_particle.wgsl to land it at that fullness. A
// powder particle carries its charge as its MASS code (docs/PLAN_powder_mass.md:
// powder is measured in eighths too), so a pouch's last partial cell lands as
// a partial cell of grains -- nothing is lost to "dust" any more.
//
// `partRoom` is how many more grid particles the ring can be trusted to take
// this tick (ContainerParticleRoom); the pour is charged only for particles
// that fit under it (rule 2, charged BEFORE emission). A stack of more than
// one vessel does not pour (returns 0): isolate one first.
int ContainerPour(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                  const Vec3* target, int partGravity, uint32_t tick,
                  uint32_t seed, std::vector<ParticleSpawn>& spawns,
                  SplatterEvent* splat, uint32_t partRoom = 0xFFFFFFFFu,
                  const std::vector<MaterialDef>* mats = nullptr);

// PFLAG_MEASURED in sim_particle.wgsl, its only reader (check_invariants.py
// keeps the two equal): a whole-voxel liquid particle with this bit lands at
// the fullness its payload's state nibble names instead of full. Bit 16, the
// next free one after DRIP. Set only by vessels (ContainerPour, spills).
constexpr uint32_t kPFlagMeasured = 65536u;

// HOW MANY MORE GRID PARTICLES THE RING CAN BE TRUSTED WITH THIS TICK, from
// what the CPU knows without a readback: the snapshot's live count (kSnapshot-
// Latency ticks old), every CPU spawn stream that could have been added since
// at its per-tick cap, and what this tick has already queued. The spawn kernel
// VAPORIZES a spawn that finds the ring full (sim_particle.wgsl `spawn`), so a
// pour charged for a particle that vaporized destroyed matter. THE LIMIT: this
// bound cannot see GPU-born particles (explosion ejecta, MPM splash-out) added
// inside the snapshot's latency window, so a ring driven to its cap by those
// within those few ticks can still vaporize a pour. Closing that needs a GPU
// refund ledger like the scoop's; the ring at that fill is already a state
// nothing else in the game survives either.
uint32_t ContainerParticleRoom(bool snapValid, uint32_t snapParticleCount,
                               size_t queuedThisTick);

// THE SAME POUR AS MLS-MPM FLUID (owner, 2026-09-23: "when using the flask to
// take or pour water, they're cubic microvoxels; i want ... the poured water
// going out to be mpm fluid"). What the seam can hold -- a non-viscous liquid,
// ContainerPoursAsFluid, the CPU twin of sim_fluid_seam's seamLiquid -- leaves
// the flask as FluidSpawnOps instead of grid particles: one particle per EIGHTH
// (the seam's own unit, fpPack fullness 1), so the stream is exactly what the
// flask held, the last partial cell included, and it settles back into
// fullness voxels where it comes to rest. The arc is solved in the solver's
// gravity (sim.fluidGravity, substep-exact) and under its CFL cap; pressure and
// cohesion bend it a little from there. `room` is the particle budget left
// this tick (kFluidCap and the spawn stream, charged BEFORE emission, rule 2).
// The particles carry the flask's material and nothing else, so a poured
// stream and the pond it lands in are the same colour because they are the
// same material. Returns eighths poured; `splat` as ContainerPour.
bool ContainerPoursAsFluid(const MaterialDef& m);
int ContainerPourFluid(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                       const Vec3* target, uint32_t tick, uint32_t seed,
                       uint32_t room, std::vector<FluidSpawnOp>& out,
                       SplatterEvent* splat,
                       const std::vector<MaterialDef>* mats = nullptr);

// A fill written as text, for the capture tools (--shot-mob): "water:0.5"
// or a mixture "water:0.4+oil:0.2", each fraction of `capacity`; a bare name
// is a full vessel. Unknown names are skipped; the total is capped at
// `capacity`.
alchemy::Composition ContainerParseFillSpec(const std::string& spec, int capacity,
                                            const std::vector<MaterialDef>& mats);

// WHAT COMES OUT FIRST from a mixed vessel: the portion at the MOUTH, which is
// the top layer -- the lowest density (the 3D sim's order, the same one the
// alchemy bench settles by). Oil over water pours oil; sand in lava floats and
// pours before the lava. Ties go to the first portion. Without a material
// table, the first portion. 0 for an empty vessel. Every pour, apply and
// scoop-stream path takes its material from here, one substance per tick.
uint16_t ContainerTopMat(const ItemInstance& st, const std::vector<MaterialDef>* mats);

// THE SCOOP STREAM: what a scooped cell looks like on its way into the flask.
// GHOST MPM particles (FluidSpawnOp::flags kFluidOpGhost, FP_GHOST in
// common.wgsl): rendered by the fluid surface like any water, homed onto
// `mouth` by g2p and dead after `life` ticks, and never matter -- they do not
// settle, react, stain, splash, or enter the seam's mass books. The eighths
// were already paid for by the scoop ledger; this is only their picture.
// Eight per cell from the cell's sub-lattice, `room` as above. Returns the
// particles emitted.
constexpr uint32_t kFluidOpGhost = 1u << 8;      // FluidSpawnOp::flags bit
constexpr uint32_t kFluidOpLifeShift = 16;       // ghost life, ticks, bits 16..23
uint32_t ContainerScoopStream(IVec3 cell, uint32_t mat, Vec3 mouth, int life,
                              uint32_t seed, uint32_t tick, uint32_t room,
                              std::vector<FluidSpawnOp>& out);
// THE SCOOP STREAM RUN BACKWARDS: the held vessel APPLYING its contents to a
// body (apply mode, TB_APPLY). `n` ghost particles leave `mouth` and home onto
// `target` (the skin point the brush struck) over `life` ticks. The same ghosts
// as the scoop's, so the same promise: a picture only. The coat itself is
// MobSystem::PourOnBody's, paid from the flask's fill by the tick. Returns the
// particles emitted, capped by `room`.
uint32_t ContainerApplyStream(Vec3 mouth, Vec3 target, uint32_t mat, int life,
                              int n, uint32_t seed, uint32_t tick, uint32_t room,
                              std::vector<FluidSpawnOp>& out);

// ---- THROWING AND BREAKING (owner, 2026-09-23: "holding down a button with
// it equipped charges up a throw ... if the flask hits something with a high
// velocity or is hit by something with a high velocity it should break and
// spawn all of its contents immediately into the world") -------------------
//
// A THROWN VESSEL IS A DROPPED ITEM WITH SPEED. DropItemToWorld already turns
// a stack into a debris body that remembers its fill (the WorldItem's
// ItemInstance), so the
// throw is that call with a launch velocity and the flight is Jolt's. The
// session counts the ticks Q is held (PlayerSession::throwTicks) and lets go
// on the release; the charge is on the TICK clock, so a wind-up is the same
// length at any frame rate and on a peer.
//
// BREAKING is decided per body, per tick, by two independent witnesses, and
// either is enough:
//   * a NEW CONTACT (Physics::ContactImpacts) whose closing speed along the
//     normal reaches `breakSpeed` -- the relative speed, so a flask thrown at
//     a wall and a rock thrown at a flask on the ground are the same test;
//   * a VELOCITY JUMP of `breakSpeed` in one tick -- what the contact list
//     cannot see (it is capped per step and drops anything touching the
//     player) and what a blast does to it.
// Gravity alone adds a third of a metre a second per tick, far under any
// sensible threshold, so a flask in free fall never breaks in the air.
bool ContainerThrowable(const ItemDef& def);
// The wind-up so far, 0..1, after `heldTicks` ticks of the button.
float ContainerThrowCharge(const ItemDef& def, int heldTicks);
// Launch speed, world voxels/s: throwMinSpeed on a tap, throwSpeed at full.
float ContainerThrowSpeed(const ItemDef& def, int heldTicks);
bool ContainerShouldBreak(const ItemDef& def, float contactSpeed, Vec3 dv);

// WHAT A BROKEN VESSEL LETS OUT. The body is gone the tick it breaks; its
// contents are a SPILL that empties into the world from where it was, all of
// it at once when the per-tick spawn budgets allow (1024 eighths of water is a
// quarter of kMaxFluidSpawnsPerTick) and over the next few ticks when they do
// not -- the budget is charged BEFORE emission (rule 2) and nothing is lost to
// it. The same two roads the pour takes: a liquid the MPM seam can hold as
// fluid particles, one per eighth, exact; anything else as grid particles, a
// cell each, the last at its measured fullness (ContainerPour).
struct ContainerSpill {
  Vec3 at{};       // where the vessel was, world voxels (its centre of mass)
  Vec3 vel{};      // its velocity before the blow, voxels/s
  Vec3 away{};     // unit, off the surface it struck; zero when unknown
  // The portion coming out now, and the rest queued behind it: a mixed
  // vessel bursts one substance at a time, all on the same tick while the
  // spawn budgets allow. ContainerSpillStep draws `mat`/`units` from `rest`
  // when the current portion runs out.
  uint16_t mat = 0;
  int units = 0;   // eighths of `mat` still to come out
  alchemy::Composition rest;
  uint32_t seed = 0;
  bool splatted = false;  // the SplatterEvent goes out once, with the burst
  bool Done() const { return units <= 0 && rest.Empty(); }
};
// One tick of a spill. Returns the eighths emitted; `splat`, on the first
// tick anything leaves, is filled with the burst's SplatterEvent (the caller
// queues it), so whoever it breaks over is wet with it.
// Grid particles are MEASURED (see ContainerPour) and limited by `partRoom`.
int ContainerSpillStep(ContainerSpill& sp, const std::vector<MaterialDef>& mats,
                       uint32_t tick, uint32_t fluidRoom,
                       std::vector<FluidSpawnOp>& fluid,
                       std::vector<ParticleSpawn>& parts, SplatterEvent* splat,
                       uint32_t partRoom = 0xFFFFFFFFu);

// THE BREAK PASS, once per tick (session.cpp phase H, and the vessel-break
// gate): every vessel in `ground` against both witnesses -- the NEW contacts
// of the last Physics::Step and its velocity jump since the last call
// (`lastVel`, which this maintains). A broken one's body is destroyed (its
// registry entry goes with it through OnBodyGone) and its contents appended
// to `spills` at its centre of mass. Ghost bodies are the peer's to break.
// Returns the number broken.
int ContainerBreakPass(WorldItems& ground, const ItemLibrary& items,
                       Physics& phys, DebrisSystem& debris,
                       std::vector<std::pair<uint64_t, Vec3>>& lastVel,
                       std::vector<ContainerSpill>& spills);

// Spend up to `cells` whole cells of contents (the triage "apply to this
// limb"). Returns the eighths actually spent; empties the fill at zero.
int ContainerSpend(ItemStack& st, int cells);

// THE PORTRAIT POUR BRUSH'S DRAIN (MobSystem::PourOnBody): what a second of
// pouring on your own body costs, in cells, for a brush disc of `radius`
// world voxels. Scales with the disc's AREA -- a brush twice as wide covers
// four times the skin and empties the flask four times as fast -- anchored so
// the default brush (kPourBrushRefRadius) spends `container.applyCells` a
// second. One function because the tick spends by it and the panel shows it.
constexpr float kPourBrushRefRadius = 0.5f;
float PourBrushCellsPerSec(const ItemDef& def, float radius);

// Is `target` close enough to AIM at, rather than tip toward?
bool ContainerInReach(const ItemDef& def, Vec3 mouth, Vec3 target);

// THE POUR POINT (owner, 2026-09-23: "about 1 metre in the direction the
// character is looking, so they can choose to pour in the air in front of them
// or directly on the ground" -- and then "exactly where the mouse cursor is
// looking"). A point ON THE CROSSHAIR RAY: from `from` (the render eye -- the
// boom in third person, the head in first) along `fwd`, `container.aimDist`
// past where the ray draws level with `head` (the character's eye), so in
// third person it is a metre in front of the CHARACTER, not of the camera.
// Short of that where the ray first meets a solid or liquid cell (`kindAt`,
// the CPU mirror) or a body other than the `ignore` ones (your own limbs, the
// flask in your fist). The frame computes it once (FrameIntent::pourAim) and
// both the marker sphere and the tick's pour use it -- the sphere is where
// the stream goes.
Vec3 ContainerPourPoint(const ItemDef& def, Vec3 from, Vec3 head, Vec3 fwd,
                        const std::function<CellKind(IVec3)>& kindAt,
                        const Physics& phys,
                        const std::vector<uint64_t>& ignore);

// Hotbar stacks of vessels: a stack of N empty flasks cannot all hold the one
// scoop. Before filling, the rest of the stack moves out to a free hotbar slot,
// else a free bag slot, leaving the selected slot a stack of one. False when
// there is nowhere to put them.
bool ContainerIsolateOne(ItemStack* slots, int nSlots, int slot,
                         ItemStack* spill, int nSpill);

// ---- WHAT IS IN IT, VISIBLY (owner, 2026-09-26: "flasks are tinted / contain
// the color of the thing that they contain ... and the volume of it
// represented by the % volume") -------------------------------------------
//
// The contents' colour as an ImGui/unpackColor swatch (0xAABBGGRR, alpha
// forced opaque), 0 for an empty vessel: the MAIN portion's (ContainerMainMat).
// The gauges read this one colour; the icon draws each portion as a band.
uint32_t ContainerFillSwatch(const ItemStack& st,
                             const std::vector<MaterialDef>& mats);
// WHAT A VESSEL SHOWS in a hand or on the ground (phys/fillview.h, which owns
// the view, the word and why the surface is level). Built from the item and
// what is in it; the holders recompute the word from their rotation.
using ContainerHeldFill = BodyFillView;
ContainerHeldFill ContainerHeldFillOf(const ItemDef& def, const ItemInstance& st);
// The same from bare contents -- for a holder that keeps them beside a def
// rather than in a stack (Mob::HeldContents). The 3D view shows ONE material
// (the fill word has room for one id): the portion with the most eighths, at
// the level of the whole fill. Layers show in the icon and on the bench.
ContainerHeldFill ContainerHeldFillFrom(const ItemDef& def, const alchemy::Composition& c);
// The portion with the most eighths (ties: the first), 0 when empty.
uint16_t ContainerMainMat(const alchemy::Composition& c);
// How brightly the contents glow, 0..1: the material's own `emission`, the
// one number the held and grounded flask glow by (microbody.wgsl) -- the UI
// reads it too, so the icon glows exactly when the flask does.
float ContainerFillGlow(const ItemInstance& st,
                        const std::vector<MaterialDef>& mats);

// "water 64/128", "empty", for tooltips and the HUD.
std::string ContainerFillText(const ItemDef& def, const ItemStack& st,
                              const std::vector<MaterialDef>& mats);
