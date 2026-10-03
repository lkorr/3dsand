#pragma once
// WHERE A BURNING CREATURE'S TICK GOES (--burn-npc).
//
// Owner report 2026-09-30: a clothed NPC catching fire takes the game to ~4 fps
// for a second or two. The mob side of a tick is one PerfScope (GameLogic /
// PostStep) and one number per scope cannot say which of the burn walk, the
// flush, the carve, the Jolt rebuild or the recount it was -- so this splits
// it, and keeps the WORST tick's whole breakdown (the hitch IS the worst tick;
// a mean over a burn hides it).
//
// OFF by default: one predictable branch per scope. The --burn-npc harness
// turns it on; nothing in the game does.
#include <chrono>
#include <cstdint>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include <cstdio>
#include <string>

namespace burnprof {

enum Phase : uint8_t {
  // TOP LEVEL: these do not nest in each other and sum to the mob tick.
  kPreTick,     // MobSystem::PreTick, all of it
  kPostStep,    // MobSystem::PostStep
  kContact,     // MobSystem::ApplyContactDamage
  kHairTuck,    // MobSystem::SyncHairTuck
  kTopCount,
  // NESTED inside the above.
  kBurnLimbs = kTopCount,  // MobSystem::BurnLimbs (the living + dead rigs)
  kBurnOne,     // MobSystem::BurnOneLimb, every call
  kFlush,       // Mob::FlushBurn (a burn's batched carve), all of it
  kCarve,       // Mob::CarveLimb, every cause
  kRebuild,     // Mob::RebuildLimbBody (the Jolt collider)
  kIndex,       // MobSystem::BuildBurnIndex
  kCrossHeat,   // Mob::BuildCrossLimbHeat
  kTail,        // Infect/Pulp/Heal/twins after the limb loop
  kRecount,     // Mob::RecountBurn
  kDeadFlesh,   // MobSystem::BurnDeadFlesh (severed flesh)
  // Inside BurnOneLimb: the world walk, the seeding, the candidate loop, and
  // the worn-shell march (Mob::WornShellAlong) wherever it is asked from.
  kWalk,
  kSeed,
  kCandLoop,
  kWorn,
  // The candidate list's construction from the front (the front cells and
  // their lattice neighbours) and the post-loop sweep over every candidate
  // that clears the queued bits and rebuilds the front. Both scale with the
  // CANDIDATE count, not the evaluated count (rule 6: attribute before
  // eliminating -- see BurnOneLimb's bounded-window note).
  kQueue,
  kFrontSweep,
  kCount
};

inline const char* Name(int p) {
  static const char* k[kCount] = {"preTick", "postStep", "contact", "hairTuck",
                                  "burnLimbs", "burnOne", "flush", "carve",
                                  "rebuild", "index", "crossHeat", "tail",
                                  "recount", "deadFlesh", "walk", "seed",
                                  "candLoop", "worn", "queue", "frontSweep"};
  return p >= 0 && p < kCount ? k[p] : "?";
}

// Plain counters beside the clocks (rule 6: a duration wants its denominator).
enum Counter : uint8_t {
  kSeedProbes,   // face samples transformed by the world-contact seeding
  kSeedHits,     // ...that landed on a live lattice voxel
  kSeedNew,      // ...that queued a candidate not already queued
  kCandidates,   // candidates queued in total (front + neighbours + seeds)
  kEvaluated,    // ...of which the front budget let the loop evaluate
  kFront,        // front cells held at the top of each BurnOneLimb visit
  kNCount
};
inline const char* CounterName(int c) {
  static const char* k[kNCount] = {"seedProbes", "seedHits", "seedNew",
                                   "cands", "evaluated", "front"};
  return c >= 0 && c < kNCount ? k[c] : "?";
}

struct Profile {
  bool on = false;
  uint64_t n[kNCount] = {};
  double cur[kCount] = {};
  double tot[kCount] = {};
  uint64_t calls[kCount] = {}, curCalls[kCount] = {};
  double worstTotal = 0;
  uint32_t worstTick = 0;
  double worst[kCount] = {};
  uint64_t worstCalls[kCount] = {};
  uint32_t ticks = 0, over16 = 0, over33 = 0;
  double sumTotal = 0;
};

// An inline VARIABLE, not a function-local static: the scopes sit in loops
// run tens of thousands of times a tick, and a local static pays its
// initialisation guard on every call even while profiling is off.
inline Profile g_profile;
inline Profile& Get() { return g_profile; }

// Microseconds per TSC tick, measured once against the steady clock (the
// scopes read the TSC: the worn march is asked tens of thousands of times a
// tick and two QueryPerformanceCounter calls each would be the cost measured).
inline double UsPerTick() {
  static const double k = [] {
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(20)) {}
    const uint64_t t1 = __rdtsc();
    const double us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - c0).count();
    return us / (double)(t1 - t0);
  }();
  return k;
}

