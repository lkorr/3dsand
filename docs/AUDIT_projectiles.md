# Audit: the penetrator, the heart organ and matter spell flights (41b7f3e, 612ff72, d3397a0)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| PJ-1 | MED | Spell splash/lodged coat on a peer-owned body writes the ghost only, unrouted; matter deleted | `src/game/pierce.cpp:447` | small | no | partial (stop-gap) `36317d3` |
| PJ-2 | MED | Splash radius floor 0.1 vox assumes human 8 cells/vox; coarse bodies (dummy, critter) miss | `src/game/spell.cpp:2345` | small | no | fixed, gates pending `45a392d` |

## Findings

### PJ-1 (MED) Spell splash/lodged coat on a peer-owned body writes the ghost only, unrouted; matter deleted

**Status:** PARTIAL (stop-gap) on branch `audit-1014` in `36317d3`. SpellMatterStrike poured a splash coat, or a lodged lump's coat or fire, onto whatever Mob the ray met, ghosts included. It also set tookMatter, so the VM removed the matter from the drop and it existed on neither machine. This is the suggested stop-gap: a new MatterLandsHere(OwnedHere) test skips the coat and ignite on a body this machine does not own, so tookMatter stays false and the matter drops through the op stream on both machines. Single-player always owns the body, so nothing changes there. The full fix (coat applied on the owner's machine through a MeleeHit variant) needs mob.cpp and melee.*, which this lane cannot edit.

**Where:** src/game/pierce.cpp:447 (also pierce.cpp:535, pierce.cpp:544)

**What:** SpellMatterStrike applies a liquid splash's coat (PourOnBody), and a stopped lump's wound coat or ignite (PourOnBody / IgniteLimb), to whatever Mob the ray resolves to. It never checks MobSystem::OwnedHere and queues no wire record, unlike the channel itself, which PierceAlongRay routes as MeleeHitKind::Pierce. On a peer's ghost the coat lands on an inert copy the owner never hears about. Because tookMatter is still set, the VM strips the matter from the drop, so it exists on neither machine.

**Trace:**
- src/game/pierce.cpp:398: FirstBodyOn -> mobs.FindOwner(body) returns ghost NPCs and remote avatars too
- src/game/pierce.cpp:447: mobs.PourOnBody(fb.mobId, ...) with no OwnedHere test
- src/game/mob.cpp:30229: PourOnBody resolves through FindCreature only, then writes stain
- src/game/pierce.cpp:450: out.tookMatter = marked > 0
- src/game/spell.cpp:5027: bodyTookCore = r.tookMatter
- src/game/spell.cpp:5061: the core Spray is excluded from `drop`, so no op is emitted
- contrast src/game/pierce.cpp:320-327: the channel is routed via QueueMeleeHit when !OwnedHere
- src/game/mob.cpp:11245: OwnedHere is false for RemoteGhost and for owner_ != local

**Failure scenario:** Co-op: A casts an acid or lava projectile at B, or at an NPC that B's machine owns. On A, the ghost gets a static coat that never eats (ghosts skip StainTick), and the acid Spray is dropped. B's machine gets no MeleeHit and no op: no coat, no damage, no acid on the ground.

**Suggested fix:** In SpellMatterStrike, when the struck Mob is !OwnedHere, do not write the ghost. Either queue a MeleeHit variant carrying material, amount, radius and the ray in the struck slot's frame, applied owner-side through PickBody + PourOnBody / IgniteLimb, or (stop-gap) leave tookMatter false so the matter drops through the op stream. Update the stale known-limit line at DESIGN.md:9139.

**Verifier's note:** Read pierce.cpp:295-345 and 380-547, PourOnBody at mob.cpp:30222-30331, OwnedHere at mob.cpp:11245, and spell.cpp:5020-5065. DESIGN.md:9139 does list 'a peer's ghost is struck like a local creature' as a known limit. Blame shows it was written in d3397a0, before merge b84c33d routed the channel. It says nothing about the matter being deleted, so the coat path is an unfinished half of that routing, not a documented decision. The lodged-solid coat/ignite half is latent today, as the claim says.

### PJ-2 (MED) Splash radius floor 0.1 vox assumes human 8 cells/vox; coarse bodies (dummy, critter) miss

