# Audit: scar healing and stab pricing (3fd2cb2)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| SC-1 | HIGH | Machine races (android, automaton) regrow plating, circuitry and clockwork as pink `scar` flesh | `src/game/mob.cpp:16771` | small | no | fixed, gates pending `4197ab0` |
| SC-2 | HIGH | NPC amputee's open stump is lost on save/load, park/unpark and handoff, then fully regenerates | `src/game/mob.cpp:31512` | small | no | fixed, gates pending `0cf4fc1` |
| SC-3 | MED | KerfStabDepth crosses one-cell layers whose core misses every lattice centre for free | `src/phys/kerf.h:335` | small | no | fixed, gates pending `28b037c` |
| SC-4 | MED | Piece cut off below the sever fraction (fingers, ear, nose) regrows in place as boneless scar | `src/game/mob.cpp:16861` | medium | yes | open |

## Findings

### SC-1 (HIGH) Machine races (android, automaton) regrow plating, circuitry and clockwork as pink `scar` flesh

**Status:** FIXED on branch `audit-1014` in `4197ab0`. The scar pass's isTissue read an empty MobDef::tissue mask as 'every cell is tissue', so the android (bleed null) regrew plating, circuitry and its core as scar flesh. The automaton's clockwork (rubble oil, so it is in the tissue mask) did the same. For the scar, an empty mask now means nothing regrows, and shell materials (MaterialDef::shell: brass, clockwork, alloy, circuitry...) never scar. The hp half of the heal (ScarTick step 2) is unchanged, so machines still regain hp.

