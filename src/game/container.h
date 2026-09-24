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
// code 0..7 = 1..8 eighths), a whole 8 for anything else.
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
  // The ledger as of the last snapshot seen, so the next one's delta is what
  // that one tick's clears removed.
  bool haveLedger = false;
  uint32_t ledgerTick = 0;
  uint32_t ledgerEighths = 0;
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
int ContainerSettle(ContainerScoopMemo& memo, uint32_t snapTick, uint32_t ledger,
                    const ItemDef* def, ItemStack* st);

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
int ContainerPour(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                  const Vec3* target, int partGravity, uint32_t tick,
                  uint32_t seed, std::vector<ParticleSpawn>& spawns,
                  SplatterEvent* splat);

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
// Species is (mat - 1) & 3, the rule exciteEmit uses, so a poured stream and
// the pond it lands in are the same colour. Returns eighths poured; `splat` as
// ContainerPour.
bool ContainerPoursAsFluid(const MaterialDef& m);
int ContainerPourFluid(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                       const Vec3* target, uint32_t tick, uint32_t seed,
                       uint32_t room, std::vector<FluidSpawnOp>& out,
                       SplatterEvent* splat);

// THE SCOOP STREAM: what a scooped cell looks like on its way into the flask.
// GHOST MPM particles (FluidSpawnOp::species kFluidOpGhost, FP_GHOST in
// common.wgsl): rendered by the fluid surface like any water, homed onto
// `mouth` by g2p and dead after `life` ticks, and never matter -- they do not
// settle, react, stain, splash, or enter the seam's mass books. The eighths
// were already paid for by the scoop ledger; this is only their picture.
// Eight per cell from the cell's sub-lattice, `room` as above. Returns the
// particles emitted.
constexpr uint32_t kFluidOpGhost = 1u << 8;      // FluidSpawnOp::species bit
constexpr uint32_t kFluidOpLifeShift = 16;       // ghost life, ticks, bits 16..23
uint32_t ContainerScoopStream(IVec3 cell, uint32_t mat, Vec3 mouth, int life,
                              uint32_t seed, uint32_t tick, uint32_t room,
                              std::vector<FluidSpawnOp>& out);

// ---- THROWING AND BREAKING (owner, 2026-09-23: "holding down a button with
// it equipped charges up a throw ... if the flask hits something with a high
// velocity or is hit by something with a high velocity it should break and
// spawn all of its contents immediately into the world") -------------------
//
// A THROWN VESSEL IS A DROPPED ITEM WITH SPEED. DropItemToWorld already turns
// a stack into a debris body that remembers its fill (WorldItem::fill), so the
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
// whole cell each (the pour's one rounding, under a cell per flask).
struct ContainerSpill {
  Vec3 at{};       // where the vessel was, world voxels (its centre of mass)
  Vec3 vel{};      // its velocity before the blow, voxels/s
  Vec3 away{};     // unit, off the surface it struck; zero when unknown
  uint16_t mat = 0;
  int units = 0;   // eighths still to come out
  uint32_t seed = 0;
  bool splatted = false;  // the SplatterEvent goes out once, with the burst
};
// One tick of a spill. Returns the eighths emitted; `splat`, on the first
// tick anything leaves, is filled with the burst's SplatterEvent (the caller
// queues it), so whoever it breaks over is wet with it.
int ContainerSpillStep(ContainerSpill& sp, const std::vector<MaterialDef>& mats,
                       uint32_t tick, uint32_t fluidRoom,
                       std::vector<FluidSpawnOp>& fluid,
                       std::vector<ParticleSpawn>& parts, SplatterEvent* splat);

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

// "water 64/128", "empty", for tooltips and the HUD.
std::string ContainerFillText(const ItemDef& def, const ItemStack& st,
                              const std::vector<MaterialDef>& mats);
