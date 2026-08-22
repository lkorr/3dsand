// selftest.cpp — the gate harness: ordering, baseline diffing, reporting.
//
// The registry itself is assembled here from the per-domain translation units
// so that adding a gate means touching one file plus one line in kGroups.

#include "test/selftest.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "gpu/resources.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {

// Each domain file exposes its gates through one of these.
const std::vector<Gate>& SimGates();
const std::vector<Gate>& RenderGates();
const std::vector<Gate>& PlayerGates();
const std::vector<Gate>& MobGates();
const std::vector<Gate>& BodyGates();
const std::vector<Gate>& WorldIoGates();
const std::vector<Gate>& SpellGates();

// THE EXECUTION ORDER, and it is load-bearing.
//
// Gates share one World/Simulation and several depend on state a previous gate
// left behind, so this reproduces the order the single RunSelftest ran them in
// — NOT the order the per-domain files happen to be linked in. Grouping by file
// silently reordered debris/prefab the first time this was written, which is
// exactly the class of bug the ordering note in selftest.h warns about.
//
// Add a gate by putting its name here as well as in its domain file. A gate
// missing from this list is a link-time-visible mistake (it never runs), which
// is the failure mode we want rather than one that runs in an arbitrary slot.
const char* const kOrder[] = {
    "determinism", "sleep",       "pond-freeze",    "evaporation",
    "blood-stain", "flung-liquid", "far-fog",       "far-downsample",
    "screenshots", "player-walk", "player-waterjump", "player-plants", "debris",
    "prefab",      "mob",         "settle-back",    "player-body",
    "save-load",   "save-entities", "region-store", "streaming",     "spells",
    "perf",
};

const std::vector<Gate>& Registry() {
  static std::vector<Gate> all = [] {
    std::vector<Gate> pool;
    for (const auto* g : {&SimGates(), &RenderGates(), &PlayerGates(),
                          &MobGates(), &BodyGates(), &WorldIoGates(),
                          &SpellGates()})
      pool.insert(pool.end(), g->begin(), g->end());

    std::vector<Gate> v;
    for (const char* name : kOrder)
      for (const Gate& g : pool)
        if (std::strcmp(name, g.name) == 0) v.push_back(g);
    // Anything defined but not ordered would never run: say so loudly rather
    // than dropping it.
    for (const Gate& g : pool) {
      bool listed = false;
      for (const char* name : kOrder)
        if (std::strcmp(name, g.name) == 0) listed = true;
      if (!listed)
        std::fprintf(stderr,
                     "selftest: gate '%s' is not in kOrder and will not run\n",
                     g.name);
    }
    return v;
  }();
  return all;
}

std::string Format(const char* fmt, ...) {
  va_list a, b;
  va_start(a, fmt);
  va_copy(b, a);
  int n = std::vsnprintf(nullptr, 0, fmt, a);
  va_end(a);
  std::string out;
  if (n > 0) {
    out.resize((size_t)n);
    std::vsnprintf(&out[0], (size_t)n + 1, fmt, b);
  }
  va_end(b);
  return out;
}

