# Per-voxel body reactivity — burning, dissolving, and the road to armour

Status: **research / roadmap. No code landed.** Written 2026-08-27 against HEAD
(`7b5e602`). Numbers below are measured from the repo, not estimated, unless
marked otherwise.

Goal being assessed: a mob is not "on fire" — *individual voxels of a mob* are
on fire. Cloth catches easily, flesh does not; flesh chars through
`flesh -> cooked -> charred -> ash`; acid eats a leg off without necessarily
killing the creature; a burning mob walking into a bush sets the bush alight;
fire on a body spreads superlinearly with the size of the burning front.

---

## 1. Verdict

**Viable, and cheaper than it sounds — but not by extending the system that
looks like it should extend.** The obvious move (turn on `BurnBodies` for micro
bodies, run it over mob limbs) is the wrong one, and would be a hard performance
failure. The right shape is a different pass with the same *rules*, and it is
maybe 3 packages of work.

Three facts decide the whole design:

1. **Almost everything needed already exists.** Mob limbs already carry
   authoritative per-voxel lattices, already carve per voxel, already own
   copy-on-write render bricks, already split into debris when a carve
   disconnects them, and already round-trip through save/load. Mob skin
   materials (`skin`, `robe_cloth`, `leather`, `hair_white`, `bone`) are already
   real entries in the *shared* material table, already tagged
   `flammable / organic / dissolvable`. There is already a working CPU mirror of
   the reaction table (`DebrisSystem::BurnBodies`) that burns rigidbodies using
   the authored JSON rules. This is not a new system; it is a fourth population
   for an existing one.

2. **A full-lattice scan is off the table by two orders of magnitude.** Measured
   voxel counts in `assets/mobs/*.vox`:

   | def | skinScale | total skin voxels | largest limb |
   |---|---|---|---|
   | mina | 8 | **63,648** | 31,456 |
   | asha | 8 | 22,304 | 5,248 |
   | wizard | 4 | 6,060 | 1,554 |
   | critter | 2 | 692 | 352 |

   `kBurnScanPerTick` is **4096 voxels per tick across every rigidbody in the
   world**. One mina is fifteen times that budget. Scanning bodies is what
   `BurnBodies` does and it is correct for a 200-voxel plank; it does not
   survive contact with a 63k-voxel character. The pass must be driven by an
   **active front**, not by a cursor over the lattice.

3. **The binding constraint is PCIe, not CPU.** `Simulation::UploadMicroBodies`
   re-uploads the **entire brick pool** (`kMicroBodyPoolWordsWorld = 1<<20` =
   4 MiB) whenever `MicroBodySet::dirty` is set. That is fine today because the
   only thing that dirties it is a carve — a rare event. A burning mob dirties
   it *every tick*, which puts a 4 MiB/tick write on the frame path against a
   documented budget of **< 1 MB/tick** (DESIGN.md §11). This is the single
   thing most likely to make the feature "work" in a selftest and tank the
   frame rate in play.

---

## 2. What already exists (inventory, with the useful file references)

