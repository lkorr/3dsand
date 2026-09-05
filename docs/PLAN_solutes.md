# Solutes: concentrations of a dissolved substance in a liquid — plan of record

Status 2026-09-03: **NOTHING BUILT.** This is the design decision record and the
package order. No code, no assets, no gates yet.

Revised same day: §3.3.1 (the dilution floor) and §3.4.1 (multigrid vs. a rate
limit) were added after working the blood-into-water case, which is the one that
exposes both. §3.3.1 is a **correctness requirement on P2**, not polish.

The question this answers: *"if I add 10 salt voxels to 100 water voxels, how do
I get 0.1 salt per water voxel to disperse through the liquid — without a unique
material id per concentration, and in a way that upscales to every
material/liquid pair?"*

`sim_step.wgsl:624` already carries the TODO in prose: *"Blood mixing into water
is a different (and unimplemented) thing."* This is that thing.

## 1. The decision, in one line

**Store absolute solute MASS per cell in a sparse aux layer; derive
concentration. Species is a data table, not a material id.**

Everything below follows from that sentence. The two rejected alternatives:

* **A material id per concentration.** `saltwater_1` … `saltwater_16`. Dies at
  the second species: the id space is the product |liquids| x |species| x
  |bands|, every reaction rule must be written per band, and the 4096-slot
  material space is gone. Also violates guideline 4 outright.
* **Store concentration, not mass.** Tempting, because concentration is what you
  render and what rules test. It is wrong because **every operator then has to
  re-derive it across two cells of different fullness, and bleeds mass on each
  rounding.** A pond loses its salt over a few thousand ticks and nobody can see
  why. Mass is conserved by construction under integer add/subtract;
  concentration is not.

`c = mass / fullness` is derived data — reconstructible, disposable, never
stored. Guideline 3.

## 2. Storage

### 2.1 Not in the voxel word

