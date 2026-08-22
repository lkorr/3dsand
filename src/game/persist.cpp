#include "game/persist.h"

namespace {
constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
         ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}
}  // namespace

EntityIO MakeEntityIO(DebrisSystem& debris, MobSystem& mobs,
                      PlayerAvatar* avatar) {
  EntityIO io;
  io.sections.push_back(EntitySection{
      FourCC('D', 'B', 'R', 'S'), DebrisSystem::kSaveVersion,
      [&debris] { debris.Reset(); },
      [&debris](std::vector<uint8_t>& out) { debris.SaveState(out); },
      [&debris](const uint8_t* d, size_t n, uint32_t v) {
        return debris.LoadState(d, n, v);
      }});
  io.sections.push_back(EntitySection{
      FourCC('M', 'O', 'B', 'S'), MobSystem::kSaveVersion,
      [&mobs] { mobs.Reset(); },
      [&mobs](std::vector<uint8_t>& out) { mobs.SaveState(out); },
      [&mobs](const uint8_t* d, size_t n, uint32_t v) {
        return mobs.LoadState(d, n, v);
      }});
  if (avatar) {
    io.sections.push_back(EntitySection{
        FourCC('A', 'V', 'T', 'R'), PlayerAvatar::kSaveVersion,
        // Reset = despawn AND drop any pending restore: an older save without
        // an AVTR section must not apply a previous load's damage state.
        [avatar] {
          avatar->Despawn();
          avatar->ClearPendingRestore();
        },
        [avatar](std::vector<uint8_t>& out) { avatar->SaveState(out); },
        [avatar](const uint8_t* d, size_t n, uint32_t v) {
          return avatar->LoadState(d, n, v);
        }});
  }
  return io;
}
