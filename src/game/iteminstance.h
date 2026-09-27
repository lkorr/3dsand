#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "game/composition.h"  // a vessel's contents: up to 16 portions
#include "sim/bytestream.h"
#include "sim/mattable.h"  // MatRemap: saved material ids -> running ids
#include "sim/voxload.h"   // PrefabVoxel: a damaged shell's lattice

// ONE ITEM, AS AN OBJECT (rule-unification W2-M).
//
// Every record that says "there is an item here" — a hotbar/bag/equipment
// slot (ItemStack), a creature's pack, a thing lying on the ground
// (WorldItem), a piece of gear on the wire (net::WireGear), a pickup grant
// (net::ItemGrant), a ground body's announce (net::BodyAnnounce), a loot entry
// (LootPiece) — embeds THIS, and a conversion between two of them copies it
// whole. Until 2026-09-24 each of those carried its own hand-picked subset of
// {name, count, dye, fill, damage}, and every crossing between two subsets
// dropped whatever the narrower one lacked: a flask's contents vanished on a
// network pickup (ItemGrant had no fill) and when a corpse's pack rose with it
// (CarriedItem had no fill), and "item damage" was a uint32 threaded through
// three structs that nothing ever wrote.
//
// BY NAME. `name` is the ItemDef's name, never a library index: an index is
// file order and dies on every R hot-reload (item.h's index hazard), and every
// one of these records outlives a reload somewhere (a save, a packet, a
// corpse). The library is asked `ItemLibrary::Of(instance)` at the moment a
// def is actually wanted.

// ---- WHAT A WORN PIECE HAS BEEN THROUGH -------------------------------------
//
// A shell on the body is a rig slot and carries its own damage: burnt-through
// cloth, a hole a blade opened, the hp that is left. The moment the piece comes
// OFF, that rig slot is destroyed — and with it every trace of what happened to
// the garment, unless something outside the body is holding it. This is that
// something: one entry per cover entry in the item's own order.
//
// WHY NOT REBUILD FROM THE DEF. Because the def is the PRISTINE piece. Taking
// off your boots to put on a different pair and finding the first pair mended
// is the bug this exists to make unrepresentable.
//
// THE LATTICE IS THE AUTHORITATIVE ONE — the skin where a shell has a separate
// skin, the collider where it does not. Storing the derived side would be
// storing something the next re-derive overwrites (phys/lattice.h's one-way
// rule), which is a subtler way of losing the damage.
//
// PER INSTANCE (W2-M). It used to be keyed by item NAME in a map on the
// player's kit, so two robes in one pack shared one set of holes. It is now a
// field of the ItemInstance, so each robe is its own robe.
struct WornShellDamage {
  float hp = -1.0f;                  // < 0 = as authored
  // ---- CONDITION, as two counts rather than as a fraction -------------------
  // `live` of `atSpawn` voxels are still there. Both 0 means "never measured",
  // which reads as whole. Kept ALONGSIDE the exact lattice: the lattice is the
  // truth (it puts the holes back where they were), these are a SUMMARY for a
  // piece that is not on a body, where there is no shell to count.
  uint32_t atSpawn = 0;
  uint32_t live = 0;
  std::vector<PrefabVoxel> lattice;  // empty = as authored
  bool Empty() const {
    return hp < 0.0f && lattice.empty() && live >= atSpawn;
  }
};

struct WornDamage {
  std::vector<WornShellDamage> shells;
  bool Empty() const {
    for (const WornShellDamage& s : shells)
      if (!s.Empty()) return false;
    return true;
  }
  void Clear() { shells.clear(); }
  // How much of the whole piece is still there, 0..1. Volume-weighted across
  // its shells (Mob::WornCondition computes the same thing off the live rig and
  // the two must agree).
  float Condition() const {
    uint64_t a = 0, l = 0;
    for (const WornShellDamage& s : shells) { a += s.atSpawn; l += s.live; }
    if (!a) return 1.0f;
    const float f = (float)l / (float)a;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }
};

// THE LEGACY FILL WORD: material id | eighths << 16, ONE material. What PLYR
// v6/v7, ITMS v3/v4, MOBS v7/v8 and net protocol 1 carried before a vessel
// could hold a mixture (2026-09-26). Read-only now: an old record's word
// becomes a one-portion Composition, and nothing writes the word again.
inline alchemy::Composition ContentsFromLegacyFill(uint32_t f) {
  alchemy::Composition c;
  const uint16_t mat = (uint16_t)(f & 0xFFFFu), amt = (uint16_t)(f >> 16);
  if (mat != 0 && amt != 0) c.Add(mat, amt);
  return c;
}

