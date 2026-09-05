#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/spell.h"

// The PLAYER's side of the spell system: what glyphs they own, which are bound
// to which number key, the grimoire of saved word lists, and the half-spoken
// spell on the stack. docs/PLAN_magic_grammar.md §12.
//
// Deliberately separate from SpellSystem/CasterState (game/spell.h), which are
// the player-agnostic VM (thesis 4): a mob casts through the same Cast() call
// with its own CasterState and never needs any of this. Equally deliberately
// separate from Player, which is a clean movement controller and stays that
// way — nothing here is bolted onto it.
//
// EVERYTHING HERE CROSSES BY NAME (DESIGN.md §8b). A slot, a page, a word all
// hold glyph NAMES or page NAMES; indices into GlyphLibrary::glyphs are file-
// order dependent and die on every R reload.

// ---- the tongue: two banks on the number row (§12a) -------------------------
// `1`-`0` speak bank A (slots 0..9); `Shift+1`-`0` speak bank B (10..19).
// Twenty live words, one hand, no menu. The bank is `slot / kGlyphBank`.
constexpr int kGlyphBank = 10;
constexpr int kGlyphBanks = 2;
constexpr int kGlyphSlots = kGlyphBank * kGlyphBanks;

// What a bound slot holds: nothing, a glyph, or a grimoire page (a macro).
enum class SlotKind : uint8_t { None = 0, Glyph, Page };

struct GlyphInventory {
  // Glyph indices the player OWNS (into GlyphLibrary::glyphs).
  std::vector<int> owned;
  // slot -> glyph index, or -1. A slot holding a page has -1 here and the
  // page's name in `page`.
  int bound[kGlyphSlots];
  std::string page[kGlyphSlots];

  GlyphInventory() {
    for (int i = 0; i < kGlyphSlots; i++) bound[i] = -1;
  }

  bool Owns(int glyphIndex) const {
    for (int g : owned)
      if (g == glyphIndex) return true;
    return false;
  }
  void Grant(int glyphIndex) {
    if (glyphIndex >= 0 && !Owns(glyphIndex)) owned.push_back(glyphIndex);
  }
  // Binds a glyph to a slot; refuses a glyph the player does not own. -1
  // clears the slot (page included).
  bool Bind(int slot, int glyphIndex) {
    if (slot < 0 || slot >= kGlyphSlots) return false;
    if (glyphIndex >= 0 && !Owns(glyphIndex)) return false;
    bound[slot] = glyphIndex;
    page[slot].clear();
    return true;
  }
  // Binds a grimoire page by NAME. The caller has checked the page exists
  // (a slot may legitimately hold a page that later disappears: it then
  // speaks nothing, and the strip shows `?`).
  bool BindPage(int slot, const std::string& name) {
    if (slot < 0 || slot >= kGlyphSlots || name.empty()) return false;
    bound[slot] = -1;
    page[slot] = name;
    return true;
  }
  int At(int slot) const {
    return (slot >= 0 && slot < kGlyphSlots) ? bound[slot] : -1;
  }
  const std::string& PageAt(int slot) const {
    static const std::string kNone;
    return (slot >= 0 && slot < kGlyphSlots) ? page[slot] : kNone;
  }
  SlotKind KindAt(int slot) const {
    if (slot < 0 || slot >= kGlyphSlots) return SlotKind::None;
    if (!page[slot].empty()) return SlotKind::Page;
    return bound[slot] >= 0 ? SlotKind::Glyph : SlotKind::None;
  }

  // THE DEBUG DEFAULT (plan §12a: ownership is real, the acquisition loop —
  // loot, tutors — is out of scope; Grant/Owns are its seam). Grants every
  // glyph and binds the first bank in library order.
  void GrantAllAndBind(const GlyphLibrary& lib) {
    owned.clear();
    for (int i = 0; i < kGlyphSlots; i++) {
      bound[i] = -1;
      page[i].clear();
    }
    for (int i = 0; i < (int)lib.glyphs.size(); i++) {
      owned.push_back(i);
      if (i < kGlyphSlots) bound[i] = i;
    }
  }
};

// ---- the grimoire: macros (§12b) ---------------------------------------------
//
// A macro is a saved list of glyph names with a name of its own. Speaking it
// pushes its words onto the stack exactly as if you had spoken them, and the
// six rules apply to the result. That sentence is the whole mechanic;
// everything below is consequence.
struct GrimoirePage {
  std::string name;
  std::vector<std::string> words;   // glyph NAMES and page NAMES
  bool readOnly = false;            // an authored starter (glyphs.json conjoined)
};

struct Grimoire {
  std::vector<GrimoirePage> pages;  // the player's own, editable

  int Find(const std::string& name) const {
    for (size_t i = 0; i < pages.size(); i++)
      if (pages[i].name == name) return (int)i;
    return -1;
  }
};

