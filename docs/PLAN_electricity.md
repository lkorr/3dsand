# Electricity — plan (2026-10-03)

Owner ask: shooting electricity at something arcs and runs through it if it
conducts. Copper carries it very far, water fast. Wet raises susceptibility.
Wood burns or ignites when electrified. A spark is a tiny low-reach shock;
lightning is huge. Androids spark when hit instead of leaking coolant. A
shocked pool shocks the mobs in it. Electrolysis (molten salt -> sodium +
chlorine) happens in the world as on the bench.

## STATUS (2026-10-03, endgame): COMPLETE

Every package has landed on main; the endgame closed the decay and the
player gate.

| Package | What | Commits |
|---|---|---|
| E5a | Androids spark, no fluid; Electrify reaches powder and gas on the bench | 3eb7e51 |
| E3 | `arc` / `lightning`, the spark / shock / lightning glyphs, the strike path, storm ground strikes, gate `elec-strike` | 6c55d94 (merge e9ddcfa) |
| E1 | The charge field: `sim_elec.wgsl`, `src/sim/elec.*`, materials `electric`, gate `elec-field` | 859e40e, 8143b2d |
| E5b | The charge glow in the raymarch, the F11 charge view | 2357be5 |
| E4 | Shocks reach bodies: elecQuery, `DamageCause::Electric`, stun / twitch / knock-down / ignite, gates `elec-water-mob`, `elec-stun`, `elec-replay` | f376cd6 |
| E2 | What charge does in the CA: charge as partner, ohmic ignition / char, crackle, gates `elec-electrolysis`, `elec-ignite`, `elec-crackle-bounded` | ecbfdd6 (main at e772227) |
| Endgame | Proportional decay (`sim.elecDecayShift`), gate `elec-player-stun`, the ops-replay reporter fix | branch `worktree-agent-a73d261dd6db03139` |
| Wave 2 B | One query box per BODY with a P/air grid, sized from the creature cap (bug 2: ~8 bodies); bodies CONDUCT by their material (world-pitch body cells, the field's max-plus and wet rule, `electric.shock` felt, ohmic sear / char and crackle per body cell, worn shells cover, bodies conduct into bodies); body-material `electric` blocks; gates `elec-crowd`, `elec-body-matter` (docs/PLAN_electricity_wave2.md package B) | branch `worktree-agent-ad10d2716979e16d7` |
| Wave 2 E | Gameplay (docs/PLAN_electricity_wave2.md E): strikes scan from the real top (aim + 272) and skip columns entered from inside; spell strikes seek past the mirror after an 8-tick stepped leader; wards re-checked at the struck cell; strength scales the bolt; a budget-refused strike refunds its tariff; storms roll per player id; peer bolts flash + clap; stun refuses Q/E/G (talk kept); `Cues::Zap` / `Cues::Shock` + PLACEHOLDER takes (`scripts/gen_elec_sounds.py`, incl. weather/thunder); pixel STUNNED cue; gate `elec-strike-play` | worktree branch (package E) |
| Wave 2 A | The charge field (docs/PLAN_electricity_wave2.md package A): the spreading loss (`sim.elecSpreadLoss` / `elecSpreadFree`), gate `elec-bulk`; 12-bit resist, dry wood 1,500; the owner check (phantom charge); the `C_ELEC` fast path; the per-tick cell cache; parallel re-key; brine via the solute layer (`electric.dissolved`); sodium / acid / lava / molten glass / shore mud blocks; elec-strike's fade / sleep / two collar checks; observed keys seeded | branch `worktree-agent-a2ef8b498c9bb613f` |

**Decay (endgame).** A stored P now loses `max(sim.elecDecay, P >>
sim.elecDecayShift)` a tick (8 and 3), taken off the whole stored field --
own cells and halo alike -- in round 0. With no source: lightning 30,000 is
gone in 55 ticks (under `gore.shockMinP` 60 after 47), an arc's 2,000 in 35,
a spark's 200 in 17. It was a flat 8: 3,750 / 250 / 25. A held source loses
nothing within `sim.elecRounds` chunks and 7/8 per `elecRounds` chunks
beyond that (a held arc still holds ~450 at the far end of a 512-cell copper
wire). Why it is not applied to the self term only: DESIGN.md "Electricity
-- charge field", "Decay is PROPORTIONAL above a floor".

**Gates (endgame `--verify`, all PASS):** elec-field (fade 19 ticks, was
~25), elec-electrolysis, elec-ignite, elec-crackle-bounded (now at the game's
own decay: every page back 55 ticks after the source; `elecCrackle.decay` 0),
elec-water-mob, elec-stun, elec-player-stun (new), elec-replay, elec-strike,
android-sparks, ops-replay (after the reporter fix).

**Wave 2, package A (2026-10-04).** Bulk conductors no longer carry a strike
across the window: a cell loses (n - 2) x 150/4096 of what it receives per
conducting neighbour past two (a wire is free). A forced strike into a sea
basin charges 33 cells out (P >= 20 to 31) over 20 pages, onto rain-wet ground
25 / 15 pages; the same gate with the loss off fills both fixtures (89 / 83
pages, 66 / 59). Dry wood is 1,500 (12-bit resist): a spark cannot enter it,
lightning ~20 cells of a 1-wide beam. Readers check the page's owner (no
phantom charge after a window shift). The elec rows record only while
`World::ElecMayBeLive` (a source op, pages in the snapshot, or a doorbell
ring): ~61 us/frame of GPU off every CA-active tick of the `explosion` perf
scenario, which has no charge. Rounds after a chunk's first in a tick read a
per-page cell cache. Brine conducts (salt `electric.dissolved` 2). The world
hash did not move (determinism 0fa43063).

