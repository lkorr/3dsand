#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ITEMS AND THE HOTBAR: what the player is carrying and what is in their hand.
//
// Deliberately shaped like game/caster.h's GlyphInventory, and for the same
// reasons: a plain data structure with no engine coupling, owned by main.cpp,
// that neither Player nor PlayerAvatar has to know about. Player stays a clean
// movement controller; the avatar is told which prop to show and nothing more.
//
// WHAT AN ITEM IS. An item is a NAME plus the behaviour class the engine
// switches on, and — for anything held — the name of the rig part that
// represents it. It is NOT a mesh: the sword's art is a part of the avatar's
// own rig (assets/mobs/mina.*, `sword`), authored and severable exactly like a
// hand, so equipping is "show that part" rather than "spawn an object and
// constrain it to a bone". That choice is what makes a dropped sword, a
// severed sword-arm and a burnt sword all work with no new code.
//
// SCOPE. This is a hotbar, not an inventory screen: 10 slots, one selected,
// no stacking beyond a count, no crafting, no grid UI. The interesting
// decisions are meant to be about what you have in hand right now.

// What the engine does with an item when it is held. Adding a kind means
// adding a case where the tick loop dispatches on Held(), not a new system.
enum class ItemKind : uint8_t {
  None = 0,
  Melee,   // swung: drives game/melee.h, cuts with its part's authored `edge`
};

struct ItemDef {
  std::string name;        // display name, and the id other data refers to
  ItemKind kind = ItemKind::None;
  // Rig part that shows this item in hand, "" for an item with no visible
  // prop. Resolved against the avatar's def at equip time; an unknown name is
  // a diagnostic, never a load failure (the item is simply invisible).
  std::string part;
  // ---- melee ----
  // Damage per hit at full swing speed. Scaled by how fast the edge is
  // actually travelling, so a lazy wave scratches and a committed cut opens.
  float damage = 12.0f;
  // Extra carve radius beyond the blade's own authored halfWidth, world
  // voxels. This is the difference between a cut and a cleave; keep it small,
  // since the blade geometry is supposed to be what decides the wound.
  float carveBonus = 0.0f;
  // Reach in world voxels from the shoulder, used to size the swing arc.
  float reach = 9.0f;
};

// The item library. Loaded once; indices into it are what a slot stores, the
// same way a glyph slot stores an index into GlyphLibrary::glyphs.
struct ItemLibrary {
  std::vector<ItemDef> items;

  int Find(const std::string& name) const {
    for (size_t i = 0; i < items.size(); i++)
      if (items[i].name == name) return (int)i;
    return -1;
  }
  const ItemDef* At(int i) const {
    return (i >= 0 && i < (int)items.size()) ? &items[i] : nullptr;
  }
};

// Which slots exist. 10, on the number row, matching kGlyphSlots — the two
// hotbars are the same shape on purpose, since magic mode and item mode are
// the same hand reaching for the same keys.
constexpr int kItemSlots = 10;

struct ItemStack {
  int def = -1;      // index into ItemLibrary::items, -1 = empty
  int count = 0;
  bool Empty() const { return def < 0 || count <= 0; }
};

struct Inventory {
  ItemStack slots[kItemSlots];
  int selected = 0;

  const ItemStack& Selected() const { return slots[Clamp(selected)]; }
  // The def index of what is in hand, or -1 for an empty hand.
  int HeldDef() const { return Selected().Empty() ? -1 : Selected().def; }

  void Select(int slot) { selected = Clamp(slot); }
  // Scroll wheel: wraps, because a hotbar that stops at the ends makes the
  // player look at it.
  void Scroll(int delta) {
    if (delta == 0) return;
    int n = kItemSlots;
    selected = ((selected + delta) % n + n) % n;
  }

  // Adds to the first slot already holding this def, else the first empty one.
  // Returns the slot, or -1 when the hotbar is full — the caller decides what
  // that means (here: the pickup is refused and the item stays in the world).
  int Add(int defIndex, int count = 1) {
    if (defIndex < 0 || count <= 0) return -1;
    for (int i = 0; i < kItemSlots; i++)
      if (!slots[i].Empty() && slots[i].def == defIndex) {
        slots[i].count += count;
        return i;
      }
    for (int i = 0; i < kItemSlots; i++)
      if (slots[i].Empty()) {
        slots[i] = {defIndex, count};
        return i;
      }
    return -1;
  }

  // Removes one from a slot, emptying it at zero. Returns the def removed.
  int TakeOne(int slot) {
    int s = Clamp(slot);
    if (slots[s].Empty()) return -1;
    int d = slots[s].def;
    if (--slots[s].count <= 0) slots[s] = ItemStack{};
    return d;
  }

 private:
  static int Clamp(int s) { return s < 0 ? 0 : (s >= kItemSlots ? kItemSlots - 1 : s); }
};

// Loads assets/items/items.json. Errors are appended to `errors` and reported
// the way materials and glyphs are: a malformed item is skipped loudly, a
// missing file is a failure. Hot-reloadable (R) alongside them — nothing here
// is cached by index anywhere except the hotbar, which is re-validated on
// reload by the caller.
bool LoadItems(const std::string& path, ItemLibrary& out, std::string& errors);