The word is full (CLAUDE.md's table). The one apparent gap is **bit 15**: liquids
store fullness as `state + 1` over 0..7 (`transferLiquid` writes `sf - t - 1u`
and `df + t - 1u`, both capped at 7; `sim_fluid.wgsl:546` reads
`voxState(w) + 1u`), so the top bit of the state nibble is never set on a liquid.

**Do not take it.** It is *de facto* free but *unprotected*: 24 of the 32
`voxState` call sites across `assets/shaders/` and `src/` do `voxState(x) + 1u`
with **no `& 7u` mask**, so setting bit 15 would silently give 24 readers a
fullness of 9..16. Claiming it costs an audit of all 24 and buys one bit, and the
page-table sentinel below already gives the fast path that bit would have bought.

### 2.2 A sparse aux page layer, mirroring the voxel pool

Per-cell `{ u8 species, u8 mass }`, packed 2 cells per `u32`. Paged exactly like
`pagetable.cpp`, with the same sentinel scheme:

```
solutePage[chunkSlot] -> SOL_EMPTY | SOL_UNIFORM(species, mass) | page index
page = kChunkVol * 2 B = 4096 * 2 = 8 KiB   (half a voxel page)
```

Precedent for a chunk-keyed aux layer is `waterbody.h:458` (`ChunkBody()`, chunk
slot -> body index) and its comment is the argument for this one too.

**`SOL_UNIFORM` is what makes the feature cheap, and it is not a lucky break: a
fully-mixed body is the STEADY STATE of the whole feature.** The common case
compresses to a page-table entry. `SOL_EMPTY` covers essentially the whole world.
A real page exists only for a body mid-dispersion, which is transient by
construction.

**`kSolutePoolPages` is DERIVED, never a literal** — same rule as `kPoolPages`
(`world.h:918`), same fatal-abort-on-exhaustion consequence, and it needs its own
`Alloc()` failure verdict rather than sharing the voxel pool's.

### 2.3 Species is data

`assets/materials/solutes.json`, authored like `materials.json`, hot-reloaded
with R:

```json
{ "name": "salt", "yieldPerVoxel": 256, "saturation": 96, "diffusivity": 12,
  "densityPerUnit": 40, "tint": "#e8f0ff", "precipitatesTo": "salt",
  "dissolvesFrom": "salt", "solventTags": ["water"] }
```

|liquids| + |species| authored rows produce |liquids| x |species| behaviours.
Adding `sugar` / `ink` / `poison` touches no code.

**Two species in one cell** needs a defined answer or it is a silent nondeterminism
source. Default: **refuse the transfer** (immiscible — safe, deterministic, and
correct for most pairs). Opt into a reaction per ordered pair in a small mixing
table. Acid-into-blood is a mixing-table row, not a special case.

### 2.4 The numbers for the motivating case

10 salt voxels at `yieldPerVoxel: 256` = 2560 units into 100 water cells =
**25.6 units/cell**; `c = 25.6/256 = 0.1`. u8 holds it with 0.04% quantization
error.

For contrast, the cheap version that will be proposed at some point — reuse the
4-bit stain amount, which already rides `packVoxKeepStain` through `tryMove` for
free — gives full-strength = 15, so the same 10 salt = 150 units over 100 cells =
**1.5, rounding to 1 or 2**: a 33% error, a hard cap of 7 species shared with
blood and soot, and bloodstained water can never also be brine. Fine for
prototyping the operators in P1. Not the feature.

## 3. Dispersion — the part that decides whether this is shippable

### 3.1 The complexity problem

Nearest-neighbour exchange diffusion homogenizes in **O(L^2) ticks** for a domain
L cells across. At the 30 Hz sim tick:

| L (cells across) | ticks | wall |
|---|---|---|
| 10 (a 100-voxel pond) | ~100 | 3.3 s |
| 20 | ~400 | 13 s |
| 40 | ~1,600 | 53 s |
| 100 | ~10,000 | 5.6 min |

Above ~L=40 this is both unwatchable and a rule-2 violation: the whole body stays
awake for the duration, so cost scales with the size of the lake rather than with
activity.

### 3.2 The crossover is already in the tuning file

`sim.waterBodyMinVolume = 65536` voxels ~ **40^3** — which is, to within the
precision this estimate deserves, exactly where the table above turns from
seconds into minutes. The existing threshold sits at the algorithmic crossover.
Exploit that rather than inventing a second one:

* **Below the threshold** (unbodied liquid: ponds, spills, flows, puddles, and
  every straddling chunk the body system refuses) — **plain pair-exchange
  diffusion**. L is small, so O(L^2) is seconds. This case needs nothing clever
  and it fully covers the motivating scenario.
* **At or above it** (lake-scale, body-labelled) — **reservoir + excess**, below.

The expensive algorithm only ever runs where the domain is small. Rule 2 is
satisfied by construction rather than by tuning.

### 3.3 Pair-exchange diffusion (P2 — the baseline)

Pull-only is broken: if A pulls from B while B pulls from A off the same old
values, mass doubles. Use paired exchange with a single owner:

```
axis, parity <- f(tick, substep)          // deterministic, cycles all 6 dirs
owner        =  cell with even coord along axis; owns the pair (c, c+axis)
d = (mA*fB - mB*fA) / (fA + fB)           // the equal-concentration split
d = (d * species.diffusivity) >> 8        // clamp so neither side goes negative
if (d == 0) { return; }                   // FIXPOINT — do NOT markDirty
write BOTH cells                          // reach 1, disjoint pairs, no atomics
```

The owner writes both sides, pairs are disjoint, single-writer holds without
atomics or CAS. Exactly conservative: the same integer is subtracted and added.

**The `d == 0` early-out is load-bearing.** Without it every body of liquid in
the world stays awake forever — `gotcha-light-gated-rules-never-sleep` again, and
the integer fixpoint is the same shape as the equalize fixpoint in
`gotcha-ca-liquid-levelling-limit`. Convergence is to "uniform within 1 unit",
not to "uniform".

### 3.3.1 The dilution floor — REQUIRED, not an optimization

That same early-out has a consequence that only shows up at low concentration,
and it is the difference between blood dispersing and every lake in the world
being permanently pink.

**Integer diffusion stalls below its own quantum.** Once a plume has spread far
enough that neighbouring cells differ by less than one unit, `d` is 0
*everywhere* and diffusion **stops dead** — it does not asymptotically fade, it
quantizes to a halt. The plume freezes as a faint, oddly-shaped blob. And because
§1 makes mass strictly conserved, that blob is **permanent**: a lake beside which
anything has ever bled stays visibly tinted in frozen blotches forever.

Worked case: a bleeding mob puts ~2,560 units into a lake of ~10^6 cells. The
equilibrium concentration is 0.00256 units — three orders of magnitude below the
quantum. The blood never disperses at all; it sits where it landed.

**The fix is a per-species dilution floor.** Below `species.floor`, a cell's mass
decays to zero and is **discarded** — deliberately non-conservative, and correct:
it is the same shape as smoke decaying to air. It is what makes a substance
disperse *and be gone*, and it is what lets the chunk collapse back to
`SOL_EMPTY` and stop costing a page.

This means gate assertion (a) in §6 must be scoped: **exact conservation holds
only above the floor.** A conservation gate written without that scope will fail
the moment the floor does its job, and the temptation will be to "fix" the floor.
Assert conservation on a body held above the floor, and assert *disappearance*
separately below it.

### 3.4 Reservoir + excess (P4 — deferred, and BLOCKED)

For lake-scale bodies, split the field:

```
c(cell) = c_body + excess(cell)
```

* `c_body` — one number per (body, species): total mass / total volume.
  **Authoritative for bulk.** Dissolving adds to it, and it is instantly correct
  everywhere: O(1), not O(L^2).
* `excess(cell)` — the local deviation, in the aux layer. **Authoritative for the
  plume.** Sums to zero over the body, and relaxes toward zero by §3.3 over a
  handful of cells.

Convergence becomes exponential at a fixed rate, **independent of L**. This is a
well-mixed-reactor model, not physical diffusion — instant bulk transport is
wrong physics and the right game model, and after two seconds nothing
distinguishes them on screen.

**What this does NOT flatten.** For any injection that is small relative to the
body — blood from a wound, a splash of acid, anything at creature scale —
`c_body` rounds to **zero** and contributes nothing. The entire visible plume is
the excess field, simulated per-cell by §3.3. The reservoir cannot flatten a
blood plume because it has nothing to say about one. The instant-uniform artifact
is only reachable by injecting a large fraction of the body's own mass (salinating
a lake on purpose), and then it shows as the far shore changing tint on the same
frame as the near shore.

