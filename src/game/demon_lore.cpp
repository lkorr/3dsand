// demon_lore.cpp — see demon_lore.h.

#include "game/demon_lore.h"

#include "game/caster.h"
#include "game/spell.h"

namespace demon {

bool IsNameGlyph(const GlyphLibrary& lib, int index) {
  if (index < 0 || index >= (int)lib.glyphs.size()) return false;
  return lib.glyphs[(size_t)index].id.rfind("summon_", 0) == 0;
}

int GrantAllNames(GlyphInventory& inv, const GlyphLibrary& lib) {
  int fresh = 0;
  // Library order: the same inventory whatever order the button is pressed in.
  for (int i = 0; i < (int)lib.glyphs.size(); i++) {
    if (!IsNameGlyph(lib, i) || inv.Owns(i)) continue;
    inv.Grant(i);
    fresh++;
  }
  return fresh;
}

int CountNames(const GlyphLibrary& lib) {
  int n = 0;
  for (int i = 0; i < (int)lib.glyphs.size(); i++) n += IsNameGlyph(lib, i) ? 1 : 0;
  return n;
}

}  // namespace demon