namespace {

const Gate* Find(const std::string& name) {
  for (const Gate& g : Registry())
    if (name == g.name) return &g;
  return nullptr;
}

// Expand the requested gates with their transitive dependencies, then emit
// them in REGISTRY order. Registry order is the order the gates were written
// to run in, and several gates depend on world state left by an earlier one
// without saying so — keeping registry order means a subset run reproduces the
// same sequence the full run would, just with the irrelevant gates removed.
std::vector<const Gate*> Plan(const std::vector<std::string>& only) {
  std::unordered_set<std::string> want;
  if (only.empty()) {
    for (const Gate& g : Registry()) want.insert(g.name);
  } else {
    std::vector<std::string> stack = only;
    while (!stack.empty()) {
      std::string n = stack.back();
      stack.pop_back();
      if (!want.insert(n).second) continue;
      const Gate* g = Find(n);
      if (!g) {
        std::fprintf(stderr, "selftest: no such gate '%s' (try --list)\n",
                     n.c_str());
        continue;
      }
      for (const char* d : g->deps) stack.push_back(d);
    }
  }
  std::vector<const Gate*> plan;
  for (const Gate& g : Registry())
    if (want.count(g.name)) plan.push_back(&g);
  return plan;
}

// Baseline: gate name -> was it failing at the recorded commit. Hand-editable
// JSON, deliberately a flat object so a human can read a diff of it.
std::unordered_map<std::string, bool> LoadBaseline(const std::string& path) {
  std::unordered_map<std::string, bool> known;
  std::ifstream f(path);
  if (!f) return known;
  std::string text((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  // Scan for `"name" : "pass"|"fail"` pairs. No JSON dependency on purpose —
  // this is read before anything else is initialised, and the file is a flat
  // string->string map by design.
  //
  // STRICT about what sits between the key and the value: only whitespace and
  // one colon. A lenient version of this (search forward for the next quoted
  // token) silently paired a prose key with a LATER gate's verdict when the
  // file still carried comment arrays, which is how a "known failure" quietly
  // becomes an unnoticed regression. Keep it strict; keep prose in
  // tests/BASELINE.md instead.
  size_t i = 0;
  while ((i = text.find('"', i)) != std::string::npos) {
    size_t e = text.find('"', i + 1);
    if (e == std::string::npos) break;
    std::string key = text.substr(i + 1, e - i - 1);

    size_t p = e + 1;
    while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
    if (p >= text.size() || text[p] != ':') { i = e + 1; continue; }
    p++;
    while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
    if (p >= text.size() || text[p] != '"') { i = e + 1; continue; }

    size_t ve = text.find('"', p + 1);
    if (ve == std::string::npos) break;
    std::string val = text.substr(p + 1, ve - p - 1);
    if (val == "fail" || val == "pass") known[key] = (val == "fail");
    i = ve + 1;
  }
  return known;
}

const char* StatusWord(Status s) {
  return s == Status::Pass ? "PASS" : s == Status::Fail ? "FAIL" : "SKIP";
}

void WriteJson(const std::string& path, const std::vector<Result>& results) {
  std::ofstream f(path);
  if (!f) {
    std::fprintf(stderr, "selftest: cannot write %s\n", path.c_str());
    return;
  }
  f << "{\n  \"gates\": {\n";
  for (size_t i = 0; i < results.size(); i++) {
    const Result& r = results[i];
    std::string detail;
    for (char ch : r.detail) {  // escape for JSON
      if (ch == '"' || ch == '\\') detail += '\\';
      if (ch == '\n') { detail += "\\n"; continue; }
      detail += ch;
    }
    f << "    \"" << r.name << "\": {\"status\": \""
      << (r.status == Status::Pass ? "pass"
          : r.status == Status::Fail ? "fail" : "skip")
      << "\", \"seconds\": " << (int)(r.seconds * 100) / 100.0
      << ", \"detail\": \"" << detail << "\"}"
      << (i + 1 < results.size() ? "," : "") << "\n";
  }
  f << "  }\n}\n";
  std::printf("wrote %s\n", path.c_str());
}

}  // namespace

void Ctx::Grab(const char* path) {
  rhi::Buffer shot =
      CreateBuffer(ctx.device, (uint64_t)width * height * 4,
                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                   "screenshot");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  rhi::TexelCopyTexture srcT{};
  srcT.texture = offscreen;
  rhi::TexelCopyBuffer dstB{};
  dstB.buffer = shot;
  dstB.bytesPerRow = width * 4;
  dstB.rowsPerImage = height;
  rhi::Extent3D ext{width, height, 1};
  enc.CopyTextureToBuffer(srcT, dstB, ext);
  ctx.queue.Submit(enc.Finish());
  std::vector<uint8_t> pixels((size_t)width * height * 4);
  bool got = false;
  got = rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(), (size_t)(pixels.size()));
  if (got && WriteBmpFile(path, pixels, width, height))
    std::printf("wrote %s\n", path);
}

int List() {
  std::string group;
  for (const Gate& g : Registry()) {
    if (group != g.group) {
      group = g.group;
      std::printf("\n%s:\n", group.c_str());
    }
    std::printf("  %-24s", g.name);
    if (!g.deps.empty()) {
      std::printf(" needs:");
      for (const char* d : g.deps) std::printf(" %s", d);
    }
    if (g.advisory) std::printf("  [advisory]");
    std::printf("\n");
  }
  std::printf("\nrun one:  sandvox --selftest --gate <name>\n");
  return 0;
}

int Run(Ctx& c, const Options& opt) {
  std::vector<const Gate*> plan = Plan(opt.only);
  if (plan.empty()) {
    std::fprintf(stderr, "selftest: nothing to run\n");
    return 2;
  }
  // Resolve the baseline next to the ASSETS dir rather than the CWD: the exe
  // is normally run as ./build/Release/sandvox.exe from the checkout root, but
  // the tuner's Play button and a CI runner both invoke it from elsewhere, and
  // a baseline that silently fails to load reports every known failure as a
  // fresh regression.
  std::string bpath = opt.baselinePath;
  if (bpath.empty()) {
    namespace fs = std::filesystem;
    fs::path assets(AssetDir());
    fs::path guess = assets.parent_path() / "tests" / "baseline.json";
    bpath = fs::exists(guess) ? guess.string() : std::string("tests/baseline.json");
  }
  auto known = LoadBaseline(bpath);

  std::printf("=== selftest === (%zu gate%s)\n", plan.size(),
              plan.size() == 1 ? "" : "s");

  // The shared offscreen target every render-touching gate draws into.
  c.offscreen = c.ctx.device.CreateTexture({c.width, c.height, 1}, rhi::TextureFormat::RGBA8Unorm, rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc, "offscreen");
  c.view = c.offscreen.CreateView();

  std::vector<Result> results;
  std::unordered_map<std::string, Status> outcome;

  for (const Gate* g : plan) {
    // A gate whose dependency did not pass cannot produce a meaningful
    // verdict. Report SKIP so the run does not blame it for upstream damage.
    const char* blockedBy = nullptr;
    for (const char* d : g->deps) {
      auto it = outcome.find(d);
      if (it != outcome.end() && it->second != Status::Pass) blockedBy = d;
    }
    Result r;
    r.name = g->name;
    if (blockedBy) {
      r.status = Status::Skip;
      r.detail = std::string("depends on ") + blockedBy + ", which did not pass";
      std::printf("%s: SKIP (%s)\n", r.name.c_str(), r.detail.c_str());
    } else {
      // The gate bodies still print their own "name: PASS (numbers)" lines,
      // including the nested sub-gate lines ("mob gait: ...", "body blast:
      // ..."). Those ARE the diagnostic output, so the harness does not
      // reprint a verdict — it only records the status for the baseline diff
      // and the JSON. Timing is printed here since no gate measured itself.
      double t0 = NowSeconds();
      std::string detail;
      r.status = g->fn(c, detail);
      r.seconds = NowSeconds() - t0;
      r.detail = detail;
    }
    outcome[r.name] = r.status;
    results.push_back(std::move(r));
  }

  if (!opt.jsonPath.empty()) WriteJson(opt.jsonPath, results);

  // Verdict. A gate already failing in the baseline is reported but does not
  // turn the run red — that is the whole point: an agent sees at a glance
  // whether it introduced a failure or inherited one.
  std::vector<std::string> regressions, fixed, inherited;
  for (const Result& r : results) {
    const Gate* g = Find(r.name);
    if (g && g->advisory) continue;
    bool wasFailing = known.count(r.name) && known[r.name];
    if (r.status == Status::Fail && !wasFailing) regressions.push_back(r.name);
    if (r.status == Status::Fail && wasFailing) inherited.push_back(r.name);
    if (r.status == Status::Pass && wasFailing) fixed.push_back(r.name);
  }

  if (!inherited.empty()) {
    std::printf("\nknown-failing at baseline (not yours): ");
    for (size_t i = 0; i < inherited.size(); i++)
      std::printf("%s%s", inherited[i].c_str(),
                  i + 1 < inherited.size() ? ", " : "");
    std::printf("\n");
  }
  if (!fixed.empty()) {
    std::printf("FIXED since baseline: ");
    for (size_t i = 0; i < fixed.size(); i++)
      std::printf("%s%s", fixed[i].c_str(), i + 1 < fixed.size() ? ", " : "");
    std::printf("\n  (update tests/baseline.json to lock these in)\n");
  }
  if (!regressions.empty()) {
    std::printf("\nREGRESSIONS (%zu): ", regressions.size());
    for (size_t i = 0; i < regressions.size(); i++)
      std::printf("%s%s", regressions[i].c_str(),
                  i + 1 < regressions.size() ? ", " : "");
    std::printf("\n=== selftest FAIL ===\n");
    return 1;
  }
  std::printf("=== selftest PASS === (%zu known failure%s carried)\n",
              inherited.size(), inherited.size() == 1 ? "" : "s");
  return 0;
}

}  // namespace selftest
