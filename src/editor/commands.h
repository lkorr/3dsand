// commands.h — EDITOR COMMANDS: every authoring action as a named, undoable
// command (docs/PLAN_world_editor.md §2.3 / P5; DESIGN.md §16.P5).
//
// A command is { name, args (JSON) }. A registered handler applies it and
// returns its INVERSE -- the command(s) that put back exactly what it
// changed. Three consumers, one code path:
//
//   * the in-game editor (F8, ui/editor_ui.cpp) calls nothing else: every
//     click that changes a file is a command, so every click undoes;
//   * `--edit-script <file.jsonl>` applies a list of them headlessly and
//     saves -- how an agent builds, and the file it leaves is a readable
//     record of what it did (RunScript below);
//   * the session JOURNAL (build/editor/session_<time>.jsonl) records what a
//     person did, in the same format, so a session can be replayed.
//
// THE FILES ARE THE TRUTH (§2.3). Commands write authored files -- the map's
// refs/<group>.json (immediately, through refs::Place/Move/...), a
// structure's .vox/.struct.json (on struct.save) -- and the live world is
// re-derived from them (the refs store re-applies; structures::Reload
// re-stamps). Undo is a data diff, never a GPU readback.
//
// UNDO GUARANTEE (the editor-commands gate): apply a script, undo every
// step, and every file it touched is BYTE-IDENTICAL to where it started --
// a group file the script created is removed again; redo every step and the
// files are byte-identical to the applied state. That requires the refs
// files to be in refs::WriteGroup's canonical form (they are unless someone
// hand-formats one; the first edit canonicalises it).
//
// COORDINATES. Ref commands speak WORLD voxels. vox.* and slot.* commands
// speak the HOUSE FRAME of the structure being edited (struct_edit.h: origin-
// relative, y = 0 is the floor, +Z is the front) -- the frame a save cannot
// move -- or world voxels with "frame": "world" (converted through the
// instance the structure was opened from, so a turned house edits right).

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "editor/struct_edit.h"
#include "world/refs.h"

namespace editor {

// The copy/paste buffer: a box of materials (0 = air), house frame axes.
struct Clipboard {
  bool valid = false;
  IVec3 size{};

  std::vector<uint16_t> cells;   // (z * size.y + y) * size.x + x
  std::string from;              // the structure it was copied from
  uint16_t At(int x, int y, int z) const { return cells[((size_t)z * size.y + y) * size.x + x]; }
};

// What a command carries beyond its JSON args: an inverse's bulk data (a
// 500,000-cell fill's previous materials would be ~50 MB as JSON).
struct Payload {
  std::vector<int32_t> cells;   // x, y, z, mat quads (house frame)
  std::string a, b;             // file bytes (an undone save)
  Json json;                    // a slot list snapshot
  std::shared_ptr<refs::Ref> ref;
  uint64_t u = 0;
  // vox.paste as recorded: the clipboard it pasted (a redo must paste the
  // same voxels, whatever has been copied since).
  std::shared_ptr<Clipboard> clip;
};

struct Command {
  std::string name;
  Json args = Json::object();
  std::shared_ptr<Payload> payload;
};


// What the handlers touch. Owned by a Session.
struct Context {
  refs::RefStore* refs = nullptr;   // null: ref.* refuse ("no map loaded")
  std::string assetDir;
  std::vector<std::string> mats;    // material names by id (LoadMaterialNames)
  // Every structure buffer this session has opened, by asset name. A buffer
  // stays loaded (with its unsaved edits) until saved-and-closed or reverted,
  // so an undo entry for a house that is not the open one still applies.
  std::map<std::string, std::unique_ptr<StructEdit>> structs;
  std::string active;     // the open structure ("" = none)
  std::string instance;   // the ref id it was opened through ("" = by asset name)
  Clipboard clip;
  // A structure asset was written (save, or an undone save): the in-game
  // editor re-stamps every instance (structures::Reload). Optional.
  std::function<void(const std::string& asset)> onAssetSaved;
  // Load (once) and return a buffer. Null + err.
  StructEdit* Load(const std::string& asset, std::string& err);
  StructEdit* Active() { return active.empty() ? nullptr : Load(active, scratchErr); }
  std::string scratchErr;
};

// ---- the registry -----------------------------------------------------------------
using Handler = std::function<bool(Context&, const Command&, std::vector<Command>& inverse,
                                   std::string& err)>;
struct CommandDef {
  std::string name;
  std::string args;      // the argument shape, for --edit-commands and the guide
  std::string help;      // one line
  Handler apply;
  // False: session state, not a file edit (struct.open, vox.copy): journaled,
  // never an undo step.
  bool undoable = true;
  bool internal = false; // "_"-prefixed: inverses only, not for scripts
};
class Registry {
 public:
  void Register(CommandDef d);
  const CommandDef* Find(const std::string& name) const;
  std::vector<const CommandDef*> All() const;   // name order
 private:
  std::map<std::string, CommandDef> defs_;
};
// The process registry, with every built-in command (idempotent).
Registry& Commands();
// "name  args  -- help" per public command, one per line.
std::string CommandListText();

// ---- a session: apply, undo / redo, journal ---------------------------------------
class Session {
 public:
  static constexpr size_t kMaxUndo = 256;
  Context ctx;
  // How many steps undo keeps (the in-game editor: 256). RunScript lifts it
  // for the length of a script so a failed line can roll the WHOLE run back.
  size_t maxUndo = kMaxUndo;

