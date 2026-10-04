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

// Threads that run tasks, the caller included (>= 1).
int Threads();

// fn(i) for i in [0, n). `grain` items are claimed at a time (>= 1).
void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn);

// True on a pool worker thread (profiling scopes that keep process-global
// accumulators record only off it).
bool OnWorker();

// True while the calling thread is running a ParallelFor task -- on a worker,
// or on the caller while it drains its share. A memo a task may READ must not
// be WRITTEN while this is true anywhere (MobSystem::MaterialIdNamed).
bool InTask();

}  // namespace workpool
