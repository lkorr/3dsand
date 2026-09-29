// editor_ui.h — THE IN-GAME EDITOR MODE (F8; docs/PLAN_world_editor.md P5,
// DESIGN.md §16.P5, docs/EDITOR_GUIDE.md §13).
//
// Press F8 and the game stops being played and starts being built: the
// simulation PAUSES (the banner says so; each edit advances it exactly one
// tick so the change lands through the MutationQueue), the camera comes off
// the body and flies (hold the right mouse button to look, WASD / Q E to
// move, the wheel sets the speed, F frames the selection), the cursor is
// free, and a toolbar, an inspector and a status bar come up. Every change
// the editor makes is an editor::Command (editor/commands.h): Ctrl+Z / Ctrl+Y
// undo and redo it, and the session is journaled to build/editor/.
//
// Two things are edited:
//   * REFERENCES (the map's refs/*.json): select, move with the axis gizmo,
//     turn with the yaw ring, place from a palette, duplicate, delete, link
//     waynodes, edit props in the inspector (the P6/P7 kind panels included).
//     Written to the group file at once.
//   * A STRUCTURE (a house's asset): open a placed house and the voxel tools
//     come alive -- pencil, brush, box select with fill / hollow / shell /
//     replace / clear, copy / paste with turn and mirror, eyedropper, line --
//     plus its SLOTS (doors, beds, chests, markers, waynodes) with the same
//     gizmo. Edits are in memory (shown live in the world) until Ctrl+S
//     writes the .vox and .struct.json and re-stamps every copy of the house.
//
// This header is imgui-free (main.cpp includes it); the drawing is in the
// .cpp.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "math3d.h"

struct UIState;
namespace refs {
class RefStore;
}
namespace editor {
class Session;
}

namespace editor_ui {

class EditorMode {
 public:
  EditorMode();
  ~EditorMode();

  // `store` may be null (a harness map: structures by asset name only).
  // `groundAt(x, z)` = the terrain's top voxel y (World::TerrainHeight).
  void Init(refs::RefStore* store, const std::string& assetDir,
            std::function<int(int, int)> groundAt);
  void SetStore(refs::RefStore* store);

  bool Active() const;
  // F8 while playing: the camera starts where the player's eye is.
  void Enter(Vec3 eye, float yaw, float pitch);
  // F8 while editing: exits at once, or asks first when a structure has
  // unsaved edits (Save / Discard / Cancel).
  void RequestExit();
  // True once, the frame the editor has finished exiting (main restores the
  // pause state, the cursor and the player's view).
  bool TakeExited();

  // The editor's camera (main renders from it while Active).
  Vec3 CamPos() const;
  float CamYaw() const;
  float CamPitch() const;
  void SetCamera(Vec3 pos, float yaw, float pitch);   // (harness / fly-to)

  // "open in editor" from the References page (a structure ref id).
  void OpenStructureRef(const std::string& refId);

  // Per frame, inside the ImGui frame (after Overlay::Draw): input, the
  // world overlays, the panels. `fovY` = the render camera's.
  void Frame(UIState& ui, float dt, float fovY);

  // The live preview of the open house: world cells and their new material
  // (0 = air) since the last call. main.cpp queues them as cell ops through
  // the MutationQueue (PrefabPlacer::QueueWords) and runs one tick.
  std::vector<std::pair<IVec3, uint16_t>> TakeWorldCells();
  // A command changed something the world must catch up with (one tick).
  bool TakeWantsTick();

  editor::Session& Session();

  // ---- harness hooks (--shot-editor) ----
  // Pin the cursor at window pixels (x, y) (< 0 = the real mouse).
  void SetScriptedCursor(float x, float y);
  // Select a tool by its key number (1..8) and set the box selection (house
  // frame, the open structure).
  void SetTool(int key);
  void SetSelection(IVec3 lo, IVec3 hi);
  void SelectRef(const std::string& id);
  void StartPaste();
  std::string LastMessage() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> p_;
};

}  // namespace editor_ui
