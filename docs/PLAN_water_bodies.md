# Water bodies: basin labels, pooled-level bookkeeping, and pressure drains

> **Status: design, 2026-08-27. Nothing implemented.** Originated in an owner
> conversation; the owner intends to implement soon and likes this direction
> specifically. **§1–§12 are the sim half (packages A–F); §13–§16 are the look
> half (packages G–J), written the same day and deliberately sequenced after,
> because every render tier reads the sim tier's state.** Read CLAUDE.md (the
> three inviolable rules) and
> `docs/RESEARCH_water_architecture.md` first — this is a **new option in that
> document's decision space**, not a replacement for `docs/PLAN_mpm_fluids.md`.
> Every constant quoted below is against the 2026-08-27 tree; re-verify before
> acting on a number.
>
> **§0 is a rebuttal of this document, written by the same author. Read it
> first — it corrects §1's framing, names a competing architecture, and lists
> what this design does not cover.**

---

---

## 1. The idea

A still body of water is currently simulated as N independent voxels that happen
to be adjacent. Every fact about it that a player cares about — its level, its
volume, the pressure at its floor, where it drains to — is *implicit* in those
voxels and can only be recovered by moving mass through every one of them.

Instead: **give the body an identity and a small descriptor, and do the bulk
bookkeeping on the descriptor.** Then draining a lake through a hole in its floor
is arithmetic on one number plus an edit to the surface layer, rather than a
pressure wave propagating through 87,000 cells at reach 1.

What that buys, in order of value:

1. **Draining stops costing O(volume).** `RESEARCH_water_architecture.md` §4.1.4
   already identifies draining as the one place a heightfield beats a 3D solver
   by ~20×, and identifies that as the strongest argument for a column layer.
   This gets the same win **without** the heightfield's fatal single-span-per-
   column assumption (§4.1.1), because a basin label is 3D. A flooded cave under
   a lake is a different basin, not a corrupted column.
2. **A lake at rest costs O(1) regardless of volume**, so oceans become
   affordable for reasons unrelated to rendering.
3. **Pressure becomes a real quantity** the rest of the game can read: drain jet
   velocity, current strength, whirlpools, force on the player, structural load
   on a dam.
4. It is **additive**. It sits beside the CA and MPM rather than replacing
   either, and it can be switched off wholesale (see §6 gate) to recover exactly
   today's behaviour — which makes it cheap to A/B and cheap to abandon.

### 1.1 What it is not

This is **not** a general water solver and must never be asked to be one. It
governs one situation: *large, still, pooled* water. Everything moving stays with
the CA and MPM exactly as today. §6 defines the boundary precisely, and the
boundary is the single most important part of this design — see §3.

---

## 2. The invariant that makes this legal

> **Voxels are authoritative. The body descriptor is derived data:
> reconstructible from the voxels, disposable, never saved, never hashed.**

Design guideline #3 (`CLAUDE.md`) forbids two owners of one fact, and
`RESEARCH_water_architecture.md` records that the *existing* single CA↔MPM seam
took four work packages to behave and that `fluid-react` has been red on seam
mass accounting. A second mass owner is the predictable way to make this feature
a net loss.

The interior voxels of a pooled body **continue to exist and continue to be the
mass**. The descriptor is a cache of aggregates over them. This costs nothing —
a still lake's chunks are asleep either way (rule 2) — and the win survives
intact, because the win was never "don't store the water," it was "don't
*transport* mass through cells that have nothing to say."

Three consequences, all of which are testable:

- **The ledger may never be debited except in the same mutation batch that
  edits voxels.** One exception, in §7.2: the sub-step remainder.
- The descriptor is not in the world hash, not in the save file, and is rebuilt
  on load. If a bug corrupts it, the world is still correct.
- Any gate may recompute a descriptor from scratch and assert equality with the
  live one. That is the primary test hook (§10.1).

---

## 3. Label basins, not water

The obvious implementation — flood-fill the water, as in the classic islands
problem — is the wrong runtime primitive, for one reason: **water moves every
tick and terrain does not.** Labeling water means re-labeling constantly;
labeling the *terrain basin that contains it* means re-labeling only when someone
digs, which is rule 2 satisfied for free.

A **basin** is a closed region of terrain below a rim, with:

- a **spill elevation** — the lowest saddle on its rim, i.e. the level at which
  it starts overflowing into a named neighbour basin;
- a **hypsometric curve** — a small monotonic table `volume → surface level`,
  plus its derivative `level → surface area`;
- a set of **partition elevations** — see §5.

### 3.1 Worldgen ponds are analytic, so most curves are free

`src/sim/tuning.h` §worldgen already places ponds on a `pondTile = 448` grid with
`pondRadiusMin = 48`, `pondRadiusSpan = 32`, `pondDepth = 26` at centre tapering
to `pondDepthRim = 3`, inside a structural berm (`pondBerm = 5`,
`pondBermWidth = 14`). Package C of the terrain overhaul made the bowl **replace**
the ground rather than `min()` into it (`PLAN_terrain_overhaul.md` §C.4), which
means the bowl is an exact analytic shape, not an emergent one.

So for every worldgen pond the curve, the spill elevation and the partition set
are **closed-form functions of the same parameters that generated the bowl**. No
measurement pass, no flood fill, no storage beyond the pond's own tile
coordinates. This is the single biggest reason to prefer basins over water: the
authoring surface already exists and already agrees with the terrain.

