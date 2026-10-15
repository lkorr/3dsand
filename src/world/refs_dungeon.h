// refs_dungeon.h — DUNGEON ENTITIES: spawners, signals (levers, pressure
// plates), gates, traps, keys and regions -- the gameplay half of a dungeon
// (docs/PLAN_dungeons.md §3.7, package D3; DESIGN.md §16.D3).
//
// Every one is an ordinary ref kind (world/refs.h): authored by name in a
// ref group -- a dungeon's assets/dungeons/<id>/refs.json (dungeon-local
// frame, world/dungeon_refs.h) or any map refs file -- activated while its
// chunk is in the window, and REMEMBERED ONLY AS ITS DELTA (the save's
// 'REFS' records): no entity state is ever kept in voxels. Every voxel any of
// them changes goes through the tick's OpBatch (CLAUDE.md rule 3). All of
// them cost nothing while out of the window (rule 2): the kinds' one tick
// returns at once when none is active.
//
// THE FRAME OF A BOX. A box prop `size: [sx, sy, sz]` (cells) runs from the
// ref's pos along the ref's RIGHT (heading yaw + 90) for sx, UP for sy, and
// FORWARD (heading yaw) for sz -- so a box turns with its ref and with a
// dungeon's placement. A `region` prop is either such a size, or the NAME of
// a `region` / `plate` ref (exact id, a sibling, or "<group>/<name>").
//
// SIGNALS. A named signal belongs to its GROUP ("dungeon/x" + "gate_a"): two
// dungeons never share one. Its level is ON while any emitter of that name
// in the group is on. Emitters:
//   lever   props: signal (default: the lever's own name), startOn (bool,
//           default false), pulse (seconds; 0 = a toggle, default 0;
//           > 0 = a momentary pull that springs back), label (prompt text).
//           Use = pull it. Delta: its state.
//   plate   props: signal (default: its own name), size ([8, 2, 8]), latch
//           (bool, default false: once pressed it stays on, saved), who
//           ("any" | "player" | "mob", default "any"). ON while a body's
//           feet are in its box.
// Receivers name the signal with props.on and may set props.invert:
//   door    (refs_doors.h) open while on, shut while off; not by hand.
//   gate    a PORTCULLIS of voxels that slides up while on, down while off.
//   trap    fires on the RISING edge.
//
// gate    pos = the bottom cell at one end of the bars' front layer; yaw =
//         the face (like a door); props: width (20), height (30), thickness
//         (1), material ("iron"), pattern ("bars" | "solid"), spacing (3),
//         speed (ticks per row, 2), on, invert. Built ONCE by IfAir CellOps
//         the first time it activates (the shell need not carry it); each
//         step of the slide is the DIFF between two row offsets, cleared by
//         conditional clears and filled by IfAir writes, so it never eats
//         what the world put in the way. Delta: raised rows + built.
//         It must HANG FROM something solid over its top row (a lintel,
//         the ceiling): lifted bars with nothing above them are an island,
//         and island detection drops them as debris.
// trap    props: type (collapse_floor | spikes | dart | flood | fire_vent),
//         on (a signal; a plate's name works -- a plate's default signal is
//         its name), region, once (default true for collapse_floor/flood),
//         cooldown (seconds, 3), plus per type:
//           collapse_floor  every SOLID cell in the region whose material
//                           names a powder `rubble` twin (materials.json:
//                           stone -> gravel, ...) becomes that twin: a
//                           conditional clear one tick, the twin IfAir the
//                           next, read off the fetch cache. from (only this
//                           material), to (override the twin).
//           spikes          damage (25 hp, Blade) to every body in the
//                           region; spikes of material ("iron") rise IfAir
//                           from the region's floor for duration (1 s).
//           dart            one bolt from pos along yaw: damage (15), range
//                           (120 voxels); the first body on the line BEFORE
//                           THE FIRST WALL is hit (D9: the line is walked
//                           through the CPU mirror, collision classes; up to
//                           8 solid cells the launcher starts in are its
//                           housing; Unknown = open, as for projectiles); a
//                           micro particle shows it, and dies at the wall.
//           flood           the region filled with liquid ("water") IfAir,
//                           ONCE: a bounded volume, never a source.
//           fire_vent       fire ("fire") IfAir at hashed cells of the
//                           region for duration (2 s).
//         Delta: fires + done.
// key     an item ref: props.item (an item id of kind key) or props.keyId
//         (the first key item that unlocks it). Use = take it into the kit
//         (bag, else hotbar); taken is the delta. Not drawn: put it on a
//         prop (a pedestal, a corpse) or in a chest's loot instead.
// region  a named box: props.size. Inert; traps and spawners name it.
// spawner props: mobs ([{base, weight}], or `base` for one), count (alive
//         at once, 1..16, default 1), radius (metres, 1.5), respawn
//         ("never" | "ticks:N" | "onLeave", default never), schedule,
//         behavior, trigger ("always" | "onEnter(<region>)"), outfit,
//         weapon, and any ROLE a schedule row names. Its creatures carry
//         derived ids (Mob::RefId "<spawner id>/s<slot>"; a corpse is
//         renamed "<id>/s<slot>#dead<serial>" so it never shares the slot's
//         id with the replacement) and walk its
//         schedule (refs_npc.h StepScheduled). Every decision -- which
//         base, where -- is rng::Hash3(RefHash(id), serial): the same
//         spawner spawns the same creatures at the same places on every
//         run. At most one spawn per spawner per tick, never more than
//         `count` alive. Delta: serial, triggered, per-slot state.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "world/refs.h"

