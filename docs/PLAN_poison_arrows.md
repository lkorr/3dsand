# PLAN — poisoned arrows and the quiver (2026-10-15)

**STATUS: IMPLEMENTED (branch `quiver`).** As built is DESIGN.md "The quiver and
poisoned arrows"; the deviations from this plan are listed in §9.

Owner ask: "apply poisons to arrows; we can do it to weapons, but arrows are a
stackable item / in a quiver." Decided with the owner in the 2026-10-15 planning
session; every decision below is theirs unless marked (proposal).

## 0. What exists today (the gaps)

- Coats live on an item's exact voxel lattice (`game/itemcoat.h`), stored in
  `ItemInstance::damage.shells[0].lattice`. Non-empty lattice => not `Plain()`
  => never `StacksWith` anything.
- `ArrowFlight` carries only the item NAME. `KitTakeAmmo` returns a bool and
  throws the instance away. `PierceAlongRay` / `Mob::PierceLimb` never call
  `CoatOnContact`; `CoatHitKind` is `Cut | Blunt | Bite`. **A coated arrow would
  deliver nothing even if one could be made.**
- No quiver item. "Quiver" = hotbar stacks before bag stacks in `KitTakeAmmo`.
  No back socket on the human rig (`held_right`, `fist.L/R`, `jaws` only).
- The item stage (`StageTakes`) takes Melee + worn pieces, not Ammo.
- `arrow.vox` = 92 voxels, 52x3x3 (12 B each => ~1.1 KB per coated arrow).

## 1. Decisions

| # | Decision |
|---|---|
| D1 | **Every arrow is its own `ItemInstance` with its own lattice** (the sword model). The QUIVER is what keeps the bag tidy: one slot however varied its arrows are. Plain arrows inside a quiver still merge into one counted entry (`StacksWith`), so a clean quiver of 24 is one record. |
| D2 | **The bow draws a RANDOM arrow from the worn quiver**, weighted by count: `hash3(seed, tick, shooterActor)` over the quiver's entries — deterministic, replay- and peer-identical. Keep poisoned and plain arrows apart by keeping separate quivers. |
| D3 | **A real quiver item**, new `ItemKind::Quiver`, worn in ONE new `EquipSlotId::Back` (appended before `Count`; save order). Switch quivers by re-equipping: from the inventory, or **Q/E on a quiver in the hotbar puts it on the back** (a swap, like Q/E into a hand) instead of into a hand. |
| D4 | **Visible on the back**: the quiver's art on a new `back` socket, plus render-only fletching rods for the arrows it holds (one per arrow up to a draw cap). Render-only, never hashed. |
| D5 | **Quiver required**: no worn quiver (or an empty one) => the bow does not draw. Loose arrows in the bag cannot be shot. |
| D6 | **Capacity is data**: `quiver.capacity` per quiver item in `items.json`. Ship one back quiver at 24. |
| D7 | **Loading**: drag a loose arrow stack (or one arrow) onto a quiver (worn or in the bag) — it takes what fits, the rest stays. **Pickups fill the worn quiver first**, bag as overflow: G on a stuck arrow, an embedded one (`PullArrowToKit` / `PullArrowSlotToKit`) or a ground item. |
| D8 | **Applying poison: brush one, copy to N.** Open a QUIVER on the item stage: it shows one representative arrow; brush it with the FLASKS row's vessel; "apply to N / all" copies that arrow's coat words voxel-for-voxel onto N arrows in the quiver through the body-coat writers (`AddBodyStain` precedence, on top of what each already wears), costing N x the brushed spend from the vessel; refused (partial N) when the vessel runs dry. |
| D9 | **A recovered arrow comes back wearing the target's coat**: the pierce exchange (§3) moves the arrow's coat into the wound channel AND the wound's fluid onto the head, exactly as a blade. While embedded it is a rig slot and its coat ticks like any limb; on the ground it is a world item and dries/exchanges like one (`CaptureGroundCoat`). Pulled or picked up, the lattice comes with it. |
| D10 | **Frozen in the quiver**, like the bag (owner rule 2026-10-01): nothing ticks arrows in a quiver. |
| D11 | **The quiver is not damageable for now**: its shell is not carved, burnt or coated. Breaking + spilling arrows is a follow-up. |

