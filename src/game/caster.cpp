#include "game/caster.h"

#include <cstdio>

// The grimoire (docs/PLAN_magic_grammar.md §12b): macros as saved word lists
// that expand inline. Everything here is by NAME, and every expansion is
// bounded by depth and by the stack (rule 2).

const GrimoirePage* FindPage(const GlyphLibrary& lib, const Grimoire& g,
                             const std::string& name, GrimoirePage& scratch) {
  const int pi = g.Find(name);
  if (pi >= 0) return &g.pages[pi];
  // The authored starters are pages too: read-only, by the conjoined id.
  for (const ConjoinedGlyph& c : lib.conjoined) {
    if (c.id != name) continue;
    scratch.name = c.id;
    scratch.readOnly = true;
    scratch.words.clear();
    for (int gi : c.glyphs)
      if (const GlyphDef* d = lib.At(gi)) scratch.words.push_back(d->id);
    return &scratch;
  }
  return nullptr;
}

GrimoireExpansion ExpandWords(const GlyphLibrary& lib, const Grimoire& g,
                              const std::vector<std::string>& words, int maxWords) {
  GrimoireExpansion out;
  if (maxWords > kSpellStackMax) maxWords = kSpellStackMax;
  if (maxWords <= 0) {
    out.truncated = !words.empty();
    return out;
  }
  // ONE EXPANSION FOR EVERY SPEAKER (spell.h, SpeakWordsOnto): a page named
  // here is spoken as its body between page marks, so the parser closes it
  // into ONE item - a page used as a glyph - rather than pasting its words
  // into the sentence around it.
  SpellStack st;
  PageExpansionReport rep;
  const PageLookup find = [&](const std::string& name) -> const std::vector<std::string>* {
    static thread_local GrimoirePage scratch;
    const GrimoirePage* p = FindPage(lib, g, name, scratch);
    return p ? &p->words : nullptr;
  };
  SpeakWordsOnto(lib, find, words, maxWords, st, rep);
  out.spoken = st.spoken;
  out.mags.resize(st.spoken.size());
  out.timing.resize(st.spoken.size());
  for (size_t k = 0; k < st.spoken.size(); k++) {
    const GlyphDef* d = lib.At(st.spoken[k]);
    const int32_t m = st.MagAt(k);
    out.mags[k] = m != kMagUnset ? m : (d ? d->magDefault : kMagOne);
    out.timing[k] = st.TimingAt(k);
  }
  out.book = st.book;
  out.dropped = rep.dropped;
  out.truncated = rep.truncated;
  out.tooDeep = rep.tooDeep;
  out.readout = rep.readout;
  return out;
}

GrimoireExpansion ExpandPage(const GlyphLibrary& lib, const Grimoire& g,
                             const std::string& name, int maxWords) {
  GrimoireExpansion ex = ExpandWords(lib, g, {name}, maxWords);
  if (ex.spoken.size() >= 2 && SpokenIsPageOpen(ex.spoken.front()) &&
      ex.spoken.back() == kSpokenPageClose) {
    ex.spoken.erase(ex.spoken.end() - 1);
    ex.spoken.erase(ex.spoken.begin());
    ex.mags.erase(ex.mags.end() - 1);
    ex.mags.erase(ex.mags.begin());
    ex.timing.erase(ex.timing.end() - 1);
    ex.timing.erase(ex.timing.begin());
  }
  return ex;
}

PageShape GrimoirePageShape(const GlyphLibrary& lib, const Grimoire& g,
                           const std::string& name) {
  PageShape out;
  const GrimoireExpansion ex = ExpandWords(lib, g, {name}, kSpellStackMax);
  const SpellTree t = ParseSpell(lib, StackOf(ex));
  if (t.Empty() || t.clauses[0].root < 0) return out;
  const std::vector<int>& items = t.nodes[(size_t)t.clauses[0].root].items;
  if (items.size() != 1 || t.nodes[(size_t)items[0]].call != name) return out;
  const SpellNode& c = t.nodes[(size_t)items[0]];
  out.ok = true;
  out.carrier = c.box;
  out.sort = c.box ? GlyphSort::Delivery : NodeSort(lib, t, items[0]);
  out.inputs = (int)c.holes.size();
  for (int h : c.holes) out.leftInputs += (h & 1) ? 0 : 1;
  out.outputs = c.box ? 1 : (int)c.items.size();
  return out;
}

namespace {

bool Reaches(const GlyphLibrary& lib, const Grimoire& g, const std::vector<std::string>& words,
             const std::string& target, int depth, std::string& path) {
  if (depth > lib.budgets.maxMacroDepth + 1) return false;
  for (const std::string& w : words) {
    if (lib.FindWord(w) >= 0) continue;
    if (w == target) {
      path = w;
      return true;
    }
    GrimoirePage scratch;
    const GrimoirePage* p = FindPage(lib, g, w, scratch);
    if (!p) continue;
    if (Reaches(lib, g, p->words, target, depth + 1, path)) {
      path = w + " -> " + path;
      return true;
    }
  }
  return false;
}

}  // namespace

bool GrimoireNameUsable(const GlyphLibrary& lib, const std::string& name, std::string& why) {
  if (name.find_first_of("@!+") != std::string::npos) {
    why = "a page name cannot contain @ ! or +";
    return false;
  }
  // The SAME lookup Expand and Reaches make (FindWord), so a name the save
  // accepts is a name expansion reaches as a page.
  if (lib.FindWord(name) >= 0) {
    why = "that name belongs to the library";
    return false;
  }
  return true;
}

bool GrimoireWouldCycle(const GlyphLibrary& lib, const Grimoire& g, const std::string& name,
                        const std::vector<std::string>& words, std::string& why) {
  std::string path;
  if (Reaches(lib, g, words, name, 0, path)) {
    why = name + " would contain itself: " + name + " -> " + path;
    return true;
  }
  return false;
}

std::string GrimoireAutoName(const GlyphLibrary& lib, const std::vector<int>& spoken) {
  std::string s;
  int lastG = -1, run = 0;
  auto flush = [&]() {
    if (lastG < 0) return;
    const GlyphDef* d = lib.At(lastG);
    if (!s.empty()) s += "-";
    s += d ? d->id : "?";
    if (run > 1) s += std::to_string(run);
  };
  for (int gi : spoken) {
    if (SpokenIsMark(gi)) continue;   // a page's marks are not words
    if (gi == lastG) {
      run++;
      continue;
    }
    flush();
    lastG = gi;
    run = 1;
  }
  flush();
  if (s.empty()) s = "page";
  // Names stay ASCII-safe and short enough to read on a key.
  if (s.size() > 48) s.resize(48);
  return s;
}

std::vector<std::string> ExpansionWords(const GlyphLibrary& lib, const GrimoireExpansion& ex) {
  std::vector<std::string> out;
  for (size_t k = 0; k < ex.spoken.size(); k++) {
    const GlyphDef* g = lib.At(ex.spoken[k]);
    if (!g) continue;
    out.push_back(SerializeWord(*g, k < ex.mags.size() ? ex.mags[k] : g->magDefault,
                                k < ex.timing.size() ? ex.timing[k] : SpellTiming{}));
  }
  return out;
}

SpellStack StackOf(const GrimoireExpansion& ex) {
  SpellStack st;
  st.book = ex.book;
  for (size_t k = 0; k < ex.spoken.size(); k++)
    st.Push(ex.spoken[k], k < ex.mags.size() ? ex.mags[k] : kMagUnset,
            k < ex.timing.size() ? ex.timing[k] : SpellTiming{});
  return st;
}