  // Apply one command. Undoable + changed something -> one undo step (or
  // part of the open group); the redo stack is cleared. Journaled either
  // way. False + err (nothing changed) on refusal.
  bool Run(const Command& c, std::string* err = nullptr);
  bool Run(const std::string& name, Json args, std::string* err = nullptr) {
    return Run(Command{name, std::move(args), nullptr}, err);
  }
  // Group everything Run between Begin and End into ONE undo step (a brush
  // drag, a multi-cell action). Nests; the outermost End closes it.
  void BeginGroup(const std::string& label);
  void EndGroup();
  // An edit made OUTSIDE the command layer but already applied (a P6/P7
  // inspector field that writes the ref itself): recorded as an undo step
  // whose redo is `forward` and undo is `inverse`; `journal` are the public
  // commands a replay would run instead.
  void Record(const std::string& label, std::vector<Command> forward, std::vector<Command> inverse,
              const std::vector<Command>& journal);
  bool Undo(std::string* msg = nullptr);
  bool Redo(std::string* msg = nullptr);
  size_t UndoDepth() const { return undo_.size(); }
  size_t RedoDepth() const { return redo_.size(); }
  std::string UndoLabel() const { return undo_.empty() ? "" : undo_.back().label; }
  std::string RedoLabel() const { return redo_.empty() ? "" : redo_.back().label; }
  void ClearHistory();
  // Bumped on every change (apply, undo, redo): the UI's cache key.
  uint64_t Revision() const { return rev_; }
  // Any structure buffer with unsaved edits (names).
  std::vector<std::string> UnsavedStructures() const;

  // Journal every command (and undo/redo) to `path` as JSONL. "" = off.
  bool OpenJournal(const std::string& path, std::string* err = nullptr);
  const std::string& JournalPath() const { return journalPath_; }
  void JournalNote(const Json& line);

 private:
  struct Entry {
    std::string label;
    std::vector<Command> forward;
    std::vector<Command> inverse;   // in APPLY order; undo runs it backwards
  };
  bool ApplyOne(const Command& c, std::vector<Command>& inv, std::string& err);
  void Push(Entry e);
  void Journal(const Command& c);
  std::vector<Entry> undo_, redo_;
  int groupDepth_ = 0;
  Entry group_;
  uint64_t rev_ = 0;
  std::string journalPath_;
};

// ---- --edit-script ----------------------------------------------------------------
// One command per line: {"cmd": "vox.box_fill", "args": {...}} or the args
// inline ({"cmd": "vox.box_fill", "min": [..], ...}). Blank lines and lines
// starting with # or // are skipped. "undo" / "redo" are commands too.
// ALL OR NOTHING: on the first refused line every step this run applied is
// undone (the files are back to where they started) and the error names the
// file, the line, the command and the field. On success every structure
// with unsaved edits is saved (struct.save) unless `save` is false.
struct ScriptResult {
  int lines = 0, applied = 0;
  std::vector<std::string> saved;   // assets written
  std::string error;                // "" = success
};
bool RunScript(Session& s, const std::string& path, ScriptResult& out, bool save = true);

}  // namespace editor
