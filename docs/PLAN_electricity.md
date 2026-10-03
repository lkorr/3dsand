# Electricity — plan (2026-10-03)

Owner ask: shooting electricity at something arcs and runs through it if it
conducts. Copper carries it very far, water fast. Wet raises susceptibility.
Wood burns or ignites when electrified. A spark is a tiny low-reach shock;
lightning is huge. Androids spark when hit instead of leaking coolant. A
shocked pool shocks the mobs in it. Electrolysis (molten salt -> sodium +
chlorine) happens in the world as on the bench.

## Owner decisions (2026-10-03)

- **Speed: a visible fast pulse,** ~50-100 cells/tick (several chunk-hops
  per tick). Not instant-in-one-tick.
- **Mob effect: a stun timer plus a twitch.** Lightning-class charge
  escalates to ragdoll.
- **Weather lightning strikes the ground in the sim, near the player only.**
  Deterministic hash-scheduled strikes in the window during storms, biased to
  tall and conductive targets, with a thunder cue. Far flashes stay
  render-only.
- **Androids: sparks only, no fluid.** Coolant stays a material but nothing
  bleeds it.

## What exists (survey 2026-10-03)

- `spark` (`materials.json:3731`) is a gas tagged `electric` and decays in
  ~2 ticks.
- Electric rules all match `neighbor: "tag:electric"` (`reactions.json`):
  - hydrogen pop (1554)
  - molten_salt -> sodium + chlorine (1601)
  - brine -> lye / hydrogen + chlorine (1799/1808, solute `salt` cMin 24)
  - thermite (2094)
  - ether_vapour (2434)
  - spark + flammable -> fire (1629)
- The `shock` effect kind lays a spark ball (`session.cpp:4376`); no rule
  authors it yet.
- Androids: `struck` sparks on every hit (`mob.cpp:11777`). Coolant bleed is
  separate (`:11828`). The power_cell breach/death `burst` lays jagged
  spark-ops arcs (`session.cpp:4260`).
- **Missing:**
  - Conductivity: water's `conductive` tag is read by nothing.
  - Any propagation.
  - Sim-side lightning: `weather.cpp:587` is lighting only.
  - An electric spell: the `spark` glyph places `fire`.
  - `DamageCause::Electric`.
  - A mob stun/status timer.
  - Mob submersion/contact state: mobs read the CPU chunk cache only.
- **Known bug this fixes:** a rising spark skips pool tops at y = 2 (mod 3),
  due to CA phase order (`selftest_chem.cpp:344`, `DESIGN.md:4394`).

## Design

### 1. Charge field (package E1)

- **Storage.** A transient per-cell u16 potential `P` in a sparse per-chunk
  page pool, shaped like the solute layer.
  - Table: `elecTable[slot]` = EMPTY | page; a page is 4096 x u16 = 2048
    words.
  - Pages exist only where `P > 0`. They are not hashed and not saved; a
    load zeroes them.
- **Material data:**
  `materials.json "electric": { "resist": N, "source": E, "ignite": {...}, "char": "<mat>" }`.
  - `resist` is the cost per cell entered.
  - No block means an insulator.
  - Packed into `_r2` bits 25..31 (resist class index), plus a per-material
    params block like `heatParams` `kHpMat` for the rest.
- **Wetness:** the effective resist of a water-stained cell is
  `min(resist, wetResist[stainAmount])`.
- **Brine:** dissolved salt lowers water's resist via the solute layer
  (optional, E2).
- **Starting numbers (tune in data):**

  | Material | resist |
  |---|---|
  | copper | 1 |
  | silver / gold | 1 |
  | iron / steel / brass | 3 |
  | brine | 2 |
  | water | 6 |
  | molten_salt | 4 |
  | wet soil | 30 |
  | wood | 60 |
  | flesh | 20 |
  | air / stone / glass | none |

  Fix the `metal` tag omissions (iron, steel, gold, brass) while there.
- **Update, per tick, post-CA kernel `sim_elec.wgsl`:**
  `P' = max(seed, max_nb6(P_nb - resist(cell)), P - decay)`, clamped at 0.
  - Max-plus with Jacobi reads makes the result order-independent, so it is
    deterministic.
  - Each listed chunk iterates to its fixpoint in workgroup shared memory,
    with the halo taken from the previous round.
  - `sim.elecRounds` (default 4-6) rounds per tick give the chunk-hop speed.
  - Indirect dispatch over an active-chunk list: chunks with `P > 0` plus
    neighbours of boundary-changed chunks. Same pattern as heat's
    want/alloc/list chain.
- **Seeds:** cells whose material has `electric.source`. The seed is
  `source` energy, or the state nibble scaled for variable-strength arcs.
- **Bounds:**
  - Reach is at most energy / min resist.
  - Pool exhaustion is a counted deterministic refusal (heat-style
    prefix-sum alloc), never an abort.
  - A page frees when all-zero.
- **Knobs** (`tuning_params.def`, NO_WGSL where possible):
  `sim.elecRounds`, `sim.elecDecay`, `sim.elecWetResist`,
  `sim.elecIgniteGain`, `sim.elecCrackle`.

### 2. CA effects (package E2)

Run after the reaction bucket, like `heatReact`, gated on `P > 0` at the
cell.