**Status:** FIXED on branch `audit-1014` in `45a392d`. Confirmed: SpellMatterStrike floored the PourOnBody disc at 0.1 voxel, which is half a skin column's diagonal only at the human's skinScale 8. On coarser lattices the hit cell's centre usually fell outside the disc and nothing was coated. A new helper, SplashFloorOn(mobs, mobId), returns max(0.1, 0.7072 / def->skinScale), applying the comment's own half-diagonal rule to the struck creature's lattice. Both the splash coat and the wound-floor coat use it. Humans are unchanged (0.088 < 0.1). This uses the def-level skinScale because per-limb SkinScaleOf is protected in mob.h, so a worn shell with its own scale gets the body's floor.

**Where:** src/game/spell.cpp:2345 (floor), src/game/mob.cpp:30286 (disc test)

**What:** SplashRadiusMille floors the coat disc at a constant 0.1 voxel, and its comment derives that from the human's 8 cells per voxel. PourOnBody keeps a cell only if its centre lies inside the disc around the ray line, and the cell PickBody hit is not exempt. On coarser lattices (dummy 1 cell/voxel, critter 2, shark 4), the hit cell's centre is usually farther from the line than 0.1-0.2 voxel. A confirmed skin hit then coats nothing, or only a deeper cell.

**Trace:**
- src/game/spell.cpp:2345-2349: clamp to kSplashRadiusFloorMille (100 = 0.1 voxel)
- src/game/pierce.cpp:403: PickBody confirms the hit cell (pk.hit)
- src/game/pierce.cpp:447: PourOnBody(..., max(0.1, splashRadius), ...)
- src/game/mob.cpp:30281-30286: cell centre in world voxels; `pu*pu + pw*pw > r2` rejects it
- src/game/mob.cpp:1355-1357 / 15597: lattice scale from artVoxelsPerMetre (dummy 10 -> 1, critter 20 -> 2, shark 40 -> 4)
- src/game/pierce.cpp:450 -> spell.cpp:5061: marked 0 means tookMatter false, so the lump drops at the hit point

**Failure scenario:** A x1 acid or lava projectile (r = 0.2 voxel) hits the training dummy square on. For an axis-aligned hit the chance that the hit cell's centre lies inside the disc is pi*0.2^2, about 13%, so about 87% of hits coat nothing. The liquid falls at the dummy's feet, and there is no eating or searing. At x0.25 (r = 0.1) almost no hit on a 1-2 cells/voxel body lands.

**Suggested fix:** Have PourOnBody always include the cell PickBody hit (hit.limb / hit cell), or floor the radius in SpellMatterStrike at about 0.71 / scale of the struck limb's lattice (SkinScaleOf / PhysScaleOf). Apply the same to the wound-floor coat at pierce.cpp:535.

**Verifier's note:** Read SplashRadiusMille and its comment, PourOnBody's candidate and depth logic, ViewOf's scale and the three sidecars' artVoxelsPerMetre. Checked the axis-aligned miss probability analytically (1 - pi*r^2 at 1 cell/voxel), and it matches the finder's Monte Carlo. Impact is lost effect, not lost matter: the lump still drops through the op stream.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | PARTIAL | Channel wounds persist as limb/body lattice state, and organ totals are recomputed from the restored lattice (mob.cpp:33702). In-flight matter flights and nextSeq_ are not saved and survive an F10 load (main.cpp:15657-15665; spell.h:1703). organHpTaken is not saved (naming only). |
| deterministic | PARTIAL | PierceChannel uses a total-order sort with fixed-order sums (kerf.h:714-719, 763-798), and PierceAlongRay follows Jolt ray order. But the strike seed is Hash3(p.seq, ...) with p.seq a session-lifetime counter (spell.cpp:4048, 4983), so the soak and glance draws depend on flight history. |
| sleeps when idle | OK | Event-driven: one CastRayBody per live matter flight per tick (spell.cpp:4937-4986 -> pierce.cpp:276). Nothing runs without flights. The strike log is capped at 16 (spell.h:1661) and the walk at 2*kPierceMaxBodies casts (pierce.cpp:269). |
| cpu mirror | OK | Reads only CPU-authoritative body lattices (pierce.cpp:77-88, debris.cpp:5811-5822). Terrain goes through the shared FlightSolidAt tiers (spell.cpp:4655). Peer ghosts are stale by design: the channel is routed (pierce.cpp:320-336), the splash coat is not (finding 1). |
| mutation queue | OK | Gore leaves via CarveLimb/DamageBody eject into the tick's ParticleSpawn stream (pierce.cpp:191-199, debris.cpp:5874-5879). The leftover drop goes via ApplySpellEffect Spray spawns under the opsUsed gate (spell.cpp:5065-5073). Coats and ignite are CPU body state. No direct voxel writes. |

