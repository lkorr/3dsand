// See workpool.h.
#include "game/workpool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace workpool {
namespace {

thread_local bool tl_worker = false;
thread_local bool tl_inTask = false;

struct Pool {
  std::vector<std::thread> threads;
  std::mutex mu;
  std::condition_variable wake, idle;
  // The job in flight. `gen` moves once per ParallelFor; a worker that saw the
  // previous generation knows a new job is posted.
  uint64_t gen = 0;
  const std::function<void(size_t)>* fn = nullptr;
  size_t n = 0, grain = 1;
  std::atomic<size_t> next{0};
  int active = 0;     // workers still inside the current job
  bool quit = false;
  // SetThreadLimit: workers whose index is at or past limit-1 sit the job out
  // (they still wake and check out, so `active` stays the plain count).
  std::atomic<int> limit{0};

  void Drain() {
    for (;;) {
      const size_t i0 = next.fetch_add(grain, std::memory_order_relaxed);
      if (i0 >= n) return;
      const size_t i1 = std::min(n, i0 + grain);
      for (size_t i = i0; i < i1; i++) (*fn)(i);
    }
  }

  void Worker(int index) {
    tl_worker = true;
    tl_inTask = true;
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(mu);
        wake.wait(lk, [&] { return quit || gen != seen; });
        if (quit) return;
        seen = gen;
      }
      const int lim = limit.load(std::memory_order_relaxed);
      if (lim <= 0 || index < lim - 1) Drain();
      {
        std::lock_guard<std::mutex> lk(mu);
        if (--active == 0) idle.notify_one();
      }
    }
  }

  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(mu);
      quit = true;
    }
    wake.notify_all();
    for (std::thread& t : threads) t.join();
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
    const int workers = ConfiguredThreads() - 1;
    for (int i = 0; i < workers; i++)
      pool->threads.emplace_back([pool, i] { pool->Worker(i); });
    return pool;
  }();
  return *p;
}

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

void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn) {
  if (n == 0) return;
  grain = std::max<size_t>(1, grain);
  Pool& p = Get();
  // Serial when there is nobody to share with, nothing to share, or we are
  // already inside a task (no nesting: a worker waiting on its own pool
  // would deadlock).
  if (p.threads.empty() || n <= grain || tl_inTask ||
      p.limit.load(std::memory_order_relaxed) == 1) {
    for (size_t i = 0; i < n; i++) fn(i);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(p.mu);
    p.fn = &fn;
    p.n = n;
    p.grain = grain;
    p.next.store(0, std::memory_order_relaxed);
    p.active = (int)p.threads.size();
    p.gen++;
  }
  p.wake.notify_all();
  tl_inTask = true;
  p.Drain();
  tl_inTask = false;
  std::unique_lock<std::mutex> lk(p.mu);
  p.idle.wait(lk, [&] { return p.active == 0; });
  p.fn = nullptr;
}

}  // namespace workpool
