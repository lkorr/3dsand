// See workpool.h.
//
// THE JOB PROTOCOL (fight64 round 4, package W). One job is in flight at a
// time. It is identified by a generation `gen`; the claim word packs the low
// 32 bits of that generation with the next unclaimed index, so a claim is a
// CAS that also proves the job it claims from is still the posted one. That is
// what lets the caller return the moment every ITEM is done instead of waiting
// for every WORKER to wake and check out (the old join): a worker that wakes
// late finds the claim word on a later generation (or exhausted) and claims
// nothing, so it can never run a finished job's function on a stale index.
//
// Posting job g+1 (caller, after job g's items are all done):
//   1. claim := (g+1, CLOSED)   -- a worker still holding g fails its CAS
//   2. fn / n / grain / done := g+1's (release stores)
//   3. claim := (g+1, 0)        -- open
//   4. gen := g+1 (release), wake sleepers
// A worker that read gen = g and then g+1's parameters (torn between steps 2
// and 4) loaded those parameters with acquire, so step 1 happens-before its
// claim load: its CAS sees generation g+1 != g and claims nothing.
//
// WAITING. Workers spin on `gen` for SANDVOX_POOL_SPIN_US (default
// kDefaultSpinUs) after each job, then sleep on a condition variable; the
// caller spins the same bound on `done` before sleeping. A spin always ends in
// a sleep, so an idle game burns no core: the pool is only ever spinning for
// the spin bound after a ParallelFor returned. SANDVOX_POOL_LEGACY=1 is the
// before-arm of package W in one binary: no spin, and the caller waits for
// every worker to check out of every job, as the pool did before.
#include "game/workpool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define WP_PAUSE() _mm_pause()
#else
#define WP_PAUSE() std::this_thread::yield()
#endif

namespace workpool {
namespace {

thread_local bool tl_worker = false;
thread_local bool tl_inTask = false;

// The spin bound when SANDVOX_POOL_SPIN_US is unset. Measured on the brawl
// (mob-cap64): the gaps between ParallelFor calls in one tick are mostly
// under this, so a worker that finished one call is still spinning when the
// next is posted; the mob pass's last call ends the spin within this bound.
constexpr int kDefaultSpinUs = 200;
constexpr uint64_t kClaimClosed = 0xFFFFFFFFull;

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// One participant's record of one job: when it ran its first item and when
// its last item returned. Written by the participant BEFORE it adds its items
// to `done` (release), read by the caller after `done` reaches n (acquire).
struct alignas(64) Part {
  std::atomic<uint64_t> gen{0};
  int64_t first = 0, last = 0;
};

struct Pool {
  std::vector<std::thread> threads;
  std::unique_ptr<Part[]> parts;   // [0] = the caller, [1 + w] = worker w
  std::mutex mu;
  std::condition_variable wake, idle;
  std::atomic<uint64_t> gen{0};
  std::atomic<const std::function<void(size_t)>*> fn{nullptr};
  std::atomic<size_t> n{0}, grain{1};
  std::atomic<int64_t> postNs{0};
  std::atomic<uint64_t> claim{kClaimClosed};
  std::atomic<size_t> done{0};
  std::atomic<int> sleepers{0};
  std::atomic<bool> callerSleeping{false};
  std::atomic<int> checkedOut{0};   // legacy join only
  std::atomic<int> limit{0};
  int64_t spinNs = 0;
  bool legacy = false;

