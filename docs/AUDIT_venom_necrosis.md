# Audit: venom necrosis (7ea0504)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| VN-1 | HIGH | Venom necrosis never settles the organ/weight delta it causes; the next unrelated carve bills it | `src/game/mob.cpp:22342` | small | yes | fixed (owner decision), gates pending `420230c` |

## Findings

### VN-1 (HIGH) Venom necrosis never settles the organ/weight delta it causes; the next unrelated carve bills it

**Status:** FIXED on branch `audit-1014` in `420230c`. Owner decision 2026-10-14: venom is billed as the flesh becomes envenomed. Mob::SettleInfectConversion bills the wound-weight and organ change of every in-place conversion (the spread, an eat that `leaves` necrotic, a joint crossing, a coat seeding, a bite's infectious rewrite) at that moment, under the infection's cause, and moves weightCharged / organCharged by exactly what it settled, so a later carve bills nothing twice and a heal re-baseline forgives nothing. A death this way is named for the infection. The rot's removal path is unchanged; its spread now bills at the spread (same total, same cause).

**Where:** src/game/mob.cpp:22342 (InfectStep's `flushes`), with the deferred charge at src/game/mob.cpp:17997.

**What:** Since 7ea0504, an infection with `infect.leaves` rewrites eaten cells in place and never tombstones them, so `flushes` is false and InfectStep never runs FlushBurn/CarveLimb. CarveLimb charges organ and wound-weight loss as a delta against MobLimb::organCharged/weightCharged. Organ cells the venom converted (heart 48 hp each, brain 3.5) therefore remain an unbilled debt. The next carve of any cause on that limb charges all of it, or a heal/graft re-sync forgives it.

**Trace:**
- assets/materials/materials.json:4912-4920: envenomed targets tag:soft_tissue, leaves necrotic; heart is soft_tissue with organHp 48 (:5321)
- src/game/mob.cpp:22236: spread rewrites heart -> envenomed (no organHp)
- src/game/mob.cpp:22268: eat writes `spec.leaves` with no tombstone and no `st.removed++`
- src/game/mob.cpp:22342-22346: `flushes = eaten && burn.removed` is false, so the forced burn-out/lethal FlushBurn is skipped
- src/game/mob.cpp:17956,17997-18010: the next CarveLimb recomputes totals and charges `organCharged - nowOrgan` under that carve's cause and records organHpTaken
- src/game/mob.cpp:17078 / 16309: a heal step or graft re-baselines instead and forgives the debt

**Failure scenario:** A snake or naga bite in the chest lets venom reach the heart and necrotize 4+ heart cells. The venom burns out having charged only its flat hp. Later an arrow graze or sword nick on the torso charges about 192 hp of organ loss at once and kills, booked to that blow and named an organ death instead of 'venom'. If a scar or healing-coat step touches the torso first, the loss is never charged.

**Suggested fix:** Keep the ledger in step inside InfectStep. On each conversion (spread, leaves-eat, joint crossing) accumulate OrganHpOf(was) - OrganHpOf(now), and likewise for WoundHpOf. Charge the organ delta under spec.cause (plus organHpTaken) before the lethal check, and advance organCharged/weightCharged by the same amounts. The minimal fallback is a LimbWoundTotals re-baseline at burn-out/lethal, which is what the old forced flush effectively did.

**Owner call:** Should a necrotized organ cell charge its organHp (DESIGN says organ loss 'by any cause' counts), or should it only be re-baselined, given the commit says the volume charge is gone? Either answer removes the deferred mis-billing.

**Verifier's note:** I confirmed that no other path writes organCharged/weightCharged (full grep of src/game) and that both Infection policy rows have chargesOrgan. The debt starts at SPREAD (heart -> envenomed), not only at the eat, and before 7ea0504 it was settled only because the flush always ran. I did not trace a specific bite's geometry to the heart. The brain in a head bite is an easier route to the same result.

## Also found by this sweep (canonical entry elsewhere)

- Record overlays restore carved lattice and hp but leave *Charged baselines at pristine: see **SH-1** in `AUDIT_shields.md`.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | PARTIAL | Necrotic cells ride the MOBS v4/AVTR v2 lattice (mob.cpp:30930-30942) with by-name remap (31278-31287), and the latch is re-derived on load (31657, avatar.cpp:293). But the overlay never restores the carve-charge baseline (OverlayLimbLattice, mob.cpp:31344-31402; finding 2), and the bite's woundWas table is unsaved by design (mob.h:1982). |
| deterministic | OK | Counter-based Hash3(limbKey, tick, posKey ^ mat*K) over lattice storage order (mob.cpp:22164-22195). CPU body state, not world-hashed. Stepped only by the owning machine (ghosts are not burnt, mob.cpp:31745-31750). |
| sleeps when idle | OK | Burn-out clears limb.infected (mob.cpp:22325-22326, 21965-21969), leaving one byte test per limb (21865). Necrotic has no infect block and no reaction rule, so a dead patch costs nothing per tick. |
| cpu mirror | N/A | No world-grid state: the CPU limb lattice is the authority and the GPU micro brick is poked from it (mob.cpp:22268-22271). The fine-skin collider lattice is not re-derived after necrosis and stays stale until the next carve or heal (cosmetic). |
| mutation queue | OK | The leaves path writes only the body lattice and the render brick (mob.cpp:22268-22271); no cell ops. The old removal path's FlushBurn carve also ran with eject=false (mob.cpp:18763-18764). |

## Coverage (as reported by the finder)

**venom necrosis.** Static only, at 20aa002 (worktree audit-1014).

**Read and traced:**
- The whole 7ea0504 diff: mob.cpp/h, materials.cpp/h, main.cpp AutopsyKinds, autopsy.h, materials.json, selftest_impact.cpp venom-wound, DESIGN.md.
- The current InfectTick / InfectStep (all of it), InfectAcrossJoint, CoatInfectTick, and the InfectSpec build in OnMaterialsReloaded.
- The `leaves` loader validation in LoadAssets: solid, not an infection, id <= 255. Shipped content passes.
- HpZeroSevers, the severpolicy.h cause table, damagecause.h, Sever's root/unseverable routing to Die, and VitalDeathCause.
- The new lethal branch. It is correct for vital/root/arm; it cannot double-kill (it checks alive_), and its indices are guarded.
- CarveLimb accounting: weight, organ, bleed and the hp-zero rules. FlushBurn's threshold and forced flush.
- LimbWoundTotals and every re-baseline site: spawn, carve, heal, graft, shell restore.
- HealTick, ScarTick and HealLimbStep. The scar pass does not mend necrotic; a coat heal does.
- StainWoundAs / woundWas / ReviveWoundVoxel / the BurnOneLimb revert, and BiteHit's infect rewrite.
- The whole MOBS v4 save/read path, its name remap and the overlay: LoadOne, unpark in persist.cpp, ApplyHandoff, ApplyState. Also the AVTR v2 overlay, RelatchInfections on load, and mattable remap semantics.
- All .svedit layers decoded: max material id 221, below the 224 insert. Tree atlases name their materials. No numeric material ids in the shifted range in src or shaders.
- reactions.json rules touching body materials: necrotic has no tag:hot rule, like envenomed, brain and heart.
- Materials rows for heart, brain, muscle, envenomed, necrotic and scar. The anatomy.js torso/skull recipes. Human limb hp/vital/severable. The relevant tuning values.

**Rejected after tracing:**
- Re-infection by venom: necrotic is not soft_tissue.
- An id > 255 reaching a body: validated, and no path found.
- Corpses: infection runs, charges nothing, and burns out.
- Debris: no infection pass.
- Idle cost after burn-out: the latch drops.
- The mixed rot+venom flush semantics (same as before).
- Twin-sync copying necrotic (fine).

**Not done or only partly done:**
- I did not measure whether a snake/naga bite actually reaches the heart. The path is argued from the anatomy recipe; a venom-coated stab beside the heart reaches it directly.
- MicroBodyPack and microbody.wgsl handling of 8-bit ids beyond comments.
- The tuner/editor round-trip of the `infect.leaves` key.
- MobState/handoff cadence in net code.
- LimbFloatDensity's count-keyed cache missing material conversions. Noted, not reported: the density delta is tiny.
- The fine-skin collider not being re-derived after necrosis. Stale gobbet materials on a later carve; cosmetic, not reported.
- The HUD (known item).
- The venom-blade / snake reds (attributed).

