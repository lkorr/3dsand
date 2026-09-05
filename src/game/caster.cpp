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

namespace {

void Expand(const GlyphLibrary& lib, const Grimoire& g, const std::vector<std::string>& words,
            int depth, std::vector<std::string>& trail, GrimoireExpansion& out, int maxWords) {
  for (const std::string& w : words) {
    if ((int)out.spoken.size() >= maxWords) {
      out.truncated = true;
      return;
    }
    const int gi = lib.Find(w);
    if (gi >= 0) {
      out.spoken.push_back(gi);
      out.readout.push_back(w);
      continue;
    }
    GrimoirePage scratch;
    const GrimoirePage* p = FindPage(lib, g, w, scratch);
    if (!p) {
      // Content removed or renamed: the word drops with a log line and the
      // readout shows `?`; the page is not deleted (DESIGN §8b).
      out.dropped++;
      out.readout.push_back("?");
      std::fprintf(stderr, "grimoire: \"%s\" names nothing that exists; dropped\n", w.c_str());
      continue;
    }
    bool cycle = false;
    for (const std::string& t : trail) cycle = cycle || t == w;
    if (cycle || depth + 1 > lib.budgets.maxMacroDepth) {
      // A cycle a save check missed (content edited under it), or a nest past
      // the cap: the word expands to nothing rather than to everything.
      out.tooDeep = true;
      out.readout.push_back("?");
      continue;
    }
    trail.push_back(w);
    Expand(lib, g, p->words, depth + 1, trail, out, maxWords);
    trail.pop_back();
    if (out.truncated) return;
  }
}

}  // namespace

GrimoireExpansion ExpandWords(const GlyphLibrary& lib, const Grimoire& g,
                              const std::vector<std::string>& words, int maxWords) {
  GrimoireExpansion out;
  if (maxWords > kSpellStackMax) maxWords = kSpellStackMax;
  if (maxWords <= 0) {
    out.truncated = !words.empty();
    return out;
  }
  std::vector<std::string> trail;
  Expand(lib, g, words, 0, trail, out, maxWords);
  return out;
}

namespace {

bool Reaches(const GlyphLibrary& lib, const Grimoire& g, const std::vector<std::string>& words,
             const std::string& target, int depth, std::string& path) {
  if (depth > lib.budgets.maxMacroDepth + 1) return false;
  for (const std::string& w : words) {
    if (lib.Find(w) >= 0) continue;
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