  // Claims and runs items of job g until none are left; returns the count.
  size_t Run(uint64_t g, Part& part) {
    const std::function<void(size_t)>* f = fn.load(std::memory_order_acquire);
    const size_t N = n.load(std::memory_order_acquire);
    const size_t G = grain.load(std::memory_order_acquire);
    const uint64_t tag = (g & 0xFFFFFFFFull) << 32;
    size_t items = 0;
    int64_t first = 0;
    uint64_t c = claim.load(std::memory_order_acquire);
    for (;;) {
      if ((c & ~0xFFFFFFFFull) != tag) break;   // a later job (or none)
      const size_t i0 = (size_t)(c & 0xFFFFFFFFull);
      if (i0 >= N) break;                         // exhausted (or CLOSED)
      if (!claim.compare_exchange_weak(c, c + G, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
        continue;
      if (items == 0) first = NowNs();
      const size_t i1 = std::min(N, i0 + G);
      for (size_t i = i0; i < i1; i++) (*f)(i);
      items += i1 - i0;
      c = claim.load(std::memory_order_acquire);
    }
    if (items == 0) return 0;
    part.first = first;
    part.last = NowNs();
    part.gen.store(g, std::memory_order_release);
    const size_t prev = done.fetch_add(items);
    if (prev + items == N && callerSleeping.load()) {
      std::lock_guard<std::mutex> lk(mu);
      idle.notify_one();
    }
    return items;
  }

  void Worker(int index) {
    tl_worker = true;
    tl_inTask = true;
    uint64_t seen = 0;
    for (;;) {
      uint64_t g = gen.load(std::memory_order_acquire);
      if (g == seen && spinNs > 0) {
        const int64_t until = NowNs() + spinNs;
        for (;;) {
          for (int k = 0; k < 16; k++) WP_PAUSE();
          g = gen.load(std::memory_order_acquire);
          if (g != seen || NowNs() >= until) break;
        }
      }
      if (g == seen) {
        std::unique_lock<std::mutex> lk(mu);
        sleepers.fetch_add(1);
        wake.wait(lk, [&] { return gen.load() != seen; });
        sleepers.fetch_sub(1);
        g = gen.load(std::memory_order_acquire);
      }
      seen = g;
      const int lim = limit.load(std::memory_order_relaxed);
      if (lim <= 0 || index < lim - 1) Run(g, parts[1 + index]);
      if (legacy && checkedOut.fetch_add(1) + 1 == (int)threads.size() &&
          callerSleeping.load()) {
        std::lock_guard<std::mutex> lk(mu);
        idle.notify_one();
      }
    }
  }
};

int ConfiguredThreads() {
  if (const char* e = std::getenv("SANDVOX_MOB_THREADS")) {
    const int n = std::atoi(e);
    if (n >= 1) return std::min(n, 32);
  }
  const int hw = (int)std::thread::hardware_concurrency();
  return std::clamp(hw / 2 - 1, 1, 7) + 1;  // workers + the caller
}

Pool& Get() {
  static Pool* p = [] {
    Pool* pool = new Pool();   // never destroyed: workers outlive static teardown
    pool->legacy = std::getenv("SANDVOX_POOL_LEGACY") != nullptr;
    int spinUs = kDefaultSpinUs;
    if (const char* e = std::getenv("SANDVOX_POOL_SPIN_US")) spinUs = std::atoi(e);
    // Bounded: a spin must end in a sleep within a fraction of a millisecond.
    spinUs = std::clamp(spinUs, 0, 1000);
    pool->spinNs = pool->legacy ? 0 : (int64_t)spinUs * 1000;
    const int workers = ConfiguredThreads() - 1;
    pool->parts.reset(new Part[(size_t)workers + 1]);
    for (int i = 0; i < workers; i++)
      pool->threads.emplace_back([pool, i] { pool->Worker(i); });
    return pool;
  }();
  return *p;
}

Stats g_stats;
int64_t g_lastEndNs = 0;

}  // namespace

int Threads() {
  Pool& p = Get();
  const int all = (int)p.threads.size() + 1;
  const int lim = p.limit.load(std::memory_order_relaxed);
  return lim > 0 ? std::min(lim, all) : all;
}

void SetThreadLimit(int n) { Get().limit.store(std::max(0, n), std::memory_order_relaxed); }

bool OnWorker() { return tl_worker; }

bool InTask() { return tl_inTask; }

const Stats& GetStats() { return g_stats; }

void ResetStats() {
  g_stats = Stats{};
  g_lastEndNs = 0;
}

int SpinMicros() { return (int)(Get().spinNs / 1000); }

bool Legacy() { return Get().legacy; }

void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn) {
  if (n == 0) return;
  grain = std::max<size_t>(1, grain);
  Pool& p = Get();
  // Serial when there is nobody to share with, nothing to share, or we are
  // already inside a task (no nesting: a worker waiting on its own pool
  // would deadlock).
  if (p.threads.empty() || n <= grain || tl_inTask ||
      p.limit.load(std::memory_order_relaxed) == 1) {
    if (!tl_inTask && !tl_worker) g_stats.serialCalls++;
    for (size_t i = 0; i < n; i++) fn(i);
    return;
  }
  // The index lives in the claim word's low 32 bits (plus one grain of
  // overshoot); nothing in the tick comes near it.
  if (n >= 0x7FFFFFFFull) {
    for (size_t i = 0; i < n; i++) fn(i);
    return;
  }
  const uint64_t g = p.gen.load(std::memory_order_relaxed) + 1;
  const uint64_t tag = (g & 0xFFFFFFFFull) << 32;
  const int64_t t0 = NowNs();
  p.claim.store(tag | kClaimClosed);                       // 1
  p.fn.store(&fn, std::memory_order_release);              // 2
  p.n.store(n, std::memory_order_release);
  p.grain.store(grain, std::memory_order_release);
  p.done.store(0);
  p.checkedOut.store(0);
  p.postNs.store(t0, std::memory_order_release);
  p.claim.store(tag);                                      // 3
  p.gen.store(g);   // 4 (seq_cst: ordered before the sleepers load below)
  if (p.legacy || p.sleepers.load() > 0) {
    // No lost wake-up: a worker bumps `sleepers` and tests `gen` under the
    // mutex before it waits, so either it saw gen move, or we saw it counted
    // and this lock waits until it is inside wait().
    { std::lock_guard<std::mutex> lk(p.mu); }
    p.wake.notify_all();
  }
  tl_inTask = true;
  p.Run(g, p.parts[0]);
  tl_inTask = false;
  const int nWorkers = (int)p.threads.size();
  auto finished = [&] {
    return p.done.load() == n && (!p.legacy || p.checkedOut.load() == nWorkers);
  };
  if (!finished() && p.spinNs > 0) {
    const int64_t until = NowNs() + p.spinNs;
    do {
      for (int k = 0; k < 16; k++) WP_PAUSE();
    } while (!finished() && NowNs() < until);
  }
  if (!finished()) {
    std::unique_lock<std::mutex> lk(p.mu);
    p.callerSleeping.store(true);
    p.idle.wait(lk, finished);
    p.callerSleeping.store(false);
  }
  const int64_t t1 = NowNs();

  // ---- the instrument: what the call cost beyond its own work -------------
  // overhead = wall - the busiest participant's span (first item to last
  // item). With perfect wake-up and join it is ~0; wake latency, a straggling
  // join and imbalance all show up in it.
  int64_t busiest = 0, lastEnd = t0;
  int parts = 0;
  for (int k = 0; k <= nWorkers; k++) {
    const Part& pt = p.parts[k];
    if (pt.gen.load(std::memory_order_acquire) != g) continue;
    parts++;
    busiest = std::max(busiest, pt.last - pt.first);
    lastEnd = std::max(lastEnd, pt.last);
    if (k > 0) {
      g_stats.wakeUs += (double)(pt.first - t0) / 1000.0;
      g_stats.wakeCount++;
    }
  }
  g_stats.calls++;
  g_stats.items += n;
  g_stats.wallUs += (double)(t1 - t0) / 1000.0;
  g_stats.overheadUs += (double)std::max<int64_t>(0, (t1 - t0) - busiest) / 1000.0;
  g_stats.joinUs += (double)std::max<int64_t>(0, t1 - lastEnd) / 1000.0;
  g_stats.participants += (uint64_t)parts;
  if (g_lastEndNs != 0 && t0 > g_lastEndNs) {
    const double gap = (double)(t0 - g_lastEndNs) / 1000.0;
    g_stats.gapUs += gap;
    g_stats.gapCount++;
    if (gap <= (double)p.spinNs / 1000.0) g_stats.gapsInSpin++;
  }
  g_lastEndNs = t1;
  p.fn.store(nullptr, std::memory_order_relaxed);
}

}  // namespace workpool
