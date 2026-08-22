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
};

// The registry, in declaration order. Defined across src/test/selftest_*.cpp
// and assembled in selftest.cpp.
const std::vector<Gate>& Registry();

struct Options {
  std::vector<std::string> only;  // --gate NAME (repeatable); empty = all
  std::string jsonPath;           // --json PATH
  std::string baselinePath;       // --baseline PATH
  bool list = false;              // --list
};

// Run the selected gates. Returns a process exit code: 0 when every gate that
// ran either passed or was already failing in the baseline.
int Run(Ctx& c, const Options& opt);

// Print the gate names and groups, one per line, and return 0.
int List();

}  // namespace selftest