- **Virtual electric neighbour.** A charged cell satisfies a rule whose
  `nbrTags` include `electric` without an actual spark face.
  - `prodNbr` deposits into an air face if there is one, else it is dropped
    (as the bench's `Deposit`).
  - All six authored electrolysis/ignition rules then work in charged pools
    with no JSON edits.
  - This fixes the y mod 3 bug for them.
- **Ohmic ignition / char.** Chance is about `P * resist * elecIgniteGain`,
  a frontier-face scaled roll (heatReact pattern) into `ignite.into` /
  `char`.
  - Copper (resist 1) never burns.
  - Wood (60) burns quickly.
- **Crackle.** A charged conductor with an air face emits `spark` into it at
  a low chance scaled by `P`. It is visible, lights adjacent flammables
  through the existing spark rule, and is self-limiting: the spark's own
  seed is small.
- **Sleep.** A chunk with `P > 0` stays dirty. When `P` decays to 0 the
  pages free and the chunk settles.

### 3. Sources and spells (package E3)

**Status (2026-10-03, worktree branch):** built, data-only to the field. The
materials, glyphs, the strike function (`game/lightning.h`:
`LightningStrike` / `PlanStrike` / `EmitStrike`, `StrikeSpecFromGlyph`,
`WeatherStrikeSpec`), storm ground strikes (`session.cpp` WeatherStrikes,
knobs `weather.strikeRate` / `strikeRadius`, `SANDVOX_STRIKE_EVERY`), the near
flash and the thunder slot, and gate `elec-strike`. Iron, steel, gold and
brass count as conductors BY NAME in `StrikeMats` until E1 fixes their tags.
At merge with E1: add the `electric` blocks to `spark` / `arc` / `lightning`;
nothing in the strike path changes. DESIGN.md "Lightning: arcs, bolts and the
strike path" is the as-built account.

- `spark` gets a small `source` (~200).
- New materials:
  - `arc`: an air-class glowing transient, `source` mid, decays 1 tick.
  - `lightning`: `source` ~30000, decays 1-2 ticks.
- Glyphs (`assets/spells/glyphs.json`):
  - `spark` -> place `spark` (not fire).
  - New `shock`: an `arc` at impact.
  - New `lightning`: a jagged column of `lightning` CellOps from a ceiling
    above the target to the first solid/conductor. Reuse
    `ReactFxAftermath`'s walk with a downward bias toward the highest
    conductive cell in a small radius.
- Everything goes through existing CellOps, so the op record, replay and net
  need no change.
- **Weather strikes:** hash(seed, tick) scheduled while `lightning > 0`, in
  the sim window only.
  - Target column: the highest conductive/tall cell in a hashed disc.
  - Same strike path as the spell.
  - Pairs with the existing render flash (`flashX/Z`) and adds a thunder cue
    slot (`sound_schema.js` + `Cues::kSlotPrefix`).

### 4. Mobs and player (package E4)

- **Query kernel.** The CPU uploads up to N limb boxes for the
  nearby/visible bodies; the GPU writes max `P` and the conductive-contact
  count per box. The result is read back async and is one tick latent, the
  same latency class as chunk-cache fetches. Under ops-replay the readback
  must be fixed-latency; verify in the `ops-replay` gate.
- **Damage:** `DamageCause::Electric` (`src/phys/damagecause.h`). Damage is
  about `P * (1 + wetGain * waterCoatFrac(limb))`.
  - Metal armour conducts: it lowers the threshold.
  - Chance to ignite hair/cloth: flammable body voxels get the ohmic roll on
    the CPU, mirroring E2.
- **Stun:** `Mob::stunUntil_`, honoured by `DecideIntent` and stroke code,
  plus a `SelfView.shocked` fact and a twitch through `HitReact`. Above
  `sim.elecRagdollP` it calls `StartRagdoll(t, "shock")`. The player gets
  the same.
- A pool shock reaches mobs automatically: their limb boxes overlap charged
  water.

### 5. Androids, bench, render (package E5)

- **Androids (data only):**
  - Robot materials get `bleedFluid: false`.
  - `android.js` `BLEED` changes to none.
  - Raise `struck` chances.
  - Regenerate the android mobs.
  - Their sparks are now real small shocks.
- **Bench:** `GatherPixelNbrs` should also offer the spark neighbour, so
  powders and gases react to Electrify as in the world.
- **Render:** the raymarch reads `P` for an emissive blue-white flicker on
  charged cells, and the debug overlay gets a charge view. It is
  render-only, so it moves no hash.

## Gates

| Gate | What it checks |
|---|---|
| `elec-copper` | A spark at one end of a 150-cell copper wire reaches the far end within K ticks. Nothing chars. Wood at the end of a copper run does char. |
| `elec-water-mob` | A lightning cell over a pond; a mob standing in it takes Electric damage and is stunned. A wet-coated twin takes more. |
| `elec-electrolysis` | An `arc` into a molten salt pool in the world yields sodium + chlorine at every pool-top parity (the y mod 3 regression). |
| `elec-idle` | After the strike, pages return to 0 and active chunks are 32 or fewer. |
| `elec-replay` | Strike plus mob shock under `SANDVOX_RECORD_OPS`, then a replay hash. |

Expected hash moves:

- E1: none until E2 lands; the field is unhashed.
- E2, E3, E5 data: move it. Rebaseline once per landing.