### 3.4.1 If a visible front is ever wanted: multigrid, not a rate limit

Rate-limiting the reservoir does **not** produce a front — every cell relaxes
toward the same target at the same rate, so a lake uniformly fades in, which
reads worse than instant. A front needs real spatial transport. The affordable
form is **two-level multigrid**, with the coarse grid being the **per-chunk
aggregates** (`kChunk = 16`, so L shrinks by 16):

| | naive (§3.3) | 2-level multigrid |
|---|---|---|
| L = 100 lake | ~10,000 ticks (5.6 min) | **~256 ticks (8.5 s)** |
| awake set | the WHOLE lake, for the duration | a **moving band** at the front |
| storage | — | 32,768 chunks x 8 B = **256 KiB** |

The awake-set row matters as much as the wall-clock one: naive diffusion is a
rule-2 violation for the whole duration, whereas a front is a band of activity
that sleeps behind itself. Multigrid is cheaper on both axes.

**Cost and risk:** one restrict/prolong pair per level. Restrict is maintained
incrementally (every solute write also deltas its chunk total) rather than as a
reduction pass. **Prolongation is the correctness risk** — it must split an
integer coarse delta across a chunk's cells with exact remainder handling, or the
mass leak §1 was designed to prevent comes back in through the back door. It
wants a `SANDVOX_PT_AUDIT`-style per-tick recount that aborts on the first
imbalance.

**The reservoir and the front are the same algorithm at different level counts** —
§3.4 is multigrid collapsed to one coarse cell per body. Level count is a knob,
not an architecture decision, so building the per-chunk aggregate layer in P2
(which the reservoir needs anyway) forecloses nothing.

**BLOCKED, and this is why it is P4 and not P1.** It needs body labelling, and
today `sim.waterBodyMode = 0` — the body system is off by default, and flipping
it to 1 is a standing open owner decision (`project-water-bodies`). It also only
ever covers bodies >= 65,536 voxels and **refuses straddles** (`waterbody.h:463`),
so it can never be the only mechanism. §3.3 must stand alone and be correct
without it. Do not build P4 until `waterBodyMode=1` has landed on its own merits.

