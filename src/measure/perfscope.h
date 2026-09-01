// perfscope.h — the process-global CPU-scope accumulator.
//
// WHY THIS EXISTS, and what it fixes.
//
// The Performance tab bills CPU time to thirteen scopes (perfnodes.h). Until
// this file, only SEVEN of them were ever written on the live path, and one of
// those seven was not a measurement at all:
//
//     cpuMs[Input] += max(0, wallMs - sum(every other scope))
//
// That is a RESIDUAL wearing a scope's name. Everything the frame loop did not
// explicitly time — the toroidal window shift, chunk fetch/evict, the whole
// per-tick game-logic body, ProcessEvents, the CPU mirror rebuild — landed in a
// bar labelled "Player + Input" whose tooltip read "effectively free; if this
// is ever visible, something is polling in a loop". So a 30 ms window shift
// while flying reported as 30 ms of input polling, and the page pointed at the
// one system that was innocent.
//
// The other six scopes (GameLogic, WaterBody, Upload, Encode, Readback, Audio)
// were declared, given nodes, given tooltips, and never written, so their bars
// sat at zero and read as "this system is free" rather than "this system is not
// measured". A zero you cannot distinguish from an absence is worse than no bar
// at all, and it is the same failure mode CLAUDE.md's rule 6 is about: the page
// reported a NUMBER with no attribution behind it.
//
// THE LAYERING PROBLEM this header solves. Half the work inside a tick happens
// under SubmitTick (src/test/support.cpp) — the page-table CPU phase, the op
// uploads, the pass-table walk — which is shared by the frame loop, every
// selftest gate, the smoke and the --perf harness. Threading a collector
// reference down through it would mean changing a signature every harness
// calls, and would still leave PageTable unable to bill itself. So the
// accumulator is a process global with an explicit on/off flag: off, a scope is
// a branch on a bool; on, it is two QPC reads.
//
// MEASUREMENT-ONLY, and it cannot move the world hash — a clock read is not an
// input to anything. `PerfScopesOn()` is false unless --telemetry or --perf
// turned it on, which is what keeps a gate's timings out of the picture and the
// cost off the default tick path.

#pragma once

#include "perfnodes.h"

namespace sandvox {

// Declared here rather than included from test/support.h: measure/ must not
// depend on test/, and this is the only symbol it would want.
double NowSeconds();

// One accumulator for the whole process, drained once per frame by whoever is
// collecting (the live telemetry path in main.cpp, or FrameClock in
// perfsuite.cpp). Not thread-safe on purpose — every writer is the main thread,
// and a lock here would be measuring the lock.
struct PerfAccum {
  double ms[kPerfScopeCount] = {};
  bool on = false;
};

inline PerfAccum& PerfScopes() {
  static PerfAccum a;
  return a;
}
inline bool PerfScopesOn() { return PerfScopes().on; }
inline void PerfScopesEnable(bool on) { PerfScopes().on = on; }

// Copy out and zero, in one operation. Two calls would let a scope opened
// across the drain point be billed to the wrong frame, and the frame boundary
// is exactly where SubmitTick's tail and the render's head meet.
inline void PerfScopesDrain(double* out) {
  PerfAccum& a = PerfScopes();
  for (int i = 0; i < kPerfScopeCount; i++) {
    out[i] += a.ms[i];
    a.ms[i] = 0;
  }
}

// RAII, not paired Begin/End calls, for the reason perfsuite.cpp's ScopeTimer
// already gives: an early `return` or `continue` past an End silently bills the
// rest of the frame to the wrong bar, and a page that exists to be trusted
// about attribution must not have that failure mode. Every `return` inside
// SubmitTick is one of these waiting to happen.
struct PerfSpan {
  PerfScope scope;
  double t0;
  bool on;
  explicit PerfSpan(PerfScope s)
      : scope(s), t0(0), on(PerfScopesOn()) {
    if (on) t0 = NowSeconds();
  }
  ~PerfSpan() {
    if (on) PerfScopes().ms[(int)scope] += (NowSeconds() - t0) * 1000.0;
  }
  // Close early and bill now, for the cases where the span ends in the middle
  // of a block rather than at a brace. Idempotent.
  void Close() {
    if (!on) return;
    on = false;
    PerfScopes().ms[(int)scope] += (NowSeconds() - t0) * 1000.0;
  }
  PerfSpan(const PerfSpan&) = delete;
  PerfSpan& operator=(const PerfSpan&) = delete;
};

// Bill a span the caller timed itself. Used where the two ends of the span are
// already on the clock for another reason (the frame loop's tick timeline) and
// a second pair of QPC reads would be measuring the measurement.
inline void PerfScopeAdd(PerfScope s, double t0, double t1) {
  if (PerfScopesOn()) PerfScopes().ms[(int)s] += (t1 - t0) * 1000.0;
}

}  // namespace sandvox
