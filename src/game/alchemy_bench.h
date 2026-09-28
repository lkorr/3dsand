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
#include <functional>
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
struct ReactionGpu;
struct SoluteDef;
struct ExplosionOp;
struct GasSpawnOp;
struct Vec3;

namespace alchemy {

// The tools (docs/PLAN_alchemy_chemistry.md package C): the hand carries and
// tilts, the stick stirs, the STOPPER tool puts a stopper in a vessel's mouth
// or takes it out, the BURNER tool lights or puts out the flame under one.
// Electrify is a button, not a tool (BenchInput::shock).
enum class BenchTool : uint8_t { Hand = 0, Stick = 1, Stopper = 2, Burner = 3 };

// One frame of the pointer over the bench, in SIM pixels (y up, 0..W/H).
struct BenchInput {
  bool over = false;     // pointer is on the bench picture
  V2 at;                 // where, sim pixels
  bool down = false;     // primary button held
  bool pressed = false;  // primary button went down this frame
  float tilt = 0;        // tilt request this frame, radians (+ = CCW)
  bool shock = false;    // Electrify pressed this frame: shocks the vessel in focus
};

struct BenchEntry {
  KitRef ref;
  std::string item;       // the stack's item name when it came on the bench
  int capacity = 0;       // eighths
  Composition before;     // what it held when it first came on
  Composition after;      // what it holds now (taken off, or at Finish)
  bool onTable = false;
  bool stoppered = false;  // now (ItemInstance::stoppered is written from this)
  bool broken = false;     // its glass burst on the bench: the item is gone
  bool inHand = false;     // one of the two the character is holding (main.cpp)
};

// WHAT A BENCH EVENT IS (a FlaskSim SimEvent in entry terms). `kind` is the
// reaction effect's kind (reactions.json "effects", MaterialDef::ruleFx) or a
// pressure event of the sim's own ("pop", "burst").
struct BenchEvent {
  std::string kind;
  int entry = -1;               // the vessel it happened in (BenchResult::vessels index), -1 = none
  V2 at;                        // sim pixels
  int32_t radius = 0, power = 0;
  float amount = 0;
  std::string what;
  uint16_t selfMat = 0, nbrMat = 0;
  std::vector<uint16_t> products;
  int count = 1;
};

// WHAT THE GAME DOES ABOUT ONE. A handler fills this; main.cpp applies it
// generically (BenchOutcomeWorldOps turns the world half into ops), so a new
// event kind is one handler and a line of JSON -- never a new branch in the
// frame loop.
struct BenchOutcome {
  bool eject = false;       // the bench closes NOW, mid-motion
  bool breakHeld = false;   // the vessels in the character's hands break; their contents burst out
  std::vector<int> burst;   // entries whose glass breaks on the bench (their contents loose on it)
  struct Blast { int32_t radius = 0, power = 0; };
  std::vector<Blast> blasts;           // REAL explosions at the hands (ExplosionOp, the grenade slot)
  std::vector<uint16_t> puff;          // gases puffed round the hands (only gas materials are used)
  int puffCells = 0;                   // how many gas voxels, all of them together
  std::string message;
};
using BenchEventHandler = std::function<void(const BenchEvent&, BenchOutcome&)>;
// The registry, keyed by kind. Built-ins: "explode", "burst", "pop".
void RegisterBenchEvent(const std::string& kind, BenchEventHandler h);
// Runs the handler for e.kind into `out`; false (and no change) for a kind
// nobody registered -- ignored, as every consumer ignores an unknown effect.
bool DispatchBenchEvent(const BenchEvent& e, BenchOutcome& out);
// The world half of an outcome at `hands` (world voxels): its blasts as
// ExplosionOps and its puff as GasSpawnOps in a small ball round the hands.
// Pure, so the gate checks exactly what the game submits.
void BenchOutcomeWorldOps(const BenchOutcome& o, const Vec3& hands, uint32_t seed,
                          const std::vector<MaterialDef>& mats, std::vector<ExplosionOp>& exps,
                          std::vector<GasSpawnOp>& gas);

struct BenchResult {
  std::vector<BenchEntry> vessels;
  Composition spilled;               // left the bench: goes to the world
  // What already WENT to the world while the bench was up (TakeSpill), per
  // material id, eighths: liquid and powder that fell off the table, and
  // gas that VENTED out of a mouth. Part of the conservation sum.
  std::vector<uint32_t> streamed;
  std::vector<uint32_t> vented;
  // THE REACTION LEDGER, per material id, in bench UNITS (unitsPerEighth to
  // an eighth): what the bench's reactions made and unmade. Every conversion
  // is in here, so ValidateBench proves
  //   before + produced - consumed == after + spilled + streamed + vented
  // per material, with a dissolved portion counted as its powder.
  std::vector<int64_t> produced, consumed;
  int unitsPerEighth = 12;
  // The bench ended in an event that ejected the player (nothing settled).
  bool ejected = false;
};

// A vessel on the table as the character holds it (main.cpp poses the
// avatar's hands off this, game/session.h BenchHold).
struct BenchVesselView {
  KitRef ref;
  Xform pose;            // sim pixels, y up; angle radians CCW
  float width = 0, height = 0;
  bool held = false;     // in the bench's hand right now
  bool stoppered = false;
  bool burner = false;
  float heat = 0;
};

// Whole eighths that fell off the table, and the vessel they left (the one
// the matter was last inside: FlaskSim::DrainSpilled's SpillBy). `ref` is
// invalid for matter that left no vessel we know of (loose gas on the
// table, the remainder of a vessel taken off it).
struct BenchSpill {
  KitRef ref;
  Composition what;
};

// Does `r` conserve every material (sum of before == sum of after + spilled)?
// Also enforces each vessel's capacity by moving the excess -- top layer
// first -- into the spill, so the result that comes back is always
// applicable. False (with `why`) only for a result that created or destroyed
// matter, which is a bug, never a player action.
bool ValidateBench(BenchResult& r, const std::vector<MaterialDef>& mats, std::string& why);

// Units -> eighths the way FlaskSim::Count rounds a material's total (half
// up, floor division): the ledger's net in eighths.
inline int64_t LedgerEighths(int64_t units, int upe) {
  const int64_t x = units + upe / 2;
  return x >= 0 ? x / upe : -((-x + upe - 1) / upe);
}

// WHERE THE HAND PUTS A VESSEL: the pose that has the grab point `grabLocal`
// (vessel frame) under the pointer `at` at `angle`, clamped to the SIM GRID
// (not the visible table): the base no lower than the table top, no glass
// below the grid floor or above its top (with room for a cork), the base
// centre inside the table's width. Liquid in a vessel the hand holds can
// therefore never leave through the top of the grid -- only through a mouth.
// Pure, so the gate drives exactly what the hand does.
Xform HandGoal(const VesselShape& shape, V2 at, V2 grabLocal, float angle, int gridW, int gridH,
               float tableY);

// WHERE A VESSEL COMING ONTO THE TABLE STANDS: upright on the table top, the
// clear spot nearest one of the table's two places (two thirds, then a third
// of the way across), wholly inside the table's width and with its glass
// clear of every vessel already on it (FlaskSim::ShapeClear). FALSE when
// there is no such spot -- the vessel is then REFUSED, never put on top of
// another (owner, 2026-09-27: a new vial spawned inside the other one on a
// small window, and the two glasses tangled and blew up). Pure: the gate
// `alchemy-place` runs exactly this.
bool FindPlaceSpot(const FlaskSim& sim, const VesselShape& shape, int tableW, float tableY, Xform& out);

class AlchemyBench {
 public:
  // One eighth of a cell is this many pixels of area on the bench, so a
  // vessel's drawn inside is capacity x this. A liquid particle is half an
  // eighth.
  static constexpr int kUnitsPerEighth = 12;
  static constexpr int kUnitsPerParticle = 6;
  // The largest table (the picture's texture is this size; a table uses the
  // top-left W x H of it). H is the SIM GRID's height, headroom included.
  static constexpr int kMaxW = 800;
  static constexpr int kMaxH = 800;
  // The narrowest table: two of the largest vessels (a full flask is ~126 px
  // wide, a full pouch a little wider; FindPlaceSpot keeps each wholly on
  // the table with its glass 4 px from the other's, and stands the first at
  // two thirds of the way across, so 280 was measured too narrow for a
  // second full flask) side by side, so the two
  // vessels the bench takes always fit whatever the window (gate
  // `alchemy-place` asserts it at this width). The panel picks its scale so
  // the table is at least this wide; a window too small for even that draws
  // it at a smaller scale, it never gets a table they overlap on.
  static constexpr int kMinW = 300;
  // The table top: vessels stand with their base this high.
  static constexpr float kTableY = 4.0f;
  // THE LIFT ROOM: the sim grid is never shorter than this, whatever the
  // visible table is (the panel draws the rest ABOVE its box). Room for the
  // largest flask (1024 eighths: ~126 x 172 px) held UPSIDE DOWN with its
  // mouth 30 px over the mouth of another standing on the table:
  // 4 + 172 + 30 + 172 = 378 to its base, plus glass and a margin for the
  // hand. Kept as low as that allows, because the panel sizes its scale so
  // this much fits on the screen (at 1600 x 900: 411 px at scale 2). Gate
  // `alchemy-lift` pours at exactly this height, so a shape that outgrows it
  // fails there, not in the hand.
  static constexpr int kLiftH = 400;
  // Vessels on the table at once: the character's two hands.
  static constexpr int kMaxOnTable = 2;

