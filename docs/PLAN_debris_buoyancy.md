# Debris in water — sinking, floating, and the raft that was not

Owner ask, 2026-09-13: *"blow voxels off a rigid body and they fall onto liquids
and not through them; explode a tree and its voxels form a structure on top of
the water when they should either fall underneath if dense enough, or float on
top in a floaty manner."*

## 1. What was actually happening

Three mechanisms, and none of them was "buoyancy is missing".

1. **A liquid was a wall to a voxel in flight.** `blocksParticle`
   (`sim_particle.wgsl`) returned true for `CLASS_LIQUID` — the comment said
   *"splash = plop onto the surface"*. A chip stopped at the first water cell and
   reinserted into the last air cell above it. The chip behind it was blocked by
   the first one and reinserted a cell higher. That is the structure: it was
   built out of the LANDING rule, one voxel at a time.
2. **A landed solid then never moves.** `CLASS_SOLID` does not move in the CA at
   all except `soloSolid` (`sim_step.wgsl`) — "no solid or powder on any of six
   faces". Every voxel in a raft touches a neighbour, so not one of them
   qualifies. The isolated ones already behaved correctly: `canDisplace` let a
   lone rock sink through water and refused to let wood sink, off `density`
   alone, with no new code.
3. **Nothing ever flagged the raft.** Island detection is driven by
   `flagSupportLoss`, which fires when a cell VACATES. Debris that arrives with
   nothing under it took support from nothing, so no flag was raised and no scan
   was ever queued.

## 2. Where buoyancy belongs, and why not in the CA

The CA cannot express floating, for three separate reasons:

* a grid voxel has no sub-voxel position, so it cannot bob;
* "is this clump loose" is a connectivity question — that is what island
  detection is FOR, and `soloSolid` is the one case decidable locally;
* the lattice licenses **distance-1 reads only** (`sim_step.wgsl`: acting cells
  are 3 apart and write at 1, so a cell 2 away can be written by a cell 3 away).
  "Is the voxel below me itself floating?" is a distance-2 question and is
  therefore not answerable in the CA at any price.

The particle ring already has every property the feature needs and pays for them
today: Q24.8 sub-voxel position (so a bob and a drift render for free), integer
determinism, a hard cap, and spare bits in `Particle.flags`. So the transit —
sink, rise, bob, settle — is a particle behaviour, and the grid only holds the
rest state.

## 3. What landed

### Phase 1 — the fluid block and the flight (`sim_particle.wgsl`)

A per-material `fluid` block, authored in `materials.json`, packed into a new
`MaterialGpu.fluidPack` word (`kFluidPack*` in `src/sim/materials.h`):

```json
"fluid": { "lift": 0..15, "drag": 0..15, "wander": 0..15 }
```

