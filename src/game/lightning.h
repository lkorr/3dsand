// lightning.h — THE STRIKE PATH: a lightning bolt (or a shock's arc burst) as a
// list of CellOps (docs/PLAN_electricity.md section 3, package E3).
//
// ONE FUNCTION, THREE CALLERS. The `lightning` glyph, the `shock` glyph and a
// storm's ground strike (session.cpp phase K, WeatherStrikes) all go through
// PlanStrike + EmitStrike. Nothing here writes a voxel: a strike is CellOps
// pushed into the tick's op stream (CLAUDE.md rule 3), so the op record, the
// replay and the net need no change -- an `ops-replay` of a stormy tick
// replays the bolt it recorded.
//
// WHAT IT IS.
//   1. TARGET. Every column within `searchRadius` (XZ disc) of the aim is
//      scanned down from THE REAL TOP -- `aim.y + kStrikeScanUp` (taller than
//      the tallest tree the generator grows), clamped to the residency
//      window -- through the strike's view of the world (WorldStrikeProbe:
//      the snapshot mirror, then the fetch cache; fixed latency) to its first
//      non-gas cell -- its TOP. A top counts only when the cell directly
//      above it is KNOWN air: a column whose first known cell is solid (a
//      canopy or a roof whose upper part no store has delivered) was entered
//      from inside, and does not compete -- that was the bolt that stopped in
//      mid-air inside a tree (wave 2, package E). The bolt takes the column
//      whose top scores highest: top y, plus `conductBonus` cells if the top
//      conducts (StrikeMats::conductive: materials.json `electric.resist`
//      1..8), ties to the nearer column, then to a hash. So a strike PREFERS
//      a tall thing and an iron rod over the ground beside it -- the
//      lightning-rod behaviour -- and never searches past what the stores
//      know. When no column qualifies the bolt lands at the aim cell itself.
//      Both callers make the stores know first: a storm strike asks the fetch
//      cache for every chunk of its search at DECIDE time, a spell strike
//      whose search leaves the stores waits a stepped leader for the same
//      fetch (StrikeSearchChunks / StrikeSearchKnown).
//   2. BOLT. A jagged column of `boltMat` from `height` cells above the top
//      down to the cell just above it: one cell per step down, a hashed
//      sideways kick on about a third of the steps, the offset pulled back so
//      it always ARRIVES at the struck column; plus up to two short hashed
//      forks. Every cell IfAir (kCellOpIfAir): the bolt can pass through a
//      region the mirror does not know without overwriting anything solid.
//   3. SPLASH. `arcs` jagged walks of `splashMat` out of the foot (the cell
//      above the top), 2 x splashRadius steps each, heading outward and down
//      -- the crackle that runs over the ground and lights what is beside the
//      rod. IfAir too.
//
// PURE. Every position is a hash of (spec.key, step), and the target is a
// function of the mirror, which is the published snapshot (fixed latency): two
// runs plan the same CellOp list (rule 1; the `elec-strike` gate plans twice
// and compares).
//
// BOUNDED (rule 2). A plan is at most kStrikeMaxCells cells. EmitStrike charges
// the WHOLE plan against the tick's StrikeBudget BEFORE it pushes one op, and
// refuses -- counted, nothing emitted -- if it does not fit there or in
// kMaxCellOpsPerTick. A strike that does not fit is a number, not half a bolt.
//
// FOR THE CHARGE FIELD (package E1, at merge). The bolt's cells are the seeds:
// `lightning` and `arc` get `electric.source` blocks in materials.json and the
// field does the rest. Nothing in this file needs to change for that; the
// target scoring may later read `electric.resist` instead of the tag set
// (StrikeMats::Resolve is the one place).
#pragma once

#include <cstdint>
#include <vector>

#include "math3d.h"
#include "sim/materials.h"
#include "sim/world.h"

#include "game/spell.h"

// How far above the aim the target scan starts, and how far below it gives up.
// UP is the real top of anything a column can hold: the tallest generated tree
// (redwood, 22 m + 4 m variance = 260 cells) with a little to spare. It was 24
// cells, so a tree or a house taller than 2.4 m over the aim was entered from
// inside. The scan skips an unknown chunk whole, so the height costs a chunk
// lookup per 16 cells of sky, not a probe per cell.
constexpr int32_t kStrikeScanUp = 272;
constexpr int32_t kStrikeScanDown = 32;
// Hard caps on one plan (rule 2): bolt height, search radius, splash.
constexpr int32_t kStrikeMaxHeight = 64;
constexpr int32_t kStrikeMaxSearch = 12;
constexpr int32_t kStrikeMaxArcs = 8;
constexpr int32_t kStrikeMaxSplash = 6;
constexpr uint32_t kStrikeMaxCells = 256;
// The tick's whole strike allowance, every caller together: two full bolts.
constexpr uint32_t kStrikeCellsPerTick = 512;

