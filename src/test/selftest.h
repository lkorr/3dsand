// selftest.h — the acceptance gate, as a registry of named gates.
//
// WHY THIS IS A REGISTRY AND NOT ONE FUNCTION
//
// This used to be a single 3300-line RunSelftest inside main.cpp. Two costs
// made that untenable once several agents worked the repo at once:
//
//   1. It was all-or-nothing and took ~67 s. An agent iterating on the prefab
//      loader paid the full run to read one line, and a failure printed no
//      machine-readable signal about WHICH gate broke.
//   2. A red run said nothing about authorship. CLAUDE.md carries a whole
//      documented ritual — rebuild clean HEAD, revert only your files — for
//      deciding whether a failure was yours. That is a tooling gap papered
//      over with prose.
//
// So: every gate is a named function with declared dependencies, `--gate NAME`
// runs one in seconds, and `--json` emits results a script can diff against
// tests/baseline.json (which records what already fails at HEAD).
//
// ORDERING IS REAL, NOT INCIDENTAL. Gates share one World/Simulation and run
// in sequence; several depend on world state a previous gate left behind. The
// worst trap, already paid for once: the streaming gate leaves the residency
// window origin ~20 chunks out, and a gate that hardcodes a world position
// instead of anchoring to `world.WindowOrigin()` then fires into solid space.
// That is why dependencies are DECLARED here rather than implied by source
// order — running `--gate spells` alone must still run what spells needs.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/item.h"
#include "game/mob.h"
#include "gpu/context.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/world.h"

namespace selftest {

// Everything a gate is allowed to touch. Passed by reference to every gate so
// no gate reaches for a global, and so the harness can reset shared state
// between gates that need a clean world.
struct Ctx {
  GpuContext& ctx;
  World& world;
  Simulation& sim;
  const std::vector<MaterialDef>& mats;
  Physics& phys;
  DebrisSystem& debris;
  MobSystem& mobs;
  Stream& stream;
  const ItemLibrary& items;

  // Shared offscreen render target, created once by the harness. The render
  // perf gate, the screenshot gates and the micro-body view sweep all draw
  // into this rather than each making their own 1080p texture.
  uint32_t width = 1920, height = 1080;
  rhi::Texture offscreen;
  rhi::TextureView view;

  // Carried between gates: `perf` measures these, and the verdict line prints
  // them. Advisory only — perf reports MARGINAL and never fails the run,
  // because the numbers track kVoxelMeters rather than correctness.
  double simMs = 0.0;
  double bestFrameMs = 1e9;

