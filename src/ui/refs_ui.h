// refs_ui.h — F1 -> World -> References (docs/PLAN_world_editor.md P1).
//
// The map's references as a person works with them: a list grouped by group
// file (filter by kind, search by text), click a row to select it, "fly to"
// to go and look, an inspector that edits kind / base / pos / yaw / props and
// writes the change straight back to assets/worldmap/<map>/refs/<group>.json,
// "place" to add a new one at the crosshair or your feet, delete, and the
// warnings list (every one names the file, the ref id and the field).
//
// Every edit goes through the plain authoring functions in world/refs.h
// (refs::Place / Move / SetProp / SetField / Delete) -- the same ones the
// in-game editor's commands will wrap (P5) -- so there is no action here an
// agent's script cannot also take, and the reverse.

#pragma once

struct UIState;

// Draws the page body (the caller has opened the section). Reads/edits
// `s.refs`; raises `s.refFlyTo`.
void DrawRefsPage(UIState& s);
