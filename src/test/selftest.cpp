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
#include "sim/pagetable.h"  // PagesHighWater for the pool-margin report
#include "test/support.h"

using namespace sandvox;

namespace selftest {

// Each domain file exposes its gates through one of these.
const std::vector<Gate>& TerrainGates();
const std::vector<Gate>& TreeGates();
const std::vector<Gate>& SimGates();
const std::vector<Gate>& CaGates();
const std::vector<Gate>& WindGates();
const std::vector<Gate>& WaterGates();
const std::vector<Gate>& RenderGates();
const std::vector<Gate>& PlayerGates();
const std::vector<Gate>& MobGates();
const std::vector<Gate>& BodyGates();
const std::vector<Gate>& AudioGates();
const std::vector<Gate>& WorldIoGates();
const std::vector<Gate>& VoxRegionGates();
const std::vector<Gate>& SpellGates();
const std::vector<Gate>& PlayerKitGates();

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
    // FIRST, and deliberately. `terrain` measures pristine worldgen at the
    // origin and asserts the CPU height mirror against the GPU's voxels — the
    // property every later gate's fixture placement silently assumes. It also
    // has to run before anything moves the window, and it leaves the origin
    // exactly where `determinism` (which does not set it) needs it.
    // FIRST OF ALL, and it costs nothing to put it there: `player-kit` is
    // pure CPU with its own fixtures — no world, no GPU, no assets — so it can
    // neither disturb the pristine worldgen `terrain` needs nor be disturbed
    // by anything. Running it before the expensive gates also means a broken
    // equipment model is reported in the first second of a full run.
    "player-kit",
    // SECOND, and for the same reason: `tree-atlas` reads assets/trees/*.svtree
    // off disk and asserts on the bytes. No world, no GPU, no state left
    // behind -- and when the atlas is wrong every gate after it is measuring a
    // forest nobody authored, so it belongs before them rather than after.
    "tree-atlas",
    "terrain",
    // SECOND, and it wants the same thing `terrain` does: pristine worldgen at
    // an unmoved origin. Its whole subject is the ANALYTIC basin registry, and
    // the authored lake at (420,420) has to be resident for that to mean
    // anything — so it runs before `streaming` shifts the window rather than
    // after, and it regenerates on the way out so `determinism` (which
    // regenerates anyway) finds exactly what `terrain` left.
    "waterbody",
    "determinism", "sleep",       "ca-skip",     "ca-slope",
    "ca-slope-hybrid", "ca-level-one", "ca-level", "ca-level-pond",
    "evaporation", "wind",      "wind-gas",   "wind-prim",
    "blood-stain", "flung-liquid", "fluid-det",     "fluid-settle",
    "fluid-excite", "fluid-onwater", "fluid-stain", "fluid-react", "far-fog",  "far-downsample",
    "far-persist",
    "screenshots", "fire-depth", "player-walk", "player-waterjump", "player-ledgegrab",
    "player-plants", "debris",
    "audio-impact", "audio-mob-voice", "audio-ambience",
    // "mob" restored to its original slot (it sat between prefab and
    // settle-back until ec764e8 dropped it from both here and MobGates()).
    // The position matters: gates share one World and several depend on what
    // an earlier one left behind, so re-adding it anywhere else would be a
    // different test.
    "prefab",      "mob",            "settle-back", "player-body",
    "ragdoll-joints",
    "save-load",   "save-entities", "region-store", "streaming",     "spells",
    "page-roundtrip", "daylight-boundary",
    // Per-voxel body reactivity. Late, and it must be: it lights real fires and
    // pours real acid at absolute coordinates, and it regenerates the world on
    // the way out so the gates after it still find pristine terrain (rule 7).
    "mob-burn",
    // LAST of the world-touching gates, and it must be: BuildVoxRegion moves
    // the residency window and resets the page table, which is the state every
    // other gate's fixture placement assumes. It restores both before it
    // returns, but running it early would make any bug in that restore look
    // like a failure somewhere else (CLAUDE.md rule 7).
    "voxregion",
    "perf",
};

