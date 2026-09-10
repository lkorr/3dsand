#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// ---- THE OTHER HALF OF "WHAT WAS THIS WORLD GENERATED FROM" ----------------
//
// biomes::EnvironmentStamp (sim/biomes.h) hashes the WORLDGEN inputs — map,
// biomes, water, trees — and prints them at boot so a moved world hash can be
// attributed to a changed file without a bisect. Nothing hashed the inputs
// that shape the world AFTERWARDS: `tuning.json` (every sim.* constant the CA
// reads), `materials.json` and `reactions.json`. Those are precisely the files
// two machines are most likely to differ on, because all three hot-reload
// (F5 / R) and none of them is in the save
// (docs/RESEARCH_multiplayer_readiness.md L8).
//
// So this is the cheapest desync-cause eliminator that exists: one line at
// boot and one field in build/last_run.json. Under the model of record
// (DESIGN.md §10) these numbers go in the join handshake and a mismatch is
// refused there, before a client spends a session diverging by a reaction
// chance. Today they are a DIAGNOSTIC — "your reactions.json is not my
// reactions.json" printed rather than deduced.
//
// The hash is FNV-1a over the file NAME, a zero byte, then its BYTES, exactly
// as biomes::HashFileSet does it for one file, so the tuner's Python side can
// reproduce it with the same three lines. Three separate numbers rather than
// one, because the useful output is WHICH file differs; a single combined
// hash would say only "something".
namespace sandvox {

inline uint32_t HashOneFile(const std::string& path, const std::string& name) {
  uint32_t h = 2166136261u;
  auto mix = [&h](const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  };
  mix(reinterpret_cast<const unsigned char*>(name.data()), name.size());
  const unsigned char zero = 0;
  mix(&zero, 1);
  std::ifstream f(path, std::ios::binary);
  // A MISSING FILE IS 0, NOT AN ABORT. This is a diagnostic, and a loader that
  // already refused to start is the thing that reports a missing asset.
  if (!f) return 0;
  std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  mix(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  return h;
}

struct TuningStamp {
  uint32_t tuning = 0, materials = 0, reactions = 0;

  std::string Line() const {
    char buf[160];
    std::snprintf(buf, sizeof buf, "tuning: tuning %08x | materials %08x | reactions %08x",
                  tuning, materials, reactions);
    return buf;
  }
  std::string Json() const {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "{\"tuningHash\":\"%08x\",\"materialsHash\":\"%08x\",\"reactionsHash\":\"%08x\"}",
                  tuning, materials, reactions);
    return buf;
  }
};

inline TuningStamp StampTuning(const std::string& assetDir) {
  TuningStamp s;
  const std::string dir = assetDir + "/materials/";
  s.tuning = HashOneFile(dir + "tuning.json", "tuning.json");
  s.materials = HashOneFile(dir + "materials.json", "materials.json");
  s.reactions = HashOneFile(dir + "reactions.json", "reactions.json");
  return s;
}

}  // namespace sandvox
