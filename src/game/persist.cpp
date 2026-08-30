#include "game/persist.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
         ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

// ---- the 'PLYR' payload ----------------------------------------------------
//
// A LENGTH-PREFIXED STRING STREAM, deliberately. The obvious alternative — a
// packed struct of fixed-width fields — cannot carry names, and names are the
// whole point (see the header). The format is:
//
//   u32 hotbarCount     then per slot: str name, i32 count
//   u32 bagCount        then per slot: str name, i32 count
//   u32 equipCount      then per slot: str name, i32 count
//   i32 hotbarSelected
//   u32 ownedCount      then per glyph: str id
//   u32 boundCount      then per slot: str id ("" = unbound)
//
// where str = u32 length + bytes, no terminator. The COUNTS are written rather
// than assumed from the compile-time constants so a build whose kItemSlots or
// Bag::kSlots has changed reads an older file correctly: it takes what fits
// and drops the rest, rather than walking off the end of the payload.
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)v);
  out.push_back((uint8_t)(v >> 8));
  out.push_back((uint8_t)(v >> 16));
  out.push_back((uint8_t)(v >> 24));
}
void PutStr(std::vector<uint8_t>& out, const std::string& s) {
  PutU32(out, (uint32_t)s.size());
  out.insert(out.end(), s.begin(), s.end());
}

// Reader that can run off the end of a truncated file without reading past it.
// `ok` latches false on the first overrun and every later read is a no-op, so
// the caller checks once at the end instead of after every field.
struct Reader {
  const uint8_t* p;
  size_t left;
  bool ok = true;

  uint32_t U32() {
    if (!ok || left < 4) {
      ok = false;
      return 0;
    }
    const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    left -= 4;
    return v;
  }
  std::string Str() {
    const uint32_t n = U32();
    // A length that cannot fit is a corrupt file, not a very long name: refuse
    // rather than allocate whatever the file asked for.
    if (!ok || n > left) {
      ok = false;
      return {};
    }
    std::string s((const char*)p, n);
    p += n;
    left -= n;
    return s;
  }
};

void SavePlayerKit(const PlayerKitRefs& r, std::vector<uint8_t>& out) {
  auto putSlots = [&](const ItemStack* v, int n) {
    PutU32(out, (uint32_t)n);
    for (int i = 0; i < n; i++) {
      PutStr(out, KitItemName(v[i], *r.items));
      PutU32(out, (uint32_t)(v[i].Empty() ? 0 : v[i].count));
    }
  };
  putSlots(r.hotbar->slots, kItemSlots);
  putSlots(r.kit->bag.slots, Bag::kSlots);
  putSlots(r.kit->equip.slots, kEquipSlotCount);
  PutU32(out, (uint32_t)r.hotbar->selected);

  const GlyphInventory& gi = r.caster->inventory;
  auto glyphName = [&](int idx) {
    return (idx >= 0 && idx < (int)r.glyphs->glyphs.size())
               ? r.glyphs->glyphs[idx].id
               : std::string();
  };
  PutU32(out, (uint32_t)gi.owned.size());
  for (int g : gi.owned) PutStr(out, glyphName(g));
  PutU32(out, (uint32_t)kGlyphSlots);
  for (int i = 0; i < kGlyphSlots; i++) PutStr(out, glyphName(gi.At(i)));
}

bool LoadPlayerKit(const PlayerKitRefs& r, const uint8_t* data, size_t len,
                   uint32_t version) {
  if (version != kPlayerKitSaveVersion) {
    std::fprintf(stderr, "PLYR: unknown version %u (this build writes %u)\n",
                 version, kPlayerKitSaveVersion);
    return false;
  }
  Reader rd{data, len};
  int dropped = 0;
  auto getSlots = [&](ItemStack* v, int n) {
    const uint32_t count = rd.U32();
    for (uint32_t i = 0; i < count && rd.ok; i++) {
      const std::string name = rd.Str();
      const int c = (int)rd.U32();
      if (!rd.ok) return;
      if ((int)i >= n) continue;   // the file has more slots than this build
      v[i] = KitItemFromName(name, c, *r.items);
      if (!name.empty() && v[i].Empty()) dropped++;
    }
  };
  getSlots(r.hotbar->slots, kItemSlots);
  getSlots(r.kit->bag.slots, Bag::kSlots);
  getSlots(r.kit->equip.slots, kEquipSlotCount);
  r.hotbar->Select((int)rd.U32());

  GlyphInventory& gi = r.caster->inventory;
  gi.owned.clear();
  {
    const uint32_t n = rd.U32();
    for (uint32_t i = 0; i < n && rd.ok; i++) {
      const std::string id = rd.Str();
      if (!rd.ok) break;
      const int idx = r.glyphs->Find(id);
      if (idx >= 0)
        gi.Grant(idx);
      else if (!id.empty())
        dropped++;
    }
  }
  {
    const uint32_t n = rd.U32();
    for (uint32_t i = 0; i < n && rd.ok; i++) {
      const std::string id = rd.Str();
      if (!rd.ok) break;
      if ((int)i >= kGlyphSlots) continue;
      // Bind refuses a glyph the player does not own, which is the right
      // answer for a file that was saved before a glyph was taken away: the
      // slot ends up empty rather than pointing at something unownable.
      gi.Bind((int)i, id.empty() ? -1 : r.glyphs->Find(id));
    }
  }
  if (dropped > 0)
    std::fprintf(stderr,
                 "PLYR: %d saved entries name content that no longer exists; "
                 "those slots are empty\n",
                 dropped);
  if (!rd.ok) {
    std::fprintf(stderr, "PLYR: payload truncated (%zu bytes)\n", len);
    return false;
  }
  r.caster->Clear(*r.glyphs);   // a half-spoken spell does not survive a load
  return true;
}

}  // namespace

EntityIO MakeEntityIO(DebrisSystem& debris, MobSystem& mobs,
                      PlayerAvatar* avatar, const PlayerKitRefs* player) {
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
  if (player && player->Complete()) {
    const PlayerKitRefs r = *player;
    io.sections.push_back(EntitySection{
        FourCC('P', 'L', 'Y', 'R'), kPlayerKitSaveVersion,
        // Reset clears everything the section owns. It runs on EVERY load,
        // including one from a file with no PLYR section — an older save must
        // not leave this session's pack standing in the loaded world, which is
        // the same rule the avatar's reset follows.
        [r] {
          for (int i = 0; i < kItemSlots; i++) r.hotbar->slots[i] = ItemStack{};
          r.hotbar->Select(0);
          for (int i = 0; i < Bag::kSlots; i++) r.kit->bag.slots[i] = ItemStack{};
          for (int i = 0; i < kEquipSlotCount; i++)
            r.kit->equip.slots[i] = ItemStack{};
          r.caster->inventory.owned.clear();
          for (int i = 0; i < kGlyphSlots; i++) r.caster->inventory.Bind(i, -1);
        },
        [r](std::vector<uint8_t>& out) { SavePlayerKit(r, out); },
        [r](const uint8_t* d, size_t n, uint32_t v) {
          return LoadPlayerKit(r, d, n, v);
        }});
  }
  return io;
}
