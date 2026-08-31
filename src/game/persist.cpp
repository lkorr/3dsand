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

void PutF32(std::vector<uint8_t>& out, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  PutU32(out, bits);
}

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

  // ---- v2: worn damage ------------------------------------------------------
  //
  //   u32 pieces  then per piece: str item, u32 shells,
  //                 then per shell: f32 hp (-1 = as authored),
  //                                 u32 atSpawn, u32 live   (v3, condition)
  //                                 u32 voxelCount, then per voxel:
  //                                   i32 x, i32 y, i32 z, u32 material,
  //                                   u32 colour
  //
  // EXACT, not a durability percentage: the owner's decision is that the holes
  // round-trip. A percentage would be cheaper and would put the wear back in
  // the wrong places, which on armour whose whole mechanic is "the world
  // reaches you through the gap" is not a cosmetic difference.
  //
  // Only DAMAGED pieces appear (PlayerKit::SetDamage erases an empty blob), so
  // a full suit of untouched armour costs four bytes.
  PutU32(out, (uint32_t)r.kit->wornDamage.size());
  for (const auto& kv : r.kit->wornDamage) {
    PutStr(out, kv.first);
    PutU32(out, (uint32_t)kv.second.shells.size());
    for (const WornShellDamage& sh : kv.second.shells) {
      PutF32(out, sh.hp);
      PutU32(out, sh.atSpawn);
      PutU32(out, sh.live);
      PutU32(out, (uint32_t)sh.lattice.size());
      for (const PrefabVoxel& v : sh.lattice) {
        PutU32(out, (uint32_t)(int32_t)v.x);
        PutU32(out, (uint32_t)(int32_t)v.y);
        PutU32(out, (uint32_t)(int32_t)v.z);
        PutU32(out, v.material);
        PutU32(out, v.color);
      }
    }
  }
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
  // ---- v2: worn damage ------------------------------------------------------
  r.kit->wornDamage.clear();
  {
    const uint32_t pieces = rd.U32();
    for (uint32_t i = 0; i < pieces && rd.ok; i++) {
      const std::string name = rd.Str();
      const uint32_t nsh = rd.U32();
      WornDamage d;
      for (uint32_t k = 0; k < nsh && rd.ok; k++) {
        WornShellDamage sh;
        const uint32_t hpBits = rd.U32();
        std::memcpy(&sh.hp, &hpBits, 4);
        sh.atSpawn = rd.U32();
        sh.live = rd.U32();
        const uint32_t nv = rd.U32();
        // A count that cannot fit is a corrupt file, not a very large robe.
        // Five u32 per voxel, so refuse BEFORE reserving whatever it asked
        // for -- the same rule Reader::Str already applies to a string length.
        if (!rd.ok || (size_t)nv * 20u > rd.left) {
          rd.ok = false;
          break;
        }
        sh.lattice.reserve(nv);
        for (uint32_t vi = 0; vi < nv && rd.ok; vi++) {
          const int32_t x = (int32_t)rd.U32();
          const int32_t y = (int32_t)rd.U32();
          const int32_t z = (int32_t)rd.U32();
          const uint32_t m = rd.U32();
          const uint32_t col = rd.U32();
          if (!rd.ok) break;
          sh.lattice.push_back(PrefabVoxel{(int16_t)x, (int16_t)y, (int16_t)z,
                                           (uint16_t)m, (uint8_t)col});
        }
        d.shells.push_back(std::move(sh));
      }
      // Damage for a piece that no longer exists is simply forgotten -- the
      // same rule the slots above follow for an unresolvable name.
      if (rd.ok && !d.Empty() && r.items->Find(name) >= 0)
        r.kit->wornDamage[name] = std::move(d);
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

// ---- the 'ITMS' payload -----------------------------------------------------
//
//   u32 count   then per item: str name, f32 x, f32 y, f32 z,
//                              u32 latticeCount, then per voxel:
//                                i32 x, i32 y, i32 z, u32 material, u32 colour
//
// Position only, no rotation: a ground item is re-dropped rather than restored
// in place, and it settles again under the same physics that put it there. A
// saved quaternion would be a pose the solver immediately overrides anyway.
//
// THE LATTICE IS WRITTEN ONLY WHEN IT DIFFERS from what the library would
// build — the owner's decision is that damage persists exactly, and a robe
// that burned half away on the ground has to come back that way. An untouched
// item costs four bytes, which is why the common case can afford the check.

void SaveWorldItems(const WorldItemRefs& r, std::vector<uint8_t>& out) {
  const std::vector<WorldItem>& all = r.reg->All();
  PutU32(out, (uint32_t)all.size());
  for (const WorldItem& w : all) {
    PutStr(out, w.item);
    BodyTransform xf{};
    r.phys->GetTransform(w.body, xf);
    PutF32(out, xf.pos.x);
    PutF32(out, xf.pos.y);
    PutF32(out, xf.pos.z);

    std::vector<PrefabVoxel> lat;
    uint32_t latScale = 1;
    const ItemDef* d = r.items->At(r.items->Find(w.item));
    uint32_t authored = 0;
    if (d) {
      uint32_t s2 = 1;
      const std::vector<PrefabVoxel>* a = ItemGroundVoxels(*d, s2);
      authored = a ? (uint32_t)a->size() : 0u;
    }
    if (!r.debris->BodyLatticeOf(w.body, lat, latScale) ||
        lat.size() == (size_t)authored)
      lat.clear();   // as the library would build it; nothing to record
    PutU32(out, (uint32_t)lat.size());
    for (const PrefabVoxel& v : lat) {
      PutU32(out, (uint32_t)(int32_t)v.x);
      PutU32(out, (uint32_t)(int32_t)v.y);
      PutU32(out, (uint32_t)(int32_t)v.z);
      PutU32(out, v.material);
      PutU32(out, v.color);
    }
  }
}

bool LoadWorldItems(const WorldItemRefs& r, const uint8_t* data, size_t len,
                    uint32_t version) {
  if (version != kWorldItemSaveVersion) {
    std::fprintf(stderr, "ITMS: unknown version %u (this build writes %u)\n",
                 version, kWorldItemSaveVersion);
    return false;
  }
  Reader rd{data, len};
  const uint32_t n = rd.U32();
  int dropped = 0;
  for (uint32_t i = 0; i < n && rd.ok; i++) {
    const std::string name = rd.Str();
    const uint32_t bx = rd.U32(), by = rd.U32(), bz = rd.U32();
    if (!rd.ok) break;
    Vec3 at{};
    std::memcpy(&at.x, &bx, 4);
    std::memcpy(&at.y, &by, 4);
    std::memcpy(&at.z, &bz, 4);
    const uint32_t nv = rd.U32();
    // Five u32 per voxel: refuse a count that cannot fit BEFORE reserving it.
    if (!rd.ok || (size_t)nv * 20u > rd.left) {
      rd.ok = false;
      break;
    }
    std::vector<PrefabVoxel> lat;
    lat.reserve(nv);
    for (uint32_t vi = 0; vi < nv && rd.ok; vi++) {
      const int32_t vx = (int32_t)rd.U32();
      const int32_t vy = (int32_t)rd.U32();
      const int32_t vz = (int32_t)rd.U32();
      const uint32_t vm = rd.U32();
      const uint32_t vc = rd.U32();
      if (!rd.ok) break;
      lat.push_back(PrefabVoxel{(int16_t)vx, (int16_t)vy, (int16_t)vz,
                                (uint16_t)vm, (uint8_t)vc});
    }
    const ItemDef* d = r.items->At(r.items->Find(name));
    // Content legitimately disappears between saves. The item is dropped with
    // a log line rather than restored as something else, which is the same
    // rule PLYR follows for a name it cannot resolve.
    if (!d) {
      dropped++;
      continue;
    }
    DropItemToWorld(*d, at, Vec3{}, *r.phys, *r.debris, r.micro, *r.reg,
                    lat.empty() ? nullptr : &lat);
  }
  if (dropped > 0)
    std::fprintf(stderr,
                 "ITMS: %d ground items name content that no longer exists\n",
                 dropped);
  if (!rd.ok) {
    std::fprintf(stderr, "ITMS: payload truncated (%zu bytes)\n", len);
    return false;
  }
  return true;
}

}  // namespace

EntityIO MakeEntityIO(DebrisSystem& debris, MobSystem& mobs,
                      PlayerAvatar* avatar, const PlayerKitRefs* player,
                      const WorldItemRefs* ground) {
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
          r.kit->wornDamage.clear();
          r.caster->inventory.owned.clear();
          for (int i = 0; i < kGlyphSlots; i++) r.caster->inventory.Bind(i, -1);
        },
        [r](std::vector<uint8_t>& out) { SavePlayerKit(r, out); },
        [r](const uint8_t* d, size_t n, uint32_t v) {
          return LoadPlayerKit(r, d, n, v);
        }});
  }
  if (ground && ground->Complete()) {
    const WorldItemRefs g = *ground;
    io.sections.push_back(EntitySection{
        FourCC('I', 'T', 'M', 'S'), kWorldItemSaveVersion,
        // Reset clears the REGISTRY only. The bodies belong to DebrisSystem,
        // whose own reset runs from its own section — clearing them here would
        // be a second owner for the same handles, and the order of the two
        // resets would then matter.
        [g] { g.reg->Clear(); },
        [g](std::vector<uint8_t>& out) { SaveWorldItems(g, out); },
        [g](const uint8_t* d, size_t n, uint32_t v) {
          return LoadWorldItems(g, d, n, v);
        }});
  }
  return io;
}
