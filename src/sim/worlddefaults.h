#pragma once
#include <cstdint>

// worlddefaults.h — the few names the game, the tools and the selftest all
// agree on about WHICH world they generate. Sim-layer on purpose: the game's
// startup worldgen used to take its seed from src/test/support.h.
namespace sandvox {

// The world seed every run generates unless told otherwise (the game, --shot,
// the benches, --voxdump and every gate).
constexpr uint32_t kDefaultSeed = 1337;

// THE HARNESS MAP (assets/worldmap/harness): the selftest's own ground. It
// carries the calm pad box over the fixture columns and the fixture lake as
// authored sites, and its planes and sea level are its own, so no gate depends
// on the shipped `default` map. --selftest / --verify / --suite / --vk-smoke load it through
// worldmap::SetMapOverride; anything else can with SANDVOX_MAP=harness.
constexpr const char* kHarnessMapName = "harness";
// The harness map's fixture lake (kind "water" site id): the waterbody gates'
// basin, the fluid bench's `worldlake` and the water --shot scenes.
constexpr const char* kHarnessLakeSite = "harness_lake";

}  // namespace sandvox
