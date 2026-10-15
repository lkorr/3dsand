# Audit: the bow, arrows, NPC archers and draw poses (dbaccd5, 4420593 and follow-ups)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| AR-1 | HIGH | An arrow that crosses the window face sticks there as a floating kinematic item instead of expiring | `src/game/arrows.cpp:468` | small | no | fixed, gates pending `bf55d52` |
| AR-2 | HIGH | NPC archers abort every draw at a peer's player: TargetNow cannot resolve ghost actor ids | `src/game/mob_archery.cpp:256` | small | no | fixed, gates pending `aa33cbd` |
| AR-3 | MED | Anything that clears the held bit mid-draw (stun, talk, inventory, pause) looses the arrow | `src/game/session.cpp:2335` | small | no | fixed, gates pending `eaafce8` |
| AR-4 | MED | ArrowFlights/MobArcheryWorld are never cleared on load or F7; stale stuck ids can delete new items | `src/game/arrows.h:230` | small | no | fixed, gates pending `348cec7` |
| AR-5 | MED | A staggered NPC archer still looses its in-progress draw; StepDraw never checks the stagger | `src/game/mob_archery.cpp:244` | trivial | no | fixed, gates pending `1324315` |

## Findings

### AR-1 (HIGH) An arrow that crosses the window face sticks there as a floating kinematic item instead of expiring

**Status:** FIXED on branch `audit-1014` in `bf55d52`. Confirmed: the arrow sweep used FlightSolidAt, which treats any out-of-window cell as solid, so a flight that reached the window face stuck there as a floating kinematic item and used up a kMaxStuck slot. The sub-step loop now checks CellInWindow(next) before solidAt. When the next sample leaves the window, the segment ends at the last in-window sample (bodies along it are still pierced) and the flight ends as Kind::Expired with no item placed. Spells keep FlightSolidAt unchanged.

**Where:** src/game/arrows.cpp:468

**What:** The arrow's half-voxel sweep asks FlightSolidAt, and FlightSolidAt answers 'out of window = solid'. A flight that reaches the residency-window face is therefore treated as hitting ground and placed as a kinematic stuck world item at the face, in mid-air. The only 'left the window = gone' expiry tests the cell at the start of the tick, and the sweep never lets that cell leave the window.

**Trace:**
- src/game/arrows.cpp:398-401 solidAt = FlightSolidAt
- src/game/flightprobe.h:30 `if (!world.CellInWindow(...)) return true;`
- src/game/arrows.cpp:468-471 wall = true at the first out-of-window sub-step
- src/game/arrows.cpp:572-579 placeItem(..., stuck=true) at the last free sample plus the bite depth
- src/game/worlditems.h:242-243 SetBodyKinematic(body, true)
- src/game/arrows.cpp:442-443 the only window expiry, which reads f.pos (always in window)

**Failure scenario:** The player misses a target, or shoots level or upward, in open ground. Roughly 22-29 m out, the flight reaches the window face and freezes there as a hovering stuck arrow that takes one of the kMaxStuck slots. NPC archers near the window edge produce the same.

**Suggested fix:** In the sub-step loop, test CellInWindow(next) before solidAt. When the point leaves the window, end the flight as Kind::Expired and place no item. Spells keep FlightSolidAt's out-of-window = solid answer.

**Verifier's note:** I read the sweep, the probe, the stuck placement and DropItemToWorldXf. None of them has a window guard. I did not re-derive the face distance; it follows from the 512 window and 10 vox/m.

### AR-2 (HIGH) NPC archers abort every draw at a peer's player: TargetNow cannot resolve ghost actor ids

**Status:** FIXED on branch `audit-1014` in `aa33cbd`. TargetNow only matched this machine's sessions and real mob ids. A ghost-band id (kPlayerActorBase + sessions + j) resolved to nothing, so an NPC archer let every draw at a peer's player down as 'the target is gone'. TargetNow now maps band index players.size()+j to w.remotes->list[j], which is the order RemotePlayersAppendActors publishes. It returns that ghost's position, and treats a ghost with no state yet or a dead one as gone, the same way the actor list does. Creature casts (MobHandCast) also track ghosts now instead of flying at a stale point.

**Where:** src/game/mob_archery.cpp:256

**What:** A peer's player is targeted by actor id kPlayerActorBase + sessions + j. StepDraw re-tracks the target every tick through MobTargetNow -> TargetNow, which resolves only this machine's sessions and real mob ids. For a ghost it returns false, so the draw is aborted as 'the target is gone' on its first step. The archer draws, lets down, and repeats without ever loosing.

