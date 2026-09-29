// refs_game.h — where references meet the game: the tick, the use verb and
// the save (docs/PLAN_world_editor.md §2.1 / §2.5, DESIGN.md §16).
//
// Kept apart from refs.h so the store stays a plain data structure a tool or
// a gate can use without the tick's headers.

#pragma once

#include <span>
#include <string>

#include "math3d.h"
#include "sim/worldio.h"
#include "world/refs.h"

struct TickAuthorityCtx;
struct SessionTick;
struct OpBatch;
class MobSystem;

namespace refs {

// How far a player may use a thing from, eye to its use point, in world
// voxels: the pickup reach (main.cpp kPickupReach) -- "within arm's length and
// a pace", the same human dimension.
constexpr float kUseReach = 24.0f;

// ---- THE TICK (called from TickAuthority when TickAuthorityCtx::refs is set)
//
// Activation (RefStore::Update against the window), then every session's use:
// a TB_USE press whose `useRef` names an ACTIVE, usable ref within reach of
// that player's eye runs the kind's onUse. Refusals are recorded, never
// silent (RefStore::Uses). Runs between phases G and H -- after the player
// phases, before mobs.PreTick -- so an NPC a ref spawns ticks the same tick.
void TickRefs(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
              OpBatch& out);

// ---- THE PROMPT (frame side) ----------------------------------------------
//
// The usable active ref nearest along the look ray (from `from` along unit
// `dir`) whose use point lies within its kind's useRadius of the ray and
// within kUseReach of `eye`. `prompt` gets the kind's text. Null = nothing.
// What the frame shows here is exactly what a press sends (the hash), so the
// tick acts on the promised thing.
const Ref* PickUsable(RefStore& s, RefCtx& ctx, Vec3 from, Vec3 dir, Vec3 eye,
                      std::string* prompt);

// ---- THE SAVE ('REFS', Region scope) ----------------------------------------
//
// One record per ref that has a delta, bucketed by the ref's AUTHORED pos.
// reset = RefStore::ResetForLoad; saveRecords = RefStore::SaveDeltas;
// loadRecord = RefStore::AcceptDelta (an id the map no longer has: Dropped,
// with a warning). Registered by game/persist.cpp MakeEntityIO.
EntitySection MakeRefsSection(RefStore& s, MobSystem* mobs);

}  // namespace refs