  // Read the shared offscreen target back and write it out as a BMP.
  void Grab(const char* path);
};

// printf-style string builder for a gate's detail line. The gates kept their
// original format strings and argument lists, so the console output reads
// exactly as it did before the split — only the "name: PASS" prefix moved to
// the harness.
std::string Format(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// One gate's outcome. `skipped` is distinct from failure: a gate whose
// dependency failed cannot run, and reporting that as FAIL would blame the
// wrong change.
enum class Status { Pass, Fail, Skip };

struct Result {
  std::string name;
  Status status = Status::Skip;
  std::string detail;   // the human-readable parenthetical
  double seconds = 0.0;

  // MEASURED VALUES THIS RUN, for `--rebaseline` to write back into
  // tests/baseline.json as `"key": "value"` pairs.
  //
  // CLAUDE.md says thresholds belong in baseline.json rather than in C++,
  // because a threshold in source costs a rebuild to tune and a threshold in
  // JSON costs nothing. Until this existed, nothing implemented that: the
  // baseline held pass/fail plus one hash, and every numeric bound in the
  // suite was a literal in a .cpp. A gate that measures something continuous
  // (terrain relief, slope, resident pages) pushes it here, reads its own
  // bound back through BaselineValue(), and becomes tunable without a compile.
  //
  // Values are STRINGS because the baseline scanner only accepts quoted
  // values — see LoadBaseline. Numbers go in as "1364", not 1364.
  std::vector<std::pair<std::string, std::string>> observed;
};

// A gate: a name, the gates it needs run first, and the body.
//
// `deps` are gate names. The harness topologically orders them, so `--gate
// spells` pulls in whatever spells needs and nothing else. A gate with no deps
// is expected to establish its own world state (most call SubmitWorldgen).
struct Gate {
  const char* name;
  const char* group;              // for --list grouping: sim, render, mob, ...
  std::vector<const char*> deps;
  bool advisory = false;          // reports, never fails the run (perf)
  Status (*fn)(Ctx&, std::string& detail);
  // Does this gate need the RENDER path — the offscreen target, a render
  // pipeline, a draw?
  //
  // Declared rather than inferred, for the Vulkan port. The question "which
  // gates can run before the Vulkan render path exists" has a machine-readable
  // answer instead of a curated list in someone's commit message that goes
  // stale the first time a gate learns to draw. `--list` prints it.
  //
  // NOTHING CONSUMES IT since phase 4b: both backends have a render path, so
  // all 23 gates run on either, and the selftest defaults to Vulkan (phase 6).
  // The flag survives as documentation — `--list` prints it, and it is the
  // ready-made answer whenever a backend or a target is again partly built.
  bool needsRender = false;
};

// The registry, in declaration order. Defined across src/test/selftest_*.cpp
// and assembled in selftest.cpp.
const std::vector<Gate>& Registry();

struct Options {
  std::vector<std::string> only;  // --gate NAME (repeatable); empty = all
  std::string jsonPath;           // --json PATH
  std::string baselinePath;       // --baseline PATH
  bool list = false;              // --list
  bool rebaseline = false;        // --rebaseline
};

// The GOLDEN world hash, read from tests/baseline.json's "determinismHash" key,
// or empty when the key is absent.
//
// WHY THIS EXISTS. The determinism gate compares two runs of the SAME build
// against each other, which proves the sim is reproducible but says nothing
// about WHAT it reproduces. A change that makes the sim do less — the phase-2b
// discovery was a build where mutate/explode dispatched zero workgroups —
// stays perfectly self-consistent and the gate stays green. Pinning the value
// turns "the sim agrees with itself" into "the sim agrees with the world we
// shipped", which is the property every later phase (and the whole Vulkan port)
// actually leans on.
//
// Set by Run() before any gate executes. Empty means "not pinned": the gate
// reports the hash and passes, exactly as it did before, so a checkout without
// the key still works.
const std::string& GoldenDeterminismHash();

// Any non-pass/fail value from tests/baseline.json, or nullptr when absent.
// `determinismHash` is one of these; GoldenDeterminismHash() is a wrapper.
//
// A gate reads its own thresholds through this so they can be retuned by
// editing JSON instead of by rebuilding, and writes the values it measured
// back through Result::observed so `--rebaseline` records them in one step.
// ABSENT MUST BE HANDLED: a checkout without the key has to still work, so
// treat nullptr as "not pinned — report, do not fail", exactly as the
// determinism gate treats an empty golden hash.
const std::string* BaselineValue(const char* key);

// Same, parsed as a number, with a fallback when the key is absent or garbage.
double BaselineNumber(const char* key, double fallback);

// A gate records a value it MEASURED, for --rebaseline to write back. The
// harness drains these into Result::observed when the gate returns.
//
// A free function rather than a field on Ctx or an extra gate argument: the
// gate signature is shared by 44 gates across 11 translation units, and
// widening it to serve the two gates that report numbers is the worse trade.
void RecordObserved(const char* key, const std::string& value);
void RecordObserved(const char* key, double value);

// Run the selected gates. Returns a process exit code: 0 when every gate that
// ran either passed or was already failing in the baseline.
int Run(Ctx& c, const Options& opt);

// Print the gate names and groups, one per line, and return 0.
int List();

}  // namespace selftest
