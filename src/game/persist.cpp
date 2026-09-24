#include "game/persist.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "sim/mattable.h"
#include "sim/waterbody.h"

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

void SavePlayerKitBytes(const PlayerKitRefs& r, std::vector<uint8_t>& out, uint32_t version) {
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
  if (version < 4) return;

  // ---- v4: the grimoire (plan §12c) ------------------------------------------
  //
  //   u32 pages    then per page: str name, u32 words, then per word: str
  //   u32 slots    then per slot: u32 kind (SlotKind), str name
  //
  // Words and slot names are glyph NAMES and page NAMES, the same contract as
  // every other slot in this section: a name that no longer resolves drops
  // that word with a log line and keeps the page.
  const Grimoire& gr = r.caster->grimoire;
  PutU32(out, (uint32_t)gr.pages.size());
  for (const GrimoirePage& p : gr.pages) {
    PutStr(out, p.name);
    PutU32(out, (uint32_t)p.words.size());
    for (const std::string& w : p.words) PutStr(out, w);
  }
  PutU32(out, (uint32_t)kGlyphSlots);
  for (int i = 0; i < kGlyphSlots; i++) {
    const SlotKind k = gi.KindAt(i);
    PutU32(out, (uint32_t)k);
    PutStr(out, k == SlotKind::Page ? gi.PageAt(i) : glyphName(gi.At(i)));
  }
  if (version < 5) return;

  // ---- v5: the dyes (game/dye.h) --------------------------------------------
  //
  //   u32 hotbarCount  then per slot: u32 dye
  //   u32 bagCount     then per slot: u32 dye
  //   u32 equipCount   then per slot: u32 dye
  //
  // A PARALLEL ARRAY, appended, rather than a fourth field on each slot record
  // — see kPlayerKitSaveVersion's note: the slot section is the first thing in
  // the payload and widening it would make every older payload unreadable, for
  // a value whose absence means precisely "undyed".
  //
  // Self-describing counts for the same reason the slot section's are: a build
  // whose Bag::kSlots has changed reads an older file correctly instead of
  // walking off the end.
  auto putDyes = [&](const ItemStack* v, int n) {
    PutU32(out, (uint32_t)n);
    for (int i = 0; i < n; i++) PutU32(out, v[i].Empty() ? 0u : v[i].dye);
  };
  putDyes(r.hotbar->slots, kItemSlots);
  putDyes(r.kit->bag.slots, Bag::kSlots);
  putDyes(r.kit->equip.slots, kEquipSlotCount);
  if (version < 6) return;

  // ---- v6: what the vessels hold (game/container.h) --------------------------
  //
  //   u32 hotbarCount  then per slot: u32 fill (PackItemFill: mat | eighths<<16)
  //   u32 bagCount     then per slot: u32 fill
  //   u32 equipCount   then per slot: u32 fill
  //
  // The dyes' parallel-array shape, for the dyes' reason. The material is an
  // ID, not a name, like the grid it came out of -- and like the grid it is
  // remapped BY NAME on load (rule-unification W1-D): the file that holds this
  // payload names the material table it was written under (sim/mattable.h),
  // so a renumbering content change no longer repaints either.
  auto putFills = [&](const ItemStack* v, int n) {
    PutU32(out, (uint32_t)n);
    for (int i = 0; i < n; i++)
      PutU32(out, v[i].Empty() ? 0u : PackItemFill(v[i].fillMat, v[i].fillAmt));
  };
  putFills(r.hotbar->slots, kItemSlots);
  putFills(r.kit->bag.slots, Bag::kSlots);
  putFills(r.kit->equip.slots, kEquipSlotCount);
}

