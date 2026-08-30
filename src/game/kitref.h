#pragma once
#include <cstdint>

// ONE ADDRESS SPACE FOR EVERY ITEM SLOT THE PLAYER HAS.
//
// A drag has to say "from bag 12 to equip 3" in a single value, because the
// character screen draws three different containers and the intent latch that
// carries a drop back to main.cpp must not grow a case per pair.
//
// This is its own dependency-free header, and that is the whole point: the UI
// layer (ui/overlay.h) REQUESTS a move and main.cpp EXECUTES it, so the two
// have to agree on the address. Given a copy each, they would be exactly the
// "two places must agree" shape scripts/check_invariants.py exists to catch —
// so instead there is one definition, and it costs the UI no game headers to
// use it (game/equipment.h drags in item.h -> anim.h -> microbody.h, which the
// overlay has no business seeing).
enum class KitSpace : uint8_t {
  None = 0,
  Bag,      // general storage grid (game/equipment.h Bag)
  Hotbar,   // the 10 number-row slots (game/item.h Inventory)
  Equip,    // worn/sheathed/quick slots (game/equipment.h Equipment)
};

struct KitRef {
  KitSpace space = KitSpace::None;
  int index = -1;
  bool Valid() const { return space != KitSpace::None && index >= 0; }
  bool operator==(const KitRef& o) const {
    return space == o.space && index == o.index;
  }
  bool operator!=(const KitRef& o) const { return !(*this == o); }
};