**Trace:**
- src/game/session.cpp:2905-2914 ghosts are appended to the actor list at index sessions + j
- src/game/mob_archery.cpp:256 `if (!TrackTarget(...) && d.targetId != 0) return abort("the target is gone");`
- src/game/mob_archery.cpp:149-161 TrackTarget -> MobTargetNow
- src/game/demon_cast.cpp:740-751 TargetNow loops local players, then mobs.IsAlive(id)
- src/game/mob.cpp:29399-29402 IsAlive matches mob.id_ only
- src/game/mob.cpp:4577-4580 FindCombatantById does resolve the player band, but is unused here

**Failure scenario:** In two-machine co-op, a host-owned archer picks the client's player. Each cadence it starts a draw (BeginDraw ignores TrackTarget's result) and aborts the next tick. Nothing is ever shot at the client, and the archer profile keeps it from switching to melee while it has arrows.

**Suggested fix:** In TargetNow, resolve player-band ids via w.mobs.FindCombatantById(id) (index-aligned with the actor list including ghosts) and use that avatar's position. Alternatively, keep the draw on brain.targetPos while brain.targetId == d.targetId.

**Verifier's note:** I read TargetNow, MobTargetNow, TrackTarget, StepDraw, IsAlive and the actor append. I did not verify the melee-suppression detail of the archer profile; the core abort path holds without it. The casters only degrade to a stale point, so this is archer-specific.

### AR-3 (MED) Anything that clears the held bit mid-draw (stun, talk, inventory, pause) looses the arrow

**Status:** FIXED on branch `audit-1014` in `eaafce8`. Confirmed: the player's loose fired whenever the bow button read up with bowDrawTicks > 0, so a stun (command zeroed), an active conversation or an uncaptured mouse all launched the arrow. A new held bit, TB_HANDS_AWAY (1u<<24, kTickInputVersion bumped from 9 to 10), carries the uncapture into the command; main.cpp sets it when !captured && !g_shotArchery. PhaseG now computes letDown = avatar.Stunned(tick) || s.talk.active || Held(TB_HANDS_AWAY), and when it is true it stops the draw clip and zeroes bowDrawTicks without a Launch, so the arrow stays in the kit, matching the NPC's let-down.

**Where:** src/game/session.cpp:2335

**What:** The player's loose is triggered by the bow button being absent while bowDrawTicks > 0. Every path that clears the held mask without the player letting go therefore fires the arrow at the current draw speed. Those paths are a stun, an active conversation, uncapturing the mouse (inventory, Esc, alt-tab) and resuming from pause. The NPC side treats a stun as letting the string down, arrow kept.

**Trace:**
- src/game/session.cpp:2319-2320 draw only while ti.Held(bowBtn)
- src/game/session.cpp:2335-2341 else-branch = THE LOOSE, w.arrows.Launch
- src/game/session.cpp:6204-6210 stunned -> p.ti.held = 0
- src/main.cpp:15836 `mouseL = captured && ...` -> :15891 feeder.Hold(TB_ATTACK, mouseL)
- src/main.cpp:15978-15981 pause Cancels edges only; the held-bit trigger is untouched
- src/main.cpp:15906 handsUp = tool == kToolMelee, so the draw is not cancelled by a hands change

**Failure scenario:** The player holds a full draw and opens the inventory (or presses Esc, or alt-tabs). The next tick launches a full-speed arrow wherever the camera points. A shock stun mid-draw does the same, and so does starting a conversation while drawn, which shoots the villager at point-blank.

**Suggested fix:** Distinguish an interruption from a release. When the avatar is stunned, s.talk.active is set, or a frame-level 'hands interrupted' flag (uncaptured or paused) is set, cancel the draw without a Launch: stopDrawClip, bowDrawTicks = 0, arrow kept. This matches the NPC's let-down.

**Verifier's note:** I read the loose branch, the stun zeroing, mouse capture gating, the pause Cancel and the handsUp source. I did not trace dialogue.cpp's ordering against PhaseA; the stun and uncapture paths stand on their own.

### AR-4 (MED) ArrowFlights/MobArcheryWorld are never cleared on load or F7; stale stuck ids can delete new items

