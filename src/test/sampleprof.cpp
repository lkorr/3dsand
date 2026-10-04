// See sampleprof.h.
#include "test/sampleprof.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace sampleprof {

#ifdef _WIN32
namespace {

constexpr int kDepth = 48;
// ~1 kHz for a few minutes. Preallocated: the sampler runs while the target
// is SUSPENDED and must not touch the heap (the target may hold its lock).
constexpr size_t kMaxSamples = 400000;

struct State {
  std::atomic<bool> run{false};
  std::thread th;
  HANDLE target = nullptr;
  std::string label;
  std::vector<uint64_t> frames;   // kMaxSamples * kDepth
  std::vector<uint8_t> depth;     // per sample
  size_t n = 0;
  uint64_t lost = 0;
};
State g;

void SampleLoop() {
  timeBeginPeriod(1);
  // A HIGH-RESOLUTION waitable timer: Windows 11 may ignore timeBeginPeriod
  // for a process with no foreground window, and Sleep(1) then sleeps a
  // 15.6 ms quantum (measured: 1,547 samples for a 21 s fight instead of
  // ~14k). Falls back to Sleep(1) where the flag is unknown.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  auto nap = [&] {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -10000;  // 1 ms, relative, 100 ns units
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
      }
    }
    Sleep(1);
  };
  while (g.run.load(std::memory_order_relaxed)) {
    if (g.n >= kMaxSamples) {
      g.lost++;
      nap();
      continue;
    }
    if (SuspendThread(g.target) == (DWORD)-1) break;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    int d = 0;
    if (GetThreadContext(g.target, &ctx)) {
      uint64_t* out = &g.frames[g.n * kDepth];
      for (; d < kDepth; d++) {
        const DWORD64 pc = ctx.Rip;
        if (pc == 0) break;
        out[d] = pc;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (fe == nullptr) {
          // A leaf with no unwind data: its return address is at [rsp].
          if (ctx.Rsp == 0) break;
          ctx.Rip = *(const DWORD64*)ctx.Rsp;
          ctx.Rsp += 8;
        } else {
          void* handlerData = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, fe, &ctx,
                           &handlerData, &establisher, nullptr);
        }
      }
    }
    ResumeThread(g.target);
    if (d > 0) {
      g.depth[g.n] = (uint8_t)d;
      g.n++;
    }
    nap();
  }
  if (timer) CloseHandle(timer);
  timeEndPeriod(1);
}

}  // namespace

void Start(const char* label) {
  if (std::getenv("SANDVOX_SAMPLE_PROF") == nullptr) return;
  if (g.run.load()) return;
  g.label = label ? label : "";
  if (g.frames.empty()) {
    g.frames.assign(kMaxSamples * kDepth, 0);
    g.depth.assign(kMaxSamples, 0);
  }
  g.n = 0;
  g.lost = 0;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                  &g.target, THREAD_ALL_ACCESS, FALSE, 0);
  g.run.store(true);
  g.th = std::thread(SampleLoop);
}

