#pragma once
// workpool.h -- A SMALL FORK-JOIN POOL FOR THE MOB TICK (PLAN_fight64_perf
// package M).
//
// One thing only: ParallelFor(n, fn) runs fn(i) for every i in [0, n) on the
// pool's workers and the calling thread, and returns when all of them have
// finished. No futures, no task graph, no nesting (a ParallelFor issued from
// inside a task runs serially on that thread).
//
// DETERMINISM IS THE CALLER'S CONTRACT, and it is a narrow one (CLAUDE.md rule
// 1, the plan's "per-entity work writes per-entity outputs, merged in id
// order"): fn(i) may READ anything nobody writes during the call and may WRITE
// only state that belongs to item i. Which worker runs which i, and in what
// order, is then unobservable -- the result is the serial loop's, bit for bit.
// Anything order-dependent (a shared budget, a push into a shared vector, an
// allocator) stays in the serial merge after the call.
//
// SANDVOX_MOB_THREADS=<n> sets the worker count (1 = serial, the A/B arm that
// proves the parallel result equals the serial one in one binary). Default:
// half the hardware threads less one, clamped to [1, 7] -- the same budget
// Physics::PhysicsWorkerThreads takes, and the two never run at once (the mob
// pass and the Jolt step are sequential phases of the tick).
#include <cstddef>
#include <functional>

namespace workpool {

// Threads that run tasks, the caller included (>= 1), after SetThreadLimit.
int Threads();

// Cap the threads a ParallelFor uses, the caller included, at runtime: 1 =
// serial, 0 = every thread the pool was built with (SANDVOX_MOB_THREADS or the
// default). It cannot raise the count past the pool's. The twice-run gate
// (mob-cap64-twice) runs one brawl at two counts in one process with it, so
// "the parallel result is the serial one" is gated, not an env A/B by hand.
// Call between ticks only, never from inside a task.
void SetThreadLimit(int n);

// fn(i) for i in [0, n). `grain` items are claimed at a time (>= 1).
void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn);

// True on a pool worker thread (profiling scopes that keep process-global
// accumulators record only off it).
bool OnWorker();

// True while the calling thread is running a ParallelFor task -- on a worker,
// or on the caller while it drains its share. A memo a task may READ must not
// be WRITTEN while this is true anywhere (MobSystem::MaterialIdNamed).
bool InTask();

// THE POOL'S OWN COST (fight64 round 4, package W). Accumulated on the calling
// thread for every ParallelFor that went parallel; ResetStats() zeroes it.
// Scheduling only -- nothing here can change any task's output.
//   overheadUs: per call, wall minus the busiest participant's span (its first
//               item's start to its last item's end) -- wake-up latency, the
//               join and imbalance; ~0 for a perfect pool.
//   wakeUs:     per participating worker, post -> its first item.
//   joinUs:     per call, the last item's end -> the caller returning.
//   gapUs:      between one call's return and the next call's post (what the
//               worker spin has to bridge; gapsInSpin of them were within it).
struct Stats {
  uint64_t calls = 0, serialCalls = 0, items = 0, participants = 0;
  double wallUs = 0, overheadUs = 0, joinUs = 0;
  double wakeUs = 0;
  uint64_t wakeCount = 0;
  double gapUs = 0;
  uint64_t gapCount = 0, gapsInSpin = 0;
};
const Stats& GetStats();
void ResetStats();

// SANDVOX_POOL_SPIN_US (default 200, clamped to [0, 1000]): how long a worker
// spins for the next job, and the caller for the join, before sleeping on a
// condition variable. SANDVOX_POOL_LEGACY=1: no spin and the old join (wait
// for every worker to check out), the in-binary before-arm.
int SpinMicros();
bool Legacy();

}  // namespace workpool