* **lift** — how much of Archimedes this material feels. **0 is the opt-out**:
  liquids do not touch it and it keeps the old stop-dead-at-the-surface
  behaviour. That is what micro spray and liquid/gas ejecta get by default, and
  it is the switch the owner asked for ("some particles will float and some will
  not"). Which WAY a material moves is never authored — it is `density` against
  the liquid's.
* **drag** — per-tick viscous damping, k/16 of velocity. This is what makes
  water read as water, and it is what ENDS the motion: rise, overshoot, fall
  back, each cycle smaller, converging to a bob instead of oscillating forever.
* **wander** — sideways drift of something already floating, 1/256 voxel/tick.
  A leaf skitters, a log barely moves. It is also the anti-tower mechanism.

Defaults are derived (lift 15 for solid/powder and 0 for liquid/gas, drag
`2400/density`, wander `1200/density`), so no material needed editing and a new
material behaves sensibly the day it is added — the `wind` block's precedent.

Bits 12..31 of `fluidPack` are free and are where the next rule goes
(waterlogging, break-up on impact, entrainment by a current).

The flight itself:

* liquid no longer blocks a particle whose lift is nonzero;
* buoyancy `g * rhoFluid / rhoSelf` capped at `sim.partBuoyMax`, then damping,
  both evaluated once per tick on the cell the particle started in;
* **a settle rule**, because a floater is never blocked by anything and would
  otherwise live forever: slow + a cell it may occupy + something under it worth
  resting on (`settleSupported`). Both halves matter — drop the support test and
  neutrally buoyant matter freezes into a voxel hanging mid-water, which is the
  original bug in new clothes;
* **reinsertion may displace a liquid it is denser than**, which is what lets a
  rock come to rest on the BED (the cell it stops in is water, not air). The
  displaced water is dropped, exactly as the brush drops it when it paints into
  a pond. Conserving it is not locally possible: displacement raises the level
  of the whole body of water, and that is a write this pass cannot reach;
* **a floater that bumps into a berth already taken does not reinsert** — it
  drifts. That single line is what stops the tower from being rebuilt;
* **patience** (`sim.partFloatPatience`, in the micro LIFE bits, which are spare
  for a non-micro particle by construction): after N wet ticks it takes any cell
  it can have. That is the rule-2 bound — a pond too small for the debris thrown
  into it jams up into a pile instead of holding particles alive forever.

### Phase 2 — landing-side support flag

`flagLandedUnsupported` raises the island-detection flag when a particle lands
with AIR below it and a solid on some other face — i.e. exactly the case
`soloSolid` cannot decide. Deliberately NOT raised when the cell below is
LIQUID: island detection counts liquid as empty, so flagging a floating raft
would convert it to a rigidbody, settle it back, and flag it again forever.

### Phase 3 — rigid bodies (`DebrisSystem`)

A felled trunk is not particles, it is a Jolt body, and it sank like a stone
because liquids are not in the collider and nothing applied buoyancy. Jolt's
`Body::ApplyBuoyancyImpulse` takes a surface plane and a buoyancy factor which
is exactly `rhoFluid / rhoBody` — the same ratio the particle kernel computes,
off the same `density` field, via the body's cached `domMat`.

## 4. Tuning

| knob | default | what it is |
|---|---|---|
| `sim.partBuoyMax` | 88 | ceiling on the buoyant term (4 g). A leaf in water is a five-gravity rocket without it. |
| `sim.partSettleSpeed` | 24 | how slow a floater must be before it looks for a berth |
| `sim.partFloatPatience` | 180 | wet ticks it may hunt before it takes anything |

## 5. Verification

**`--selftest --gate debris-float`** (measured, 2026-09-13):

```
iron 9/9 cells y111..111, bed 111 | wood 9/9 cells y118..120, waterline 119
| live 0 | bodies lowest wood y116.4 (200 ticks) iron y-31.3 (66 ticks)
```

Four claims, each failing for its own reason: the dense arm reached the bed
(which needs the displace-a-liquid rule in `resolve`), the buoyant arm ended at
the waterline and NOT above it (the reported bug), the ring drained (the bob
terminates), and the two bodies parted. The iron body's `y -31.3` is not a
typo — the harness builds no terrain collider under a CellOp basin, so it falls
out of the bottom of the world while the wood cube beside it stops at 116.
Nothing in that fixture can hold either up except the water, which makes it a
sharper control than a body resting on a bed.

**`--shot-debris-pond`** photographs the owner's own case on the authored lake:
a wooden mass 12 voxels over open water, blown apart, shot in flight, at rest,
and from directly above (a raft and a tower look identical from the bank).
Measured: `waterline y212 | wood 1 above / 46 at the waterline / 11 under`. The
11 are the patience fallback doing its job — worldgen scatters lilypads over
that surface, so a chip that cannot find free waterline within
`sim.partFloatPatience` wet ticks takes what it can get and sinks, which is a
better answer than a particle alive forever (rule 2).

**The world hash moved on purpose** — eb284643 -> 97ac0e71, rebaselined at gate
scope after confirming suite and gate agree. Debris that used to stop on a
surface now ends up somewhere else; the twice-run comparison passed.

**Attribution of the suite's other 10 failures** (this tree carries three
agents' uncommitted work): a control arm with every material's `fluid.lift`
forced to 0 — same binary, same tree, `SANDVOX_ASSET_DIR` at a scratch copy of
`assets/` — fails identically on 8 of them, so they are not this change.
`gi-bounce` and `cactus-fell` differ between the arms but PASS at `--gate`
scope on this one, i.e. shared-`World` ordering (CLAUDE.md rule 7), not
mechanism.
