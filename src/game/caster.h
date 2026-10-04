#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/contract.h"     // contract::Page: the player's contract pages (D5)
#include "game/demon_lore.h"   // demon::IsNameGlyph: which glyphs are names
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
  //
  // EXCEPT A DEMON'S NAME (verb `summon`, docs/PLAN_demons.md D1): a name is
  // knowledge you have to FIND (a book, a teacher -- dialogue `grant`), so the
  // placeholder never hands it out (game/demon_lore.h IsNameGlyph says what
  // a name is). `withNames` (game/demon.h DebugAllDemonNames:
  // SANDVOX_ALL_NAMES=1) is the debug switch that does; the dev panel's
  // "learn all demon names" (demon::GrantAllNames) is the other.
  // Name glyphs are appended after every other glyph, so excluding them moves
  // no binding in the first bank.
  void GrantAllAndBind(const GlyphLibrary& lib, bool withNames = false) {
    owned.clear();
    for (int i = 0; i < kGlyphSlots; i++) {
      bound[i] = -1;
      page[i].clear();
    }
    for (int i = 0; i < (int)lib.glyphs.size(); i++) {
      if (!withNames && demon::IsNameGlyph(lib, i)) continue;
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
  // Glyph indices AND page marks (spell.h, "A PAGE USED AS ONE GLYPH"): a page
  // named in the words is its body between two marks, `book` says which page
  // each opening mark names. `StackOf` carries both onto a stack.
  std::vector<int> spoken;
  PageBook book;
  // Per-word magnitude and timing, parallel to `spoken` (always the same
  // length): what `word@x!bounce+20` in the page said, resolved - the glyph's
  // default magnitude and on-hit timing where it said nothing.
  std::vector<int32_t> mags;
  std::vector<SpellTiming> timing;
  int dropped = 0;
  bool truncated = false;
  bool tooDeep = false;
  // The words in order with `?` for a dropped one, for the readout.
  std::vector<std::string> readout;
};

// An expansion as SERIALIZED words (`fire@2`, ...): what a page says with
// every nested page flattened, each word carrying its own properties.
std::vector<std::string> ExpansionWords(const GlyphLibrary& lib, const GrimoireExpansion& ex);
// ...and as the spoken stack it is, properties and all.
SpellStack StackOf(const GrimoireExpansion& ex);

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
// ONE PAGE SPOKEN WHOLE - a key, a hand, a readied page: its own words, not a
// call of it. The outermost page IS the spell rather than a glyph inside one,
// so its marks would wall it off from nothing (and its readout would read
// `<name>`); the pages it names inside itself keep theirs.
GrimoireExpansion ExpandPage(const GlyphLibrary& lib, const Grimoire& g,
                             const std::string& name, int maxWords);

// Can a page be saved under `name` and still be REACHED by expansion? False
// with `why` filled when the name contains the word syntax (`@` `!` `+`:
// expansion cuts a word there and looks for a glyph first, so `fire@x` speaks
// `fire`) or when the name, read as a word, IS a glyph.
bool GrimoireNameUsable(const GlyphLibrary& lib, const std::string& name, std::string& why);

// Would saving `name` = `words` make a page that contains itself, through any
// chain of pages? True with `why` filled: the save is refused (§12b).
bool GrimoireWouldCycle(const GlyphLibrary& lib, const Grimoire& g,
                        const std::string& name,
                        const std::vector<std::string>& words, std::string& why);

// WHAT SHAPE A PAGE HAS when it is used as one glyph inside another spell
// (spell.h, "A PAGE USED AS ONE GLYPH"): a CARRIER boxes what is in front of
// it like a delivery word; anything else is a VALUE of `sort`, with `inputs`
// open slots (`leftInputs` of them taking the items before it) and `outputs`
// items. `ok` false when the name is no page.
struct PageShape {
  bool ok = false;
  bool carrier = false;
  GlyphSort sort = GlyphSort::Effect;
  int inputs = 0, leftInputs = 0, outputs = 0;
};
PageShape GrimoirePageShape(const GlyphLibrary& lib, const Grimoire& g, const std::string& name);

// The page's auto-name from its expansion: `fire-trail-explosive-shotgun2`.
std::string GrimoireAutoName(const GlyphLibrary& lib, const std::vector<int>& spoken);

// ---- the player's casting state ------------------------------------------------
// Inventory + grimoire + the spell being spoken + the mana pool. One struct so
// main.cpp holds one thing rather than five.
struct PlayerCaster {
  GlyphInventory inventory;
  Grimoire grimoire;
  // THE CONTRACT PAGES (demons D5, game/contract.h): drafted in the contract
  // editor, presented to a contained demon in conversation. The player's own;
  // the stock pages are content (contract::GetContent). Saved by name and
  // text in the player's file ('CNTR', game/demon_talk.h).
  std::vector<contract::Page> contracts;
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
  // THE PAGE IS THE INTERFACE (docs/PLAN_spell_graph.md §0b). A number key
  // SELECTS the spell bound to a slot: the stack becomes that page's whole
  // expansion (or that one glyph), the HUD shows its readout and price, and
  // right-click casts it. The selection PERSISTS across casts, so a bound
  // spell fires repeatedly off one key; another key switches, Backspace
  // clears. Building the sentence is the grimoire page's job now, not the
  // number row's.
  //
  // `selected` is the slot the stack came from, for the HUD strip's highlight;
  // -1 when the stack was built some other way.
  int selected = -1;
  // The grimoire page READIED from the spellbook (ArmPage), "" when the stack
  // came from anywhere else. The character screen marks that page and casts it
  // on the limb you click (session.cpp, the inspector's cast).
  std::string armedPage;
  bool SelectSlot(const GlyphLibrary& lib, int slot) {
    stack.Clear();
    selected = -1;
    armedPage.clear();
    if (!SpeakSlot(lib, slot)) {
      Recompile(lib);
      return false;
    }
    selected = slot;
    return true;
  }
  // Put the glyph or page bound to a slot ONTO the stack, on top of whatever is
  // there. Bounded so a stuck key cannot grow the stack without limit (rule 2
  // applies to UI state too — an unbounded stack is an unbounded mana cost); a
  // page speaks as much of its expansion as fits and says so. The number row no
  // longer calls this one word at a time (see `SelectSlot`); it is the
  // primitive under it, and the `grimoire` gate's fixture.
  bool SpeakSlot(const GlyphLibrary& lib, int slot) {
    if (inventory.KindAt(slot) == SlotKind::Page) return SpeakPage(lib, inventory.PageAt(slot));
    const int gi = inventory.At(slot);
    if (gi < 0) return false;
    if (stack.Words() >= kSpellStackMax) return false;
    stack.spoken.push_back(gi);
    Recompile(lib);
    return true;
  }
  bool SpeakPage(const GlyphLibrary& lib, const std::string& name) {
    const int room = kSpellStackMax - stack.Words();
    if (room <= 0) return false;
    const GrimoireExpansion ex = ExpandPage(lib, grimoire, name, room);
    AppendStack(stack, StackOf(ex));
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
    // A page spoken inside the stack is saved as its NAME, so the new page
    // still uses it as one glyph (and follows it when it is edited).
    for (size_t k = 0; k < stack.spoken.size(); k++) {
      const int s = stack.spoken[k];
      if (SpokenIsPageOpen(s)) {
        const int pi = SpokenPageIndex(s);
        if (pi >= 0 && pi < (int)stack.book.names.size())
          p.words.push_back(stack.book.names[(size_t)pi]);
        for (int depth = 1; depth > 0 && k + 1 < stack.spoken.size();) {
          k++;
          if (SpokenIsPageOpen(stack.spoken[k])) depth++;
          else if (stack.spoken[k] == kSpokenPageClose) depth--;
        }
        continue;
      }
      if (const GlyphDef* g = lib.At(s))
        p.words.push_back(
            SerializeWord(*g, ClampMagnitude(*g, stack.MagAt(k)), stack.TimingAt(k)));
    }
    if ((int)p.words.size() > lib.budgets.maxMacroWords)
      p.words.resize(lib.budgets.maxMacroWords);
    grimoire.pages.push_back(p);
    note = "saved as " + p.name;
    noteAge = 0.0f;
    return p.name;
  }
  void Clear(const GlyphLibrary& lib) {
    stack.Clear();
    selected = -1;
    armedPage.clear();
    Recompile(lib);
  }
  // Click a page in the spellbook: the stack becomes that page, ready to be
  // cast on a limb from the portrait. False (and nothing readied) when the
  // page says nothing castable.
  bool ArmPage(const GlyphLibrary& lib, const std::string& name) {
    Clear(lib);
    if (!SpeakPage(lib, name) || compiled.Empty()) {
      Clear(lib);
      return false;
    }
    armedPage = name;
    return true;
  }

  // ---- SPELLS IN HAND (dual wielding, 2026-09-27) ---------------------------
  //
  // A spell is EQUIPPED into a hand the way an item is: Z opens the spell bar,
  // the number row / wheel moves along it (`selected`), and Q / E put the
  // selected slot's spell into the left / right hand. From then on that hand's
  // button (LMB right, RMB left — the item rule) casts it, as often as it is
  // pressed: the compile below is made once at equip and a cast does not
  // consume it. Mana is the only rate limit.
  //
  // The hand holds a SNAPSHOT of the slot, not a pointer to it: rebinding the
  // key afterwards does not change what is already in your hand, exactly as
  // moving an item on the hotbar does not change what you are holding.
  // `slot` is kept so a glyph reload (R), which re-indexes every glyph, can
  // re-speak it by name (RefreshHands).
  struct HandSpell {
    int slot = -1;          // the glyph slot it came from; -1 = no spell
    std::string name;       // the glyph's id or the page's name, at equip
    SpellStack stack;
    CastList compiled;
    bool Equipped() const { return slot >= 0 && !compiled.Empty(); }
  };
  HandSpell hand[2];        // indexed by HandIndex: 0 right, 1 left
  // The hand that cast last: a held beam follows ITS button and leaves from
  // it (the VM keeps one beam per caster).
  int beamHand = 0;

  // Speak `slot` into hand `h` (0 right, 1 left). False when the slot says
  // nothing castable; the hand is then left as it was.
  bool EquipHand(const GlyphLibrary& lib, int h, int slot) {
    if (h < 0 || h > 1) return false;
    PlayerCaster scratch;
    scratch.inventory = inventory;
    scratch.grimoire = grimoire;
    if (!scratch.SpeakSlot(lib, slot) || scratch.compiled.Empty()) return false;
    hand[h].slot = slot;
    if (inventory.KindAt(slot) == SlotKind::Page) {
      hand[h].name = inventory.PageAt(slot);
    } else {
      const GlyphDef* g = lib.At(inventory.At(slot));
      hand[h].name = g ? g->id : std::string("?");
    }
    hand[h].stack = scratch.stack;
    hand[h].compiled = scratch.compiled;
    return true;
  }
  void ClearHand(int h) {
    if (h >= 0 && h <= 1) hand[h] = HandSpell{};
  }
  void ClearHands() {
    ClearHand(0);
    ClearHand(1);
  }
  // After a glyph reload: re-speak each hand from its slot (indices moved),
  // dropping a hand whose slot no longer says anything.
  void RefreshHands(const GlyphLibrary& lib) {
    for (int h = 0; h < 2; h++) {
      const int slot = hand[h].slot;
      if (slot < 0) continue;
      if (!EquipHand(lib, h, slot)) ClearHand(h);
    }
  }
};
