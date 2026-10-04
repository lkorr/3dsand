#pragma once
// AN IN-PROCESS SAMPLING PROFILER for one thread (PLAN_fight64_perf package M).
//
// The burnprof scopes answer "how long did THIS span take" -- one hypothesis
// per scope, and a span nobody wrapped is invisible (the 64-creature brawl's
// PreTick had 7 ms of it). A sampler answers every hypothesis at once (CLAUDE.md
// verification rule 6): every ~1 ms the calling thread is suspended, its stack
// unwound, and the return addresses kept; at Stop() the addresses are named
// from the .pdb and printed as self (leaf) and inclusive (anywhere on the
// stack) shares, plus the hottest caller -> callee edges.
//
// OFF unless SANDVOX_SAMPLE_PROF is set: Start() is one getenv and nothing
// else. Diagnostic only -- it reads no sim state and writes none, so it cannot
// move a hash (the target thread is merely paused).
#include <cstdio>
#include <string>

namespace sampleprof {

// Begin sampling the CALLING thread (no-op unless SANDVOX_SAMPLE_PROF is set,
// or if already running). `label` names the report.
void Start(const char* label);
// Stop, symbolise, print the report to stdout and append it to
// build/sampleprof.txt. `topN` rows per table.
void Stop(int topN = 80);

}  // namespace sampleprof