struct Scope {
  int ph;
  uint64_t t0 = 0;
  explicit Scope(int phase) : ph(phase) {
    if (Get().on) t0 = __rdtsc();
  }
  ~Scope() { Stop(); }
  // Close early (a span that ends before its enclosing block does).
  void Stop() {
    Profile& p = Get();
    if (p.on && t0 != 0) {
      p.cur[ph] += (double)(__rdtsc() - t0) * UsPerTick();
      p.curCalls[ph]++;
    }
    t0 = 0;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// Roll the tick just finished into the totals (called at the top of PreTick,
// so "a tick" is PreTick..HairTuck of the previous one).
inline void EndTick(uint32_t tick) {
  Profile& p = Get();
  if (!p.on) return;
  double total = 0;
  for (int i = 0; i < kTopCount; i++) total += p.cur[i];
  if (total > 0) {
    p.ticks++;
    p.sumTotal += total;
    if (total > 16000.0) p.over16++;
    if (total > 33000.0) p.over33++;
    if (total > p.worstTotal) {
      p.worstTotal = total;
      p.worstTick = tick;
      for (int i = 0; i < kCount; i++) {
        p.worst[i] = p.cur[i];
        p.worstCalls[i] = p.curCalls[i];
      }
    }
  }
  for (int i = 0; i < kCount; i++) {
    p.tot[i] += p.cur[i];
    p.calls[i] += p.curCalls[i];
    p.cur[i] = 0;
    p.curCalls[i] = 0;
  }
}

inline void Count(int c, uint64_t v) {
  if (Get().on) Get().n[c] += v;
}

inline void Reset() {
  (void)UsPerTick();
  const bool on = Get().on;
  Get() = Profile{};
  Get().on = on;
}

inline std::string Report() {
  const Profile& p = Get();
  const double n = p.ticks ? (double)p.ticks : 1.0;
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "%u ticks, mean %.2f ms, worst %.2f ms at tick %u, >16ms %u "
                ">33ms %u |",
                p.ticks, p.sumTotal / n / 1000.0, p.worstTotal / 1000.0,
                p.worstTick, p.over16, p.over33);
  std::string s = buf;
  for (int i = 0; i < kNCount; i++) {
    std::snprintf(buf, sizeof buf, " %s %.0f/tick |", CounterName(i),
                  (double)p.n[i] / n);
    s += buf;
  }
  for (int i = 0; i < kCount; i++) {
    if (p.tot[i] < 1.0 && p.worst[i] < 1.0) continue;
    std::snprintf(buf, sizeof buf, " %s %.2f/tick worst %.2f (x%llu, worst x%llu) |",
                  Name(i), p.tot[i] / n / 1000.0, p.worst[i] / 1000.0,
                  (unsigned long long)p.calls[i],
                  (unsigned long long)p.worstCalls[i]);
    s += buf;
  }
  return s;
}

}  // namespace burnprof
