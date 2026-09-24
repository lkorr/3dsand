// selftest_generators.cpp — the Node generator data gates, run from the suite.
//
// WHY THIS EXISTS. scripts/test_mobgen.mjs, test_anatomy.mjs and
// test_environment.mjs hold the JS generators (mobgen.js, anatomy.js,
// biomegen.js / watergen.js / treegen.js) to the assets they describe, and
// until 2026-09-24 (rule-unification W1-E) NOTHING ran them: each was green on
// the day it was written, and a generator is a snapshot of the rig on that day
// (memory: gotcha-generated-characters-froze-the-rig-of-their-birthday — every
// bred character carried a four-commit-stale rig and no test could see it).
// scripts/post_edit_check.sh now runs the affected ones after every edit; this
// gate makes a --selftest / --verify say it too.
//
// HOW. It shells out to `node scripts/generator_parity.mjs --cache
// build/generator_parity.json`, which owns the list of tests and what each
// reads. The runner keys its cache on every input file, so in the steady
// state this gate costs one node start (~0.2 s); after an edit to a generator
// or its data it runs all three in parallel (~20 s wall). Nothing here touches
// the World, the GPU or the sim, so its kOrder slot is free.
//
// SKIP, NOT FAIL, WITHOUT NODE. A machine with no `node` on PATH (or a tree
// without scripts/) cannot run JS; that is an environment fact, not a defect
// in what the gate protects. Status::Skip is not a regression.
//
// KNOWN-RED TESTS LIVE IN tests/baseline.json (`generatorParityKnownRed`, a
// comma list), not in this file and not as a whole-gate "fail": recording the
// gate itself as known-failing would disarm it for the OTHER tests too. A test
// named there may fail without failing the gate; any other red test fails it.
// A listed test that passes again is reported, and the gate records the
// shrunk list for --rebaseline (which refuses on a new red, so the list can
// only shrink through it).

#include <cstdio>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "test/selftest.h"
#include "test/support.h"

namespace selftest {

namespace {

namespace fs = std::filesystem;

// Run a shell command, capture stdout (the caller folds stderr in), return the
// exit code. -1 when the shell itself could not be started.
int RunCapture(const std::string& cmd, std::string& out) {
  out.clear();
#ifdef _WIN32
  FILE* p = _popen(cmd.c_str(), "r");
#else
  FILE* p = popen(cmd.c_str(), "r");
#endif
  if (!p) return -1;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
#ifdef _WIN32
  return _pclose(p);
#else
  const int st = pclose(p);
  return (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
#endif
}

std::set<std::string> SplitList(const std::string& s) {
  std::set<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty() && item != "none") out.insert(item);
  }
  return out;
}

std::string Join(const std::set<std::string>& s) {
  std::string out;
  for (const auto& x : s) out += (out.empty() ? "" : ",") + x;
  return out.empty() ? "none" : out;
}

Status GateGeneratorParity(Ctx&, std::string& detail) {
  const fs::path root = fs::path(sandvox::AssetDir()).parent_path();
  const fs::path script = root / "scripts" / "generator_parity.mjs";
  if (!fs::exists(script)) {
    detail = "no scripts/generator_parity.mjs beside " + root.string() + "/assets";
    std::printf("generator-parity: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  std::string out;
  const int probe = RunCapture("node --version 2>&1", out);
  if (probe != 0 || out.empty() || out[0] != 'v') {
    detail = "node is not on PATH -- the JS generator tests cannot run here";
    std::printf("generator-parity: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  std::string nodeVersion = out.substr(0, out.find_first_of("\r\n"));

  // The command starts with `node`, not a quote, so cmd.exe's /c quote
  // stripping leaves the two quoted paths (which contain spaces) alone.
  const fs::path cache = root / "build" / "generator_parity.json";
  const std::string cmd = "node \"" + script.string() + "\" --cache \"" + cache.string() + "\" 2>&1";
  const int rc = RunCapture(cmd, out);

  // Parse the runner's contract: `<name>: PASS|FAIL (<summary>)` per test, the
  // failing checks indented under a red one, and a final
  // `generator-parity: PASS|FAIL n/m [cached]`.
  std::set<std::string> passed, failed;
  std::string verdict;
  std::vector<std::string> checkLines;
  {
    std::stringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("generator-parity: ", 0) == 0) { verdict = line.substr(18); continue; }
      if (line.rfind("    ", 0) == 0) { checkLines.push_back(line); continue; }
      const size_t colon = line.find(": ");
      if (colon == std::string::npos) continue;
      const std::string name = line.substr(0, colon), rest = line.substr(colon + 2);
      if (rest.rfind("PASS", 0) == 0) passed.insert(name);
      else if (rest.rfind("FAIL", 0) == 0) failed.insert(name);
      std::printf("generator-parity %s\n", line.c_str());
    }
  }
  if (verdict.empty() || (passed.empty() && failed.empty())) {
    detail = "the runner printed no verdict (exit " + std::to_string(rc) + "): " +
             out.substr(out.size() > 400 ? out.size() - 400 : 0);
    std::printf("generator-parity: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  const std::string* knownStr = BaselineValue("generatorParityKnownRed");
  const std::set<std::string> known = SplitList(knownStr ? *knownStr : "");
  std::set<std::string> newRed, knownRed, healed;
  for (const auto& n : failed) (known.count(n) ? knownRed : newRed).insert(n);
  for (const auto& n : known)
    if (passed.count(n)) healed.insert(n);
  // What is red NOW, for --rebaseline. It refuses on a new red (the gate
  // fails), so through it this list can only shrink.
  RecordObserved("generatorParityKnownRed", Join(knownRed));

  for (const auto& l : checkLines) std::printf("generator-parity %s\n", l.c_str());

  const bool cached = verdict.find("cached") != std::string::npos;
  detail = std::to_string(passed.size()) + "/" + std::to_string(passed.size() + failed.size()) +
           " generator tests pass" + (cached ? " (cached: no input changed)" : "") + ", node " +
           nodeVersion;
  if (!newRed.empty()) detail += "; RED: " + Join(newRed);
  if (!knownRed.empty()) detail += "; known-red at baseline: " + Join(knownRed);
  if (!healed.empty())
    detail += "; " + Join(healed) + " passes again -- drop it from generatorParityKnownRed";
  const bool ok = newRed.empty();
  std::printf("generator-parity: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& GeneratorGates() {
  static const std::vector<Gate> g = {
      {"generator-parity", "data", {}, false, GateGeneratorParity},
  };
  return g;
}

}  // namespace selftest