**Status:** FIXED on branch `audit-1014` in `348cec7`. Confirmed: ArrowFlights::Clear and MobArcheryWorld::Clear had no caller. Flights, NPC draws and the stuck-arrow cap's global ids therefore survived F7 and LoadWorld, and the cap could DestroyBody an unrelated new-world body under a reused serial. The F7 regen site and the successful-load site now call tickCtx.arrows.Clear() and tickCtx.archers.Clear(). I did not run EndDraw first, because by that point the mobs have already been reset or replaced and a stale id could find a loaded mob. The teleport path was left alone: debris is not reset there, so stuck ids stay valid, and flights expire out of window.

**Where:** src/game/arrows.h:230

**What:** ArrowFlights (live flights, plus stuck_, the global ids enforcing the stuck-arrow cap) and MobArcheryWorld are never reset; ArrowFlights::Clear() has no caller. After a load or F7 regen, DebrisSystem::Reset restarts body serials at 1, so stale ids in stuck_ can name the loaded world's bodies. The cap eviction then DestroyBody()s an unrelated registered ground item. In-flight arrows and NPC draws also carry into the loaded world, so the post-load state is not a function of the save.

**Trace:**
- src/game/arrows.h:230-234 Clear() exists, and no caller exists in src/
- src/main.cpp:15473-15479 F7: debris.Reset(); mobs.Reset(); arrows untouched
- src/main.cpp:15661-15665 load: only grenades and fluid cleared
- src/phys/debris.cpp:732 nextSerial_ = 1
- src/game/arrows.cpp:590-597 evict stuck_.front() -> HandleOfGlobalId -> ground->Find -> DestroyBody

**Failure scenario:** The player sticks 48 arrows, then quick-loads. Restored and new bodies get serials from 1 again. Each further stuck arrow evicts a stale id that may now name a restored dropped item or stuck arrow, which is then destroyed, and the restored arrows escape the cap. An arrow loosed just before the load keeps flying in the loaded world while the loaded kit still holds it, which duplicates it.

**Suggested fix:** Wherever debris and mobs are reset (LoadWorld success, F7, any other world reset), call arrows.Clear() and archers.Clear(), running EndDraw on live MobDraws first. Alternatively, tag stuck_ ids with a debris reset epoch.

**Verifier's note:** I grepped for every Clear/assignment and read both reset sites and the eviction. I did not re-check whether a save restores nextSerial_; the collision holds either way, because the pre-load ids and the loaded world's ids share one serial space.

### AR-5 (MED) A staggered NPC archer still looses its in-progress draw; StepDraw never checks the stagger

**Status:** FIXED on branch `audit-1014` in `1324315`. Confirmed: StaggerBlocksAttack only cleared new shoot requests, and StepDraw checked stun but not stagger, so a draw already under way loosed during stagger_head/chest. StepDraw now does `if (m->StaggerBlocksAttack()) return abort("staggered");` right after the stun abort: the string is let down and the arrow kept.

**Where:** src/game/mob_archery.cpp:244

**What:** A stagger is meant to block attacks ('neither swings, casts nor looses'), and a head stagger ends the swing in flight. StepDraw never consults the stagger, so a draw already under way keeps counting and looses at looseAt while the archer plays stagger_head or stagger_chest. Only new shoot requests are blocked.

**Trace:**
- src/game/mob.cpp:8314-8316 StaggerBlocksAttack clears new attack/cast/shoot requests only
- src/game/mob.cpp:34694-34698 head stagger resets stroke_ and shield, not the draw
- src/game/mob.h:2682 Stunned() reads stunUntil_ only, which stagger does not set
- src/game/mob_archery.cpp:242-256 no stagger check
- src/game/mob_archery.cpp:267-283 the loose launches the arrow

**Failure scenario:** The player puts an arrow in a drawing NPC archer's head. It plays stagger_head with its hands on its head, yet a few ticks later an aimed full-speed arrow leaves its bow hand.

**Suggested fix:** In StepDraw, `if (m->StaggerBlocksAttack()) return abort("staggered");`. The string is let down and the arrow kept, mirroring the stroke reset.

