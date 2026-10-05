// spellbook.cpp — the player's grimoire + spell bar on disk. See spellbook.h.
#include "game/spellbook.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int kSpellbookVersion = 1;
}  // namespace

std::string SpellbookPath() {
  if (const char* p = std::getenv("SANDVOX_SPELLBOOK"); p && *p) return p;
  const char* base = std::getenv("APPDATA");
  if (!base || !*base) base = std::getenv("XDG_DATA_HOME");
  if (!base || !*base) return "";
  return (fs::path(base) / "sandvox" / "spellbook.json").string();
}

bool SpellbookEnabledByEnv() {
  const char* off = std::getenv("SANDVOX_NO_SPELLBOOK");
  return !(off && *off && *off != '0');
}

bool LoadSpellbook(const std::string& path, Spellbook& out, std::string& err) {
  out = Spellbook{};
  std::error_code ec;
  if (path.empty() || !fs::exists(path, ec)) return true;   // a first launch: an empty book
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    err = path + " is not valid JSON";
    return false;
  }
  if (j.value("version", 0) > kSpellbookVersion) {
    err = path + " was written by a newer build (version " +
          std::to_string(j.value("version", 0)) + ")";
    return false;
  }
  if (j.contains("pages") && j["pages"].is_array()) {
    for (const json& jp : j["pages"]) {
      if (!jp.is_object()) continue;
      GrimoirePage p;
      p.name = jp.value("name", std::string());
      if (p.name.empty()) continue;
      if (jp.contains("words") && jp["words"].is_array())
        for (const json& w : jp["words"])
          if (w.is_string()) p.words.push_back(w.get<std::string>());
      out.pages.push_back(std::move(p));
    }
  }
  if (j.contains("bar") && j["bar"].is_array()) {
    out.hasBar = true;
    for (const json& js : j["bar"]) {
      if (!js.is_object()) continue;
      Spellbook::Slot s;
      s.slot = js.value("slot", -1);
      s.glyph = js.value("glyph", std::string());
      s.page = js.value("page", std::string());
      if (s.slot < 0 || s.slot >= kGlyphSlots) continue;
      out.bar.push_back(std::move(s));
    }
  }
  return true;
}

Spellbook SnapshotSpellbook(const GlyphLibrary& lib, const PlayerCaster& caster) {
  Spellbook b;
  for (const GrimoirePage& p : caster.grimoire.pages)
    if (!p.readOnly) b.pages.push_back(p);   // authored starters are content, not the player's
  b.hasBar = true;
  for (int i = 0; i < kGlyphSlots; i++) {
    Spellbook::Slot s;
    s.slot = i;
    switch (caster.inventory.KindAt(i)) {
      case SlotKind::Page: s.page = caster.inventory.PageAt(i); break;
      case SlotKind::Glyph: {
        const int gi = caster.inventory.At(i);
        if (gi >= 0 && gi < (int)lib.glyphs.size()) s.glyph = lib.glyphs[gi].id;
        break;
      }
      case SlotKind::None: break;
    }
    if (!s.glyph.empty() || !s.page.empty()) b.bar.push_back(std::move(s));
  }
  return b;
}

std::string SpellbookText(const Spellbook& b) {
  json j;
  j["version"] = kSpellbookVersion;
  j["about"] = "sandvox spellbook: your grimoire pages and spell-bar keys. Written by the game; "
               "safe to edit while it is closed.";
  json pages = json::array();
  for (const GrimoirePage& p : b.pages) pages.push_back({{"name", p.name}, {"words", p.words}});
  j["pages"] = std::move(pages);
  if (b.hasBar) {
    json bar = json::array();
    for (const Spellbook::Slot& s : b.bar) {
      json e{{"slot", s.slot}};
      if (!s.page.empty()) e["page"] = s.page;
      else e["glyph"] = s.glyph;
      bar.push_back(std::move(e));
    }
    j["bar"] = std::move(bar);
  }
  return j.dump(2) + "\n";
}

bool SaveSpellbook(const std::string& path, const std::string& text, std::string& err) {
  static bool backedUp = false;
  std::error_code ec;
  const fs::path p(path);
  fs::create_directories(p.parent_path(), ec);
  if (!backedUp && fs::exists(p, ec)) {
    // One copy of what was there before this session first touched it: a
    // session that somehow empties the book cannot take the old one with it.
    fs::copy_file(p, fs::path(path + ".bak"), fs::copy_options::overwrite_existing, ec);
  }
  backedUp = true;
  const fs::path tmp(path + ".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      err = "cannot write " + tmp.string();
      return false;
    }
    f << text;
    if (!f.flush()) {
      err = "short write to " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, p, ec);   // replaces atomically (MoveFileEx REPLACE_EXISTING on MSVC)
  if (ec) {
    err = "cannot replace " + path + ": " + ec.message();
    return false;
  }
  return true;
}

namespace {
void AddMissingPages(const Spellbook& b, const GlyphLibrary& lib, PlayerCaster& caster) {
  for (const GrimoirePage& p : b.pages) {
    if (caster.grimoire.Find(p.name) >= 0) continue;
    if ((int)caster.grimoire.pages.size() >= lib.budgets.maxGrimoirePages) {
      std::fprintf(stderr, "spellbook: grimoire full (%d pages), page '%s' not loaded\n",
                   (int)lib.budgets.maxGrimoirePages, p.name.c_str());
      continue;
    }
    caster.grimoire.pages.push_back(GrimoirePage{p.name, p.words, false});
  }
}

void BindSlot(const Spellbook::Slot& s, const GlyphLibrary& lib, PlayerCaster& caster) {
  if (!s.page.empty()) {
    caster.inventory.BindPage(s.slot, s.page);
  } else if (!s.glyph.empty()) {
    // Bind refuses a glyph the player does not own (an unlearned demon name):
    // the slot stays empty, as a missing glyph's does.
    caster.inventory.Bind(s.slot, lib.Find(s.glyph));
  }
}
}  // namespace

void ApplySpellbookAtBoot(const Spellbook& b, const GlyphLibrary& lib, PlayerCaster& caster) {
  AddMissingPages(b, lib, caster);
  if (!b.hasBar) return;
  for (int i = 0; i < kGlyphSlots; i++) caster.inventory.Bind(i, -1);
  for (const Spellbook::Slot& s : b.bar) BindSlot(s, lib, caster);
}

void MergeSpellbook(const Spellbook& b, const GlyphLibrary& lib, PlayerCaster& caster) {
  AddMissingPages(b, lib, caster);
  for (const Spellbook::Slot& s : b.bar)
    if (caster.inventory.KindAt(s.slot) == SlotKind::None) BindSlot(s, lib, caster);
}
