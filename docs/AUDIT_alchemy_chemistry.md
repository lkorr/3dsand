# Audit: the alchemy chemistry overhaul (9812ed8..713dacf)

2026-09-27. A senior-review pass over packages A-I of
`docs/PLAN_alchemy_chemistry.md` (world chemistry, solute layer, bench chemistry,
healing, the alchemist's shelf, the solute seam and look), followed by fixes.
Each finding has a severity, evidence, and what was done. Line numbers are at
713dacf unless noted.

Verification of the fixes: one `--verify` boot of 40 gates (determinism with
`SANDVOX_DET_PROBE=30,60,85,120,200`, ops-replay, every `alchemy-*`, `chem-*`,
`solute-*`, `heal-*` and `vessel*` gate): all PASS, 0 page faults, the twice-run
determinism probes agree at every tick, and the pinned determinism hash did not
move (no rebaseline). `--shot-bench` (stopper + acid) was looked at before and
after the stick fix.

Severity: HIGH = wrong outcome a player can hit, or a determinism / conservation
hole; MED = wrong in a narrower case or a promised guarantee not kept; LOW =
latent, cosmetic, or diagnostics.

## Bench and vessels

| # | Sev | Finding | Evidence | Status |
|---|---|---|---|---|
| B1 | HIGH | An `explode` EJECT pushed its blast and puff into the world BEFORE `finishBench` ran; when the session was then voided (ledger failed, or a vessel moved), both flasks went back into the kit unbroken, still holding sodium and water -- the reaction exploded and could explode again. The ledger-failure void path also never took what had already streamed/vented into the world out of the vessels (duplication). | `main.cpp` bench events block (push to `pendingBlasts` then `finishBench(true, ...)`); `finishBench`'s `ValidateBench` early return | FIXED: `finishBench` returns whether it wrote back; the blast and puff are applied only then. Both void paths deduct the streamed matter (`takeStreamed`). |
| B2 | MED | Only the first effect of a rule reached the bench (`effects.front()`); merged events kept the first firing's radius/power. | `flasksim_mats.h` BuildBenchChemistry; `flaskchem.cpp` RaiseEvent | FIXED: `ChemRule::fx/fxCount` carry every effect, `RaiseEvents` raises each; a merged event takes the max radius/power. |
| B3 | MED (suspected) | A reaction's product placed by `AddGasAt` (3 px ring) / `PlaceGrain` (4 px ring) only refused glass pixels, so from a full vessel a product could land across the ~3.5 px wall (outside, or in the other flask), including through a stopper's side. | `flaskchem.cpp` AddGasAt, Deposit, FlushPools; `flasksim.cpp` PlaceGrain | FIXED: a `within` vessel argument; products stay on their own side of the glass, else they wait in the vessel's pool (conserved). |
| B4 | MED | Pocket chemistry asked EVERY pair in the flask on ANY deposit: a flask that left the bench with unmet sodium over water exploded on a scoop of plain sand. It ignored light/sky/rain gates and solute conditions, and removed a held vessel by editing the stack instead of `Mob::KitTake`. | `container.cpp` ContainerPocketExplosion; `session.cpp` PhaseG pockets | FIXED: only pairs including the arriving material; gated and solute-conditioned rules skipped (a pocket is dark); hand slots go through `KitTake`. Determinism was already fine. |
| B5 | LOW-MED | The stirring stick passed straight through a stopper into the contents (the capsule pushes particles, never tests glass). | `alchemy_bench.cpp` Tick, the stick block; seen in `--shot-bench` with `SANDVOX_BENCH_STOPPER=1` | FIXED: over a stoppered vessel the stick rests upright on the cork. Checked in the picture. |
| B6 | LOW-MED | Dissolved matter carried by a particle that falls off the table enters the world as its POWDER, bypassing the 2.5 in-solution seam. | `flasksim.cpp` ~870, `DrainSpilled` | WON'T FIX here: conserved (the salt lands as grains and re-dissolves in water). A proper fix needs a dissolved spill channel through `DrainSpilled`, the streamed ledger and the void path's `Take` (which matches base materials). Listed under "Not yet" in DESIGN.md. |
| B7 | LOW | The dissolved-share grain fallback in `ContainerPour` passed an unlimited particle room. | `container.cpp` ~433 (`0xFFFFFFFFu`) | FIXED: charged against what the pour left of its room. |
| B8 | LOW | A dissolved portion can be left with no liquid (largest-remainder split, spend order) and then cannot be poured. | `container.cpp` ContainerTopMat skips dissolved | WON'T FIX: recoverable (add water and it rides the new liquid); normalising would need a pass after every Composition mutation. |
| B9 | LOW (latent) | A `yieldPerVoxel` not a multiple of 8 loses `yield % 8` units per voxel per vessel/world round trip; above 2040 nothing fits a cell. | `container.cpp` y8, `solutes.cpp` | FIXED: `LoadSolutes` refuses such a yield, and more species than the scoop ledger has counters (`kSolScoopSpecies`). |
| B10 | LOW | An event raised in the frame the bench closed was dropped; `pendingBlasts` refused by `kMaxExplosionsPerTick` were cleared anyway. | `main.cpp` (events after close); `session.cpp` PhaseK | FIXED: events handled before the close; refused blasts wait a tick. |
| B11 | LOW | Readouts: a focus with no devices kept the last vessel's stopper/flame/pressure; a stoppered flask's gas counted as fill (over-full bar); the pressure bar hardcodes 0.6. | `main.cpp` focus block; `inventory_ui.cpp` BenchParts | FIXED the first two. The 0.6 equals `SimConfig::popAt`, which the bench never overrides: left. |
| B12 | MED | An R/F5 materials reload under an open bench left it validating its tally against a table whose ids may have moved. | `main.cpp` reload block; `alchemy_bench` `mats_` | FIXED: a reload that MOVES ids closes the bench first and runs the next frame; a same-ids reload (the `--frames` harness's F5, a tuning edit) proceeds under the open bench, which runs on the rules it copied at Open. |

## World solute layer

| # | Sev | Finding | Evidence | Status |
|---|---|---|---|---|
| S1 | HIGH | Rule 1: a window shift evicts a plane of up to 1,024 slots, the staging held 256 records, and a record was claimed by `atomicAdd` -- past 256 solute-carrying slots, WHICH chunks kept their mass depended on scheduling (the rest was freed and lost, only printed). | `sim_solute.wgsl` solEvict; `stream.cpp` EvictSolutes | FIXED: `EvictSolutes` runs in batches of `kSolEvictRecords` slots, one pass + staging each; a batch cannot overflow. |
| S2 | HIGH | `solPour` can deposit into the chunk below (or above) the op's chunk; if the CA is already moving that chunk's liquid this tick it carries the mass into a neighbour nobody paged, and the store is refused (mass lost, or duplicated by half a swap). And the fault check every solute shader's comment names (`Simulation::CheckSoluteFaults`) did not exist: faults were counted and never looked at. | `sim_mutate.wgsl` cells / solPour; `sim_step.wgsl` solSwap/solTransfer; `world.h` kSolMFaults | FIXED: `cells` wakes and raises the request flag of the chunks above and below; `solPour` deposits only within one chunk vertically; a solute store fault is now a FATAL abort in SubmitTick naming the first slot and tick (support.cpp). Comments corrected. |
| S3 | MED | The GPU applies at most 256 pours (`SOL_POUR_MAX_OPS`) with no refusal counter, and the cell stream's cap (`kMaxCellOpsPerTick`) cut the tail -- where the pours are -- first, though their mass was already debited from a vessel. The WGSL comment named the wrong constant. | `support.cpp` pour partition; `sim_mutate.wgsl` | FIXED: `world.h kMaxSolutePourOpsPerTick`, clamped keep-first in SubmitTick; voxel ops are cut before pours; refused pours and their units counted into `last_run.json` `opstream` (`solPourTrunc`, `solPourUnitsTrunc`; `gasTrunc` was missing from the writer too). `check_invariants.py` pairs the WGSL copy with world.h. |
| S4 | MED | `ContainerSolutePoursDue` merged same-cell pours up to 4,095 units, but the GPU's powder fallback has room for `kSolutePourMaxEighths` (48) eighths: a break over dry ground could lose the rest (`SOLM_POUR_LOST`). | `container.cpp` ContainerSolutePoursDue | FIXED: merge capped at one entry's worth. |
| S5 | MED | Where two species met, `solTransfer` DESTROYED the arriving mass on every transfer (DESIGN.md said it was refused): brine levelling into sugar water deleted the salt. | `sim_step.wgsl` solTransfer | FIXED: the mass stays in what is left of the source; discarded (counted) only when the source empties. |
| S6 | MED (suspected) | Solute pool exhaustion under a world-wide wake over a large salted body (solWant pages N27 of every dirty chunk near solute) would abort. | `sim_solute.wgsl` solWant; pool = kNumSlots/8 | NOT FIXED: unmeasured. Needs an adversarial gate (a salted lake + a world-wide wake) before any design change; the abort names it if it happens. |
| S7 | LOW | The dilution floor can use a stall clock that only dirty-list chunks advance. | `sim_solute.wgsl` solCompact / solDiffuse | WON'T FIX: at most a floor's worth of units per cell, counted and deterministic. |
| S8 | LOW | Pending solute pours (like the pre-existing in-flight vessel spills) are not cleared on a world load; an R reload that reorders `solutes.json` does not remap the live layer's species ids; `ReadSoluteFile`'s failure only logs. | `session.h` solutePours; `worldio.cpp` | WON'T FIX: matches the existing in-flight spill behaviour; the reorder case is dev-only. |

## Reaction effects, content, healing

| # | Sev | Finding | Evidence | Status |
|---|---|---|---|---|
| R1 | MED | The reaction-effect record keyed its slot hash and `atomicMax` key on the WINDOW-RELATIVE cell, so the same firings picked different winners (different ExplosionOps in the hashed world) for a different window origin -- CLAUDE.md: an identity is the slot index. | `sim_step.wgsl` reactFxNote; `world.h` ReactFxDecodeCell | FIXED: keyed on the slot cell (world cell mod `kWorldN`); the stored origin decodes it back to the one window cell. |
| R2 | MED | Standing in a pool of enchanted water/blood heals without limit: contact coating never spends the liquid (the pour path does). | `mob.cpp` StainOneLimb; materials.json coat `contact` 120 / 90 | WON'T FIX: authored as design ("a healing spring heals whoever stands in it"), and bounded per tick (rule 2 holds). A content decision for the owner: `coat.contact: 0` makes only pour/splash/apply heal. |
| R3 | LOW-MED | Healing runs inside `BurnTick`, which the shared burn budget stops visiting near a large fire, so healing stalls there. | `mob.cpp` BurnLimbs `frontBudget`, HealTick call | NOT FIXED: `mob.cpp` has another session's uncommitted edits on main; a separate heal loop in `BurnLimbs` is the fix. |
| R4 | LOW (suspected) | The heal grow step may not skip joint-twin child cells as the mend step does (a twin cell paid for twice). | `mob.cpp` HealLimbStep | NOT FIXED: unverified; same file contention. |
| R5 | -- | No world handler for `burst` / `eject` / `shock`. | `session.cpp` ReactFxToBlasts | DECIDED: `shock` is a world effect (a flash whose default material is `spark`, so it ignites through spark's own rules, bounded like flash); `eject`, `burst` and `pop` act on a vessel or a player at the bench and are bench-only by design. `pop` added to `kKnownEffectKinds` (the bench registers it). No rule authors these today. |
| R6 | LOW | fx ids are handed out per surviving rule; an R reload or a weather switch renumbers them, so up to `kSnapshotLatency + 1` ticks of in-flight firings map to another rule's effects. | `materials.cpp`; `world.cpp` reactFxPending_ | WON'T FIX: dev-only hot reload, two ticks. |
| R7 | LOW | Comments contradicting behaviour: chlorine "2.5x denser than air" at density 6 (the sinking is the `gas_heavy` tag); quicksilver "everything but gold floats on this" while gold_dust (9,000) floats; phosphorus "+ air -> fire" (a pair rule with air can never fire; it is an air-scaled decay). | materials.json | FIXED (comments only). |

Content checks that came back clean (python over all 420 rules / 191 materials):
no rule shadowed by an earlier ungated chance-1000 rule of the same self; no pair
rule naming `air`; every new gas has an ungated decay; no supercritical chain
(slime and molten iron convert one for one, converts spend the dissolved mass);
every solvent is a liquid, every `precipitatesTo` a powder, every `cMin` at or
under saturation; every new material's `far` look-alike exists. The ruleFx
parallel indexing (tails inherit, dropped rules drop from both) and the
order-free fx record were verified.

## Not fixed, ranked

1. **R3** healing stalls near big fires (mob.cpp contention).
2. **S6** solute pool exhaustion under a world-wide wake: unmeasured; needs an adversarial gate.
3. **R2** unlimited healing from a pool: a content decision.
4. **B6** dissolved matter streaming off the bench lands as powder.
5. **R4**, **B8**, **S7**, **S8**, **R6**: latent or dev-only.

Observation: one windowed `--shot-bench` run reported `page faults 1` (the
harness's F5 had closed the bench under the since-reverted first version of B12);
the rerun on the final binary reported 0, and the 40-gate verify reported 0. It
is unattributed; a real-time windowed run is not deterministic.