// ...and the word an OLDER format gets written, which can say one material:
// the portion with the most eighths, at its own amount.
inline uint32_t LegacyFillWordOf(const alchemy::Composition& c) {
  uint32_t best = 0, mat = 0;
  for (int i = 0; i < c.n; i++)
    if (c.p[i].eighths > best) { best = c.p[i].eighths; mat = c.p[i].mat; }
  return best ? (mat | (std::min<uint32_t>(best, 0xFFFFu) << 16)) : 0u;
}

// THE CONTENTS' BYTE SHAPE, written once: a portion count, then (material id,
// eighths) per portion. Bounded on read -- a count past kMaxSubstances is a
// corrupt record -- and a zero portion is dropped rather than kept as an
// empty slot, so `n` always counts real substances.
inline void WriteContents(ByteWriter& w, const alchemy::Composition& c) {
  w.U32(c.n);
  for (int i = 0; i < c.n; i++) {
    w.U32(c.p[i].mat);
    w.U32(c.p[i].eighths);
  }
}
inline bool ReadContents(ByteReader& r, alchemy::Composition& c) {
  c = alchemy::Composition{};
  uint32_t n = 0;
  if (!r.U32(n)) return false;
  if (n > (uint32_t)alchemy::kMaxSubstances) {
    r.ok = false;
    return false;
  }
  for (uint32_t i = 0; i < n && r.ok; i++) {
    uint32_t mat = 0, amt = 0;
    r.U32(mat);
    r.U32(amt);
    if (r.ok && alchemy::ValidPortionMat(mat) && amt != 0) c.Add((uint16_t)mat, amt);
  }
  return r.ok;
}

struct ItemInstance {
  std::string name;     // ItemDef::name; "" = nothing here
  int count = 1;
  // WHAT COLOUR THIS PARTICULAR ONE IS (game/dye.h). 0 = undyed. On the
  // instance, not on the def: the def is the PATTERN.
  uint32_t dye = 0;
  // WHAT IS IN IT, for a vessel (ItemKind::Container, game/container.h): up
  // to 16 (material id, eighths of a cell) portions -- a flask can hold water
  // AND oil (game/composition.h; the alchemy bench, game/flasksim.h, is where
  // they are mixed). Empty on everything that is not a filled vessel.
  alchemy::Composition contents;
  // WHAT IT HAS BEEN THROUGH (worn pieces). Empty = as authored.
  WornDamage damage;
  // A VESSEL'S STOPPER (the alchemy bench's Stopper tool, 2026-09-27): the
  // mouth is closed, so nothing -- gas, liquid, powder -- comes out of it: it
  // pours nothing, scoops nothing, and a gas in it stays in it. Persisted:
  // PLYR v9, ITMS v6, MOBS v10, net protocol 3 (kItemFmtStopper). A break
  // still spills everything; the glass is gone.
  bool stoppered = false;

  bool Empty() const { return name.empty() || count <= 0; }
  bool Filled() const { return !contents.Empty(); }
  // Eighths in it, every portion together.
  uint32_t FillTotal() const { return contents.Total(); }
  void ClearFill() { contents = alchemy::Composition{}; }
  // A PRISTINE instance: nothing about it but its name and colour. Only these
  // merge into a stack — see StacksWith.
  bool Plain() const { return contents.Empty() && damage.Empty() && !stoppered; }
  // DO TWO OF THESE MERGE INTO ONE STACK? Same item, same colour, and BOTH
  // plain. A filled vessel never stacks (a fill is per-object state: two
  // flasks of 40 eighths merged into one count-2 stack with ONE fill, and
  // every path that charges a fill then treats the pair as one flask), and a
  // damaged piece never stacks for the same reason — two identical tunics are
  // two tunics the moment one of them has a hole in it.
  bool StacksWith(const ItemInstance& o) const {
    return name == o.name && dye == o.dye && Plain() && o.Plain();
  }
  // One of this stack, as its own object (a pickup, a drop, a throw).
  ItemInstance One() const {
    ItemInstance r = *this;
    r.count = 1;
    return r;
  }
};