Only **player-modified** basins need discovery, and only the ones the player
modified.

### 3.2 If you must discover a basin at runtime, do it on the chunk graph

A lake 500 voxels across is ~31 chunks across at `kChunk = 16`. Deterministic
label propagation (min-label flooding, mark/apply, reach 1) converges in
O(diameter): **~31 passes on the chunk graph versus ~500 on the voxel graph.**
Per-voxel connectivity inside a chunk is irrelevant to an aggregate, so the
coarse graph is not an approximation of anything that matters here.

This also composes with the existing dispatch model: the chunk graph *is* the
dirty-chunk list, already compacted, already indirect.

---

## 4. Why a connected component is not a water level

The naive version of this idea attaches one surface height to one connected
component. **That is false for a large fraction of real water, and it fails
silently** — the same failure class §4.1.1 of the research doc rejects
heightfields for.

A stream running down a hillside is *one* BFS component with a 200-voxel head
difference between its ends. A connected component is a topological fact; an
equipotential surface is a hydrostatic one; they coincide only at equilibrium.
Water exposed to air in several places on a slope is one component with several
free surfaces at different heights, which is exactly the case the owner flagged
in the original proposal.

The fix is a **classifier, not a better solver** (§6). Bodies that fail the test
are simply not this system's business.

---

## 5. Splits are the expensive operator — and the basin form makes them scheduled

Merging two bodies is a union: O(1), and the new curve is the sum of the
children's curves.

Splitting is the hard direction, because discovering that one body has become two
normally requires a re-flood. The tempting conclusion is that splits are rare —
a player would have to deliberately dam settled water. **That is wrong, and the
counterexample is generated by this feature itself:**

> **A draining lake splits itself.** Any pond with an irregular floor becomes two
> or more disconnected puddles as its level falls past an interior high point.
> That is not an exotic case; it is what *every* drain does in its final phase.