## 2. Data model

- `ItemInstance` gains `std::vector<ItemInstance> held;` — what a quiver
  holds. ONE level deep (an arrow holds nothing; a quiver inside a quiver is
  refused at every insert). Bounded on read by the def's capacity / a hard
  ceiling. `Plain()` is false when `held` is non-empty, so a loaded quiver
  never stacks.
- Byte format: `kItemFmtQuiver` after `kItemFmtStopper`: the held list
  (count, then each entry through `WriteItemInstance` at the same fmt). Bumps
  MOBS `kSaveVersion`, net protocol, and PLYR / ITMS (own append-only layouts
  in `game/persist.cpp`). `RemapItemInstance` recurses into `held`.
- `ItemDef::QuiverSpec { bool has; int capacity; std::string ammo; }` — `ammo`
  = which item it takes (default `arrow`), so a bolt case is a data row.
- Coat storage: reuse `damage.shells[0].lattice` as-is (every path already
  carries it; zero new serialization). **(proposal, revises what I said in
  the session)** — a sparse "cells that differ" encoding is a contained
  follow-up ONLY if the net measurement asks for it: worst case is ~33 KB for
  a full quiver of 30 coated arrows, saves are fine either way.

## 3. The coat in flight and on the hit

1. `KitTakeAmmo` -> `KitDrawArrow(Kit&, const ItemLibrary&, seed) ->
   ItemInstance` (empty = no shot): draws from the worn Back quiver by D2.
   `KitAmmoCount` counts the worn quiver only (D5). Same function for the
   player (session.cpp PhaseG bow block) and NPCs (`mob_archery.cpp`,
   `MobShotsLeft`).