bool LoadPlayerKit(const PlayerKitRefs& r, const uint8_t* data, size_t len,
                   uint32_t version) {
  if (version > kPlayerKitSaveVersion || version < kPlayerKitOldestLoadable) {
    std::fprintf(stderr, "PLYR: unknown version %u (this build writes %u, loads %u..%u)\n",
                 version, kPlayerKitSaveVersion, kPlayerKitOldestLoadable,
                 kPlayerKitSaveVersion);
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
        // Ids by the table this payload was written under (mattable.h).
        if (const MatRemap* mr = ActiveLoadRemap()) RemapPrefabVoxels(sh.lattice, *mr);
        d.shells.push_back(std::move(sh));
      }
      // Damage for a piece that no longer exists is simply forgotten -- the
      // same rule the slots above follow for an unresolvable name.
      if (rd.ok && !d.Empty() && r.items->Find(name) >= 0)
        r.kit->wornDamage[name] = std::move(d);
    }
  }
  // ---- v4: the grimoire ------------------------------------------------------
  r.caster->grimoire.pages.clear();
  if (version >= 4) {
    const uint32_t pages = rd.U32();
    for (uint32_t i = 0; i < pages && rd.ok; i++) {
      GrimoirePage p;
      p.name = rd.Str();
      const uint32_t nw = rd.U32();
      if (!rd.ok || (size_t)nw * 4u > rd.left) {
        rd.ok = false;
        break;
      }
      for (uint32_t k = 0; k < nw && rd.ok; k++) p.words.push_back(rd.Str());
      // Kept even when a word names nothing any more: the readout shows `?`
      // and the page is the player's to fix, not the loader's to delete.
      if (rd.ok && !p.name.empty() &&
          (int)r.caster->grimoire.pages.size() < r.glyphs->budgets.maxGrimoirePages)
        r.caster->grimoire.pages.push_back(std::move(p));
    }
    const uint32_t slots = rd.U32();
    for (uint32_t i = 0; i < slots && rd.ok; i++) {
      const uint32_t kind = rd.U32();
      const std::string name = rd.Str();
      if (!rd.ok) break;
      if ((int)i >= kGlyphSlots) continue;
      if (kind == (uint32_t)SlotKind::Page) {
        if (!name.empty()) gi.BindPage((int)i, name);
      } else if (kind == (uint32_t)SlotKind::Glyph) {
        gi.Bind((int)i, name.empty() ? -1 : r.glyphs->Find(name));
      } else {
        gi.Bind((int)i, -1);
      }
    }
  }
  // ---- v5: the dyes ----------------------------------------------------------
  // Applied only to slots that actually resolved to something: a dye on a slot
  // whose item is gone would be a colour attached to nothing, and the stack is
  // already empty by here.
  if (version >= 5) {
    auto getDyes = [&](ItemStack* v, int n) {
      const uint32_t count = rd.U32();
      for (uint32_t i = 0; i < count && rd.ok; i++) {
        const uint32_t d = rd.U32();
        if (!rd.ok || (int)i >= n) continue;
        if (!v[i].Empty()) v[i].dye = d;
      }
    };
    getDyes(r.hotbar->slots, kItemSlots);
    getDyes(r.kit->bag.slots, Bag::kSlots);
    getDyes(r.kit->equip.slots, kEquipSlotCount);
  }
  // ---- v6: vessel contents ---------------------------------------------------
  // Only onto a slot that resolved to a VESSEL: contents on anything else are
  // matter attached to nothing, and would make that stack refuse to merge.
  if (version >= 6) {
    auto getFills = [&](ItemStack* v, int n) {
      const uint32_t count = rd.U32();
      for (uint32_t i = 0; i < count && rd.ok; i++) {
        const uint32_t f = rd.U32();
        if (!rd.ok || (int)i >= n || v[i].Empty()) continue;
        const ItemDef* d = r.items->At(v[i].def);
        if (!d || !d->IsContainer()) continue;
        v[i].fillMat = ItemFillMat(f);
        if (const MatRemap* mr = ActiveLoadRemap())
          v[i].fillMat = (uint16_t)mr->Mat(v[i].fillMat);
        v[i].fillAmt = (uint16_t)std::min<int>(ItemFillAmt(f),
                                               d->container.capacity);
        if (v[i].fillAmt == 0) v[i].fillMat = 0;
      }
    };
    getFills(r.hotbar->slots, kItemSlots);
    getFills(r.kit->bag.slots, Bag::kSlots);
    getFills(r.kit->equip.slots, kEquipSlotCount);
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

// ONE ENTRY of the payload below -- the whole section is `u32 count` + these,
// and a region bucket (S4) stores each one as its own record. `posOut` is
// where the item is, which is what buckets it.
void SaveOneWorldItem(const WorldItemRefs& r, const WorldItem& w,
                      std::vector<uint8_t>& out, Vec3* posOut) {
  {
    PutStr(out, w.item);
    // v2: THE DYE (game/dye.h). Written next to the name because it is the
    // other half of what the thing on the ground IS — a dropped red tunic and
    // a dropped blue one share a name and a lattice, and the word is not
    // recoverable from either.
    PutU32(out, w.dye);
    // v3: what a dropped vessel holds (WorldItem::fill).
    PutU32(out, w.fill);
    BodyTransform xf{};
    r.phys->GetTransform(w.body, xf);
    if (posOut) *posOut = xf.pos;
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

void SaveWorldItems(const WorldItemRefs& r, std::vector<uint8_t>& out) {
  const std::vector<WorldItem>& all = r.reg->All();
  PutU32(out, (uint32_t)all.size());
  for (const WorldItem& w : all) SaveOneWorldItem(r, w, out, nullptr);
}

bool LoadWorldItems(const WorldItemRefs& r, const uint8_t* data, size_t len,
                    uint32_t version) {
  // A RANGE, not an equality. v2 inserts one word per entry and everything
  // else about the record is unchanged, so a v1 payload still reads exactly as
  // it did — with no dyes, which is what it had.
  if (version > kWorldItemSaveVersion || version < 1) {
    std::fprintf(stderr, "ITMS: unknown version %u (this build writes %u)\n",
                 version, kWorldItemSaveVersion);
    return false;
  }
  Reader rd{data, len};
  const uint32_t n = rd.U32();
  int dropped = 0;
  for (uint32_t i = 0; i < n && rd.ok; i++) {
    const std::string name = rd.Str();
    const uint32_t dye = version >= 2 ? rd.U32() : 0u;
    uint32_t fill = version >= 3 ? rd.U32() : 0u;
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
    // The lattice and the vessel's contents are ids in the table this record
    // was written under; running ids from here on (sim/mattable.h).
    if (const MatRemap* mr = ActiveLoadRemap()) {
      RemapPrefabVoxels(lat, *mr);
      if (fill != 0)
        fill = PackItemFill((uint16_t)mr->Mat(ItemFillMat(fill)), ItemFillAmt(fill));
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
                    lat.empty() ? nullptr : &lat, dye, fill);
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

void SavePlayerKit(const PlayerKitRefs& r, std::vector<uint8_t>& out, uint32_t version) {
  SavePlayerKitBytes(r, out, version);
}

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
  // ---- 'MOBG': the mob id counter, GLOBAL (S4) ------------------------------
  //
  // Registered BEFORE 'MOBS' so it sits ahead of it in world.sve and in the
  // load order: every creature a region bucket spawns draws its id from the
  // counter this restores. max(), never assignment -- the rule SetIdBand
  // states: a counter already past the saved one (a long session, a client's
  // id band) must not be pulled back into ids it has issued.
  //
  // Why it is global at all: ids are session-local today (a loaded mob gets a
  // fresh one, mob.h SaveOne), but they key RNG (gore variance, carve noise)
  // and S5b's dormant NPCs will outlive the window, so "never re-issue an id
  // this world has used" has to hold across a quit, not only within one run.
  // No reset: a load without the section leaves the counter where it is,
  // which is the pre-S4 behaviour exactly.
  io.sections.push_back(EntitySection{
      FourCC('M', 'O', 'B', 'G'), kMobGlobalSaveVersion, [] {},
      [&mobs](std::vector<uint8_t>& out) {
        const uint64_t n = mobs.NextIdCounter();
        PutU32(out, (uint32_t)(n & 0xFFFFFFFFull));
        PutU32(out, (uint32_t)(n >> 32));
      },
      [&mobs](const uint8_t* d, size_t n, uint32_t v) {
        if (v != kMobGlobalSaveVersion) return false;
        Reader rd{d, n};
        const uint64_t lo = rd.U32(), hi = rd.U32();
        if (!rd.ok) return false;
        const uint64_t saved = lo | (hi << 32);
        mobs.SetNextIdCounter(std::max(mobs.NextIdCounter(), saved));
        return true;
      }});
  // ---- 'MOBS': one record per creature, bucketed by REGION (S4) -----------
  //
  // A record is exactly one Mob::SaveOne -- the bytes the whole section has
  // always been a count-prefixed list of -- so the per-record loader wraps it
  // as a count-1 payload and hands it to the same LoadState. Nothing in the
  // record names another creature (it carries no id), so a crowd splits
  // across buckets freely. Bucketed by ORIGIN, the position LoadOne respawns
  // at. Opaque here: S5a owns what SaveOne writes.
  {
    EntitySection mobsSec{
        FourCC('M', 'O', 'B', 'S'), MobSystem::kSaveVersion,
        [&mobs] { mobs.Reset(); },
        [&mobs](std::vector<uint8_t>& out) { mobs.SaveState(out); },
        [&mobs](const uint8_t* d, size_t n, uint32_t v) {
          return mobs.LoadState(d, n, v);
        }};
    mobsSec.scope = EntityScope::Region;
    mobsSec.saveRecords = [&mobs](std::vector<EntityRecord>& out) {
      for (uint32_t i = 0; i < mobs.MobCount(); i++) {
        const Mob* m = mobs.MobAt(i);
        // SaveState's own filter: the living and the unreleased dead (MOBS
        // v6); a husk's rig is DBRS's already.
        if (!m || !m->Def() || !mobs.SavesAsRecord(*m)) continue;
        EntityRecord r;
        r.pos = m->Origin();
        ByteWriter w{r.bytes};
        m->SaveOne(w);
        out.push_back(std::move(r));
      }
    };
    // Straight to LoadOne (LoadState's loop body) rather than through a
    // count-1 LoadState: LoadState reports only whether the BYTES parsed, and
    // this has to know whether the creature went live. A full crowd is not
    // worth parsing for -- the record stays parked, Unpark's capWaits rule.
    mobsSec.loadRecord = [&mobs](const uint8_t* d, size_t n, uint32_t v) {
      // A corpse holds no living slot (MOBS v6): HasRoomForRecord answers
      // yes for a dead record whatever the crowd, and LoadOne decays the
      // oldest corpse if the dead cap is over.
      if (!mobs.HasRoomForRecord(d, n, v)) return RecordLoad::Retry;
      ByteReader rd{d, n};
      bool refused = false;
      if (mobs.LoadOne(rd, v, /*placeLimbs=*/false, &refused) != nullptr)
        return RecordLoad::Applied;
      return refused ? RecordLoad::Retry : RecordLoad::Dropped;
    };
    io.sections.push_back(std::move(mobsSec));
  }
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
    // The body is the PLAYER's, not the world's: players/<id>.svp (S4).
    io.sections.back().scope = EntityScope::Player;
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
          r.caster->grimoire.pages.clear();
        },
        [r](std::vector<uint8_t>& out) { SavePlayerKitBytes(r, out, kPlayerKitSaveVersion); },
        [r](const uint8_t* d, size_t n, uint32_t v) {
          return LoadPlayerKit(r, d, n, v);
        }});
    io.sections.back().scope = EntityScope::Player;  // players/<id>.svp (S4)
  }
  if (ground && ground->Complete()) {
    const WorldItemRefs g = *ground;
    EntitySection itms{
        FourCC('I', 'T', 'M', 'S'), kWorldItemSaveVersion,
        // Reset clears the REGISTRY only. The bodies belong to DebrisSystem,
        // whose own reset runs from its own section — clearing them here would
        // be a second owner for the same handles, and the order of the two
        // resets would then matter.
        [g] { g.reg->Clear(); },
        [g](std::vector<uint8_t>& out) { SaveWorldItems(g, out); },
        [g](const uint8_t* d, size_t n, uint32_t v) {
          return LoadWorldItems(g, d, n, v);
        }};
    // One record per item, bucketed by where it lies (S4). An entry names
    // nothing but content and its own pose, so items split freely.
    itms.scope = EntityScope::Region;
    itms.saveRecords = [g](std::vector<EntityRecord>& out) {
      for (const WorldItem& w : g.reg->All()) {
        EntityRecord r;
        SaveOneWorldItem(g, w, r.bytes, &r.pos);
        out.push_back(std::move(r));
      }
    };
    itms.loadRecord = [g](const uint8_t* d, size_t n, uint32_t v) {
      std::vector<uint8_t> one;
      PutU32(one, 1u);
      one.insert(one.end(), d, d + n);
      return LoadWorldItems(g, one.data(), one.size(), v) ? RecordLoad::Applied
                                                          : RecordLoad::Dropped;
    };
    io.sections.push_back(std::move(itms));
  }
  // ---- 'TIME': the celestial clock, GLOBAL (S4) ------------------------------
  //
  // TIME OF DAY is ComputeSky(tuning, celestial tick), and the celestial tick
  // is the sim tick unless the dev time-scale has ENGAGED the clock (world.h
  // CelestialClock). The sim tick is already in meta.svm, so the only state a
  // save was missing is the clock itself: engaged, its rational scale, its
  // integer position and remainder, and the previous position the day/night
  // wake handshake compares against. All integers, restored exactly.
  //
  // THE SCALE IS STORED BUT IS NOT AUTHORITY after a load: the frame loop
  // re-asserts the UI slider every tick (session.cpp SetScale), so what a load
  // really restores is WHERE the sun is, which is the thing the player saw.
  //
  // WEATHER HAS NO STATE OF ITS OWN to put here: WindWeatherQ (sim/wind.h) is
  // a pure function of (tuning, seed, SIM tick), all three of which a save
  // already pins (tuning is content, seed and tick are in meta). It round-trips
  // exactly when the sim tick does -- which a fresh process loading the save
  // does, and an in-session reload of an OLDER save does not, because main.cpp
  // only moves its clock forward (see the resume note there). ResumeWorldClock
  // below closes that gap for the sky; the wind would need its own clock.
  //
  // Reset to the default (disengaged) clock on every load: a pre-S4 save had
  // no clock state, and "the celestial tick is the sim tick" is exactly what it
  // was saved under.
  io.sections.push_back(EntitySection{
      FourCC('T', 'I', 'M', 'E'), kWorldTimeSaveVersion,
      [] { Celestial() = CelestialClock{}; },
      [](std::vector<uint8_t>& out) {
        const CelestialClock& c = Celestial();
        auto put64 = [&out](int64_t v) {
          PutU32(out, (uint32_t)((uint64_t)v & 0xFFFFFFFFull));
          PutU32(out, (uint32_t)((uint64_t)v >> 32));
        };
        PutU32(out, c.engaged ? 1u : 0u);
        put64(c.scaleNum);
        put64(c.scaleDen);
        put64(c.ticks);
        put64(c.rem);
        put64(c.prevTicks);
      },
      [](const uint8_t* d, size_t n, uint32_t v) {
        if (v != kWorldTimeSaveVersion) return false;
        Reader rd{d, n};
        auto get64 = [&rd]() {
          const uint64_t lo = rd.U32(), hi = rd.U32();
          return (int64_t)(lo | (hi << 32));
        };
        CelestialClock c;
        c.engaged = rd.U32() != 0;
        c.scaleNum = get64();
        c.scaleDen = get64();
        c.ticks = get64();
        c.rem = get64();
        c.prevTicks = get64();
        // Refused whole, never half-applied; a zero denominator would divide
        // by zero on the next Advance.
        if (!rd.ok || c.scaleDen <= 0) return false;
        Celestial() = c;
        return true;
      }});
  // ---- W-D: THE DISCOVERED-BODY REGISTRY (PLAN_water_relevel.md §8.3) ------
  //
  // Unconditional, and it takes no reference: WaterBodies() is a process global
  // for the reason WindPrims() is — it has to reach the frame loop, every gate
  // and both smoke harnesses, and any path that did not get it passed would
  // describe a world with no lakes in it.
  //
  // WHY THIS ONE SYSTEM PERSISTS AND THE REST OF THE WATER RECORD DOES NOT.
  // Everything else in waterbody.h is DERIVED — basins are a pure function of
  // (seed, window), the ledger is a cache of aggregates over voxels the loader
  // has just restored — and derived data is reconstructible and disposable
  // (design guideline #3). A probe disc is not: it is the residue of what the
  // player DID, so it is authored-equivalent truth and this is where it lives.
  // What crosses the boundary is a few ints an entry and nothing that decides a
  // voxel: on load the entries are re-proposed and the GPU re-adopts each one by
  // re-measuring the restored water, which is the same path it took the first
  // time.
  io.sections.push_back(EntitySection{
      FourCC('W', 'T', 'R', 'B'), sandvox::WaterBodySystem::kSaveVersion,
      // Runs on EVERY load, section present or not: a save made before W-D must
      // not leave this session's probes standing in the loaded world. It clears
      // ONLY the registry — the basins, the labelling and the GPU ledger belong
      // to the worldgen/load path and have their own owner.
      [] { sandvox::WaterBodies().ClearDiscovered(); },
      [](std::vector<uint8_t>& out) { sandvox::WaterBodies().SaveState(out); },
      [](const uint8_t* d, size_t n, uint32_t v) {
        return sandvox::WaterBodies().LoadState(d, n, v);
      }});
  return io;
}