void Stop(int topN) {
  if (!g.run.load()) return;
  g.run.store(false);
  g.th.join();
  CloseHandle(g.target);
  g.target = nullptr;

  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, nullptr, TRUE);
  alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
  // pc -> function id; function id -> name.
  std::unordered_map<uint64_t, uint32_t> pcFn;
  std::unordered_map<uint64_t, uint32_t> startFn;
  std::vector<std::string> names;
  auto fnOf = [&](uint64_t pc) -> uint32_t {
    auto it = pcFn.find(pc);
    if (it != pcFn.end()) return it->second;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    DWORD64 disp = 0;
    uint64_t start = pc;
    std::string name;
    if (SymFromAddr(proc, pc, &disp, sym)) {
      start = sym->Address;
      name = sym->Name;
    } else {
      char b[32];
      std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)pc);
      name = b;
    }
    auto st = startFn.find(start);
    uint32_t id;
    if (st != startFn.end()) {
      id = st->second;
    } else {
      id = (uint32_t)names.size();
      if (name.size() > 120) name = name.substr(0, 120);
      names.push_back(name);
      startFn.emplace(start, id);
    }
    pcFn.emplace(pc, id);
    return id;
  };
  std::vector<uint64_t> self, incl;
  std::unordered_map<uint64_t, uint64_t> edges;
  std::vector<uint32_t> fns;
  std::unordered_set<uint64_t> seenEdge;
  std::vector<uint32_t> seenFn;
  for (size_t s = 0; s < g.n; s++) {
    const uint64_t* f = &g.frames[s * kDepth];
    const int d = g.depth[s];
    fns.clear();
    for (int i = 0; i < d; i++) fns.push_back(fnOf(f[i]));
    if (self.size() < names.size()) {
      self.resize(names.size(), 0);
      incl.resize(names.size(), 0);
    }
    self[fns[0]]++;
    seenFn.clear();
    for (uint32_t id : fns)
      if (std::find(seenFn.begin(), seenFn.end(), id) == seenFn.end()) {
        seenFn.push_back(id);
        incl[id]++;
      }
    seenEdge.clear();
    for (int i = 0; i + 1 < d; i++) {
      const uint64_t k = ((uint64_t)fns[i + 1] << 32) | fns[i];
      if (fns[i] != fns[i + 1] && seenEdge.insert(k).second) edges[k]++;
    }
  }
  self.resize(names.size(), 0);
  incl.resize(names.size(), 0);
  // LEAF SOURCE LINES: a function's self time says which function, the line
  // says which loop of it (lambdas and inlined helpers fold into their caller).
  std::vector<std::pair<uint64_t, std::string>> lines;
  {
    std::unordered_map<std::string, uint64_t> byLine;
    std::unordered_map<uint64_t, std::string> pcLine;
    for (size_t s = 0; s < g.n; s++) {
      const uint64_t pc = g.frames[s * kDepth];
      auto it = pcLine.find(pc);
      if (it == pcLine.end()) {
        IMAGEHLP_LINE64 ln{};
        ln.SizeOfStruct = sizeof(ln);
        DWORD d32 = 0;
        std::string key;
        if (SymGetLineFromAddr64(proc, pc, &d32, &ln)) {
          std::string f = ln.FileName ? ln.FileName : "?";
          const size_t cut = f.find_last_of("\\/");
          if (cut != std::string::npos) f = f.substr(cut + 1);
          key = f + ":" + std::to_string(ln.LineNumber) + "  " + names[fnOf(pc)].substr(0, 70);
        } else {
          key = names[fnOf(pc)].substr(0, 70);
        }
        it = pcLine.emplace(pc, key).first;
      }
      byLine[it->second]++;
    }
    for (auto& kv : byLine) lines.push_back({kv.second, kv.first});
    std::sort(lines.begin(), lines.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
  }
  SymCleanup(proc);

  std::string out;
  char line[512];
  std::snprintf(line, sizeof line,
                "sampleprof [%s]: %zu samples (~1 ms each), %llu dropped\n",
                g.label.c_str(), g.n, (unsigned long long)g.lost);
  out += line;
  const double tot = g.n ? (double)g.n : 1.0;
  auto table = [&](const char* title, const std::vector<uint64_t>& v) {
    std::vector<uint32_t> ord(v.size());
    for (uint32_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::sort(ord.begin(), ord.end(),
              [&](uint32_t a, uint32_t b) { return v[a] > v[b]; });
    out += std::string("  -- ") + title + " --\n";
    for (int i = 0; i < topN && i < (int)ord.size(); i++) {
      if (v[ord[i]] == 0) break;
      std::snprintf(line, sizeof line, "  %6.2f%% %7llu  %s\n",
                    100.0 * (double)v[ord[i]] / tot,
                    (unsigned long long)v[ord[i]], names[ord[i]].c_str());
      out += line;
    }
  };
  table("SELF", self);
  table("INCLUSIVE", incl);
  out += "  -- SELF BY SOURCE LINE --\n";
  for (int i = 0; i < topN * 2 && i < (int)lines.size(); i++) {
    std::snprintf(line, sizeof line, "  %6.2f%% %7llu  %s\n",
                  100.0 * (double)lines[i].first / tot,
                  (unsigned long long)lines[i].first, lines[i].second.c_str());
    out += line;
  }
  {
    std::vector<std::pair<uint64_t, uint64_t>> e(edges.begin(), edges.end());
    std::sort(e.begin(), e.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    // Every edge down to 0.05% goes to the FILE (the whole call tree that
    // matters, greppable by caller); stdout keeps the head of it.
    std::string full = "  -- CALLER -> CALLEE (inclusive, >= 0.05%) --\n";
    out += "  -- CALLER -> CALLEE (inclusive; all >= 0.05% in build/sampleprof.txt) --\n";
    for (int i = 0; i < (int)e.size(); i++) {
      const double pct = 100.0 * (double)e[i].second / tot;
      if (pct < 0.05) break;
      std::snprintf(line, sizeof line, "  %6.2f%%  %s -> %s\n", pct,
                    names[(uint32_t)(e[i].first >> 32)].c_str(),
                    names[(uint32_t)e[i].first].c_str());
      full += line;
      if (i < topN * 2) out += line;
    }
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    if (FILE* f = std::fopen("build/sampleprof.txt", "a")) {
      std::fputs(out.c_str(), f);
      std::fputs(full.c_str(), f);
      std::fclose(f);
    }
  }
}
#else
void Start(const char*) {}
void Stop(int) {}
#endif

}  // namespace sampleprof