## Coverage (as reported by the finder)

**projectiles.** Read and traced, all at 20aa002 in the audit-1014 worktree:
- src/phys/kerf.h in full: PierceChannel, PierceKeepAt/PierceInAxis, PierceLatticeFor, PierceCellJ, PierceGlanceOf.
- src/game/pierce.{h,cpp} in full: Mob::PierceLimb, MobSystem::PierceLimb, PierceAlongRay, FirstBodyOn, SpellMatterStrike.
- DebrisSystem::PierceBody and DamageBody.
- mob.cpp: CarveLimb's hp and organ charge path, Mob::Damage, LimbWoundTotals, OrganDeathCause, the anatomy.becomes heart rewrite (mob.cpp:1409-1438), PourOnBody/PickBody/IgniteLimb, the ghost guards (IsGhost, StainTick, BurnHeadFinish), ScarTick hp, FindOwner/FirstCreature, restore-time organ re-sync (mob.cpp:33702).
- spell.cpp/.h: FlightMatter, SplashRadiusMille, KineticMilliJ/KineticMana (hand-checked: stone projectile 67.4 J → 7 mana, matching DESIGN), the EffectTariffIn Launch kinetic composition (lanes, inst-L, carry order), the SpellSystem::Tick sweep + matterHit + through/bodyStop + drop block, ApplySpellEffect Spray, Launch/Unit, budget clamps.
- Probe wiring in session.cpp and demon_cast.cpp; meleehit.h and melee.cpp ApplyRemotePierceHit; severpolicy.h (static_assert row order), shellresponse.h, damagecause.h; physics.cpp CastRayImpl.
- Data: glyphs.json matterProjectile/penetrator blocks; materials.json solid hardness (every solid has one).
- Docs: the DESIGN penetrator and spell-matter sections, docs/PLAN_projectiles.md, and the spell-pierce gate's replay and x0.25 assertions.

Checked and found OK:
- Channel energy never increases.
- Glance happens only when bin 0 can't be paid.
- Occupied cells ⊆ geometric bin count, so per-bin price ≤ area×cost; the per-bin price is independent of how the axis crosses the lattice.
- The carve equals the channel on the authoritative lattice, because CarveLimb flushes tombstones first.
- Converted-lattice bins match.
- PierceAlongRay advances `at` past each exited body, so overlap/twin cells are never paid twice.
- Re-handled bodies resolve as `missed`.
- Iteration order is total (sort by t,x,y,z) and nothing is keyed on pointers or unordered iteration.
- Pierce is appended last in the enum.
- Organ sums are exact (quarter values).
- Debris and live paths price identically.
- The drop and the gore leave only through ParticleSpawn streams under the same opsUsed gate as a resolve (rule 3).
- Kinetic int64 headroom is fine.

Not covered or not reported:
- Package C arrows (arrows.cpp, EmbedArrow, the flat Penetrator::hp charged once per body entered, so shell then flesh) beyond the shared PierceAlongRay.
- Gate bodies of pierce, heart and spell-pierce, beyond the lines cited.
- anatomy.js heart geometry and the re-baked .vox contents.
- Wire encoding of Penetrator.
- KerfStabDepth (10-14).
- The commit's claim that saves remap material ids by name: not verified.

Observations below my bar:
- Creature casters' ignore list is only Def()->limbs (demon_cast.cpp:710-712, 1114-1117), so a caster's own worn, held and arrow slots are struckable. Latent: no shipped NPC kit flies solid or liquid matter.
- The F10 / --load-world handler (main.cpp:15657-15665) clears grenades but not session spell flights, bombs or statuses, nor ArrowFlights. Pre-flight-era, and a dev path.
- organHpTaken is never reduced when the 10-14 scar heal restores hp and regrows the heart as scar, so a healed creature's later torso death still reads 'heart destroyed'. Naming only; documented as 'over its life'.
- Gravity-mod speed gained in flight is not priced (physics; owner call).
- The splash coat consumes the whole core item whatever area it covers: matter is not conserved. This is the DESIGN's stated intent.