  AlchemyBench() = default;
  ~AlchemyBench() { Abandon(); }
  AlchemyBench(const AlchemyBench&) = delete;
  AlchemyBench& operator=(const AlchemyBench&) = delete;

  // An empty table of `w` x `h` sim pixels. `reactions` and `solutes` are the
  // world's tables: the bench runs the same rules (flasksim_mats.h
  // BuildBenchChemistry); empty ones make a bench without chemistry.
  bool Open(int w, int h, const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions, const std::vector<SoluteDef>& solutes);
  bool IsOpen() const { return open_; }

  // Put a vessel on the table (a free spot, upright) / take it off (it
  // leaves with what is inside it). A vessel taken off and put back holds
  // what it left with; the kit is only written at Finish.
  bool Place(KitRef ref, const ItemDef& def, const ItemInstance& inst);
  // Why the last Place said no, for the panel ("" = no reason to show).
  const std::string& Refusal() const { return refusal_; }
  // A Place the table itself refused after Place said yes (the sim found no
  // clear spot for it: FindPlaceSpot). Once; "" when none. The vessel is off
  // the table again, holding what it held.
  std::string TakeLateRefusal();
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
  // caller to put into the world NOW, one BenchSpill per vessel it LEFT (up
  // to 16 substances each a call; more waits for the next) -- so two flasks
  // spilling at once each pour from their own lip. `exitX` = where on the
  // table it fell, sim pixels (a fallback for the part with no vessel). Once
  // taken it is the world's: Finish reports it in BenchResult::streamed.
  bool TakeSpill(std::vector<BenchSpill>& out, float& exitX);

