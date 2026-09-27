#pragma once
// THE ALCHEMY BENCH, as the game drives it: a WORKSTATION. A table (one
// FlaskSim, game/flasksim.h) that any of the player's vessels can be put on
// and taken off; a hand that picks them up, carries and tilts them; a
// stirring stick; and the tally that turns what happened into a TRANSFER the
// game checks and applies.
//
// Game-side, owned by main.cpp; the inventory panel only draws Pixels() and
// hands in a BenchInput each frame (ui/overlay.h, "mirror in, intent out").
// Nothing here writes a stack: Finish() returns a BenchResult and main.cpp
// applies it after ValidateBench has proven it conserves every material.
//
// THE SIM RUNS ON ITS OWN THREAD, at 60 steps a second. A table with several
// full vessels costs several milliseconds a step while it is moving; on the
// frame thread that was the game's frame rate, on its own thread it is at
// worst a bench running a little slow. Settled vessels sleep and cost nothing
// (FlaskSim's sleep), and a quiet table is not redrawn. Everything the frame
// thread sees is a snapshot published under one mutex.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game/composition.h"
#include "game/flasksim.h"
#include "game/iteminstance.h"
#include "game/kitref.h"

struct ItemDef;
struct MaterialDef;

namespace alchemy {

enum class BenchTool : uint8_t { Hand = 0, Stick = 1 };

// One frame of the pointer over the bench, in SIM pixels (y up, 0..W/H).
struct BenchInput {
  bool over = false;     // pointer is on the bench picture
  V2 at;                 // where, sim pixels
  bool down = false;     // primary button held
  bool pressed = false;  // primary button went down this frame
  float tilt = 0;        // tilt request this frame, radians (+ = CCW)
};

struct BenchEntry {
  KitRef ref;
  std::string item;       // the stack's item name when it came on the bench
  int capacity = 0;       // eighths
  Composition before;     // what it held when it first came on
  Composition after;      // what it holds now (taken off, or at Finish)
  bool onTable = false;
};

struct BenchResult {
  std::vector<BenchEntry> vessels;
  Composition spilled;               // left the bench: goes to the world
  // What already WENT to the world while the bench was up (TakeSpill), per
  // material id, eighths. Part of the conservation sum: before == after +
  // spilled + streamed.
  std::vector<uint32_t> streamed;
};

// A vessel on the table as the character holds it (main.cpp poses the
// avatar's hands off this, game/session.h BenchHold).
struct BenchVesselView {
  KitRef ref;
  Xform pose;            // sim pixels, y up; angle radians CCW
  float width = 0, height = 0;
  bool held = false;     // in the bench's hand right now
};

// Does `r` conserve every material (sum of before == sum of after + spilled)?
// Also enforces each vessel's capacity by moving the excess -- top layer
// first -- into the spill, so the result that comes back is always
// applicable. False (with `why`) only for a result that created or destroyed
// matter, which is a bug, never a player action.
bool ValidateBench(BenchResult& r, const std::vector<MaterialDef>& mats, std::string& why);

class AlchemyBench {
 public:
  // One eighth of a cell is this many pixels of area on the bench, so a
  // vessel's drawn inside is capacity x this. A liquid particle is half an
  // eighth.
  static constexpr int kUnitsPerEighth = 12;
  static constexpr int kUnitsPerParticle = 6;
  // The largest table (the picture's texture is this size; a table uses the
  // top-left W x H of it).
  static constexpr int kMaxW = 800;
  static constexpr int kMaxH = 480;
  // The table top: vessels stand with their base this high.
  static constexpr float kTableY = 4.0f;

  AlchemyBench() = default;
  ~AlchemyBench() { Abandon(); }
  AlchemyBench(const AlchemyBench&) = delete;
  AlchemyBench& operator=(const AlchemyBench&) = delete;

  // An empty table of `w` x `h` sim pixels.
  bool Open(int w, int h, const std::vector<MaterialDef>& mats);
  bool IsOpen() const { return open_; }

