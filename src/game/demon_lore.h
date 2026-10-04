// demon_lore.h — what the PLAYER knows about demons: their names
// (docs/PLAN_demons.md D2; D1 owns the summoning itself).
//
// THE NAME IS THE GLYPH (plan, "Facts": the VM has no string arguments, so one
// glyph per demon). Knowing a demon's name = owning its summon glyph in the
// player's GlyphInventory. The ways to come by one:
//   * reading it (the Harrowby cellar book: a `readable` ref whose dialogue
//     runs {"grant": "summon_skerrick"}, game/dialogue.h);
//   * the dev panel's "learn all demon names" button (Spells section; applied
//     in the tick, session.cpp, next to the dev mana overrides);
//   * SANDVOX_ALL_NAMES=1 (D1 owns that env read; it can call GrantDemonNames).
//
// ONE PLACE says which glyphs are names. Today: a glyph whose id starts with
// "summon_" (the plan names them summon_<demon>). When D1's summon verb lands
// the test can become "its program uses SpellVerb::Summon" HERE, and every
// caller follows. GrantAllAndBind's exclusion of names (D1) should ask this
// too, so the debug grant and the exclusion can never disagree.

#pragma once

struct GlyphLibrary;
struct GlyphInventory;

namespace demon {

// Is glyph `index` of `lib` a demon's name?
bool IsNameGlyph(const GlyphLibrary& lib, int index);
// Grant every name glyph `lib` holds. Returns how many were NEW to `inv`.
int GrantAllNames(GlyphInventory& inv, const GlyphLibrary& lib);
// How many name glyphs `lib` holds.
int CountNames(const GlyphLibrary& lib);

}  // namespace demon