const std::vector<Gate>& Registry() {
  static std::vector<Gate> all = [] {
    std::vector<Gate> pool;
    for (const auto* g : {&TerrainGates(), &TreeGates(),
                          &SimGates(), &CaGates(), &WindGates(), &WaterGates(),
                          &RenderGates(),
                          &PlayerGates(),
                          &MobGates(), &BodyGates(), &WorldIoGates(), &AudioGates(),
                          &VoxRegionGates(),
                          &SpellGates(), &PlayerKitGates()})
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

// Every non-pass/fail value from the baseline, keyed by name — the golden world
// hash is just the first inhabitant. File-scope because gates live in other TUs
// and take no options argument; Run() sets it before the first gate runs.
std::unordered_map<std::string, std::string> g_baselineVals;

// Baseline: gate name -> was it failing at the recorded commit. Hand-editable
// JSON, deliberately a flat object so a human can read a diff of it.
//
// Also picks up every value that is NOT "pass"/"fail" — "determinismHash", and
// any threshold a gate pins — into g_baselineVals. Same flat string->string
// shape, so the scanner below needs no new syntax, only a second place to put
// the value. Keys starting with '_' are prose (`_about`, `_smoke_about`) and
// are kept out of the map so nothing can accidentally read one as a threshold.
std::unordered_map<std::string, bool> LoadBaseline(const std::string& path) {
  std::unordered_map<std::string, bool> known;
  g_baselineVals.clear();
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
    else if (!key.empty() && key[0] != '_') g_baselineVals[key] = val;
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

// Replace the quoted value of `"key": "..."` in place, leaving comments, key
// order and prose untouched. Returns false when the key is absent — which is
// SILENT AND DELIBERATE for gate verdicts (the baseline records only gates
// worth pinning), but means a gate's new observed key must be seeded into the
// file by hand once or --rebaseline will appear to work and write nothing.
bool ReplaceJsonValue(std::string& text, const std::string& key,
                      const std::string& val, std::string* oldOut) {
  const std::string keyPat = "\"" + key + "\"";
  size_t p = text.find(keyPat);
  if (p == std::string::npos) return false;
  size_t colon = text.find(':', p + keyPat.size());
  if (colon == std::string::npos) return false;
  size_t q1 = text.find('"', colon + 1);
  if (q1 == std::string::npos) return false;
  size_t q2 = text.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  if (oldOut) *oldOut = text.substr(q1 + 1, q2 - q1 - 1);
  text = text.substr(0, q1 + 1) + val + text.substr(q2);
  return true;
}

void RebaselineSelftest(const std::string& path,
                        const std::vector<Result>& results) {
  // Read the determinism gate's observed hash from its detail string.
  // The determinism gate's detail is "hash <hex> (N ticks, ...)" — the hash
  // is the second token.
  std::string newHash;
  for (const Result& r : results) {
    // Pass OR pinnedOnly. pinnedOnly means the twice-run comparison was
    // IDENTICAL and only the golden pin differs -- which is precisely the run
    // whose hash we are here to record. Requiring Pass made this function
    // unreachable in the only case it exists for.
    if (r.name == "determinism" &&
        (r.status == Status::Pass || r.pinnedOnly)) {
      // Parse "hash XXXXXXXX ..." from the detail
      size_t p = r.detail.find("hash ");
      if (p != std::string::npos) {
        p += 5;
        size_t e = r.detail.find(' ', p);
        if (e == std::string::npos) e = r.detail.size();
        newHash = r.detail.substr(p, e - p);
      }
    }
  }

  // Read baseline, replace values
  std::ifstream fi(path);
  if (!fi) {
    std::fprintf(stderr, "rebaseline: cannot read %s\n", path.c_str());
    return;
  }
  std::string text((std::istreambuf_iterator<char>(fi)),
                   std::istreambuf_iterator<char>());
  fi.close();

  // Update determinismHash if we have a new one
  const std::string oldHash = GoldenDeterminismHash();
  if (!newHash.empty() && newHash != oldHash) {
    std::string was;
    if (ReplaceJsonValue(text, "determinismHash", newHash, &was))
      std::printf("\n*** determinismHash: %s -> %s ***\n", was.c_str(),
                  newHash.c_str());
  }

  // Update gate pass/fail status
  int changed = 0;
  for (const Result& r : results) {
    if (r.status == Status::Skip) continue;
    // A pinnedOnly failure records as PASS, and it has to: the write above just
    // updated the pin that made it fail, so it will pass on the next run.
    // Recording "fail" would enter it in the known-failing set and mask the
    // very regression the gate exists to catch — a rebaseline that quietly
    // disarms `determinism` is worse than one that refuses.
    const char* newVal =
        (r.status == Status::Pass || r.pinnedOnly) ? "pass" : "fail";
    std::string oldVal;
    if (!ReplaceJsonValue(text, r.name, newVal, &oldVal)) continue;
    if (oldVal != newVal) {
      std::printf("  %s: %s -> %s\n", r.name.c_str(), oldVal.c_str(), newVal);
      changed++;
    }
  }

  // Update measured values (Result::observed). These are what make a threshold
  // tunable without a rebuild — but only for keys that already exist in the
  // file, so a gate that adds one must seed it there by hand once. Say so out
  // loud rather than dropping it silently, because a value that never lands is
  // indistinguishable from a value that never moved.
  int vals = 0;
  for (const Result& r : results) {
    for (const auto& kv : r.observed) {
      std::string oldVal;
      if (!ReplaceJsonValue(text, kv.first, kv.second, &oldVal)) {
        std::printf("  %s: NOT IN BASELINE (add \"%s\": \"%s\" by hand)\n",
                    kv.first.c_str(), kv.first.c_str(), kv.second.c_str());
        continue;
      }
      if (oldVal != kv.second) {
        std::printf("  %s: %s -> %s\n", kv.first.c_str(), oldVal.c_str(),
                    kv.second.c_str());
        vals++;
      }
    }
  }
  changed += vals;

  std::ofstream fo(path);
  if (!fo) {
    std::fprintf(stderr, "rebaseline: cannot write %s\n", path.c_str());
    return;
  }
  fo << text;
  std::printf("\n*** REBASELINED %s (%d gate%s changed) ***\n", path.c_str(),
              changed, changed == 1 ? "" : "s");
}

}  // namespace

// Values the CURRENT gate has recorded, drained by Run() when it returns.
std::vector<std::pair<std::string, std::string>> g_observed;
bool g_pinnedOnly = false;

void RecordObserved(const char* key, const std::string& value) {
  g_observed.emplace_back(key, value);
}

// See Result::pinnedOnly. A gate calls this after it has proved its own
// invariant still holds and found that only the RECORDED value differs.
void MarkPinnedOnly() { g_pinnedOnly = true; }

void RecordObserved(const char* key, double value) {
  // Integers as integers: a threshold reading "1364" is diffable in a way that
  // "1364.000000" is not, and every value pinned so far is a count or a voxel.
  char buf[64];
  if (value == (double)(long long)value)
    std::snprintf(buf, sizeof buf, "%lld", (long long)value);
  else
    std::snprintf(buf, sizeof buf, "%.3f", value);
  g_observed.emplace_back(key, buf);
}

const std::string* BaselineValue(const char* key) {
  auto it = g_baselineVals.find(key);
  return it == g_baselineVals.end() ? nullptr : &it->second;
}

double BaselineNumber(const char* key, double fallback) {
  const std::string* v = BaselineValue(key);
  if (!v || v->empty()) return fallback;
  try {
    size_t used = 0;
    const double d = std::stod(*v, &used);
    return used == 0 ? fallback : d;
  } catch (...) {
    return fallback;
  }
}

const std::string& GoldenDeterminismHash() {
  static const std::string kEmpty;
  const std::string* v = BaselineValue("determinismHash");
  return v ? *v : kEmpty;
}

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
    // Declared, not inferred — see the Gate::needsRender comment. Printing it
    // is what makes "which gates could run before the Vulkan render path
    // exists" a question with an answer in the binary rather than in a commit
    // message.
    if (g.needsRender) std::printf("  [needs-render]");
    std::printf("\n");
  }
  std::printf("\nrun one:  sandvox --selftest --gate <name>\n");
  std::printf("[needs-render] = drives the offscreen target / a draw; the rest\n"
              "are compute + readback only. All 23 run on both backends since\n"
              "phase 4b; the flag remains as documentation.\n");
  return 0;
}