  // Put a vessel on the table (a free spot, upright) / take it off (it
  // leaves with what is inside it). A vessel taken off and put back holds
  // what it left with; the kit is only written at Finish.
  bool Place(KitRef ref, const ItemDef& def, const ItemInstance& inst);
  void Remove(KitRef ref);
  bool OnTable(KitRef ref) const;
  bool Uses(KitRef ref) const;
  // What a vessel the bench has had holds now (live, approximate while
  // things move; exact at Finish). False if the bench has never had it.
  bool Contents(KitRef ref, Composition& out) const;
  // The vessel under the hand or last picked up, for the readout.
  KitRef Focus() const;
  // Where a vessel on the table stands and how big it is (sim pixels), as of
  // the last published step. For scripted captures.
  bool PoseOf(KitRef ref, Xform& pose, float& width, float& height) const;

  // Every vessel on the table as of the last published step, in the order
  // they came on.
  std::vector<BenchVesselView> Vessels() const;

  // One frame: the pointer and tool go to the sim thread, the newest picture
  // comes back into Pixels().
  void Frame(const BenchInput& in, BenchTool tool);

  // WHAT FELL OFF THE TABLE SINCE THE LAST CALL, whole eighths, for the
  // caller to put into the world NOW (up to 16 substances a call; more waits
  // for the next). `exitX` = where on the table it fell, sim pixels. Once
  // taken it is the world's: Finish reports it in BenchResult::streamed.
  bool TakeSpill(Composition& out, float& exitX);

  // Stops the sim, lets whatever is in flight land, tallies, closes.
  BenchResult Finish();
  void Abandon();

  const std::vector<uint32_t>& Pixels() const { return front_; }
  int W() const { return w_; }
  int H() const { return h_; }
  // True once per new picture (the frame thread uploads only then).
  bool TakeFresh() { const bool f = fresh_; fresh_ = false; return f; }

 private:
  struct Cmd {
    enum Kind { kPlace, kRemove } kind;
    int entry;
    VesselShape shape;
    Composition contents;
  };
  struct Slot {                // per entry, sim-thread side
    int sim = -1;              // FlaskSim vessel index, -1 when off the table
    Xform cmd;                 // the smoothed pose sent to the sim
    Xform goal;                // where the hand (or the table) wants it
  };
  void Run();
  void Apply(Cmd& c);
  void Tick(const BenchInput& in, bool pressed, float tilt, BenchTool tool, float dt);
  bool FreeSpot(int entry, float nearX, Xform& out) const;

  // ---- frame-thread state ----
  bool open_ = false;
  int w_ = 0, h_ = 0;
  const std::vector<MaterialDef>* mats_ = nullptr;
  std::vector<BenchEntry> entries_;
  std::vector<uint32_t> front_;
  bool fresh_ = false;
  std::vector<uint32_t> streamed_;   // per mat id, taken by TakeSpill

  // ---- shared, under mu_ ----
  mutable std::mutex mu_;
  BenchInput input_;
  bool pressedLatch_ = false;
  float tiltAcc_ = 0;
  BenchTool tool_ = BenchTool::Hand;
  std::vector<Cmd> cmds_;
  std::vector<uint32_t> back_;
  bool backReady_ = false;
  std::vector<Composition> live_;                      // per entry
  std::vector<uint32_t> streamOut_;                    // per mat id, not yet taken
  double streamXSum_ = 0, streamXW_ = 0;
  int heldPub_ = -1;                                   // entry in the hand
  std::vector<std::pair<int, Composition>> removed_;   // entry, what left with it
  int focus_ = -1;                                     // entry
  struct PoseView { Xform x; float w = 0, h = 0; bool on = false; };
  std::vector<PoseView> poses_;                        // per entry
  bool quit_ = false;

  // ---- sim-thread state (the frame thread touches it only when the thread
  // is not running: Open before it starts, Finish after it is joined) ----
  std::thread thread_;
  FlaskSim sim_;
  std::vector<Slot> slots_;                // per entry
  int held_ = -1;                          // entry
  V2 grabLocal_;
  float heldAngle_ = 0;
  int simFocus_ = -1;
  uint32_t ticks_ = 0;
};

}  // namespace alchemy