  // THE CHEMISTRY'S EVENTS since the last call (DispatchBenchEvent each).
  std::vector<BenchEvent> TakeEvents();
  // The devices of a vessel on the table, as of the last published step.
  // `pressure` is a fraction of where its stopper pops (FlaskSim::PressureFraction).
  bool Devices(KitRef ref, bool& stoppered, bool& burner, float& heat, float& pressure) const;
  // For scripted captures (--shot-bench) and gates: act on a vessel directly.
  void SetStopper(KitRef ref, bool on);
  void SetBurner(KitRef ref, bool on);
  void Shock(KitRef ref);
  // Breaks a vessel's glass on the bench (a BenchOutcome's `burst`), by entry.
  void BurstEntry(int entry);

  // Stops the sim, lets whatever is in flight land, tallies, closes.
  // `abrupt` (an event ejected the player): nothing settles, the table is
  // tallied where it stands.
  BenchResult Finish(bool abrupt = false);
  void Abandon();

  const std::vector<uint32_t>& Pixels() const { return front_; }
  int W() const { return w_; }
  int H() const { return h_; }
  // True once per new picture (the frame thread uploads only then).
  bool TakeFresh() { const bool f = fresh_; fresh_ = false; return f; }

 private:
  struct Cmd {
    enum Kind { kPlace, kRemove, kStopper, kBurner, kShock, kBurst } kind;
    int entry;
    VesselShape shape;
    Composition contents;
    bool on = false;
  };
  struct Slot {                // per entry, sim-thread side
    int sim = -1;              // FlaskSim vessel index, -1 when off the table
    Xform goal;                // where the hand (or the table) wants it
  };
  void Run();
  void Apply(Cmd& c);
  void Tick(const BenchInput& in, bool pressed, float tilt, BenchTool tool, float dt);
  bool FreeSpot(int entry, float nearX, Xform& out) const;
  // THE MOUTH UNDER THE POINTER (sim thread): the vessel on the table whose
  // mouth -- the cork's box, with a margin for the hand -- is at `at`, as a
  // FlaskSim index; -1 for none. A hand click there puts the stopper in or
  // takes it out instead of picking the vessel up.
  int MouthAt(V2 at) const;
  // The hovered mouth's ghost cork (or, stoppered, a ring round the real
  // one), painted over a rendered picture.
  void PaintMouthHover(std::vector<uint32_t>& pic) const;