namespace refs {

// Called from RegisterAllKinds: lever, plate, gate, trap, key, region,
// spawner.
void RegisterDungeonKinds();

// ---- pure helpers (the kinds, the editor, the gates) ---------------------------
// The inclusive cell AABB of a box `size` in r's frame (see THE FRAME OF A BOX).
void BoxOfRef(const Ref& r, IVec3 size, IVec3& lo, IVec3& hi);
// A `region` prop resolved: false + `why` when it names nothing.
bool RegionOf(const RefStore& s, const Ref& r, IVec3& lo, IVec3& hi, std::string* why = nullptr);
// The level of `signal` in `group` right now (levers' deltas, plates pressed
// this tick or latched).
bool SignalLevel(RefStore& s, const std::string& group, const std::string& signal);

// What spawn number `serial` of this spawner is: the base and the min-corner
// cell it is placed at (the footprint centred on a point within radius).
// `attempt` redraws only the point (the base is the serial's alone): the
// spawner tries attempt 0, 1, .. kSpawnTries-1 and takes the first whose body
// column is open over support in the fetch cache, else its own pos.
struct SpawnPlan {
  bool ok = false;
  std::string base;
  Vec3 foot{};
};
SpawnPlan PlanSpawn(const Ref& spawner, uint32_t serial, uint32_t attempt = 0);

// ---- status (gates, the References page) ------------------------------------------
struct GateStatus {
  int raised = 0, height = 0;   // rows lifted (height = fully open)
  bool wantOpen = false;
  bool built = false;
  uint32_t steps = 0;           // rows moved since activation
};
bool GateStatusOf(const std::string& id, GateStatus& out);

struct TrapStatus {
  std::string type;
  uint32_t fires = 0;
  bool done = false;            // a once-trap that has run its course
  uint32_t cells = 0;           // collapse: cells converted; flood/fire/spikes: cells written
  uint32_t ops = 0;             // CellOps emitted in total
  uint32_t hits = 0;            // bodies damaged
  float wallT = -1.0f;          // dart: range the last bolt flew before a solid cell (-1 = not fired)
  std::string note;
};
bool TrapStatusOf(const std::string& id, TrapStatus& out);

struct SpawnerStatus {
  uint32_t serial = 0;              // spawns made in total
  bool triggered = false;
  std::vector<std::string> slots;   // the creatures' derived ref ids (MobSystem::FindMobByRef)
  std::vector<std::string> bases;   // what each slot last spawned
  std::string note;
};
bool SpawnerStatusOf(const std::string& id, SpawnerStatus& out);

// Forget every live entity's scratch (a gate's reset; a load resets through
// the kinds' deactivate).
void ResetDungeonEntities();

}  // namespace refs