Plus: landslides and powder collapse into ponds (see the "worldgen powder meets a
reactive liquid" failure mode), and explosion cave-ins.

The basin formulation converts this from a detection problem into a scheduling
one. Alongside the spill elevation, precompute the **partition elevations**: the
levels at which this basin separates into which children. They are a static
property of the terrain, known before the water ever gets there. Then:

- Falling through a partition elevation is an **anticipated event at a known
  level**, handled by splitting the descriptor and dividing the volume by the
  children's curves — exact integer, no search.
- Rising back through it is the union.
- Only *terrain edits* invalidate a partition set, and only for the basin edited.

This is a strictly stronger property than any runtime BFS provides, and it is the
main argument for §3.

### 5.1 Terrain edits still need a lazy path

A player carving a channel between two basins must eventually merge them. Detect
the *possibility* cheaply (a mutation touched a rim chunk of a labeled basin),
then schedule the re-derive over N ticks. Water is slow; a few ticks of a
slightly stale level is invisible.

**But the schedule must be a function of the tick number, not of when the CPU got
around to it.** A re-derive "when convenient" is a scheduling-dependent outcome
and breaks rule 1 through the back door. `basinId % N == tick % N` is the shape.

---

## 6. The classifier: which bodies this system governs

A body is **pooled** — and therefore governed — only if all of:

1. **It sits below its basin's spill elevation.** Not overflowing means not
   flowing.
2. **Surface z-spread under a threshold.** Cheap, direct, and it is precisely the
   error term of the whole model (§4).
3. **Volume above a size threshold.** Small ponds are cheap to simulate honestly
   and there is nothing to win; the model's error is also relatively largest
   there.
4. **Quiescent for K ticks** — no excite events, no MPM particles in its cells.

Anything else is a **stream** and belongs entirely to the CA/MPM, untouched.

**Jurisdiction is not all-or-nothing, though.** A violent drain in an otherwise
still pond keeps the pooled descriptor for its bulk and hands only a region
around the throat to MPM — see §15.4. §6.1's full release is the exit for a body
that stops being pooled *everywhere*; a local excite is the exit for a body that
stops being pooled *in one place*. Getting this wrong in the conservative
direction (releasing the whole lake because someone poked a hole in it) throws
away the entire win at exactly the moment it matters.

### 6.1 The threshold trap

Any threshold means a body near it flips representation. A pond sitting exactly
at the size or quiescence boundary will flap every tick, and **every flap is a
seam crossing where mass can be lost**.

Two requirements, non-negotiable:

- **Hysteresis.** Distinct enter and exit thresholds, with a real gap.
- **Both directions mass-exact.** Adoption reads the voxel sum into the ledger;
  release writes any remainder back into voxels before dropping the descriptor.
  Neither may round.

### 6.2 The off switch

`sim.waterBodyMode = 0` must reproduce today's behaviour bit-for-bit — no
descriptors, no adoption, no ledger. This is what makes the whole feature cheap
to evaluate and cheap to abandon, and it is what `--sweep sim.waterBodyMode=0,1`
needs in order to be a one-invocation differential.

---

## 7. The drain ledger

### 7.1 Units and the exact step cost

Liquid fullness is the state nibble, code `0..7` meaning `1..8` eighths
(`common.wgsl:22`). So the ledger's unit is **eighths**, integer, and:

> Lowering a body's surface by one eighth costs exactly **`area` eighths**, where
> `area` is the surface cell count. (Each of `area` cells loses `1/8` voxel;
> `area × 1/8` voxels = `area` eighths.)

The running tally is therefore an integer compared against an integer, with no
scaling and no rounding anywhere. This is the cleanest part of the design.

`area` must come from the curve derivative, **not** from a running assumption
that it is constant. A bowl narrows as it empties; at the default pond
(`pondDepth 26`, `pondDepthRim 3`) the area at the bottom is a small fraction of
the area at the rim, so a fixed-area tally would drift badly and in the direction
of draining too slowly at the end.

### 7.2 The legitimate ledger/voxel divergence

Between surface-lowering events the ledger and the voxel sum genuinely disagree
by the accumulated remainder, in `[0, area)` eighths. This is not a bug and it is
the one sanctioned exception to §2. It must be:

- **stored per body**, as its own field, not implied;
- **included in every conservation gate**, or the first such gate reports a leak
  that does not exist;
- **written back to voxels on release** (§6.1).

### 7.3 Determinism of the lowering pass

Given `(bodyId, level, remainder)`, "is this cell on the surface and does it drop
an eighth" is a **per-cell independent predicate at reach 0**. It is lattice-safe
by construction and needs no mark/apply.

Accumulation into the per-body tally is an integer `atomicAdd`. That is already
the engine's sanctioned idiom — `RESEARCH_water_architecture.md` §4.2 records
Q16.16 `atomicAdd` for the P2G scatter as "the standard trick, not a determinism
tax" — because integer addition is associative and commutative, so the final
value does not depend on order. The `atomicCAS`/exchange ban is untouched.

**Do not read the tally in the same pass that adds to it.** Accumulate this tick,
act next tick. Standard mark/apply cadence.

### 7.4 Dither the surface step

Dropping every surface cell by an eighth in one pass reads as the whole lake
snapping down a step. Stagger it with `hash3(seed, tick, cellIndex)` against the
remainder fraction so the level descends as a dissolving noise pattern rather
than a plane. Free, deterministic, and it is the difference between "the water is
going down" and "someone edited the water."

### 7.5 A worked number, to calibrate expectations

Default worldgen pond, radius ~48 vox: surface area ≈ 7,240 cells, so **one
eighth-step costs ≈ 7,240 eighths**. Total volume at mean depth ~12 vox ≈ 87,000
voxels ≈ 700,000 eighths.

A 1-voxel-square hole under 2 m of head (§8): `v = sqrt(2gh) ≈ 6.3 m/s`,
`Q = C_d·A·v ≈ 0.6 × 0.01 m² × 6.3 ≈ 0.038 m³/s`. At `kVoxelMeters = 0.10` one
voxel is 0.001 m³, so ≈ 38 voxels/s ≈ **1.25 voxels/tick ≈ 10 eighths/tick** at
the 30 Hz tick (`kTickDt = 1/30`, `src/test/support.h:27`).

Therefore: **~724 ticks (~24 s) per eighth-step of surface drop**, and a **~39
minute full drain**. Two things follow. First, the feature is about *slow bulk* —
the drama is at the jet, not the lake. Second, that is 70,000 ticks of pressure
propagation through ~87,000 cells that the CA now never has to do, which is the
entire point.

---

## 8. Pressure drains: Torricelli, and the single-evaluation rule

Head `h` = body level − hole elevation. Then `v = sqrt(2gh)` and orifice discharge
`Q = C_d · A · sqrt(2gh)`. Integer-deterministic via integer sqrt or a small
lookup table; `h` is already an integer in voxels.

> **Single-evaluation rule: one evaluation of `h` must produce both the emitted
> particle momentum and the ledger debit.** If the jet is emitted by one rule and
> the lake decrements by another, they will disagree under every edge case and
> you have built a mass pump.

### 8.1 Emission goes through the existing seam, with its existing budget

`sim_fluid_seam.wgsl`'s `spawnAppend` + `FluidSpawnOp` is the entry point:
CPU-charged budget, `slot >= FLUID_CAP` belt-and-braces refusal, per-particle
`px/py/pz`, `vx/vy/vz`, `species`, `mat`. Nothing new is needed to make a drain
spit MPM.

Two traps in that path, both of which break §8 if ignored:

1. **`spawnAppend` clamps velocity to `±FLUID_VMAX`** (the CFL cap derived from
   the substep knob). A Torricelli velocity under high head *will* exceed it. The
   clamp is correct and must stay — but then the **momentum you asked for is not
   the momentum you got**, so either cap `h` before computing `Q` so the two stay
   consistent, or accept the clamp and spread the flow over more particles at
   legal speed. Do not silently let them diverge.
2. **Emission must be bounded per hole per tick** (rule 2: bound every emergent
   process). When that cap binds, **debit the ledger by what was actually
   emitted, never by the analytic `Q`.** This exact mismatch — analytic demand vs
   granted supply — is the standard way this class of feature leaks mass.

### 8.2 The pleasant consequence

Because the jet is real MPM, everything downstream is already built: it splashes,
it carves, it pushes the player, it interacts with the CA at the existing seam.
The drain is not a new fluid system, it is a *source term* with a physically
meaningful magnitude.

---

## 9. The current field stays a pure function

Model it on wind: `windAt()` is a **pure function in `common.wgsl`, never a
stored field** — nothing to store, nothing to desync, nothing to save, sleeps for
free. `currentAt(p)` should be the same: a function of the body descriptor
(level, hole list, spill points), which is a handful of words.

Superpose two terms per hole, and note they have very different reach:

| Term | Falloff | Character |
|---|---|---|
| Radial sink (inflow) | `1/r²` (3D point sink) | Violent but **only a couple of voxels wide** at any realistic `Q`. Must be clamped near `r → 0`. |
| Tangential vortex | `Γ/2πr` (free vortex) | **Reaches far.** This is the visible, navigable, player-affecting part. |

That asymmetry is not a compromise, it is why real whirlpools look enormous while
the actual suction is a small throat: the swirl is the wide term. Design the
danger around the tangential component and the *lethality* around the throat.

Chirality per hole from a hash of the hole position — free, deterministic, and it
stops every drain in the world spinning the same way.

**The level model is the boundary condition the flow field solves against, not a
competing mover.** Only one of them may move mass.

§14 gives the full field — the two flow regimes, its accuracy envelope, and the
primitive layout it should borrow from `windPrims`. §13 covers what the renderer
does with it.

---

## 10. Verification design

Per CLAUDE.md "authoring cheap-to-verify work" — this must be provable with
`--gate waterbody` alone, and the knobs must live in `tests/baseline.json`.

### 10.1 The primary hook: descriptor vs recompute

Because the descriptor is derived (§2), a gate can recompute it from the voxels
and assert equality. That single assertion catches label corruption, bad merges,
bad splits and ledger drift at once, and needs no reference world.

### 10.2 Passes the gate should have

- **A — conservation.** Sum(voxel eighths) + Σ(ledger remainders) + Σ(in-flight
  MPM mass) is invariant across a full drain. Fails on every §7.2/§8.1 mistake.
- **B — split scheduling.** A pond with a known interior high point drains past a
  partition elevation; assert two descriptors appear at exactly the predicted
  level, with volumes summing to the parent's.
- **C — hysteresis.** Park a body at the size threshold and run 200 ticks; assert
  the descriptor does not flap and mass is flat (§6.1).
- **D — off switch.** `sim.waterBodyMode=0` reproduces the pinned hash exactly.
  One `--sweep sim.waterBodyMode=0,1` invocation proves both reachability and
  equivalence.
- **E — idle cost.** A large pooled lake keeps the active-chunk count at rest
  under the existing ≤32 assertion. If a labeled body cannot sleep, the feature
  is a regression regardless of what it enables.
- **F — determinism.** Same seed, two runs, drain in progress at the compare
  tick. Catches a non-tick-scheduled re-derive (§5.1).

### 10.3 Instrument at the point of failure

Per CLAUDE.md rule 6 (a bare count is not a measurement): when conservation
fails, the reporter must say **which body, which term, and by how much** — not
"mass changed by 37." The `voxStore` page-fault probe is the precedent, and it is
the reason this doc specifies the remainder as a stored field rather than an
implicit one.

---

## 11. Work packages

| # | Package | Content | Verifiable by |
|---|---|---|---|
| **A** | Descriptor + gate skeleton | Body descriptor struct, derived-from-voxels recompute, `sim.waterBodyMode` off by default, `--gate waterbody` with pass D only. No behaviour change. | `--sweep sim.waterBodyMode=0,1` — hashes must be *identical* at this stage |
| **B** | Basin registry, analytic | Curves/spill/partition elevations closed-form from the worldgen pond parameters (§3.1). Adoption + release with hysteresis (§6). Passes A, C, E. | `--gate waterbody` |
| **C** | The ledger + surface stepping | Eighth-accounting, remainder, dithered lowering (§7). Still no drains — driven by a test-only debit. Pass A. | `--gate waterbody` |
| **D** | Torricelli drains | Head → `Q` → `FluidSpawnOp`, single-evaluation rule, emission cap, clamp reconciliation (§8). Passes A, B, F. | `--gate waterbody`, then one acceptance run |
| **E** | `currentAt()` | Sink + vortex superposition, chirality hash, player force (§9). Render/feel work; no new mass path. | visual + pass E |
| **F** | Runtime basin discovery | Chunk-graph label propagation for player-carved basins (§3.2), tick-scheduled re-derive (§5.1). | `--gate waterbody` pass F |

A through C are the whole architectural risk and none of them change the world
hash. D is where it becomes a feature. E and F are optional for a first cut — E
is the fun, F is the completeness.

### 11.1 The look packages (§13–§16)

Sequenced after A–F because every one of them reads tier-1 state. None of them
move the world hash except H.

| # | Package | Content | Verifiable by |
|---|---|---|---|
| **G** | Wave layer | Tier 3: Gerstner sum with speeds from the dispersion relation, `tanh(kh)` shoaling and shore refraction from the tier-1 level, shore amplitude fade, current Doppler, bounded impact-ripple ring buffer (§13.1–13.2). Render-only; must not touch the CA. | visual; hash must be **unchanged** |
| **H** | Vortex primitive + local excite | Vortex in `currentAt()`, closed-form surface dip, `Γ` from `hash3` + ambient circulation, `r_excite` criterion, region excite for violent drains (§14.3–14.5). Moves the hash. | `--gate waterbody`, `--rebaseline` |
| **I** | `funnelAt()` | Promote the analytic core to a shared derived field; patch the §15.2 consumer table; `Γ`-decay closing; the "no raw voxel reads" invariant + its check (§15). | `--gate waterbody`; a pass asserting no consumer disagrees with `funnelAt()` |
| **J** | Interface emitter | Shared thin-surface MPM emitter primitive (§16), first consumer is the funnel wall + throat. Built as a primitive, not as drain code — waterfalls and shore breakers are the other callers. | particle-count ceiling in `baseline.json` |

G is independent of H–J and is the cheapest large visual win in the document —
it needs only a level and a depth, both of which package B provides.

---

## 12. Open questions

1. **Where does the descriptor live?** CPU-side (simplest, but the lowering pass
   is GPU and needs the level each tick — a small uniform array indexed by body
   id) or a GPU buffer (no upload, but recompute-and-compare gets harder). Lean
   CPU with a per-tick upload; body counts are tiny.
2. **How many bodies?** A cap needs to exist (rule 2). What happens at the cap —
   refuse adoption of the smallest, presumably, since unadopted just means
   "simulated the old way," which is a safe degradation.
3. **Does the sea (terrain overhaul package D) participate?** It is the extreme
   case: one body, effectively infinite volume, level fixed by fiat. Probably it
   is a *degenerate* body with an infinite curve rather than a special case — but
   package D is not landed yet and this should not constrain it.
4. **Interaction with WP5** of `PLAN_fluid_overhaul.md`, which is still an open
   A/B. This design *reduces* the pressure on that decision: if pooled bulk is
   handled here, the question of whether CA liquid movement survives is only
   about non-pooled water. Worth re-reading §7 and §9 of the research doc with
   that in mind before either is decided.
5. **Does the level model want to feed the render-only surface displacement**
   already noted as free in research §4.3? A body with a known level and area is
   exactly the input a cheap animated lake surface wants. — **Answered yes, §13.**
6. **Where does the `r_excite` particle budget actually land?** §14.5 puts a
   violent drain at ~33,000 particles against a ~40,000 largest-measured scene.
   Shell-instead-of-ball (§16) is the stated mitigation but is unmeasured. This is
   the one number in the look packages that could force a redesign.
7. **Does `funnelAt()` want to generalise?** It is "an analytic region that reads
   as air despite the voxels." Steam pockets, explosion cavities and a diving
   bell are the same shape. Resist until there is a second real caller — but note
   that if there is one, this becomes a small named subsystem rather than a drain
   feature.
8. **Does the impact-ripple ring buffer belong to the body or to the world?**
   Per-body is tidier and sleeps with the body; world-global is simpler and
   survives a body being released mid-ripple.

---

## 13. The three render tiers — and why waves are a separate system

> Everything from here to §16 is the **look** half of the design, added
> 2026-08-27 after the packages above. It is deliberately sequenced *after*
> A–F: tiers 2 and 3 read tier 1's state, so the bookkeeping has to exist first.

The tempting mistake is to ask one field to produce both currents and waves. It
cannot, and the reason is sharp:

> **A current is transport** — water goes somewhere. **A wave is oscillation** —
> water goes in a circle and comes back, with essentially zero net transport
> (Stokes drift is a second-order dribble). They also run at wildly different
> speeds: a 2 m wave travels at ~1.8 m/s while a pond current is ~0.1 m/s, so
> **the waves move ~20× faster than the water they are made of.**

A field that advects mass cannot make waves; a field that oscillates cannot
transport. Build one thing that does both and you get jelly that drifts.

So: three tiers, split by frequency.

| Tier | What | Timescale | In the sim? | Owns mass? |
|---|---|---|---|---|
| 1 | **Level** — the ledger (§7) | minutes | yes | **yes**, via voxels |
| 2 | **Current** — `currentAt()` (§14) | seconds | yes: biases MPM, pushes player and debris | no |
| 3 | **Waves / ripples** — displacement (§13.1) | 0.1–1 s | **no — render only** | no |

They cohere because **tier 3 reads tiers 1 and 2**, one-way. That is the whole
trick: the renderer shows water flowing and rippling everywhere, continuously, at
full pixel resolution, while the sim below it moved a surface down by one eighth
somewhere and otherwise slept.

`RESEARCH_water_architecture.md` §4.3 already flags render-only surface
displacement as "nearly free, no seam, no mass conservation, no determinism
exposure, because it is not in the sim." This is that — made to *look correct* by
feeding it the sim's actual state instead of arbitrary constants.

### 13.1 Waves: one formula, and the detail that decides whether it looks like water

Sum 4–8 Gerstner waves, analytic in `(x, z, t)`. Zero storage, zero sim cost,
evaluated only where a ray hits the water surface — cost is O(water pixels), not
O(volume).

**Set each component's speed from its wavelength via the dispersion relation.**
This is the single highest-leverage accuracy decision in the whole render tier
and it costs nothing — it is purely *how you choose the constants*:

```
ω² = g·k·tanh(k·h)          k = 2π/λ,  h = depth
  deep   (h ≫ λ):  tanh → 1     ⇒  c = sqrt(gλ/2π)   — longer waves are FASTER
  shallow(h ≪ λ):  tanh(kh)≈kh  ⇒  c = sqrt(g·h)     — λ cancels, all speeds equal
```

Water is **dispersive**, unlike sound or light in vacuum. Dimensional analysis
forces it: in deep water the only quantities available are `g` [m/s²] and `k`
[1/m] (density cancels, depth is irrelevant because the wave does not feel the
bottom), and the only speed constructible from them is `sqrt(g/k)`. In shallow
water the wave *does* feel the bottom, so the quantities are `g` and `h`, and the
only speed is `sqrt(gh)` — with no `k` in it, hence non-dispersive.

If every octave scrolls at one speed the surface reads as a moving texture. In
the default pond (`pondDepth = 26` → 2.6 m at `kVoxelMeters = 0.10`) the spread
across the octaves worth rendering is **4×**:

| λ | `k·h` at centre | `tanh(kh)` | `c` |
|---|---|---|---|
| 0.5 m | 32.7 | ≈1.00 | 0.88 m/s |
| 2 m | 8.2 | ≈1.00 | 1.77 m/s |
| 8 m | 2.04 | 0.967 | 3.48 m/s |

Then the same `tanh(kh)` pays a second time, because **`h` is already known** from
the tier-1 level and the terrain. Approaching a bank at `h = 0.3 m`, the 8 m wave
slows from 3.48 to **1.70 m/s** while the 0.5 m wave barely changes. That
differential slowdown *is* shoaling: waves steepen in the shallows and **refract
to run parallel to the shore**. It is the effect that sells a lake, and it falls
out of a term already being computed. At the very edge the user's naive intuition
becomes exactly true — everything converges on `sqrt(gh)` and travels together.

Footnote for calibration: below ~1.7 cm surface tension takes over and the trend
*reverses* (capillary waves get faster as they get shorter, minimum phase speed
~0.23 m/s). Far under a 10 cm voxel, so it matters only as the explanation for why
very fine ripples look qualitatively different from chop.

### 13.2 Coupling to the current, ripples, and shores

- **Flow reads as flow** by evaluating wave phase at `position − current·t`. The
  Doppler stretch downstream is what makes a surface look like it is *going
  somewhere*. Tier 2 → tier 3, ~free.
- **Ripples from impacts need state**, because a ripple is the memory of an
  event. Stay in the `windAt()` idiom: a small **bounded ring buffer** of recent
  impacts, each drawn as an analytic expanding ring with amplitude decay — a pure
  function of `(eventList, t)`, bounded by construction (rule 2). The upgrade path
  if reflection and interference off banks is ever wanted is a per-body 2D
  wave-equation texture, but that is stored state plus a solver. Do not start
  there.
- **Fade wave amplitude to zero near shores.** Sum-of-sinusoids does not reflect
  off banks, and shallow water damps chop anyway — so the cheap fix is also the
  physically right one.
- **Foam** on convergence lines of the current field, thinning on divergence.
  Free: the field is analytic, so its divergence is closed-form.

### 13.3 Cost, and the one mistake that is expensive

Evaluate all of this at the water-surface hit, not through the volume. The perf
audit already identified the raymarch **media march** as what collapsed FPS
during fires; do not add per-sample work to it without a gate (§15.3).

**The `ptr<uniform, T>` rule is not optional.** `common.wgsl:523–534` records it
in as many words: a function that dynamically indexes `windPrims` on a **by-value**
uniform spills the whole struct to scratch, which cost 220 ms vs 20 ms frames.
Any current-primitive array must take `ptr<uniform, T>` **and so must every
caller**, from the first line written.

### 13.4 The render/sim boundary

A render-only wave cannot push anything. The moment displacement is fed back so
that boats bob, a render field has become authoritative for sim — guideline #3.

If that is wanted: expose surface height as a **read-only query the game reads**,
keep the body's *level* (tier 1, sim) strictly separate from its *displacement*
(tier 3, render), and never let the CA see tier 3. Breaking waves, whitewater and
spray are not a displacement problem at all — that is where MPM takes over
(§16), and `RESEARCH_water_architecture.md` §4.3 names hfFluid's spray/foam mass
exchange as the template.

---

## 14. The current field: accuracy, regimes, and vortices

Structurally this is a clone of the wind field: `windPrims` is
`array<vec4<i32>, 96>` (32 primitives × 3 words) present in **both** `TickParams`
(`common.wgsl:410`) and `RenderParams` (`common.wgsl:638`), with `windPrimAtQ`
for the sim and `windPrimAt` for the renderer. `currentAt()` should be the same
shape, for the same reasons, subject to §13.3.

### 14.1 Two regimes

**Pooled water** — at equilibrium there is no current except what sources and
sinks drive, so the field is a superposition of singularities:

- drain sinks: `1/r²` radial inflow + `Γ/2πr` tangential (§9)
- inflows, waterfalls, rain
- **wind stress on the surface**, worth building because the real effect has
  vertical structure: a surface layer moving downwind with a *return flow beneath
  it moving back upwind*. That is what makes a lake read as a body of water
  rather than a scrolling texture, and it is one extra term with a
  depth-dependent sign, driven by the `windAt()` that already exists.

**Flowing water (streams)** — nearly free, because the governing quantity is
already generated. Manning/Chézy gives `v ∝ sqrt(slope · depth)`, direction from
the bed gradient, and `Land.slope` exists as a Q8 field (`worldgen.wgsl:443`,
`256 == 1 voxel/voxel == repose`).

> **Trap, and it is a repeat of one this repo already paid for.** Use the
> **landform** gradient, not the full one. `worldgen.wgsl:550` states that
> `Land.slope` is `g2` accumulated through the hill octave *specifically because*
> `d(slope)/dcolumn` through the grain octave is 96 Q8 — the whole gate range in
> ONE column. A current built on the fine gradient is per-voxel noise. See the
> "slope gates must read the landform" failure.

### 14.2 Accuracy envelope — and why the errors are harmless

Superposition of sources, sinks and vortices is a **real solution of Laplace's
equation**, not a hack; incompressible irrotational flow away from boundaries is
approximately what pond water does. What it does not get:

- no-flow boundary conditions at terrain (one image singularity fixes a flat
  wall; general terrain does not)
- no separation, no eddies behind obstacles, no turbulence

**This is tolerable precisely because of §2.** The current field owns no mass, so
its errors are cosmetic and behavioural, never conservation bugs. A wrong current
pushes a leaf the wrong way; it cannot lose water. That is the payoff for keeping
it a pure function, and it is the reason this tier can be tuned by eye.

### 14.3 The vortex primitive

One more primitive in the superposition: position, axis, circulation `Γ`, core
radius, decay. The free-vortex surface depression is closed-form and drops
straight into the tier-3 displacement as one extra term:

```
z(r) = − Γ² / (8π²·g·r²)
```

**Setting `Γ` by hash is physically legitimate, not a fudge.** A real bathtub
vortex is not created by the drain — it is residual ambient circulation being
concentrated as fluid moves inward: `Γ` is conserved, so `v_θ = Γ/2πr` blows up as
`r` shrinks. The swirl is an *initial condition*. So `Γ` from `hash3` of the hole
position plus a contribution from the body's ambient circulation is the right
*kind* of quantity. (It is also why the Coriolis story is a myth: residual motion
beats Coriolis by orders of magnitude below lake scale.)

An **air core** forms when the depression at the drain radius exceeds the depth:

```
air core  ⟺  Γ² ≥ 8π²·g·h·r_drain²
```

### 14.4 A violent drain, in numbers

Scale §7.5 by hole area. A 1 m × 1 m hole is 100× the 1-voxel case ⇒ ≈3,800
voxels/s. Against the default pond (≈87,000 voxels, ≈7,240 surface cells):

| Quantity | Value |
|---|---|
| Time to empty | **~23 s** |
| Surface descent | ~0.5 vox/s = **4.2 eighths/s** |
| Per surface cell | one eighth-step every ~0.24 s ⇒ **~14% of surface cells step per tick** |
| Torricelli exit speed at h = 2.6 m | **7.1 m/s** |
| `Γ` needed for an air core (r_drain = 0.5 m) | **22.4 m²/s**, i.e. `v_θ ≈ 7.1 m/s` at the throat |

Two conclusions, and the first one was a surprise:

1. **The level model survives this fine.** 14% of cells stepping per tick under
   the §7.4 dither reads as a smooth, slightly shimmering, continuously
   descending surface. Shaving the surface is *not* what breaks.
2. **The throat is what breaks.** Water there is genuinely moving at ~7 m/s and
   no surface displacement will sell that as still water with a picture painted
   on it. Note the coincidence in the table: the swirl speed needed for an air
   core is essentially the Torricelli exit speed, which is why real drains grow
   air cores so readily.

### 14.5 Therefore: excite locally, do not eject the body

The machinery already exists — the excite/settle seam (`sim_fluid_seam.wgsl`,
`fluidExciteMode`). A violent drain should **excite a region around the hole into
MPM** while the remaining ~95% of the pond stays pooled and cheap. Then the
funnel and its air core are **real**: real particles, real hole, real spray, real
interaction with anything that falls in. The ledger debits what MPM actually took
(§8.1's granted-not-demanded rule) and the bulk keeps costing O(1).

The excite radius has a clean criterion — **use the analytic model to decide
where the analytic model stops being good enough.** Excite out to where the
funnel dip exceeds about one voxel:

```
r_excite = Γ / sqrt(8π²·g·0.1)
```

> **Honest cost flag.** For the `Γ ≈ 22 m²/s` above that is **~2.5 m ≈ 25
> voxels**, and a 25-voxel-radius hemisphere is **~33,000 particles** — against a
> largest-measured lab scene of ~40,000 (`RESEARCH_water_architecture.md` §4.2).
> A violent drain sits at the top of the measured envelope. This is a real budget
> question, not a rounding error. Mitigation if it bites: excite a **shell** (the
> surface annulus plus the throat column) rather than a solid ball — the interior
> of a funnel is water nobody can look at. See §16.

---

## 15. `funnelAt()`: making the analytic core honest

For the in-between case — a drain too strong for a flat surface, too weak to
justify tens of thousands of particles — the funnel can be a **render-only carve**:
the raymarcher tests each water sample against an analytic hyperboloid and treats
inside-the-funnel as air. Closed-form normal (better shading than a voxel
surface), zero storage, zero sim cost. It is the `microvox` trick from design
guideline #2.

The obvious objection is that it lies: a player swimming into the core is
underwater while looking at air.

### 15.1 The reframe — a lie is a fact with one consumer

Guideline #3 does not forbid derived representations; it forbids **unowned
diverging** ones. `windAt()` is the standing precedent: a pure analytic function,
no stored state, consumed by the sim *and* the renderer, and nobody calls it a
lie — because there is exactly one authority for "what is the wind here."

> **Make `funnelAt(p)` the single authoritative answer to "is this point air,
> notwithstanding what the voxel says," and have every consumer call it.** Then
> there is no divergence to have. It is derived data with one owner, exactly like
> `microvox`.

This is tractable rather than whack-a-mole because the consumers are **few and
enumerable**.

### 15.2 The consumer table

| Consumer | Site | Change |
|---|---|---|
| Player buoyancy / swim | `player.cpp:479` — `inLiquid = liquidCells > 0` | do not count cells inside the funnel |
| Submerged view | `shadeSubmerged` / `waterSurface`, `raymarch.wgsl:3517`, `:3968` — already take `underwater : bool` | flag comes from the same function |
| Media march absorption | Beer–Lambert path, `raymarch.wgsl:2498` | funnel segments contribute no absorption |
| Submerged look knobs | `tuning.h:1611` block (`subVignette`, murk distance) | gate on the same flag |
| Audio muffling | `audio/cues` | same query |
| MPM particles, debris, projectiles | drag / buoyancy sites | same query |

One of those is *the* site: `liquidCells` is counted in a single place.

**And `submersion` is already a fraction, not a bool** (`player.h:49` distinguishes
ankle-deep from fully-under; see the "liquid state is a fraction" failure). So a
player straddling the funnel wall gets partial buoyancy automatically and crossing
into the core is a smooth ramp, not a pop. The hardest part of selling this was
built already, for an unrelated reason.

**The forces come free.** `currentAt()` is right there: `Γ/2πr` spins the player
around the wall, the throat's Torricelli velocity pulls them down. Being caught in
a whirlpool becomes an experience rather than a diorama, at no extra cost, drawn
from the same field the visual is.

### 15.3 Three residuals, honestly

1. **The CA needs no patch — a lucky break.** One might expect water voxels beside
   the funnel to fall inward and fill it. Physically they should not: a steady
   vortex funnel is *held open by rotation*, not collapsing. The CA leaving them
   alone is the correct appearance, not something being got away with.

   The corollary is a hard requirement: **the funnel radius must be driven by live
   `Γ`, and `Γ` must decay when flow stops.** Otherwise a permanent cavity stands
   open in still water, which is instantly and obviously wrong. Get that right and
   the closing is free — as `Γ` decays the funnel shrinks and a player inside
   becomes underwater again automatically, because everyone reads the one
   function.

2. **Anything reading the raw voxel buffer without going through `funnelAt()`
   diverges.** That is discipline, not design, and this repo already has the
   pattern for it — the page table's "address voxels ONLY through
   `voxWordAt`/`voxWordIndex`/`voxStore`". Make it a stated invariant with a
   check, not a convention.

3. **A mathematical surface is too perfect.** No chop, no wobble, no droplets, no
   spray at the throat. Every physics consumer can be patched and it will *still*
   read as CG, because real water at a violent interface is shredding. This is the
   residual that actually breaks the illusion — and §16 is the answer to it.

---

## 16. The general pattern: analytic bulk, MPM interface

The choice is not between ~33,000 particles and zero.

> **Emit MPM as a thin surface layer — the funnel wall and the throat — and leave
> the bulk analytic. Hundreds of particles, not tens of thousands; roughly 1% of
> the full-excite cost.**

That buys real droplets flung off tangentially, real spray where the flow necks
down, real splashing where it exits, and real interaction with anything falling in
— on the only part of the volume anyone can see. The interior of a funnel is water
that cannot be looked at.

It is the same principle as the **skin/collider resolution split**: full resolution
where it is observed, cheap where it is not. And
`RESEARCH_water_architecture.md` §4.3 already names the mechanism — hfFluid's
spray/foam mass exchange, listed there as "worth stealing regardless of the
decision."

**This generalises past the drain, and is probably the most reusable idea in this
document.** Analytic bulk + MPM interface is also the right shape for waterfalls,
shore breakers, spillways, and any place where a large volume of water has a small
violent surface. It is worth building the interface-emitter as a *shared
primitive* in package J rather than as part of the drain.

---

## 17. Relationship to the existing documents

- `docs/RESEARCH_water_architecture.md` — the decision record. **This is
  effectively "Option F"** in its §5 space, and it is the first option that gets
  the draining win (§4.1.4) without the heightfield's column assumption
  (§4.1.1) or a second solver seam (§4.1.3).
- `docs/PLAN_mpm_fluids.md` — architecture of record, unchanged. This adds a
  bookkeeping layer above MPM, not a competitor to it.
- `docs/PLAN_fluid_overhaul.md` — active work queue; WP5 open (see §12.4).
- `docs/PLAN_terrain_overhaul.md` — supplies the pond bowls this depends on
  (§3.1) and package D (the sea) is the open interaction (§12.3).