  // ---- frame-thread state ----
  bool open_ = false;
  int w_ = 0, h_ = 0;
  const std::vector<MaterialDef>* mats_ = nullptr;
  std::vector<BenchEntry> entries_;
  std::vector<uint32_t> front_;
  bool fresh_ = false;
  std::string refusal_;
  std::vector<uint32_t> streamed_;   // per mat id, taken by TakeSpill
  std::vector<uint32_t> vented_;     // per mat id, gas taken by TakeSpill

  // ---- shared, under mu_ ----
  mutable std::mutex mu_;
  BenchInput input_;
  bool pressedLatch_ = false;
  float tiltAcc_ = 0;
  BenchTool tool_ = BenchTool::Hand;
  float wind_ = 0.12f;                                 // tuning tools.alchemyWind, as of the last Frame
  std::vector<Cmd> cmds_;
  std::vector<uint32_t> back_;
  bool backReady_ = false;
  std::vector<Composition> live_;                      // per entry
  std::vector<uint8_t> liveOn_;                        // per entry: live_ counted it on the table
  // Not yet taken, per SOURCE then per mat id: [0] left no known vessel,
  // [1 + entry] left that entry.
  std::vector<std::vector<uint32_t>> streamOut_;
  double streamXSum_ = 0, streamXW_ = 0;
  int heldPub_ = -1;                                   // entry in the hand
  struct Removed { int entry; Composition c; bool stoppered; };
  std::vector<Removed> removed_;                       // what left with each vessel taken off
  std::vector<int> refused_;                           // entries the table had no spot for
  std::string lateRefusal_;                            // frame thread (Frame fills it)
  std::vector<BenchEvent> events_;                     // not yet taken
  std::vector<uint8_t> brokenPub_;                     // per entry: burst
  int focus_ = -1;                                     // entry
  struct PoseView {
    Xform x; float w = 0, h = 0; bool on = false;
    bool stoppered = false, burner = false; float heat = 0, pressure = 0;
  };
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
  int mouthHover_ = -1;                    // FlaskSim vessel whose mouth is pointed at
  bool redraw_ = false;                    // Tick changed the look (hover, a stopper)
  uint32_t ticks_ = 0;
};

}  // namespace alchemy