// The result of expanding a word list: the spoken glyph indices, and what was
// lost on the way. `dropped` words are names that resolve to nothing (content
// removed or renamed) — the page is kept and the readout shows `?` in their
// place (DESIGN §8b). `truncated` is the 16-word stack bound (rule 2: no
// unbounded expansion, ever).
struct GrimoireExpansion {
  std::vector<int> spoken;
  int dropped = 0;
  bool truncated = false;
  bool tooDeep = false;
  // The words in order with `?` for a dropped one, for the readout.
  std::vector<std::string> readout;
};

// A page name resolves against the player's pages first, then the library's
// authored starters (read-only). Returns the page or null.
const GrimoirePage* FindPage(const GlyphLibrary& lib, const Grimoire& g,
                             const std::string& name, GrimoirePage& starterScratch);

// Expand `words` (glyph names and page names) recursively, depth-capped by
// budgets.maxMacroDepth, length-capped by `maxWords` (the stack's remaining
// room). Never fails: a missing name drops one word, a cycle or a too-deep
// nest stops expanding at the cap.
GrimoireExpansion ExpandWords(const GlyphLibrary& lib, const Grimoire& g,
                              const std::vector<std::string>& words, int maxWords);

// Would saving `name` = `words` make a page that contains itself, through any
// chain of pages? True with `why` filled: the save is refused (§12b).
bool GrimoireWouldCycle(const GlyphLibrary& lib, const Grimoire& g,
                        const std::string& name,
                        const std::vector<std::string>& words, std::string& why);

// The page's auto-name from its expansion: `fire-trail-explosive-shotgun2`.
std::string GrimoireAutoName(const GlyphLibrary& lib, const std::vector<int>& spoken);

// ---- the player's casting state ------------------------------------------------
// Inventory + grimoire + the spell being spoken + the mana pool. One struct so
// main.cpp holds one thing rather than five.
struct PlayerCaster {
  GlyphInventory inventory;
  Grimoire grimoire;
  SpellStack stack;
  CasterState mana;

  // Cached compile of `stack`, refreshed whenever the stack changes so the
  // HUD can show the running cost draining live BEFORE the cast.
  CastList compiled;
  SpellReadout readout;

  // Last cast's outcome, for the HUD flash.
  CastOutcome lastOutcome = CastOutcome::Nothing;
  float lastOutcomeAge = 0;
  // What the last speak/capture had to say ("page full", "3 words did not
  // fit", "saved as ..."). Shown by the HUD for a moment.
  std::string note;
  float noteAge = 99.0f;

  void Recompile(const GlyphLibrary& lib) {
    compiled = CompileSpell(lib, stack);
    readout = DescribeSpell(lib, compiled);
  }
  // Speak the glyph or page bound to a slot. Pressing a number SPEAKS, it
  // never casts. Bounded so a stuck key cannot grow the stack without limit
  // (rule 2 applies to UI state too — an unbounded stack is an unbounded
  // mana cost); a page speaks as much of its expansion as fits and says so.
  bool SpeakSlot(const GlyphLibrary& lib, int slot) {
    if (inventory.KindAt(slot) == SlotKind::Page) return SpeakPage(lib, inventory.PageAt(slot));
    const int gi = inventory.At(slot);
    if (gi < 0) return false;
    if ((int)stack.spoken.size() >= kSpellStackMax) return false;
    stack.spoken.push_back(gi);
    Recompile(lib);
    return true;
  }
  bool SpeakPage(const GlyphLibrary& lib, const std::string& name) {
    const int room = kSpellStackMax - (int)stack.spoken.size();
    if (room <= 0) return false;
    const GrimoireExpansion ex = ExpandWords(lib, grimoire, {name}, room);
    for (int gi : ex.spoken) stack.spoken.push_back(gi);
    if (ex.truncated) {
      note = "the stack is full: " + name + " was cut short";
      noteAge = 0.0f;
    } else if (ex.dropped > 0) {
      note = name + ": " + std::to_string(ex.dropped) + " word(s) no longer exist";
      noteAge = 0.0f;
    }
    Recompile(lib);
    return !ex.spoken.empty();
  }
  // `=` in magic mode: save the stack to the first empty page under an
  // auto-name. Returns the page's name, or "" when the grimoire is full or
  // nothing is spoken.
  std::string CaptureStack(const GlyphLibrary& lib) {
    if (stack.Empty()) return "";
    if ((int)grimoire.pages.size() >= lib.budgets.maxGrimoirePages) {
      note = "the grimoire is full";
      noteAge = 0.0f;
      return "";
    }
    GrimoirePage p;
    p.name = GrimoireAutoName(lib, stack.spoken);
    // Unique: a second capture of the same sentence gets a suffix.
    std::string base = p.name;
    for (int k = 2; grimoire.Find(p.name) >= 0 && k < 100; k++)
      p.name = base + "-" + std::to_string(k);
    for (int gi : stack.spoken)
      if (const GlyphDef* g = lib.At(gi)) p.words.push_back(g->id);
    if ((int)p.words.size() > lib.budgets.maxMacroWords)
      p.words.resize(lib.budgets.maxMacroWords);
    grimoire.pages.push_back(p);
    note = "saved as " + p.name;
    noteAge = 0.0f;
    return p.name;
  }
  void Clear(const GlyphLibrary& lib) {
    stack.Clear();
    Recompile(lib);
  }
};