**Open:**
- Wet wood is still a WIRE: a full water coat makes any plank 12, so a
  rain-soaked 1-wide beam carries lightning as far as water would in a line
  (the spreading loss only bites in sheets and bulk). Physically a film on a
  beam is thin and resistive; a fix is a per-material floor on the wet rule
  (owner decision).
- The glow is emission only (lights no neighbour) and does not reach plants,
  raster bodies or the far cascade.
- The strike's target scan starts 24 cells over the aim, so a tree taller
  than that is struck inside its canopy; no thunder sample is recorded.
- `ops-replay`'s ticket arm no longer reaches its release: the gate's poured
  sand pile (1,024 cells, ticket chunk lo+(1,2,2)) still has grains moving
  at tick 200 (MOVE+powder every snapshot; 3 sand words change in the last
  tick), so the ticket never idles out and the record holds one decision
  (the activation) where it held two. Not electricity: every charge path in
  sim_step skips ticket slots (`gInTicket`) and no electricity commit touches
  powder motion. It arrived between 716e8ba (02:31, release seen) and 3eb7e51
  (the first electricity commit); the leading candidate is 1d5f52a ("Worldgen
  lays each powder at its own repose; author sand 34"), the others in range
  that touch the CA are 8414b13 (per-colour work lists, sparse cell pools)
  and c2be48c (camask publishes the chunk's own repose snapshot).

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

**Status (2026-10-03, worktree branch):** built. DESIGN.md "Electricity --
shocks reach bodies" is the as-built account; `game/mob_shock.cpp`,
`sim_elec.wgsl` elecQuery, `World::QueueElecQuery` / `TakeElecHits` /
`ElecMayBeLive`, knobs `gore.shock*`, gates `elec-water-mob`, `elec-stun`,
`elec-replay`. Differences from the sketch below: the answers ride the
SNAPSHOT RING (K = World::kSnapshotLatency, consumed at T + K + 1), not a
one-tick readback; the stun is `Mob::stunUntil_` (a tick); the ragdoll knob
is `gore.shockRagdollP`; corpses do not ask. (Was open: `sim.elecDecay` was
linear, so residual charge stayed lethal for a long time -- closed by the
endgame's proportional decay, see STATUS.)

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