2. `ArrowFlight` carries the `ItemInstance` (name + lattice), not the name.
   `AppendRenderBodies` draws its coat (the render body's stain words).
3. On a hit that gets IN (embedded or through): `CoatOnContact` with a new
   `CoatHitKind::Pierce` — the striker is the arrow's lattice (head cells,
   located along the art's +x axis from `ArrowSpec::tip`, as `edgeU` locates
   a blade's), the target the voxels the channel went through
   (`PierceResult`). Phys side: `phys/coatcontact.h` gets the channel as the
   contact set. A glance exchanges nothing (same as the flat damage rule).
4. `Mob::EmbedArrow` takes the instance, writes its lattice coats onto the new
   follower slot; `PullArrowToKit` / `PullArrowSlotToKit` capture the slot's
   lattice back (`CaptureLimbToItem`'s rule) and insert into the worn quiver
   (D7). Ground stick / drop / strap: `DropItemToWorld` already carries an
   instance's coat; pass the instance through.
5. Peer route: the `MeleeHitKind::Pierce` record already carries the item —
   carry the instance (it is an `ItemInstance` on the wire already via
   `WriteItemInstance`).

**Calibration, not rules:** the bodkin is ~10-15 cells; expect a much smaller
venom seed than a blade's 7-20. The `arrow` gate prints the seeded count; if
it is small, report numbers to the owner (projectile-wave precedent), don't
add a multiplier.

## 4. The quiver item

- `items.json`: `back_quiver`, kind `quiver`, `quiver: {capacity: 24, ammo:
  "arrow"}`, art `.vox` (leather tube) + grip context for the `back` socket.
- `assets/mobs/human.json`: a `back` socket on the torso (races inherit it;
  check the naga / goblin / robot races' torsos sit it sensibly —
  `--shot-mob`).
- Worn on the body through the SAME borrowed-slot path a held item uses
  (`Mob::EquipItem`), not a fitted shell: it is a rigid prop. Not carvable
  (D11): mark its slot so melee / fire / coat passes skip it.
- Fletching rods: render-only bodies (game/renderonly.h, like
  `PresentDrawnBow`'s nocked arrow), one per held arrow up to ~12 drawn, fanned
  in the quiver mouth.
- Q/E: when the selected hotbar stack is a Quiver, the move target is
  `EquipSlotId::Back`, not a hand. Inventory drag onto the Back slot works by
  the slot table (`accepts {ItemKind::Quiver}`).
- Drag arrow stack onto a quiver: a new `MoveResult`-reporting kit op
  (`KitLoadQuiver`), refuses WrongKind / Full out loud.
- HUD: the bow's ammo readout shows the worn quiver's count; empty / none says
  why the bow won't draw.

## 5. The item stage on a quiver (D8)

- `StageTakes`: also a Quiver with at least one arrow. The stage opens on a
  REPRESENTATIVE arrow (first coated entry, else the plain stack), split out
  as its own instance on the first stroke.
- Strokes are the existing tick input (`PlayerSession::itemStroke`) with the
  quiver's `KitRef` + `heldIndex`.
- "Apply to N" is a new tick input (`itemStageCopy {quiver ref, source index,
  n}`): copies the source arrow's coat word-for-word onto N other entries
  (splitting plain arrows out of the counted stack), through `AddBodyStain`,
  spends N x (source arrow's total coat in eighths, via the stroke's spend
  rate) from the vessel, stops at a dry vessel and reports how many were done.

## 6. Migration (D5 makes this one landing)

- Starting kit: a worn `back_quiver` holding 24 arrows (was 30 loose).
- NPC archers: `behaviors.json` `ranged.quiver` becomes the count loaded into a
  worn quiver at spawn (`mob_archery.cpp` ~396); loot keeps the quiver with
  its arrows.
- Gates: `arrow`, `archer`, `pierce` fixtures equip a quiver. New gate
  `poison-arrow`: brush + copy 5 arrows with venom (vessel spend exact), shoot
  one into a human torso, assert (a) venom seeded along the channel, (b) the
  pulled arrow wears blood + leftover venom and returns into the quiver, (c)
  the draw sequence is identical over two runs (D2 determinism), (d) save /
  load round-trips a mixed quiver bit-exact.

## 7. Packages (proposal)

| Pkg | What | Depends |
|---|---|---|
| A | `ItemInstance::held`, `QuiverSpec`, formats (MOBS/PLYR/ITMS/net), `Back` slot, `KitDrawArrow`, Q/E-to-back, `KitLoadQuiver`, pickups into quiver, starting kit + NPC migration, gate fixtures | — |
| B | Flight carries the instance; `CoatHitKind::Pierce` exchange; embed / pull / ground carry the lattice | A (instance in hand) |
| C | Quiver art + `back` socket + fletching rods + HUD readout | A |
| D | Item stage on a quiver + apply-to-N input | A |

B, C and D are independent of each other once A lands.

## 8. Open / deferred

- Quiver damage + spilling (D11, deferred).
- Sparse coat encoding (only if net asks).
- Venom dose per arrow: measure in `poison-arrow`, report.
- Bolts / thrown darts: `QuiverSpec::ammo` already allows them; no content.

## 9. As built (deviations from the plan above)

- **No authored `back` socket.** Every generated character carries its own
  `sockets` diff from mobgen.js, so an authored socket on the human would have
  been thirty character re-bakes (and a collision with a concurrent face
  re-bake). The back point is DERIVED at runtime from the `torso` box
  (`Mob::SocketWorldXf`, the `MirrorHeldSockets` pattern); an authored `back`
  socket still wins. Fletching is the arrow's own art (up to 12 tails), not
  rods.
- **Copy cost** is the brush's own price per level per cell (the radius
  cancels out of `PourBrushCellsPerSec` / `kStrokeAmount` / pi r^2), not a new
  tuning knob.
- **Copy pattern** is the coat of WHAT THE FLASK POURS only, so copying a
  bloody, brushed arrow does not copy its blood.
- **NPC quivers** ride the gear list (`Mob::CaptureGear` / `ApplyWireGear`,
  `equipSlot` Back) and the loot list (`LootPiece::Kind::Back`).
- **Peers:** the owner trades coats from `MeleeHit::embedInst`; the shooter
  does not trade with a ghost locally.
- **No unload gesture** (not asked for): shoot them, or drag the quiver off.
