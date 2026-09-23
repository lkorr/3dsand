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

// POUR: throw up to `spec.pourPerTick` cells from `mouth` so they come down on
// `target` (world voxels, a point), charging `st`. Particles are appended to
// `spawns`; `splat`, when given and anything left the vessel, is filled with
// the matching SplatterEvent (the caller queues it and sets sourceMob).
// `partGravity` is sim.partGravity (24.8 fixed voxels/tick^2), the number the
// kernel integrates with, so the arc solved here is the arc it flies.
int ContainerPour(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 target,
                  int partGravity, uint32_t tick, uint32_t seed,
                  std::vector<ParticleSpawn>& spawns, SplatterEvent* splat);

// Spend up to `cells` whole cells of contents (the triage "apply to this
// limb"). Returns the eighths actually spent; empties the fill at zero.
int ContainerSpend(ItemStack& st, int cells);

// Clamp a pour target to the vessel's reach from `mouth`.
Vec3 ContainerClampTarget(const ItemDef& def, Vec3 mouth, Vec3 target);

// Hotbar stacks of vessels: a stack of N empty flasks cannot all hold the one
// scoop. Before filling, the rest of the stack moves out to a free hotbar slot,
// else a free bag slot, leaving the selected slot a stack of one. False when
// there is nowhere to put them.
bool ContainerIsolateOne(ItemStack* slots, int nSlots, int slot,
                         ItemStack* spill, int nSpill);

// "water 64/128", "empty", for tooltips and the HUD.
std::string ContainerFillText(const ItemDef& def, const ItemStack& st,
                              const std::vector<MaterialDef>& mats);