// The materials a strike names, resolved BY NAME once per material table
// (never a hard-coded id), plus the per-id "a bolt prefers this" table.
struct StrikeMats {
  uint32_t lightning = 0, arc = 0;
  std::vector<uint8_t> conductive;  // by material id: 1 = a strike prefers it
  std::vector<uint8_t> passable;    // by material id: 1 = air or gas (the scan goes through)
  size_t resolvedFor = 0;           // mats.size() it was built for (0 = never)
  void Resolve(const std::vector<MaterialDef>& mats);
  bool Ready() const { return lightning != 0 && arc != 0; }
  bool Conductive(uint32_t m) const { return m < conductive.size() && conductive[m]; }
  bool Passable(uint32_t m) const { return m == 0 || (m < passable.size() && passable[m]); }
};

struct StrikeSpec {
  IVec3 aim{};                // the world cell aimed at
  int32_t searchRadius = 4;   // XZ radius of the target search (0 = the aim column only)
  int32_t height = 40;        // bolt height above the struck top (0 = no bolt: the shock)
  int32_t conductBonus = 6;   // cells of height a conductive top is worth
  uint32_t boltMat = 0;       // lightning
  uint32_t splashMat = 0;     // arc (0 = no splash)
  int32_t splashRadius = 2;
  int32_t arcs = 4;
  int32_t forks = 2;          // short side branches off the bolt
  // When true the aim cell IS the target (no column search): the shock glyph,
  // which goes off where the spell resolved.
  bool atAim = false;
  uint32_t key = 0;           // hash key: (tick, source) -- every position derives from it
};

struct StrikePlan {
  IVec3 target{};             // the struck top cell (the bolt ends just above it)
  bool targetKnown = false;   // the mirror covered the chosen column
  bool targetConductive = false;
  uint32_t targetMat = 0;
  int32_t columnsScanned = 0;
  // Columns whose first known cell was solid with no known air above it (the
  // inside of a canopy, a roof above the stores): they did not compete.
  int32_t columnsHidden = 0;
  int32_t boltCells = 0, splashCells = 0;
  std::vector<CellOp> cells;
};

// The per-tick strike allowance, owned by the authority (TickAuthorityCtx::
// strikes) and reset at the head of every tick. Telemetry is monotonic.
struct StrikeBudget {
  uint32_t cap = kStrikeCellsPerTick;
  uint32_t used = 0;          // cells charged this tick
  uint64_t emitted = 0;       // strikes emitted, ever
  uint64_t refused = 0;       // strikes refused for budget, ever
  uint64_t cells = 0;         // cells emitted, ever
  void NewTick() { used = 0; }
};

// THE STRIKE'S VIEW OF THE WORLD: the snapshot mirror (3x3x3 chunks round the
// primary), then the on-demand fetch cache (World::Cached -- the chunks a
// weather strike asked for when it was decided), else unknown. Both stores are
// fed by the fixed-latency readback, so what a strike sees is a function of the
// tick (rule 1). Never the analytic terrain: a bolt into a house must stop at
// the roof, and only real voxels know where the roof is.
SpellProbe WorldStrikeProbe(const World& world);

// A glyph's strike block (GlyphStrike, glyphs.json "strike") as a spec at
// `aim`. Repetition (`scaleMille`, 1000 = said once) raises the bolt and adds
// arcs, inside the caps; STRENGTH (`strengthMille`, the effect's own 0..1000:
// a spent carrier, a lane's share) scales the same two, so a weak bolt is a
// shorter bolt with fewer arcs -- less lightning, so less charge in the field.
// No bolt = the shock: it goes off AT the aim.
StrikeSpec StrikeSpecFromGlyph(const GlyphStrike& g, IVec3 aim, int32_t scaleMille,
                               uint32_t key, int32_t strengthMille = 1000);

// Every world chunk the target search of `spec` reads (its XZ disc x the
// scan's vertical span, clamped to the window), in a fixed order. A storm
// strike requests them all from the fetch cache when it is decided.
void StrikeSearchChunks(const StrikeSpec& spec, const World& world, std::vector<IVec3>& out);
// True when every chunk of StrikeSearchChunks is in the snapshot mirror or the
// fetch cache -- the search can run now. Both stores are functions of the tick,
// so the answer is too (rule 1). `missing` (optional) receives the rest.
bool StrikeSearchKnown(const StrikeSpec& spec, const World& world,
                       std::vector<IVec3>* missing = nullptr);

// The storm's ground strike: the spec session.cpp WeatherStrikes fires.
StrikeSpec WeatherStrikeSpec(const StrikeMats& mats, IVec3 aim, uint32_t key);

// Plan a strike. `probe` may be null (no mirror): the bolt lands at the aim.
StrikePlan PlanStrike(const StrikeSpec& spec, const StrikeMats& mats,
                      const SpellProbe* probe, const World& world);

// Charge `plan` to `budget` and push its cells, or refuse the whole of it.
bool EmitStrike(const StrikePlan& plan, StrikeBudget& budget,
                std::vector<CellOp>& cellOps);

// Plan + emit: THE strike function every caller uses.
bool LightningStrike(const StrikeSpec& spec, const StrikeMats& mats,
                     const SpellProbe* probe, const World& world,
                     StrikeBudget& budget, std::vector<CellOp>& cellOps,
                     StrikePlan* planOut = nullptr);
