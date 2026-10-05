#pragma once
#include <string>
#include <vector>

#include "game/caster.h"

// THE PLAYER'S SPELLBOOK ON DISK: the grimoire pages they wrote and what each
// spell-bar key holds, kept across launches in a per-user JSON file.
//
// WHY ITS OWN FILE, NOT THE WORLD SAVE. The world save (F9 / F10, the PLYR
// section in game/persist.cpp) already carries the grimoire, but only for a
// world you explicitly saved and loaded; a plain close-and-relaunch started
// from an empty book. The spellbook is the player's, not the world's.
//
// WHERE IT LIVES: OUTSIDE EVERY CHECKOUT, ON PURPOSE. Default
// `%APPDATA%/sandvox/spellbook.json`; `SANDVOX_SPELLBOOK=<path>` overrides.
// Concurrent agent sessions run `git clean`, `git checkout --`, worktree
// switches and asset rewrites in the repo; a file under the repo -- even a
// gitignored one -- is one `git clean -fdx` from gone. Nothing in the repo
// may write the default path; tests must never touch it (CLAUDE.md).
//
// WHO USES IT: only the interactive game. Every harness (selftest, --shot,
// --frames and its scripted --shot-* scenes, lab scenes) leaves it alone, so
// a scripted page never lands in your book and your book never changes what a
// harness measures. `SANDVOX_NO_SPELLBOOK=1` turns it off for a session.
//
// EVERYTHING BY NAME (caster.h, DESIGN.md §8b): pages hold glyph and page
// NAMES, the bar holds a glyph name or a page name per slot. A name that no
// longer resolves keeps its page (the readout shows `?`) and empties its slot.
struct Spellbook {
  struct Slot {
    int slot = -1;
    std::string glyph;   // a glyph name, or
    std::string page;    // a page name (exactly one of the two is set)
  };
  std::vector<GrimoirePage> pages;
  std::vector<Slot> bar;
  bool hasBar = false;   // the file said what the bar holds (else: keep defaults)
};

// "" when no per-user directory can be found (the feature then stays off).
std::string SpellbookPath();
// True if the game should use the spellbook this session (not a harness, not
// disabled by SANDVOX_NO_SPELLBOOK).
bool SpellbookEnabledByEnv();

// Read `path`. A missing file is an empty book and returns true. A file that
// does not parse returns FALSE with `err` set, and the caller must not save
// over it this session (an unreadable book is still the player's book).
bool LoadSpellbook(const std::string& path, Spellbook& out, std::string& err);

// The caster's player-written pages and bar, as a book.
Spellbook SnapshotSpellbook(const GlyphLibrary& lib, const PlayerCaster& caster);
// The book as the exact text written to disk; equal text = nothing to save.
std::string SpellbookText(const Spellbook& b);
// Write atomically (temp file + rename). The first save of a session also
// copies the previous file to `<path>.bak`.
bool SaveSpellbook(const std::string& path, const std::string& text, std::string& err);

// BOOT: the book's pages become the grimoire's (a page already there by name
// is kept), and when the book has a bar it REPLACES the default bindings.
void ApplySpellbookAtBoot(const Spellbook& b, const GlyphLibrary& lib, PlayerCaster& caster);
// AFTER A WORLD LOAD (F10 / --load-world): the save's grimoire and bar stand,
// and the book fills in around them -- pages the save lacks are added, empty
// slots get the book's binding. Nothing the save holds is overwritten, and
// nothing in the book is lost by loading an older world.
void MergeSpellbook(const Spellbook& b, const GlyphLibrary& lib, PlayerCaster& caster);