bool ResumeWorldClock(uint32_t savedSimTick, uint32_t simTickNow) {
  CelestialClock& c = Celestial();
  if (c.engaged || savedSimTick == simTickNow) return false;
  // Engage at the saved position, at 1x in the UI's own quantisation (1024),
  // so the first SetScale(1.0) the frame loop makes is a no-op rather than a
  // re-base.
  c.engaged = true;
  c.scaleNum = 1024;
  c.scaleDen = 1024;
  c.ticks = (int64_t)savedSimTick;
  c.prevTicks = c.ticks;
  c.rem = 0;
  return true;
}

// ---- MobParking (persist.h, PLAN_save_system.md S5b) -------------------------

void MobParking::Bind(MobSystem& mobs, ChunkStore& store, bool enabled) {
  if (boundMobs_ == &mobs && boundStore_ == &store && enabled_ == enabled &&
      mobs.HasParkFn() == enabled)
    return;
  boundMobs_ = &mobs;
  boundStore_ = &store;
  enabled_ = enabled;
  if (!enabled) {
    mobs.SetParkFn(nullptr);
    return;
  }
  ChunkStore* st = &store;
  Stats* stats = &stats_;
  mobs.SetParkFn([st, stats](const Mob& m, std::vector<uint8_t>& record) {
    // THE SAME RECORD A SAVE WRITES: section, version, origin, SaveOne bytes
    // (persist.cpp's 'MOBS' saveRecords builds exactly this). Its matTable
    // stays 0, "this process's running table" (mattable.h).
    ChunkStore::EntityRecord r;
    r.section = FourCC('M', 'O', 'B', 'S');
    r.version = MobSystem::kSaveVersion;
    r.pos = m.Origin();
    r.bytes = std::move(record);
    st->DormantEntities(ChunkStore::RegionOfVoxel(r.pos)).push_back(std::move(r));
    stats->parked++;
    return true;
  });
  // Something new may be parked right at a face the window is about to cross.
  pending_ = true;
}