int Run(Ctx& c, const Options& opt) {
  // Make a harness tick behave like a game frame: block for the readback map
  // after a kicked readback so World::Snap() actually becomes valid. See the
  // block comment on SetHarnessSnapshotDrain (test/support.h) for why the
  // harnesses need this and the game does not.
  SetHarnessSnapshotDrain(true);
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

  std::printf("=== selftest === (%zu gate%s, backend vulkan)\n", plan.size(),
              plan.size() == 1 ? "" : "s");

  // The shared offscreen target every render-touching gate draws into.
  // Gate::needsRender remains declared (and printed by --list) as
  // documentation of which gates drive the render path, but nothing skips on
  // it: all 23 gates run.
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
      g_observed.clear();
      g_pinnedOnly = false;
      r.status = g->fn(c, detail);
      r.seconds = NowSeconds() - t0;
      r.detail = detail;
      r.observed = std::move(g_observed);
      g_observed.clear();
      r.pinnedOnly = g_pinnedOnly;
      g_pinnedOnly = false;
    }
    outcome[r.name] = r.status;
    results.push_back(std::move(r));
  }

  if (!opt.jsonPath.empty()) WriteJson(opt.jsonPath, results);
  WriteJson("build/last_run.json", results);

  // Verdict. A gate already failing in the baseline is reported but does not
  // turn the run red — that is the whole point: an agent sees at a glance
  // whether it introduced a failure or inherited one.
  std::vector<std::string> regressions, fixed, inherited, pinnedMoved;
  for (const Result& r : results) {
    const Gate* g = Find(r.name);
    if (g && g->advisory) continue;
    bool wasFailing = known.count(r.name) && known[r.name];
    // A gate that failed ONLY because its pinned value moved is not a
    // regression to a run that was invoked to move it. Outside --rebaseline it
    // still is one: `--selftest` must go red when the world changes under you.
    if (r.status == Status::Fail && r.pinnedOnly && opt.rebaseline) {
      pinnedMoved.push_back(r.name);
      continue;
    }
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
  // Vulkan runs with --vk-validation: print whatever the messenger collected
  // during the whole suite (nothing pops a scope mid-run), and let a hazard
  // turn the run red — a sync-validation message IS a barrier bug (§6.2).
  const size_t vkMsgs = c.ctx.ReportVkValidation("selftest");

  // Page faults, over the WHOLE suite (PLAN_page_table.md §2.4, §4.4). The
  // counter is monotonic and unconditional, so this one read covers every gate
  // that ran — it is what turns "a kernel can never write through a sentinel"
  // from a structural claim into a measurement made on every run. Non-zero
  // means some chunk a kernel wrote was not materialized before its dispatch,
  // which is risk 1 and is always a bug.
  uint32_t pageFaults[4] = {0, 0, 0, 0};
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.pageFaults, 0,
                        pageFaults, 16, "pageFaults");
  // WHAT WAS LOST AND WHERE, from voxStore's three spare words: [2] the widest
  // word it dropped, [1]/[3] the highest and lowest refusing chunk slot.
  //
  // The count on its own names nothing. Finding the 58 faults this reporting
  // was written for cost a day of turning worldgen features off one at a time —
  // ponds, shores, ruins, caves, the sediment wedge, evaporation, the MPM seam
  // — because "58 voxels went missing somewhere" is compatible with all of
  // them. The word decoded to `stone, stain wet/1` and the span to a single
  // chunk, and that is the whole answer in one line: a pond's water staining
  // the rock behind its bank, into a chunk still held as a JITTER sentinel.
  std::string lost;
  if (pageFaults[0]) {
    const uint32_t m = pageFaults[2] & 0xFFFu;
    const char* nm = m == 0 ? "air"
                     : m < c.mats.size() ? c.mats[m].name.c_str() : "?";
    lost += Format(" | lost %s (id %u, word 0x%08x: state %u stamp %u"
                   " stain %u/%u)", nm, m, pageFaults[2],
                   (pageFaults[2] >> 12) & 0xF, (pageFaults[2] >> 16) & 0x7,
                   (pageFaults[2] >> 24) & 0xF, (pageFaults[2] >> 28) & 0x7);
    if (pageFaults[1] != 0) {
      const uint32_t hi = pageFaults[1] - 1, lo = 0xFFFFFFFFu - pageFaults[3];
      const IVec3 a = c.world.SlotToWorldChunk(lo);
      const IVec3 b = c.world.SlotToWorldChunk(hi);
      lost += Format(" | refusing chunks (%d,%d,%d)..(%d,%d,%d), entries"
                     " 0x%08x/0x%08x", a.x * 16, a.y * 16, a.z * 16, b.x * 16,
                     b.y * 16, b.z * 16, c.world.PageEntryOfSlot(lo),
                     c.world.PageEntryOfSlot(hi));
    }
  }
  std::printf("page faults over the suite: %u%s%s\n", pageFaults[0],
              pageFaults[0] == 0 ? " (a sentinel write is a lost voxel: 0 is the"
                                   " only acceptable value)"
                                 : "  *** SENTINEL WRITES LOST VOXELS ***",
              lost.c_str());

  // Pool high-water over the WHOLE suite. Under §3.8's fatal-exhaustion policy
  // kPoolPages is safety-critical rather than advisory, and §3.8 requires the
  // margin to be a TRACKED NUMBER rather than an assumption: the suite is the
  // worst case the gates can produce, so its high-water is what kPoolPages must
  // be sized against. Reported unconditionally in paged mode so a change that
  // eats the headroom shows up as a moving number long before it shows up as an
  // abort. Dense is the identity map and has no pool, so it is omitted there.
  if (c.world.residency == World::Residency::Paged && c.world.pages) {
    const uint32_t hw = c.world.pages->PagesHighWater();
    std::printf("page pool high water over the suite: %u of %u (%.1f%%, "
                "%.1f MiB of %.1f MiB reserved)\n",
                hw, kPoolPages, 100.0 * (double)hw / (double)kPoolPages,
                (double)hw * kChunkVol * 4.0 / (1024.0 * 1024.0),
                (double)kPoolPages * kChunkVol * 4.0 / (1024.0 * 1024.0));
  }

  if (!pinnedMoved.empty()) {
    std::printf("\nPINNED VALUES MOVED (%zu): ", pinnedMoved.size());
    for (size_t i = 0; i < pinnedMoved.size(); i++)
      std::printf("%s%s", pinnedMoved[i].c_str(),
                  i + 1 < pinnedMoved.size() ? ", " : "");
    std::printf("\n  each of these verified its own invariant and differs from "
                "the baseline only in a RECORDED value, which is what this run "
                "was invoked to update.\n");
  }

  if (!regressions.empty() || vkMsgs > 0 || pageFaults[0] != 0) {
    if (!regressions.empty()) {
      std::printf("\nREGRESSIONS (%zu): ", regressions.size());
      for (size_t i = 0; i < regressions.size(); i++)
        std::printf("%s%s", regressions[i].c_str(),
                    i + 1 < regressions.size() ? ", " : "");
    }
    if (vkMsgs > 0)
      std::printf("\nvulkan validation reported %zu message%s (see above)",
                  vkMsgs, vkMsgs == 1 ? "" : "s");
    if (pageFaults[0] != 0)
      std::printf("\n%u page fault%s: a sim kernel wrote through a sentinel and"
                  " the voxel was LOST (PLAN_page_table.md risk 1)",
                  pageFaults[0], pageFaults[0] == 1 ? "" : "s");
    if (opt.rebaseline) {
      std::printf("\n*** REFUSING to rebaseline: the run has errors ***\n");
    }
    std::printf("\n=== selftest FAIL ===\n");
    return 1;
  }

  if (opt.rebaseline) {
    RebaselineSelftest(bpath, results);
    std::printf("*** THIS WAS A REBASELINE, NOT A PASS. ***\n");
  }

  std::printf("=== selftest PASS === (%zu known failure%s carried)\n",
              inherited.size(), inherited.size() == 1 ? "" : "s");
  return 0;
}

}  // namespace selftest