## 4. Concentration-dependent behaviour

Not `if (mass > X)` in the kernel — that is behaviour as code, one branch per
species forever, guideline 4. Two mechanisms, by kind of property:

### 4.1 Discrete -> a condition term on the existing reaction table

`doReactions` (`sim_step.wgsl:379`) already evaluates conditions
(`nbrMatches`, `lightMatches`, `scaledChance`). Concentration is one more term:

```json
{ "self": "blood", "solute": "acid", "cMin": 96,
  "neighbor": "flesh", "chance": 400, "neighborBecomes": "gore" }
```

**Constraint, verified:** `ReactionGpu` is **exactly 32 bytes with a
`static_assert`** (`materials.h:296-313`) and `cond` is packed through bit 23 —
only bits 24..31 remain, which will not hold a species id (8) plus a threshold
(8) plus a direction. So this costs **one new `u32` (36 B) or a parallel side
array**. Decide in P3; the side array keeps the 32 B cache line intact and is
probably right, since a solute condition is rare across the rule set.

**Rule insertion re-rolls later rules** (`project-weak-flame-burn-tint`): rules
for the same `self` are tried in file order, at most one fires per cell per tick.
Adding solute rules perturbs the RNG of every later rule for that material and
moves the world hash. Expected — one `--rebaseline`, no investigation.

### 4.2 Continuous -> a linear modifier, no branch

```
density = m.density + ((species.densityPerUnit * c) >> 8)
```

`canDisplace` (`sim_step.wgsl:108`) already reads `t.density` as an i32; it
becomes a function of the cell rather than of the material. This is the one that
pays for itself: **brine is denser than fresh water, so it sinks and stratifies**
— a halocline out of one multiply. Same modifier shape for viscosity and for the
MPM `restDensity`.

Cost is controlled by the sentinel, and this is why §2.2's sentinel matters more
than it looks: `SOL_EMPTY` (almost everywhere) -> no lookup;
`SOL_UNIFORM(species, mass)` -> answered from the page-table entry already
loaded, **no page touch**; only a real page costs a fetch. The steady-state lake
never reads a solute page to answer a density query.

## 5. Packages, ordered by risk

| P | Scope | Depends on |
|---|---|---|
| **P1** | Aux pool + sentinels + `solutes.json` loader + advection through `tryMove`/`transferLiquid` + dissolve/precipitate reactions + hash + save round-trip | — |
| **P2** | Pair-exchange diffusion (§3.3) + **the dilution floor (§3.3.1)** + per-chunk aggregates, with the fixpoint sleep. **Completes the motivating scenario.** | P1 |
| **P3** | Concentration conditions on reactions (§4.1) + continuous density/viscosity modifier (§4.2) | P1 |
| **P4** | Reservoir + excess for lake-scale bodies (§3.4); multigrid front (§3.4.1) only if §8.1 says so | P2, **and `waterBodyMode=1` landed** |

P1+P2 is the shippable unit. **P2 is not shippable without §3.3.1** — the floor is
a correctness requirement, not a polish item.

**The blood-in-water case, which is the one most likely to be asked for, is
entirely P1-P3 and needs no P4:** advection carries it on the current (P1), pair
diffusion billows and fades it (P2), and `densityPerUnit > 0` makes the plume
SINK through the water column and pool on the bottom (P3, §4.2) — which is the
difference between a physical substance and a decal, and is a stronger visual
than any front. P4 may never be needed at all.

## 6. Gates

**`--gate solute`** — one invocation, three assertions, no second pass to
interpret it (CLAUDE.md "authoring cheap-to-verify work"):

```
drop N salt into a pond sized so equilibrium c stays ABOVE species.floor,
run 300 ticks, assert
  (a) total solute mass == exactly N * yieldPerVoxel      (conservation)
  (b) awake chunks back to <= 32                           (it SETTLES)
  (c) max(c) - min(c) < 2 units                            (it settled UNIFORM)
```

(a) catches an averaging regression; (b) catches a missing fixpoint early-out;
(c) catches a diffusion that settles into stripes. All three fail differently —
a bare "it looks mixed" would catch none of them (rule 6).