// MATERIAL IDS BY NAME ON LOAD (sim/mattable.h, W1-D): an instance read from a
// save names materials twice — what a vessel holds, and the voxels of a worn
// piece's damaged lattice — and both are ids in the table that file was
// written under. One call puts them into the running table's ids.
inline void RemapItemInstance(ItemInstance& it, const MatRemap& r) {
  // Rebuilt through Add, so two saved materials that map to one running id
  // merge into one portion instead of standing as two.
  if (!it.contents.Empty()) {
    alchemy::Composition c;
    for (int i = 0; i < it.contents.n; i++) {
      // A dissolved portion remaps its powder and keeps the bit.
      const uint16_t p = it.contents.p[i].mat;
      const uint32_t m = r.Mat(alchemy::BaseMat(p));
      if (m != 0)
        c.Add((uint16_t)(m | (alchemy::IsDissolved(p) ? alchemy::kDissolvedBit : 0)), it.contents.p[i].eighths);
    }
    it.contents = c;
  }
  for (WornShellDamage& s : it.damage.shells) RemapPrefabVoxels(s.lattice, r);
}

// ---- the byte shape, written once ------------------------------------------
//
// The wire records (net/mobsync, net/debrissync) and the MOBS save record all
// carry an ItemInstance through these two, so a field added here is added to
// every one of them at once. PLYR/ITMS (game/persist.cpp) keep their own
// append-only layouts and reuse WriteWornDamage's shape field for field.
inline void WriteWornDamage(ByteWriter& w, const WornDamage& d) {
  w.U32((uint32_t)d.shells.size());
  for (const WornShellDamage& s : d.shells) {
    w.F32(s.hp);
    w.U32(s.atSpawn);
    w.U32(s.live);
    w.PodVec(s.lattice);
  }
}

// Bounded: a shell count past `maxShells` is a corrupt record, refused before
// anything is sized on the sender's word.
inline bool ReadWornDamage(ByteReader& r, WornDamage& d,
                           uint32_t maxShells = 64) {
  uint32_t n = 0;
  if (!r.U32(n)) return false;
  d.shells.clear();
  if (n > maxShells) {
    r.ok = false;
    return false;
  }
  for (uint32_t i = 0; i < n && r.ok; i++) {
    WornShellDamage s;
    r.F32(s.hp);
    r.U32(s.atSpawn);
    r.U32(s.live);
    r.PodVec(s.lattice);
    if (!r.ok) break;
    // PrefabVoxel::bruise was padding in files written before 2026-09-26, and
    // a garment never bruises anyway (only a creature's own skin does).
    for (PrefabVoxel& v : s.lattice) v.bruise = 0;
    d.shells.push_back(std::move(s));
  }
  return r.ok;
}

// THE ITEM RECORD'S FORMAT. `fmt` is what the caller's format carries:
//   kItemFmtLegacy  -- the one-material legacy fill word (MOBS < 9, net 1);
//   kItemFmtMixed   -- the contents as a Composition (MOBS v9, net protocol 2);
//   kItemFmtStopper -- that, then the stopper word (MOBS v10+, net 3+).
// Callers pass their format's answer; there is no default, so no reader
// guesses. (A `bool` still converts: true is kItemFmtMixed.)
constexpr int kItemFmtLegacy = 0, kItemFmtMixed = 1, kItemFmtStopper = 2;
// The MOBS section's answer (game/mob.h MobSystem::kSaveVersion's history).
inline int ItemFmtOfMobs(uint32_t version) {
  return version >= 10 ? kItemFmtStopper : version >= 9 ? kItemFmtMixed : kItemFmtLegacy;
}
inline void WriteItemInstance(ByteWriter& w, const ItemInstance& it, int fmt) {
  w.Str(it.name);
  w.U32((uint32_t)(it.count < 0 ? 0 : it.count));
  w.U32(it.dye);
  if (fmt >= kItemFmtMixed) {
    WriteContents(w, it.contents);
  } else {
    w.U32(LegacyFillWordOf(it.contents));
  }
  WriteWornDamage(w, it.damage);
  if (fmt >= kItemFmtStopper) w.U32(it.stoppered ? 1u : 0u);
}

inline bool ReadItemInstance(ByteReader& r, ItemInstance& it, int fmt) {
  uint32_t count = 0;
  r.Str(it.name);
  r.U32(count);
  r.U32(it.dye);
  if (fmt >= kItemFmtMixed) {
    ReadContents(r, it.contents);
  } else {
    uint32_t fill = 0;
    r.U32(fill);
    it.contents = ContentsFromLegacyFill(fill);
  }
  it.count = (int)count;
  const bool ok = ReadWornDamage(r, it.damage) && r.ok;
  it.stoppered = false;
  if (ok && fmt >= kItemFmtStopper) {
    uint32_t s = 0;
    r.U32(s);
    it.stoppered = s != 0;
  }
  return ok && r.ok;
}