| Capability | Where | Notes |
|---|---|---|
| Per-voxel authoritative limb lattice | `MobSystem::Limb::skinVoxels` (`src/game/mob.h:499`) | int16 `PrefabVoxel`, skin-resolution, **this is the truth**; `voxels` (collider) is derived from it by majority-fill |
| Per-voxel carve of a *live* limb | `MobSystem::CarveLimb` (`mob.h:638`) | erase → re-skin → rebuild collider → split disconnected pieces. Damage is already geometric, not an hp threshold (DESIGN.md §7 "Carving living bodies") |
| COW render brick per damaged limb | `MicroBodyOwn/Edit/Free` (`src/sim/microbody.h:160-181`) | already used by both `DebrisSystem::DamageBody` and `MobSystem::CarveLimb` |
| CPU mirror of the reaction table over body voxels | `DebrisSystem::BurnBodies` (`src/phys/debris.cpp:706`) | runs the real JSON rules — decay/emit/pair, per-mille chances, file order, first-fire-wins — with counter-based `Hash3(serial, tick, ...)` RNG |
| Body → grid fire emission | same, via `cellOps` + `kCellOpIfAir` | burning debris emits **real** fire voxels into the CA, which then spread by normal rules. The "burning mob ignites the bush" half is already solved *for debris* |
| Grid → body ignition | same, via `world.Cached()` chunk cache | mob limbs already register `AddTerrainAnchor`, so chunks around them are already fetched |
| Charred-shape follow-through | `burnedSinceRebuild` / `burnedSinceShatter` | batched collider rebuild + batched connectivity re-check, one Jolt rebuild per tick globally |
| Emissive rendering of a hot voxel | `assets/shaders/microbody.wgsl:285` | `emberFlicker(mat.emission)` already runs per micro voxel. A voxel whose material becomes `ember` glows and flickers **with zero renderer work** |
| Neighbour-count gating ("needs N hot neighbours") | `scaleByNeighbors` (`src/sim/materials.h:213-233`, `sim_step.wgsl:315`) | already authorable: `minCount` 1..4, multiplier 1.0x..4.75x at a full 6. This is *exactly* the "one voxel burns itself out, a big front spreads" mechanic |
| Armour's attachment model | `src/game/item.h` — "a borrowed slot" | a held item **is a rig part** while worn, which already gives it severing, dropping to debris, per-voxel carving and micro detail. Armour is the same trick on a different socket |
| Materials for it | `assets/materials/materials.json` ids 47-57 | `robe_cloth`, `robe_trim`, `robe_shadow`, `skin`, `hair_white`, `leather`, `bone`, `steel`, `grip_leather` |
| Test hook | `debris` gate, "body burn" subtest (`src/test/selftest_phys.cpp:306`) | asserts a burning plank emits >5 fire ops and loses voxels. The template for the limb version |

Two things worth saying out loud because they are load-bearing and easy to
miss:

- **The player gets this for free.** The avatar is a `MobDef` with a different
  driver (`src/game/avatar.*`). Anything that burns a mob limb burns the
  player's limb. "Quickly take off the burning cloak" is a real mechanic on the
  same code path, not a parallel implementation.
- **A severed burning limb keeps burning with no new code.** `DetachLimb`/`Die`
  hand the limb to `DebrisSystem::AdoptBody`, and `BurnBodies` already burns
  debris. The two passes meet at the existing seam.

---

## 3. What is actually missing

Five gaps. Three are small, two are the real work.

### 3.1 `BurnBodies` explicitly refuses micro bodies — for a reason that has expired

`src/phys/debris.cpp:726-733`:

> `// MICRO BODIES DO NOT BURN (v1). ... a per-body edit is invisible and a
> copy-on-write pool is explicitly out of scope for v1`

The COW pool shipped. `MicroBodyOwn`/`MicroBodyEdit` exist and the carve path
uses them. The *second* stated reason — that the pass maps body-local coords
straight onto world cells, and a micro voxel is 1/scale of one — is still live
and still correct, and is the thing the new pass has to answer properly.

Note the entailment: fixing this also makes **severed micro limbs** burn, which
is a visible gap today (cut off a burning arm and the fire stops).

### 3.2 The CPU mirror silently ignores `Reaction.cond`

`debris.cpp` reads `packed`, `nbrMat`, `nbrTags`, `nbrClass`, `chance`,
`prodSelf`, `prodNbr` — and **never `cond`**. So on the CPU side, both light
gating *and* `scaleByNeighbors` are dropped: a rule authored as "only fires with
≥3 hot neighbours, at up to 4x" fires unconditionally at base chance on a body.

This is a latent divergence today (no current rule uses `cond` on a body
material) and a **blocker** tomorrow, because `scaleByNeighbors` is precisely
the mechanism the "exponential spread on a big front, self-extinguish on a lone
voxel" behaviour is supposed to be authored with. Port `scaledChance()` from
`sim_step.wgsl:315` into the CPU mirror; it is ~30 lines and shared by both the
debris and the limb pass.