**Verifier's note:** I read StepDraw, its single caller, StartStagger, and Stunned/StaggerBlocksAttack. No upstream guard exists.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | PARTIAL | Stuck, dropped and strapped arrows are ITMS ground items, and ammo rides the saved kit. Embeds and in-flight arrows are dropped on save (embeds are a known issue), stuck arrows come back non-kinematic (debris.cpp:8121), and ArrowFlights/MobArcheryWorld are never reset on load (arrows.h:230 Clear() has no caller). |
| deterministic | PARTIAL | Flights are 24.8 fixed point, stepped once a tick in TickAuthority (session.cpp:6341) with hash seeds, and NPC draws use rng::Hash3. However, flights, draws and stuck_ ids survive LoadWorld/F7 (main.cpp:15661), so the post-load world depends on pre-load history. |
| sleeps when idle | OK | ArrowFlights::Tick runs only when Live()>0 (session.cpp:6341). MobArcheryTick returns early with no requests and no draws (mob_archery.cpp:322). Bounds: 64 flights for 300 ticks each, 48 stuck arrows, 8 embeds per creature, 64 draws. |
| cpu mirror | GAP | FlightSolidAt treats out-of-window cells as solid (flightprobe.h:30), so a flight leaving the window becomes a floating stuck item. In-window cells outside the mirror correctly use the fetch cache plus TerrainHeight with prefetch (arrows.cpp:405-417). |
| mutation queue | OK | No voxel writes: channel gore goes to the tick's out.spawns (arrows.cpp:489-490), and every way an arrow ends is a debris body or a rig slot. The G pull-out is a documented frame-layer take (DESIGN.md:9041). |

## Coverage (as reported by the finder)

**archery.** Static only, at 20aa002 (snapshot worktree). Read in full: src/game/arrows.{h,cpp}, mob_archery.{h,cpp}, flightprobe.h, renderonly.h, worlditems.h, pierce.{h,cpp} (PierceAlongRay, Mob::PierceLimb, SpellMatterStrike), phys/kerf.h PierceChannel/PierceKeep, meleehit.h, and the archery-related sections of DESIGN.md (9033-9063). Traced end to end:
- session.cpp: PhaseA's bow walk slow, PhaseG's bow block, ResolveHands, the stun zeroing and TickAuthority's phase order (MobArcheryTick after PhaseH, arrows after PhaseK).
- dialogue TickSession command zeroing.
- main.cpp: mouse capture/TB_ATTACK feeding, pause cancel, G take / PullArrowToKit, the health-inspector pull, the render-only wiring (AppendRenderBodies/PresentDrawnBow), the LoadWorld/F7/teleport resets, the first-person hide mask and R reload.
- mob.cpp: EmbedArrow, PullArrow, RemoveAppendedSlots, DetachLimb/ShedGearBeforeDetach, ReleaseRigToDebris, AdoptDeadAvatar, Die/EnterDeadRagdoll, DriveWornShells, LimbRole, TotalHp, AI arm/stagger gating, StartStagger, IsAlive/FindCombatantById, Reset.
- demon_cast TargetNow/MobTargetNow; ai_behavior ranged scoring/Shoot request/LeadPoint/drawing arbitration; remoteplayer actor band.
- debris: Reset/serials/GlobalIdOf/StrapBody/SettleBodies/PostStep cull/RestoreBody/LoadBodyGroup.
- persist ITMS/DBRS; stream window centring/hysteresis; melee.cpp's bow/ammo loader and ApplyRemotePierceHit; the bodyreg render-only diff; the pose.cpp diff of 9382bd2.

Checked and NOT reported:
- Shield/held-item hits break the arrow (DESIGN: 'on a held item nowhere').
- A stuck arrow stays kinematic when its terrain is dug out (worlditems.h says it does not fall until taken).
- The G pull is a frame-layer take (DESIGN.md:9041).
- Penetrator round-trips the wire as a POD.
- The embed cap and per-call bounds hold.
- The render-only bodies feed no sim buffer.
- Flight math is integer, and the float aim is single-authority.

Seen but not raised (unverified or minor):
- Stuck arrows restore after load as sleeping DYNAMIC bodies (RestoreBody only allows kinematic for followers, debris.cpp:8121), so they fall when woken.
- Peers never see a remote player's draw or any arrow in flight (no wire field), a presentation gap.
- The shooter's ghost and the owner each price a routed Pierce, so an owner-side embed plus a shooter-side 'through' could duplicate an arrow; I did not trace a concrete divergence.
- ArmArcher can leave the right hand stowed and return -1 if the second Stow fails, contrary to its 'nothing changed' contract.
- A burnt or carved arrow slot is pulled back as a whole arrow.

NOT audited:
- The selftest gates (arrow, archer, pierce, wound).
- overlay.cpp's HUD bow meter (84e30d3) and inventory_ui beyond the arrow rows.
- The clip JSONs and gen_archery_clips.py.
- PresentDrawnBow's geometry and pose.cpp's pitchFollow maths.
- DebrisSystem::PierceBody internals.
- bugreport changes in e8bf0c9.
- df7fdbd's prop-bleed change, read only via its call sites.

