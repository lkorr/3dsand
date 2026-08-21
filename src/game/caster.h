#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/spell.h"

// The PLAYER's side of the spell system: what glyphs they own, which are bound
// to which number key, and the half-spoken spell on the stack.
//
// Deliberately separate from SpellSystem/CasterState (game/spell.h), which are
// the player-agnostic VM (thesis 4): a mob casts through the same Cast() call
// with its own CasterState and never needs any of this. Equally deliberately
// separate from Player, which is a clean movement controller and stays that
// way — nothing here is bolted onto it.
//
// This is a PLACEHOLDER for a real acquisition loop. Right now the player owns
// every glyph in the library and the bindings are the identity mapping.

// Which slots exist. 10, bound to the number row (1234567890): the design
// intent is that the number of glyphs immediately at hand is a real
// constraint, so that choosing what to keep bound — and what to conjoin into
// one key — is the interesting decision.
constexpr int kGlyphSlots = 10;

struct GlyphInventory {
  // Glyph indices the player OWNS (into GlyphLibrary::glyphs).
  std::vector<int> owned;
  // slot -> glyph index, or -1 for an empty slot.
  int bound[kGlyphSlots];

  GlyphInventory() {
    for (int i = 0; i < kGlyphSlots; i++) bound[i] = -1;
  }

  bool Owns(int glyphIndex) const {
    for (int g : owned)
      if (g == glyphIndex) return true;
    return false;
  }
  void Grant(int glyphIndex) {
    if (!Owns(glyphIndex)) owned.push_back(glyphIndex);
  }
  // Binds to a slot; refuses a glyph the player does not own.
  bool Bind(int slot, int glyphIndex) {
    if (slot < 0 || slot >= kGlyphSlots) return false;
    if (glyphIndex >= 0 && !Owns(glyphIndex)) return false;
    bound[slot] = glyphIndex;
    return true;
  }
  int At(int slot) const {
    return (slot >= 0 && slot < kGlyphSlots) ? bound[slot] : -1;
  }

  // Placeholder acquisition: grant everything and bind in library order.
  void GrantAllAndBind(const GlyphLibrary& lib) {
    owned.clear();
    for (int i = 0; i < kGlyphSlots; i++) bound[i] = -1;
    for (int i = 0; i < (int)lib.glyphs.size(); i++) {
      owned.push_back(i);
      if (i < kGlyphSlots) bound[i] = i;
    }
  }
};

// The player's casting state: inventory + the spell being spoken + the mana
// pool. One struct so main.cpp holds one thing rather than five.
struct PlayerCaster {
  GlyphInventory inventory;
  SpellStack stack;
  CasterState mana;

  // Cached compile of `stack`, refreshed whenever the stack changes so the
  // HUD can show the running cost draining live BEFORE the cast. Cost being
  // computable before casting is what makes the mana/health crossover legible,
  // which is the whole tension mechanic.
  Spell compiled;
  SpellReadout readout;

  // Last cast's outcome, for the HUD flash.
  CastOutcome lastOutcome = CastOutcome::Nothing;
  float lastOutcomeAge = 0;

  void Recompile(const GlyphLibrary& lib) {
    compiled = CompileSpell(lib, stack);
    readout = DescribeSpell(lib, stack, compiled);
  }
  // Speak the glyph bound to a slot. Pressing a number SPEAKS, it never casts.
  bool SpeakSlot(const GlyphLibrary& lib, int slot) {
    int gi = inventory.At(slot);
    if (gi < 0) return false;
    // Bound so a stuck key cannot grow the stack without limit (rule 2 applies
    // to UI state too — an unbounded stack is an unbounded mana cost).
    if (stack.spoken.size() >= 16) return false;
    stack.spoken.push_back(gi);
    Recompile(lib);
    return true;
  }
  void Clear(const GlyphLibrary& lib) {
    stack.Clear();
    Recompile(lib);
  }
};