**The pond must be sized above the floor, and the gate must say so in a comment.**
Conservation and the §3.3.1 dilution floor are in deliberate tension: below the
floor, mass is discarded on purpose. A conservation gate written without that
scope will go red the first time the floor works correctly, and the obvious
"fix" is to break the floor.

**`--gate solute-dilute`** is therefore the other half, and asserts the opposite:
inject a creature-scale amount into a body large enough that equilibrium is below
the floor, and assert that after N ticks **the solute is GONE** (no residual
tinted cells, chunk collapsed back to `SOL_EMPTY`). This is the one that catches
"every lake is permanently pink after a battle" — which is otherwise a bug nobody
finds until a playthrough is hours old, and which the conservation gate above
would actively certify as correct.

Plus: **`--gate solute-seam`** (excite a solute-carrying cell into MPM and settle
it back; assert mass identical), **`--gate solute-evap`** (evaporate a brine pool
to saturation, assert it precipitates a salt voxel and mass balances across the
phase change), and thresholds in `tests/baseline.json`, not in C++.

`--sweep sim.soluteDiffusivity=0,12,64` proves the knob reaches the kernel in one
invocation, no file touched.

## 7. What will actually bite

1. **Solute mass is AUTHORITATIVE state.** It must enter the world hash and must
   round-trip the save format — `gotcha-save-format-drops-stain` is the exact
   precedent for getting this wrong, and it was found late. New save version.
   The page table stays derived/unhashed; the solute pages do not.
2. **The MPM seam — better news than expected.** `FluidParticle` has **twelve
   reserved i32 words** (`_r0`.. `_r11`, `common.wgsl:2620`), zero-initialized at
   both spawn sites (`sim_fluid_seam.wgsl:408`, `:1123`) and never read. `_r0`
   takes the `{species, mass}` payload outright — no struct growth, no repack.
   (`attr` bits 23..31 are also free, bit 22 being ORIGIN, if a narrower field is
   ever wanted.) **Until that is wired, a particle that carries liquid out of the
   grid and back DELETES its solute** — so P1 must either wire `_r0` or refuse to
   excite solute-carrying cells. Refusing is the cheaper correct first cut.
3. **A second pool means a second fatal abort.** Size it against `--autofly-hard`
   *plus a deliberately stirred lake* — the voxel pool's lesson
   (`gotcha-page-pool-retire-queue`) is that streaming harnesses do not reach the
   exhaustion band; **ACTIVITY does**, and a dispersing lake is pure activity.
4. **Still water does not mix.** A settled pond is at the CA equalize fixpoint,
   so advection contributes nothing and diffusion is the *only* transport. Do not
   ship P1 alone and conclude the feature works.
5. **Perf budget has no headroom right now.** `sim` was measured at **13.13 ms
   against a 13.00 budget** on 2026-09-03 (contended machine, so possibly not
   real). Advection is structurally near-free — it rides stores that already
   happen — but "mostly free" and "fits the current budget" are different claims
   and only the first is likely true. Re-measure `--perf` clean *before* P1, so
   there is an uncontended control to diff against.

## 8. Open decisions for the owner

1. **Does bulk transport need to look physical?** §3.4 makes a lake uniform
   instantly at the *bulk* level. **This does not affect blood, acid or anything
   at creature scale** — for those `c_body` rounds to zero and the whole visual is
   the per-cell excess field (§3.4). It is only reachable by injecting a large
   fraction of a body's own mass on purpose. If a visible front there matters, the
   answer is two-level multigrid (§3.4.1), **not** a rate limit — and since the
   reservoir is multigrid with one level, this is a knob, not an architecture
   fork. **Deferrable; nothing in P1-P3 depends on the answer.**
2. **36-byte `ReactionGpu` or a parallel side array?** (§4.1)
3. **Does `waterBodyMode=1` land?** P4 is dead without it; P1-P3 do not care.
4. **How many species, realistically?** u8 species allows 255. If the real answer
   is under 8, the whole aux layer could be a per-page species + u8-per-cell mass
   at half the storage — cheaper, but it forecloses acid-in-blood in one cell.