void MobParking::ResetWaits() {
  waits_.clear();
  haveOrigin_ = false;
  pending_ = true;
}

bool MobParking::GroundKnown(World& world, IVec3 wc, uint32_t tick) {
  const uint64_t key = World::PackChunkKey(wc);
  auto it = waits_.find(key);
  if (it == waits_.end()) {
    // First sight: whatever the cache holds may predate this residency.
    waits_[key] = Wait{wc, tick};
    world.RequestChunkFetch(wc, World::FetchSource::Mob);
    return false;
  }
  const CachedChunk* cc = world.Cached(wc);
  if (cc != nullptr && cc->voxels.size() == kChunkVol && cc->version >= it->second.since)
    return true;
  world.RequestChunkFetch(wc, World::FetchSource::Mob);  // coalesced if queued
  return false;
}

uint32_t MobParking::Unpark(MobSystem& mobs, ChunkStore& store, World& world,
                            uint32_t tick, uint32_t budget) {
  stats_.lastCall = 0;
  if (!enabled_) return 0;
  const IVec3 wo = world.WindowOrigin();
  const bool moved = !haveOrigin_ || wo.x != lastOrigin_.x ||
                     wo.y != lastOrigin_.y || wo.z != lastOrigin_.z;
  if (!moved && !pending_) return 0;
  const int n = (int)(kWorldN / kChunk);
  if (moved) {
    // A chunk that left the window forgets its wait: its cached copy is stale
    // the moment it re-enters, and asking again is what proves freshness.
    for (auto it = waits_.begin(); it != waits_.end();) {
      const IVec3& c = it->second.wc;
      const bool in = c.x >= wo.x && c.x < wo.x + n && c.y >= wo.y &&
                      c.y < wo.y + n && c.z >= wo.z && c.z < wo.z + n;
      it = in ? std::next(it) : waits_.erase(it);
    }
  }
  haveOrigin_ = true;
  lastOrigin_ = wo;
  pending_ = false;

  const int m = kUnparkMarginChunks;
  auto chunkOf = [](float v) { return (int)std::floor((double)v / (double)kChunk); };
  auto inside = [&](const Vec3& p) {
    const int cx = chunkOf(p.x), cy = chunkOf(p.y), cz = chunkOf(p.z);
    return cx >= wo.x + m && cx < wo.x + n - m && cy >= wo.y + m &&
           cy < wo.y + n - m && cz >= wo.z + m && cz < wo.z + n - m;
  };
  const uint32_t kMobs = FourCC('M', 'O', 'B', 'S');
  const IVec3 lo = ChunkStore::RegionOfChunk(wo);
  const IVec3 hi = ChunkStore::RegionOfChunk({wo.x + n - 1, wo.y + n - 1, wo.z + n - 1});
  uint32_t made = 0;
  for (int rz = lo.z; rz <= hi.z; rz++)
    for (int ry = lo.y; ry <= hi.y; ry++)
      for (int rx = lo.x; rx <= hi.x; rx++) {
        std::vector<EntityRecord>& dormant = store.DormantEntities({rx, ry, rz});
        for (size_t i = 0; i < dormant.size();) {
          if (dormant[i].section != kMobs || !inside(dormant[i].pos)) {
            i++;
            continue;
          }
          // Every wait below leaves the record where it is and asks for a
          // later call; none of them drops it.
          if (made >= budget) {
            stats_.budgetWaits++;
            pending_ = true;
            i++;
            continue;
          }
          // The LIVING cap; a dead record is never held by it (MOBS v6).
          if (!mobs.HasRoomForRecord(dormant[i].bytes.data(),
                                     dormant[i].bytes.size(),
                                     dormant[i].version)) {
            stats_.capWaits++;
            pending_ = true;
            i++;
            continue;
          }
          // The record's position is the body's MIN CORNER (Mob::Origin), and
          // the ground the body rests on runs under the whole footprint -- so
          // the chunks under the corner AND half a chunk on in x and z (the
          // centre column of anything up to a chunk wide), at the feet and
          // one below. Every one is asked every time (no short-circuit), so
          // the fetches go out together rather than one call apart.
          const Vec3 p = dormant[i].pos;
          const float half = 0.5f * (float)kChunk;
          const int xs[2] = {chunkOf(p.x), chunkOf(p.x + half)};
          const int zs[2] = {chunkOf(p.z), chunkOf(p.z + half)};
          const int fy = chunkOf(p.y);
          bool ground = true;
          for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++) {
              if (a == 1 && xs[1] == xs[0]) continue;
              if (b == 1 && zs[1] == zs[0]) continue;
              for (int dy = 0; dy >= -1; dy--)
                ground &= GroundKnown(world, {xs[a], fy + dy, zs[b]}, tick);
            }
          if (!ground) {
            stats_.groundWaits++;
            pending_ = true;
            i++;
            continue;
          }
          // Take it OUT of the bucket first, then apply (ApplyRegionEntities'
          // order): the creature is live now, and the next save writes it back
          // from MobSystem. A record LoadOne refuses for GOOD (def retired,
          // short read) is logged there and dropped -- the load path's rule.
          // A refused SPAWN (BuildRig short of a physics body or micro brick;
          // the cap was checked above) is transient: the record goes back
          // where it was and this call stops spawning, so a pool that stays
          // full costs one failed rig build per call, not one per record.
          EntityRecord r = std::move(dormant[i]);
          dormant.erase(dormant.begin() + (ptrdiff_t)i);
          ByteReader rd{r.bytes.data(), r.bytes.size()};
          bool refused = false;
          // The record's lattice ids are in ITS table: a creature parked by a
          // build with a different materials.json comes back by name.
          const ScopedLoadRemap remapScope(store.Tables().RemapFor(r.matTable));
          // placeLimbs: it comes back in the pose it left in (mob.h LoadOne).
          if (mobs.LoadOne(rd, r.version, /*placeLimbs=*/true, &refused) !=
              nullptr) {
            made++;
            stats_.unparked++;
          } else if (refused) {
            stats_.spawnWaits++;
            pending_ = true;
            // Back at the index it came from, so the walk resumes after it.
            dormant.insert(dormant.begin() + (ptrdiff_t)i, std::move(r));
            // Spend the rest of this call's budget: nothing more spawns now.
            budget = made;
            i++;
          } else {
            stats_.failed++;
            std::fprintf(stderr,
                         "unpark: a parked creature record (v%u) in region "
                         "(%d,%d,%d) failed to apply and is dropped\n",
                         r.version, rx, ry, rz);
          }
        }
      }
  stats_.lastCall = made;
  stats_.maxCall = std::max(stats_.maxCall, made);
  return made;
}