**Where:** src/game/mob.cpp:16771 (isTissue's empty-mask fallback), with src/game/mob.cpp:16465 (the scar gate never checks bleedMat).

**What:** The passive scar heal decides what regrows from MobDef::tissue. That mask is built only from a sidecar's `bleed` block, as the materials whose rubble is the creature's blood. The android deletes its bleed block, so the mask is empty, and an empty mask means "every cell is tissue". Every lost or burnt plating, circuit, alloy or core cell is therefore rebuilt as `scar` (hardness 4, not a shell, bleeds blood). The automaton's clockwork crumbles to oil, so it counts as tissue and also regrows as scar.

**Trace:**
- src/game/mob.cpp:16465 `scar = gore.scarHeals && scarMat_ != 0 && WoundsHeal()` (no bleedMat check)
- src/game/mob.cpp:15652 WoundsHeal = alive && def.woundHeals && gore.woundHeals (woundHeals defaults true)
- assets/mobs/android.json:113 `"bleed": null`, so bleedMat = 0
- src/game/mob.cpp:1158-1202 tissue is filled only inside the bleed block when bleedMat != 0
- src/game/mob.cpp:16771 `def.tissue.empty() || ...` makes every cell tissue
- src/game/mob.cpp:16809 the only filter, `scar && !isTissue(rmat)`
- src/game/mob.cpp:16823 the grown word is scar->mat with art 0
- assets/materials/materials.json:4973 clockwork has rubble oil, so it is in the automaton's tissue mask
- assets/materials/materials.json:5351 scar: hardness 4, no shell, rubble blood

**Failure scenario:** A sword chips an android's forearm plating and the android survives. With no bleed there is nothing to wait for, so within minutes the crater fills with pale-pink scar. The next blow on that patch is priced as soft flesh (hardness 4) instead of chipping 45-180 plating, and it can part. If an automaton is cut into its clockwork, the gap grows pink flesh that afterwards bleeds blood.

**Suggested fix:** Skip the ScarPass when def.bleedMat == 0 (machines have no living tissue). Do not treat an empty mask as "everything" for the scar pass. Also restrict scar regrowth to flesh-like recipe cells (not `shell`), so the automaton's clockwork is excluded. Later, a def could optionally name its own repair material.

**Verifier's note:** I confirmed the android bleed:null path, that no android variant re-adds bleed, and the clockwork material fields. I did not separately verify the hp-regen half: machines regaining hp may be acceptable, but they cannot opt out today because woundHeals lives inside the bleed block.

### SC-2 (HIGH) NPC amputee's open stump is lost on save/load, park/unpark and handoff, then fully regenerates

**Status:** FIXED on branch `audit-1014` in `0cf4fc1`. Confirmed. OverlayMobRecord re-severed limbs with DetachLimb, which never reopens the parent's stump, and the MOBS record carries no wound state, so after a load, unpark or handoff an NPC amputee stopped bleeding, regenerated and scarred over. A new Mob::ReopenStumpOf(cut) rebuilds the stump the way Sever() places it: woundLocal at the joint, woundFluid and woundScale from the matter at the cut, and stumpOpen. It only acts on blooded base body limbs, because worn shells, held items and hair never get a stump. OverlayMobRecord now reopens all stumps before its detach loop. PlayerAvatar::Spawn now calls the same helper instead of its own inline copy. Side effect: the avatar no longer opens a stump on the limb under a severed worn slot, and its reopened stump now gets the matter-derived woundFluid and woundScale.

**Where:** src/game/mob.cpp:31512 (OverlayMobRecord re-severs with DetachLimb, which does not reopen the parent's stump).

**What:** An NPC's open stump (MobLimb::stumpOpen, bleedBudget, gushTicks) is not stored in the MOBS record, and the NPC load/overlay path does not re-derive it. After a save/load, a park/unpark or a network handoff, an NPC amputee no longer bleeds. The new creature-wide regen gate and ScarTick's wait now let it regain hp on every limb and scar the stump. DESIGN says an amputee never regenerates and the stump kills. The player avatar already re-derives stumps on load, so only NPCs are affected.

**Trace:**
- src/game/mob.cpp:30930 SaveOne writes kind/hp/xf per limb, with no wound state
- src/game/mob.cpp:31512 OverlayMobRecord calls DetachLimb(i,false) for severed limbs
- src/game/mob.cpp:28073 DetachLimb clears stumpOpen on the detached slot only
- src/game/mob.cpp:27955 Sever is the only NPC setter of parent.stumpOpen
- src/game/mob.cpp:31655 / 32388 LoadOne and ApplyHandoff only RelatchInfections
- src/game/mob.h:7695 parking goes through SaveOne/LoadOne
- src/game/avatar.cpp:296-317 the player path reopens parent stumps (the NPC path has no equivalent)
- src/game/mob.cpp:16474 / 16561 the regen gate and the ScarTick wait now find no open stump

**Failure scenario:** A villager loses a forearm and is bleeding out from the upper-arm stump. The player saves and reloads, or the villager wanders out of the window and is parked, or ownership hands off to the peer. On return the stump has stopped bleeding, every limb regains 0.1 authored hp per minute, and the stump face scars over. The villager survives and recovers instead of dying.

**Suggested fix:** After OverlayMobRecord (in LoadOne and ApplyHandoff), reopen the parent stump for every severed base limb, the same way PlayerAvatar::Spawn does: set woundLocal at the joint anchor and stumpOpen = true. Share that code with avatar.cpp. Alternatively, persist stumpOpen/bleedBudget/gushTicks in the record (a format bump).

**Verifier's note:** The unsaved bleed state predates 3fd2cb2. This commit turns it from "stops bleeding" into "fully recovers". Re-derivation also reopens cauterised stumps, as the avatar path already accepts (the conservative side).

_Also reported independently by: AUDIT_done_sweep.md (G5: persists)._

### SC-3 (MED) KerfStabDepth crosses one-cell layers whose core misses every lattice centre for free

**Status:** FIXED on branch `audit-1014` in `28b037c`. KerfStabDepth priced each one-cell layer as the mean cost of the lattice centres inside it. A layer with no centre (common on oblique bores) was priced like a cavity and crossed for free. Each layer is now charged its summed cost divided by its geometric lattice count, PierceChannel's rule, using one shared layerOf predicate counted over the box of the layers that hold matter. A layer with no lattice centre is priced at the next sampled layer's rate instead of free. scar-heal arm D's synthetic bore has 1 geometric cell per layer, so its expected depths are unchanged. Side effect: a layer that is partly air (a curved surface) now costs that fraction, as in PierceChannel.

**Where:** src/phys/kerf.h:335 (`price = n[k] ? sum/n : 0; if (price <= 0) { at = k+1; continue; }`).

**What:** The stab bore is priced per one-cell layer, as the mean cost of the cells whose centres fall in a core box floored at 0.5 cells half-extent, about one cell of volume. When the bore is oblique to the limb's lattice, many layers inside solid matter contain no cell centre. They are priced 0 and crossed for free, like a cavity, so stab depth and bone penetration depend on the thrust's angle to the lattice rather than on the matter alone.

**Trace:**
- src/game/melee.cpp:985-993 stab halfWidth = stabThick, length = radius*stabWidth
- src/phys/kerf.h:312-313 hw/hl = max(half*sc*0.35, 0.5): a near-unit core box
- src/phys/kerf.h:319-329 cells are kept only when their centre is inside the box, binned by floor(dw)
- src/phys/kerf.h:334-338 an empty bin costs 0 and the walk advances
- src/phys/kerf.h:348 leftover budget extends the depth further
- src/game/mob.cpp:15171 and src/phys/debris.cpp:5673 both stab paths use this walk
- src/phys/kerf.h:667-679 PierceChannel already normalises by geometric count for arrows

**Failure scenario:** Two identical dagger thrusts hit the same shin, one along a lattice axis and one at about 40 degrees. The aligned one stops in the bone. The oblique one gets one or more bone layers with no sampled centre for free and goes into the flesh behind. The finder's re-implementation measured about +22% mean depth on coarse cores, with roughly half of oblique bores passing a 3-layer bone slab that stops the aligned bore.

**Suggested fix:** Price each layer by the depth it covers, as PierceChannel does: scale the summed cost by (core area x 1 cell) / (geometric lattice points in that layer's core box), so a full layer of one matter costs the same at any angle. Or widen the probe (radius >= sqrt(3)/2 cells) before treating a layer as empty.

**Verifier's note:** I checked the floor, the empty-bin branch and both callers, and that the scar-heal gate's stab arm uses an axis-aligned bore. I did not reproduce the finder's numpy statistics; that a unit-volume oblique box often holds zero lattice points is elementary.

### SC-4 (MED) Piece cut off below the sever fraction (fingers, ear, nose) regrows in place as boneless scar

**Where:** src/game/mob.cpp:16861 (the grow frontier accepts any missing tissue recipe cell that touches present matter).

**What:** When a blade carve splits a limb, the far piece leaves as debris. The limb is severed (and the stump opened) only if the piece is at least gore.woundSeverFraction of it. Below that, the limb keeps its slot with no stump. Once the cut stops bleeding, the passive scar heal treats the missing piece as an ordinary crater and regrows it outward from the cut face (bone excepted), while the cut-off piece still lies on the ground.

**Trace:**
- src/game/mob.cpp:18469-18481 parted components become EmitCarvedFragment debris or particles
- src/game/mob.cpp:18551-18555 Sever only when partedOff >= woundSeverFraction*n (0.28, tuning_params.def:1928)
- src/game/mob.cpp:27955 stumpOpen is set only in Sever
- src/game/mob.cpp:16561 ScarTick waits only on bleed/stump/gush/alight/infected/pulp
- src/game/mob.cpp:16809 only bone (non-tissue) is skipped
- src/game/mob.cpp:16861-16880 any missing tissue cell with a present 6-neighbour enters the frontier

**Failure scenario:** A cut through a hand takes off the fingers (under 28% of the hand), or an ear, a nose or a naga tail tip. The fragment drops as debris and the cut bleeds out its budget. Over the next minutes the fingers or ear regrow in place as pale scar flesh with hollows where the bone was, beside the severed original on the floor.

**Suggested fix:** Record the parted region at the split (a per-limb "lost" mask of recipe cells, or a sub-stump flag) and exclude it from the scar frontier. Alternatively, let time close only the cut face: scar a cap over the exposed layer and do not regrow past it.

**Owner call:** Should passive healing close a cap over a parted face only, or regrow the lost part? DESIGN.md (heals itself, slowly) says stumps never close and a crater to the bone keeps its notch, but does not address sub-sever parts.

**Verifier's note:** I read the split, both sever conditions and the grow loop, and found no memory of the parted component. The magic coat heal sharing this loop may be intended to regrow ("magic erases scars"), so a fix should apply to the ScarPass only.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | PARTIAL | Scar cells persist in limb lattices: SaveOne PodVec plus by-name remap (mob.cpp:30941-30942, 31278-31282) and AVTR (avatar.cpp:1290, 1335-1339). scarSig is derived and the pace is stateless. The pause/regen gate's inputs (stumpOpen/bleedBudget/gushTicks) are not saved, and NPC loads do not re-derive stumps (mob.cpp:31512-31518, 31655-31657); see finding 2. |
| deterministic | OK | Keyed on tick, id_ and limb, with integer counts and doubles in a fixed order (mob.cpp:16581-16592, 16889-16915). Runs in the serial BurnHeadFinish (mob.cpp:21663). KerfStabDepth is a fixed-order integer layer walk (kerf.h:331-350). It writes no grid state. |
| sleeps when idle | PARTIAL | A settled limb costs the wait checks and a modulo per tick, plus ScarSigOf every 3rd tick (mob.cpp:16561-16578). But the signature includes the creature-wide burnFrac_ (16551), so healthy limbs re-walk the full recipe while any other limb burns or mends (finding 6). There is also one full walk per limb after every spawn or load (scarSig 0). |
| cpu mirror | N/A | Reads only the limb's own lattice and recipe (HealLimbStep, mob.cpp:16668-16881) and never a world cell or KindAt. |
| mutation queue | N/A | Writes CPU body state only: lattice, brick and Jolt collider (mob.cpp:17043-17094). No CellOp, BrushOp or particle. Scar voxels reach the grid only through the existing carve ejection stream (LimbVoxelsToParticles, mob.cpp:15492-15498). |

## Coverage (as reported by the finder)

**scar heal.** I read the 3fd2cb2 diffs for src/game/mob.{h,cpp}, src/phys/kerf.h, src/game/melee.cpp, src/phys/debris.cpp, materials.json, reactions.json, tuning.json, tuning_params.def and tuning.h. I also read the scar-heal gate (selftest_wound.cpp:10619-10875) and the selftest_combat/selftest_mob changes, which are shield-only. I checked that 3fd2cb2..20aa002 touches none of these paths except buoyancy (369900f).

Traced end to end:
- HealTick -> ScarTick -> HealLimbStep(ScarPass): pace and time base (30 Hz, telescoping floor so no fraction is lost), hp regen and burn cap (no overflow, the dead are excluded), the settle signature, the surface/underWhole rule, tissue and exempt filters, tombstone revival, the twin rule and twin sync reconcile, the collider/brick rebuild, and RecountBurn/burntAway accounting.
- The callers: the BurnLimbs pots, the serial BurnHeadFinish (ghost/released refusal), and PlayerAvatar::BurnParts.
- MobDef::tissue construction and sidecar null-merge; the android, automaton and sylvan sidecars against the material table.
- Every stumpOpen writer (Sever, cauterise, stanch, DetachLimb, avatar load), BleedTick, and Sever/DetachLimb bleed handover.
- Persistence: SaveOne, ReadMobRecord name remap, OverlayMobRecord, LoadOne, ApplyHandoff, parking, and AVTR save/Spawn.
- CarveLimb's connectivity split and cut-through sever.
- KerfStabDepth and both consumers (CutLimb, CutBody); BuildStrikeParts stab dimensions and the owner-side rebuild of parts over the wire (reach is rebuilt, so player, NPC and remote stabs match). Arrow pierce pricing compared (hardness-consistent).
- Body-voxel ejection into the grid (scar can land as a world voxel). The reactions mirror flesh's four rules exactly, with no keepAwake, so they are no new criticality.
- The tree atlas resolves materials by name, so the id shift is harmless there.

Stab-bias numbers come from a throwaway numpy re-implementation of the layer walk; nothing was built or run.

Checked and found OK: time base, fractional accumulation, the hp cap, the dead/undead gate, arrows and worn/held slots (excluded or harmless), regrowth into other slots (separate lattices; twins handled), shells skipping stab pricing, and reach applying only to stabs.

Not audited:
- The shield, naga, biome-pill and plan-doc parts of the bundle.
- Id-baked assets beyond trees after the scar insertion shifted ids 225+ by one. I trusted the commit note for harrowby's .svedit, and default_ground.svedit is empty. Far-palette slot assignment for the shift was not checked.
- DamageBody spall internals beyond SpallGrow.
- Whether alight is correctly re-derived after load for still-burning cells.
- Exact blade halfL values: item scale was not resolved, so I give a range.
- Any perf measurement (estimates only).
- Gates other than scar-heal.

