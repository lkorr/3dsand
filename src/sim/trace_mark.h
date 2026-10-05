#pragma once
// trace_mark.h -- one "M <text>" line into each boot-to-boot determinism trace
// that is open (SANDVOX_PHYS_TRACE, SANDVOX_DEBRIS_TRACE, SANDVOX_FETCH_TRACE).
//
// Those traces were built to diff two BOOTS. A gate that runs the same fixture
// several times in ONE process (mob-cap64-twice) writes every run into the same
// file; a mark between the runs is what lets a script split the file and diff
// run against run. No-op (one null test each) when no trace is open.
//
// Defined beside each trace's FILE* (physics.cpp, debris.cpp, world.cpp).
void PhysTraceMark(const char* what);
void DebrisTraceMark(const char* what);
void FetchTraceMark(const char* what);

inline void TraceMarkAll(const char* what) {
  PhysTraceMark(what);
  DebrisTraceMark(what);
  FetchTraceMark(what);
}
