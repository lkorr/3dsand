#pragma once
// THE ALCHEMY BENCH, as the game drives it: one FlaskSim (game/flasksim.h)
// over the vessel the player opened, plus at most one vessel brought in to
// pour from, the tools that act on them, and the tally that turns what
// happened into a TRANSFER the game checks and applies.
//
// Game-side, owned by main.cpp; the inventory panel only draws Pixels() and
// hands in a BenchInput each frame (ui/overlay.h, "mirror in, intent out").
// Nothing here writes a stack: Finish() returns a BenchResult and
// ApplyBenchResult is the one place its numbers reach the kit, after
// ValidateBench has proven they conserve every material.
#include <cstdint>
#include <string>
#include <vector>

#include "game/composition.h"
#include "game/flasksim.h"
#include "game/iteminstance.h"
#include "game/kitref.h"

struct ItemDef;
struct MaterialDef;

namespace alchemy {

enum class BenchTool : uint8_t { Stir = 0, Pour = 1 };

// One frame of the pointer over the bench, in SIM pixels (y up, 0..W/H).
struct BenchInput {
  bool over = false;     // pointer is on the bench image
  V2 at;                 // where, sim pixels
  bool down = false;     // primary button held
  bool pressed = false;  // primary button went down this frame
  float tilt = 0;        // tilt request this frame, radians (+ = CCW)
};

struct BenchEntry {
  KitRef ref;
  std::string item;       // the stack's item name when it came on the bench
  int capacity = 0;       // eighths
  Composition before;     // what it held then
  Composition after;      // what it holds now (Finish / removal)
};

struct BenchResult {
  std::vector<BenchEntry> vessels;   // the opened one first
  Composition spilled;               // left the bench: goes to the world
};

// Does `r` conserve every material (sum of before == sum of after + spilled)?
// Also enforces each vessel's capacity and the 16-substance limit by moving
// the excess -- top layer first -- into the spill, so the result that comes
// back is always applicable. False (with `why`) only for a result that
// created or destroyed matter, which is a bug, never a player action.
bool ValidateBench(BenchResult& r, const std::vector<MaterialDef>& mats, std::string& why);

class AlchemyBench {
 public:
  // The sim's grid (the panel draws it at an integer scale).
  static constexpr int kGridW = 336;
  static constexpr int kGridH = 288;
  // One eighth of a cell is this many pixels of area on the bench, so a
  // vessel's drawn inside is capacity x this.
  static constexpr int kUnitsPerEighth = 8;

  bool Open(KitRef ref, const ItemDef& def, const ItemInstance& inst,
            const std::vector<MaterialDef>& mats);
  bool IsOpen() const { return open_; }
  KitRef Target() const { return entries_.empty() ? KitRef{} : entries_[0].ref; }

  // Brings a vessel on to pour FROM, beside the target, upright. A source
  // already on the bench is taken off first (it keeps what is inside it).
  bool SetSource(KitRef ref, const ItemDef& def, const ItemInstance& inst);
  void ClearSource();
  KitRef Source() const { return source_ >= 0 ? entries_[source_].ref : KitRef{}; }
  bool Uses(KitRef ref) const;

  void Frame(float dt, const BenchInput& in, BenchTool tool);

  // Lets whatever is in flight land, tallies, and closes.
  BenchResult Finish();
  void Abandon();

  const std::vector<uint32_t>& Pixels() const { return pixels_; }
  int W() const { return kGridW; }
  int H() const { return kGridH; }
  // The source's ACTUAL tilt (it lags the hand, and glass stops it).
  float SourceTilt() const;
  float RequestedTilt() const { return tilt_; }
  // Where the pointer would have to be to hold the source by its pivot, and
  // the middle of the target's mouth (sim pixels) -- for scripted captures.
  V2 SourcePivot() const { return pivot_; }
  V2 TargetMouth() const;
  // Eighths currently in the target / source vessel (live, for the panel's
  // numbers; exact only at Finish).
  Composition LiveTarget() const;
  Composition LiveSource() const;

 private:
  Xform SourcePose() const;
  void PlaceSourceAt(V2 pivotWorld);

  bool open_ = false;
  FlaskSim sim_;
  std::vector<BenchEntry> entries_;   // [0] = target
  std::vector<int> simIndex_;         // per entry, FlaskSim vessel index
  int source_ = -1;                   // entry index of the current source
  // The source's pose: a PIVOT (the lip corner on the pouring side, which the
  // pointer holds) and a tilt about it.
  V2 pivot_;
  float tilt_ = 0;
  bool pivotLeft_ = true;
  bool dragging_ = false;
  V2 grabOffset_;
  float accum_ = 0;
  std::vector<uint32_t> pixels_;
};

}  // namespace alchemy
