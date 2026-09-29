// refs_doors.h — DOORS, CONTAINERS AND BEDS: the P6 reference kinds
// (docs/PLAN_world_editor.md §2.4 / §5 P6, DESIGN.md §16.P6).
//
// THE DOOR, in one paragraph. Closed, a door is nothing but voxels: the leaf
// is ordinary static cells in the wall (it collides, burns, sleeps and costs
// nothing). The `door` ref only NAMES that leaf -- a box of cells and the
// edge it hangs on. A use OPENS it: the box's solid cells are read off the
// fetch cache, cleared by conditional CellOps (the MutationQueue, rule 3), and
// the same voxels become ONE dynamic Jolt body hung on a HingeConstraint to
// the world whose position motor drives it to the open angle. A second use
// CLOSES it: the motor drives back to 0, and once the leaf is home and the
// box is clear in the grid, the body is destroyed and the EXACT captured words
// (material, palette nibble, stain) are written back by CellOps. A box that is
// not clear (something put in the doorway) makes the close WAIT and retry; a
// body in the doorway stops the swing itself. The open state, with the
// captured words, is the ref's save delta, so an open door survives a save,
// a load and the window leaving and coming back. A leaf that no longer
// matches what was hung (burned, cut, blown apart) stops being a door: the
// hinge is dropped, the pieces are ordinary debris, and the prompt says
// "Broken door".
//
// THE DOOR'S FRAME (all integers, world voxels):
//   pos    the leaf's BOTTOM cell on the HINGE side, in the leaf's FRONT layer
//          (the face it swings toward);
//   yaw    the direction the door OPENS TOWARD (heading, 0 = +Z, 90 = +X);
//          multiples of 90 only;
//   props  width (cells along the leaf, default 9), height (default 20),
//          thickness (default 1, extends BEHIND the front face), hinge
//          ("left" | "right", default "left": seen by someone standing on the
//          side it opens toward, facing the door, the hinge is on their left),
//          openAngle (degrees, default 95), locked (bool), autoClose (seconds,
//          0 = never).
//
// CONTAINER: a Bag owned by the ref. Initial contents from props.items
// ([{"item": "bread", "count": 3}, ...]); once anything is taken or put, the
// whole bag is the ref's delta and the props no longer matter. Opened by the
// use verb into the loot panel (take one / take all / put by drag).
//
// BED: an anchor. pos is the mattress cell where the HEAD lies, yaw points
// from head to foot, props.length (cells, default 18). The player's use says
// "Rest" and prints a line (sleeping through time is not in this slice); P7's
// villagers lie on BedAnchorOf.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "world/refs.h"

struct Bag;
struct Kit;
struct ItemInstance;

namespace refs {

// Called from RegisterAllKinds: `door`, `container`, `bed`.
void RegisterDoorKinds();

// ---- door geometry (pure: the kind, the References page's arc, the gates) ----
struct DoorGeom {
  bool ok = false;
  std::string why;            // when !ok: which field, in words a person can act on
  int width = 9, height = 20, thickness = 1;
  IVec3 along{1, 0, 0};       // the leaf runs from the hinge along this axis
  IVec3 face{0, 0, 1};        // ...and swings toward this one (the yaw)
  float openRad = 0.0f;       // the open angle, radians (positive)
  float openSign = 1.0f;      // the hinge-angle sign that swings toward `face`
  Vec3 hinge{};               // the hinge line at the leaf's bottom, world voxels
  IVec3 pos{};                // the ref's pos: hinge-side bottom cell, front layer
  IVec3 lo{}, hi{};           // the leaf box, inclusive cell AABB
  bool locked = false;
  float autoCloseSec = 0.0f;
  std::vector<IVec3> Cells() const;   // every cell of the box, i-j-k order
  Vec3 Center() const;                // the closed leaf's centre
  // The leaf's far (latch) edge at mid-height, swung `rad` about the hinge.
  Vec3 LatchAt(float rad) const;
};
DoorGeom DoorGeometry(const Ref& r);

enum class DoorPhase : uint8_t {
  Closed,    // voxels in the wall; nothing live
  Opening,   // a use asked; waiting for the leaf's chunks in the fetch cache
  Open,      // the leaf is a hinged body (swinging out, or standing open)
  Closing,   // motor driving home
  Settling,  // home: waiting for the box to be clear before the write-back
  Broken,    // the leaf no longer matches: not a door any more
};
const char* DoorPhaseName(DoorPhase p);

// What a door is doing, for gates and the References page. False when the
// id is not a door this process has seen active.
struct DoorStatus {
  DoorPhase phase = DoorPhase::Closed;
  uint64_t body = 0, joint = 0;
  float angle = 0.0f;             // hinge angle, radians (0 when closed)
  uint32_t leafCells = 0;         // captured leaf size (0 when closed)
  uint32_t blockedTicks = 0;      // Settling: ticks the box has been refused
  std::string note;               // the last thing it had to say
};
bool DoorStatusOf(const std::string& id, DoorStatus& out);

// ---- containers ---------------------------------------------------------------
// The contents as they are now: the delta if there is one, else props.items
// (names the library lacks are skipped when `lib` is given).
void ContainerContents(RefStore& s, const Ref& r, const ItemLibrary* lib, Bag& out);
// Replace the contents (writes the delta).
void ContainerSetContents(RefStore& s, const Ref& r, const Bag& bag);
// Take the `n`th non-empty stack (the loot panel's order) into `kit` -- bag
// first, hotbar as overflow; the whole stack. False + `msg` when there is no
// such stack or no room (nothing moves).
bool ContainerTake(RefStore& s, const std::string& id, int n, Kit& kit,
                   const ItemLibrary& lib, std::string* msg);
// Put `stack` in (merging); on success `stack` is emptied. False + `msg` when
// the container is full.
bool ContainerPut(RefStore& s, const std::string& id, ItemInstance& stack,
                  std::string* msg);
// The container's title ("chest" unless props.title says otherwise).
std::string ContainerTitle(const Ref& r);

// ---- beds -------------------------------------------------------------------------
struct BedAnchor {
  Vec3 head{};            // where the head rests (world voxels, cell centre)
  Vec3 foot{};            // where the feet end
  float headingRad = 0;   // head -> foot, heading convention (0 = +Z)
};
bool BedAnchorOf(const Ref& r, BedAnchor& out);

}  // namespace refs