### 3.3 Whole-pool re-upload

Covered in §1.3. `UploadMicroBodies` (`src/sim/simulation.cpp:570`) writes
`min(pool.size(), 1<<20)` words on any dirty. Needs to become incremental.

### 3.4 No in-place single-voxel brick write

`MicroBodyEdit` re-derives dims, rebases the origin, re-packs the whole payload
and may reallocate the block. That is right for a carve (the shape changed) and
catastrophic per tick for a state change (the shape did *not* change — one
voxel's material did).

Needs a sibling: `MicroBodyPoke(set, model, x, y, z, mat, art)` — resolve the
word, rewrite one 16-bit half, mark the word dirty. No realloc, no dims change,
no origin shift, no rig fix-up. Voxel *removal* can also go through poke (write
material 0) and defer the shrinking re-pack to a batched threshold, exactly the
way `burnedSinceRebuild` already defers the Jolt rebuild.

### 3.5 There is no per-voxel burn state, and nowhere obvious to put one

The micro brick voxel is 16 bits, fully spent: 8 bits material + 8 bits art
colour (`microbody.h:44-51`). There is no spare bit for "char progress".

**Do not add a side table.** Encode burn state as *material identity* —
`flesh -> flesh_cooked -> flesh_charred -> ash` — which is what the whole
material/reaction system is for, needs zero engine changes, renders correctly
for free (each stage authors its own colour and emission), and keeps behaviour
in JSON per DESIGN.md §6 and CLAUDE.md guideline 4. Headroom is fine: micro
bodies cap at material id 255, and the table currently holds **96**.

One concrete gotcha this creates: `microbody.wgsl:269` lets a nonzero **art
colour override the material colour**. A charred voxel that keeps its art slot
will still be painted robe-red. The state transition must zero the art slot, or
charring is invisible on exactly the painted surfaces it matters most on.

---

## 4. The performance analysis

### 4.1 Why the front, not the lattice

Fire lives on a **surface**. An interior voxel cannot ignite until something
exposes it. So the burning set of a body is a 2D front over a 3D volume, and it
is bounded by the surface area, not the voxel count. For mina's 31,456-voxel
torso the surface is order 5-6k voxels, and the *actively burning* subset at any
instant is a fraction of that — charred voxels drop out of the set, unignited
ones were never in it.

Estimated (not measured) steady state for a fully-engulfed mob: **a few hundred
to ~2000 active voxels**. Six neighbour rolls each is ~12k tests/tick per
burning mob — well under 0.2 ms. Twenty simultaneously burning mobs is ~0.5 ms
of CPU. That is affordable; a lattice scan of the same twenty mobs is 1.2M
voxels/tick and is not.

This is CLAUDE.md rule 2 stated for a new population: **a mob that is not
burning must cost exactly zero**, and a mob that is burning must cost
proportional to how much of it is on fire.

### 4.2 Iterate the world side, not the body side

For ignition and for acid, the naive query is "for each surface voxel of the
limb, what is in the world cell it occupies?" That is 5k queries against the
chunk cache.

Invert it. A mina limb's world-space AABB is order **4x4x8 = 128 world cells**.
Walk *those*, find the cells holding `tag:hot` or `acid`, and map only those
back into limb-local skin coordinates to seed the front. Two orders of magnitude
cheaper, and it degrades gracefully: a limb touching nothing interesting costs
one AABB walk over ~100 cached cells and exits.

`BurnBodies`' existing `AnyDirtyNear` chunk-dirty-flag gate
(`debris.cpp:1210`) sits in front of even that, so a mob standing in a settled
world does no work at all.

### 4.3 Neighbour lookup needs a dense index, allocated lazily

`skinVoxels` is a sparse `std::vector`. Spreading fire needs O(1) "what is at
(x±1, y, z)". Building an `unordered_map` per limb per tick is the 63k-element
cost the design is trying to avoid.

The answer: a per-limb **dense occupancy index** (`dims`-sized `uint16` array,
voxel index or sentinel), allocated the first time the limb burns or is carved
and freed when the fire goes out and the limb settles. Memory for a mina torso
is roughly 100-250 KB depending on how much of the bounding box is filled —
acceptable for the handful of limbs on fire at once, unacceptable as a
permanent per-limb allocation, which is exactly why it is lazy.

Reusing the GPU brick as the index is tempting (it *is* dense) but it is
declared render-only derived data in three places; reading it back for sim
purposes is the first step toward the "unowned diverging representation" the
architecture guidelines warn about. Keep it derived.

### 4.4 The upload budget, done properly

Per-model min/max dirty ranges are the easy fix and they have a real ceiling: a
mina torso model is 15.7k words = 63 KB, so twenty burning models with fire
spread across them is ~1.26 MB/tick — *over* budget. Honest v1, but say so.

The scalable form is a **patch list**: accumulate `(wordIndex, value)` pairs,
upload to a small scratch buffer, and scatter them into the pool with a trivial
compute dispatch. 2000 patches/tick is 16 KB. Do the ranges first if it gets the
package landed, but the patch list is where this ends up.

### 4.5 The CPU-vs-GPU fork

The endgame ("simulate every voxel") eventually wants this on the GPU — the
brick pool is already a GPU buffer, and a compute pass over burning bricks would
be nearly free.

The reason not to do it yet: the CPU **needs the results**. Voxel loss drives
the collider rebuild, the connectivity split, hp and death. GPU-side burning
therefore requires an async readback channel with one-tick latency — a real
feature, not a detail, and it must never become a synchronous readback on the
frame path (rule 3). Given that the active front is only hundreds of voxels, CPU
is the right answer until mob counts reach the low hundreds. Revisit at that
crossover, not before.

### 4.6 Where the cost actually lands, in order

Ranked by what will bite first:

1. Brick upload traffic (§3.3) — 4 MiB/tick unless fixed. **Fix before anything
   else.**
2. Jolt collider rebuilds. Burning removes voxels; `RebuildLimbBody` replaces
   the Jolt handle and rebuilds the limb's joint, its children's joints, and the
   intra-mob exclusion set. This is the most expensive single operation in the
   whole feature. Batch it as hard as `BurnBodies` already batches its own
   (`kBurnRebuildVoxels = 12`, at most one per tick globally) and consider a
   per-mob rather than per-limb cadence.
3. Connectivity re-checks (does the burnt-through leg fall off?) — O(n) flood
   over the limb. Same batching discipline; `burnedSinceShatter` already scales
   its threshold with body size and that logic transfers.
4. The front scan itself — comfortably last, which is the point of §4.1.

---

## 5. Determinism

Rule 1 governs the **grid**. Mobs and bodies are CPU float gameplay state and
are explicitly outside the hashed domain (`debris.h:22-24`). So limb burning
does not have to be bit-deterministic on its own terms.

It *does* have to not poison the grid. Burning limbs write fire into the world
through `cellOps`, and those writes are hashed. Therefore:

- Roll the RNG on `Hash3(mobSerial ^ limbIndex, tick, voxelIndex, ruleIndex)` —
  counter-based, exactly as `BurnBodies` does with `b.serial`.
- **Never key a roll on a Jolt-derived float** (limb world position, velocity,
  contact point). That injects physics float state into a grid write and makes
  the world hash frame-rate dependent. This is the one way to break rule 1 from
  here, and it looks entirely innocent at the call site.
- Run the pass once per tick from `PreTick`, never per frame.
- A mob needs a stable `serial` the way `Body` has one; `Mob::id` may serve if
  it is stable across save/load, which needs checking.

Expect the pinned world hash to move once burning mobs emit fire ops in any
gate that has a mob near flammable terrain. Plan for `--rebaseline`.

---

## 6. Roadmap

Ordered so that every package is independently landable and independently
verifiable with a single `--gate` run (CLAUDE.md "authoring cheap-to-verify
work").

### Package A — prerequisites (no behaviour change, no hash change)

The plumbing, landed on its own so the interesting package is not carrying it.

1. `MicroBodyPoke` — in-place single-voxel material/art write (§3.4).
2. Incremental brick upload — dirty word ranges per model, `UploadMicroBodies`
   writes only those (§3.3, §4.4). Patch-list scatter deferred.
3. Port `scaledChance()` + the `cond` light gate into the CPU reaction mirror
   and share it between `BurnBodies` and the future limb pass (§3.2).

Verify: existing `debris` gate stays green; no world hash movement. If the hash
moves here, item 3 changed a rule's behaviour and that is worth knowing before
it is buried under package B.

### Package B — burning limbs

1. `MobSystem::BurnLimbs`, called from `PreTick` alongside the existing bleed
   pass. Per-limb active front (`std::vector<uint32_t>` of skin voxel indices),
   lazily-allocated dense occupancy index, world-side AABB scan for ignition
   sources (§4.2), shared per-tick scan and op budgets mirroring
   `kBurnScanPerTick` / `kBurnOpsPerTick`.
2. Materials + reactions in JSON: `flesh` (rename/alias `skin`), `flesh_cooked`,
   `flesh_charred`, plus `cloth_burning`, `hair_burning`. Authored chances make
   cloth ignite ~10-20x more readily than flesh; flesh ignition uses
   `scaleByNeighbors` with `minCount: 2..3` so a lone hot voxel dies out.
3. Emission into the grid: burning limb voxels emit real `fire` via
   `kCellOpIfAir` cell ops, same as `BurnBodies`. This is the "runs through a
   bush and lights it" half, and it needs no new mechanism.
4. Batched collider rebuild + connectivity check reusing the carve path's
   existing thresholds.
5. Enable micro bodies in `BurnBodies` so severed burning limbs keep burning
   (§3.1), sharing the front/index machinery.

Verify: a new `mob-burn` gate. Assertions to write — a cloth-clad limb held in
fire loses voxels and emits fire ops; a bare flesh limb given exactly one hot
neighbour self-extinguishes (this is the interesting one — it is the assertion
that `minCount` reached the CPU mirror); a burning limb adjacent to `leaves`
ignites them in the grid; an idle mob in a settled world does zero burn work
(assert the front is empty and the pass early-outs).

### Package C — acid and dissolution

Mechanically the same pass with a different source. `skin`, `robe_cloth`,
`leather` are already `dissolvable`, and `acid + tag:dissolvable -> air` is
already an authored rule — so acid dissolving a limb is mostly *reusing* B's
front machinery with the world-side scan looking for liquid instead of heat.

The one genuinely new piece is **submersion**: a limb standing in acid needs the
face layer of skin voxels adjacent to each acid world cell, not the whole
surface. At skinScale 8 one world cell maps to 512 skin voxels, of which one
64-voxel face is exposed. Get this wrong and a leg in a puddle dissolves 8x too
fast, or the whole leg dissolves at once.

"Loses a leg but does not die" is already the existing behaviour: `CarveLimb`
severs on disconnection or under `kLimbCollapseFraction`, and only a `vital`
limb kills (`MobLimbDef::vital`, `mob.h:35`).

### Package D — armour and clothing

**LANDED 2026-08-29.** Built as `docs/PLAN_items_equipment.md`, which supersedes
this section; DESIGN.md §8c is the architecture of record. The prediction below
held: a shell is a rig part, so per-voxel burning and dissolving arrived with no
armour-specific fire code, and `--gate armor-react` measures 114 skin voxels
lost bare against 0 under a steel plate purely from `steel` not carrying
`tag:dissolvable`.

One thing this section did NOT anticipate, and it is the interesting part. "The
world reaches the body" is a question the burn pass asks about the WORLD, and a
shell is in neither the grid nor the body's lattice — so fire lapping at a
sleeve read to the arm underneath exactly as fire lapping at the arm. Occlusion
(`Mob::WornAlong`) is the one genuinely new mechanic armour needed, it has to be
asked along a segment rather than at a point, and it only works for limbs
thicker than a grid cell. See §8c.

**Do this last. It is genuinely modular and it genuinely benefits from being
second.**

The reason is `item.h`'s "borrowed slot" model: a worn item **is a rig part**
while worn, and therefore already inherits severing, dropping to debris,
per-voxel carving, and micro detail. If burning is implemented as *"limb parts
burn per voxel"*, then armour burns per voxel on the day it exists, with no
armour-specific fire code. If armour is built first, package B has to be written
against two populations instead of one and gets no additional information from
it.

Two mechanics that then fall out for free rather than being features:

- **Ripping off a burning cloak** is `DetachPart` → `AdoptBody`, and the cloak
  keeps burning as debris because `BurnBodies` burns it. Already true today for
  the sword.
- **Full plate wading through acid** is `steel` not carrying the `dissolvable`
  tag, plus geometric coverage — the acid front simply finds no dissolvable
  surface voxel to attack. There is no "immunity" flag to invent; it is an
  emergent consequence of the material table and what is physically in the way.

The one thing armour genuinely adds and burning cannot fake: **layering**. Cloth
over flesh means the fire has to eat the cloth before the flesh is exposed, and
that is a coverage/adjacency question the front pass answers naturally *if the
two layers are adjacent voxels*. Which they are, if armour is authored as
geometry occupying the voxels just outside the limb. Worth confirming that the
rig can express "this part sits over that part" spatially rather than as a
parent/child relationship.

---

## 7. Suggested interactions

Things that follow from the above at near-zero marginal cost, roughly in order
of payoff-per-line:

- **Water and rain douse a burning mob.** `water` is already `tag:extinguisher`
  and `fire + tag:extinguisher -> smoke` is already a rule. A burning mob
  jumping in a pond should already work once the front reads liquid neighbours
  from the world-side scan. Test it; it is free.
- **Wet reduces flammability.** The stain system already carries a per-voxel
  amount (`kStain*`, 0..15) and `water` already `WASHES`. A skin voxel with a
  water stain resisting ignition is a `scaleByNeighbors`-adjacent authored rule,
  not new machinery.
- **Blood is flammable-adjacent, and oil is not.** A mob soaked in `oil`
  (`chance: 350` to flash to fire, vs wood's 18) becomes a walking bomb. This is
  already authorable the moment oil can stain a limb.
- **Cooked flesh as a state that is not just damage.** `flesh_cooked` is a
  *different material* — it can have different hardness, different bleed
  behaviour, and be a distinct visual. A creature can be seared without being
  destroyed, which is the readability win DESIGN.md §7 argues for with wounds.
- **Charred voxels are structurally weak.** Give `flesh_charred` and
  `cloth_charred` low `hardness`, so a burnt limb shatters under a blow that a
  healthy one shrugs off. Emergent, from data.
- **Smoke inhalation / hot air near the head.** Cheap: the head limb's world
  AABB already gets scanned; count `smoke`/`fire` cells and apply hp. Gives fire
  a threat model that is not "wait for your voxels to burn away".
- **Hair burns first and fast.** `hair_white` at `hardness: 3` with a very high
  ignition chance and full consumption to `air` — the visual read of "that
  caught, briefly and violently" for almost no cost.
- **Lava contact = instant ignition + melt.** `lava` is `tag:hot`; the melt path
  (`MaterialDef::molten`) already exists for the laser and would apply to body
  voxels through the same product mechanism.
- **Fire lights a dropped item.** Free once §3.1 lands — a dropped wooden staff
  is a debris body with a micro brick.
- **Standing in fire heats metal armour.** `steel` does not burn, but a
  `steel -> steel_hot` state that then cooks the flesh *inside* it is a
  wonderfully nasty interaction and costs one material plus two rules. This is
  the kind of thing the layering in package D unlocks.

---

## 8. The ordering question, answered directly

**Burning first, armour second.** Not because armour is unimportant, but
because:

- Armour's attachment model (`item.h`, borrowed rig slots) already exists and is
  proven by the sword. It is not a research risk.
- Per-voxel body reactivity *is* a research risk — specifically §3.3, §4.3 and
  §4.6.2 — and it is a risk that armour does nothing to reduce.
- Every piece of burning written against limbs applies to armour unchanged,
  because armour is a limb. The converse is not true.
- The one thing armour adds to burning — layering — is a coverage question, and
  it is cheaper to answer once the front pass exists and can be observed than to
  design for in the abstract.

The exception: if the immediate goal is the *feel* of "set the enemy's cloak
alight" rather than the system, the wizard already wears `robe_cloth` as limb
geometry. Package B alone gets that, with no armour system at all.

---

## 9. Risks, in the order they will actually hurt

1. **Whole-pool upload** (§3.3). Will not show up in a headless selftest. Will
   show up as a frame-rate cliff the moment two mobs burn on screen. Fix in
   package A, before writing any burning.
2. **Collider rebuild churn** (§4.6.2). A limb losing voxels steadily wants a
   rebuild steadily; each one replaces the Jolt handle and rebuilds joints.
   Under-batch it and burning mobs will be the most expensive thing in the game.
3. **Float state leaking into grid writes** (§5). Silent until a determinism
   gate fails somewhere unrelated. Mitigate by construction: the RNG key must
   contain no float-derived term.
4. **`cond` divergence** (§3.2). Rules will behave differently on bodies than in
   the grid, and the symptom is "the tuning I authored does nothing" rather than
   a crash.
5. **The art-colour override** (§3.5). Charring will appear to do nothing on
   painted surfaces. One line, but a confusing hour if it is not written down —
   which is why it is.
6. **Acid submersion granularity** (package C). 512 skin voxels per world cell
   at skinScale 8; attacking the volume instead of the exposed face is an 8x
   error that will read as "acid is absurdly strong".
7. **Selftest gate ordering.** Gates share one `World` and depend on prior state
   (CLAUDE.md rule 7, `kOrder` in `src/test/selftest.cpp`). A new `mob-burn`
   gate that emits fire into the world must be placed where it does not poison
   later gates, and must be measured at the same scope in both arms of any A/B.

---

## 10. Landed after the fact: the combustion clock, and what §3.5 actually cost
*(2026-08-30, on owner report that a burning character went out too quickly to
spread the fire or take much damage from it.)*

**Burn duration is now one slider, `combustion.burnDurationPct`, default 200.**
It is read by the reaction COMPILER, not by a kernel — `LoadAssets` divides the
chance of every rule marked `"burnDuration": true` in `reactions.json` by it, so
the scaled value is rounded into the same integer an authored chance compiles to
and the GPU cannot tell the two apart. Nothing reaches a shader, there is no
`TUNE_*` constant, and the world costs the same to run at any setting. Tuner tab
"Combustion", next to Weather, which is the other tab the reaction compiler owns.

Two classes of rule carry the mark, and the second is the one worth writing
down:

* the rules that **retire** a burning voxel (ember → ash/smoke/air; burning
  cloth, undercloth, hair and flesh → their charred or spent forms). Halving
  these is the feature.
* the **relight** rules (charred → burning). These are the only rules in the
  file that put matter back into a burning state, so they are the only closed
  loop in the fire economy, and its gain is (relight chance) × (how long a
  neighbour stays lit). Scaling only the retire side multiplies that gain by the
  same factor. **This is a ceiling argument, not a fix for anything observed at
  the default** — measured at 200%, scaling the relight rules moved the
  `mob-burn` corpse from 13 voxels still alight to 15 — but at the slider's 800%
  the unscaled gain would cross 1.0 and a fire would never go out (rule 2).

Ignition, emit and extinguisher rules are deliberately NOT scaled. Ignition is
how fast fire spreads and is a separate lever the file already argues about at
length; leaving it alone is also what makes a longer burn spread *further*,
since a voxel that stays lit twice as long emits its upward flame twice as many
times. Emit is left alone so a longer burn is a brighter one rather than a
dimmer one spread thin. Dousing must beat the burn to the tick at any setting.

### §3.5 was right about the mechanism and wrong about the symptom

Risk 5 predicted "charring will appear to do nothing on painted surfaces". What
actually shipped is the opposite and worse: every reaction that rewrites a body
voxel **clears** its art colour (`BurnLimbView::Set`), deliberately, because the
burn chain's whole visual account of itself is the material it turns into. So
charring is perfectly visible — and the PAINT is what is lost.

That is invisible on a creature painted in shades of its own flesh, and it is
severe on one where the paint is carrying an identity the material is not. The
base `human` model wore its linen as art colour over `skin` voxels: a lick of
flame cooked them to `flesh_cooked`, the paint went, and **the player read as
naked rather than as scorched**.

The fix is where the rest of the burn model lives — in the material table.
`undercloth` / `undercloth_burning` / `undercloth_charred` is a near-copy of the
cloth chain with the becomes-air branch deleted, so the linen burns for exactly
as long as a robe does and responds to the slider identically, but cannot
disappear. `undercloth_charred` authors no rules at all: it is the terminus, so
a torched character keeps a blackened garment and the chunk it sits in sleeps.
The 3,712 painted voxels in `assets/mobs/human.vox` (hips and upper thighs) were
repainted to the new id; the art layer is untouched, so an unburnt character
looks exactly as it did.

**The general rule this establishes: if paint is the only thing distinguishing
two parts of a body, fire will erase the distinction. Anything that must survive
a burn has to be a material.**

### Deep char

`flesh_cinder` is one stage past `flesh_charred` — near-black, crisp, and inert
— reached from charred under a three-face front, so one pass of flame leaves a
body dark brown and a sustained torching leaves it black. It is a state rather
than a palette jitter because it has different rules: **none**. That makes the
char chain CONVERGE under fire instead of cycling charred → burning → charred,
which is strictly better for rule 2 than the relight rule alone — the terminus
is an inert material rather than a coin flip that never ends.

### What asserts all of this, and for how much

Three subtests were added to `--gate mob-burn`, and all three are **table-only**:
no fixture, no ticks, no GPU, microseconds each.

* `burn duration knob` compiles `materials.json` + `reactions.json` twice, at
  100% and 200%, and asserts that every rule whose chance moved moved by exactly
  the factor and that nothing but the chance moved. The second half is the one
  worth having: it is what would catch the knob quietly rescaling an ignition or
  an emit rule, which would look like a working feature and play like a
  different game. (`--sweep` cannot reach this knob — it only reloads shaders.)
* `linen chars, never bares` walks the reachability closure from `undercloth`
  and asserts no rule in it produces air, that the closure stays inside the
  three linen materials, and that the terminus is inert. Stated as reachability
  rather than as "undercloth_burning has no air branch" because the hole could
  be opened by any of the three, or by a fourth added between them later.
* `deep char` asserts charred → cinder is authored and that cinder is inert.

### Two things the gate itself had to learn

* **A quiet window is a DURATION and must scale with the clock.** Subtest H gave
  a corpse 630 ticks to go out. That number was chosen against the chances
  `reactions.json` authors, which is exactly what this slider multiplies — so
  the literal was a deadline that silently tightened every time the slider went
  up, and at 200% it failed with 13 voxels still alight. "The fire takes twice
  as long to go out and you gave it the same wall clock" is the feature working.
  It is now `630 * burnDurationPct / 100`.
* **A bare count bought one hypothesis; attribution bought all of them**
  (CLAUDE.md rule 6, again). "13 still alight" is consistent with a long tail
  and with a relight loop stuck at gain 1.0, and those are completely different
  bugs. Splitting the count by material and re-probing after a further half
  window printed the whole answer on one line — `15 cloth + 0 flesh; 0 after 315
  further quiet ticks (still falling)` — in the run that would otherwise have
  been the first of a series of eliminations.
