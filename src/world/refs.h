// refs.h — REFERENCES: authored, placed things with stable ids
// (docs/PLAN_world_editor.md §2.1, package P1; DESIGN.md §16).
//
// A REFERENCE is one authored, placed thing — a house, a villager, a door, a
// chest, a well — with an id that never changes. It is the unit everything
// else names: a schedule names a bed by ref id, a dialogue names a speaker, a
// save names the door it remembers as open.
//
//   assets/worldmap/<map>/refs/<group>.json      (group = a place: "harrowby")
//   {
//     "group": "harrowby",
//     "refs": [
//       { "id": "harrowby/osric", "kind": "npc", "base": "human", "pos": [x, y, z], "yaw": 180, "props": { "name": "Osric" } },
//       ...
//     ]
//   }
//
// THE FOUR FACTS THIS FILE IS BUILT AROUND
//
//   1. THE GROUP FILE IS THE TRUTH, and it is a file a person edits. One ref
//      per line, stable key order (id, kind, base, pos, yaw, props, then any
//      field this build does not know, in file order), unknown props and
//      unknown fields KEPT and written back verbatim. WriteGroup(ParseGroup(x))
//      is byte-identical to x for any x WriteGroup produced (`refs-roundtrip`).
//   2. KINDS ARE A REGISTRY, NOT A SWITCH. `kind` is an open string resolved at
//      load against RefKindRegistry. A system owns the kinds it registers
//      (P4 structure, P6 door/container/bed, P7 npc/waynode; P1 ships `marker`
//      and a temporary `npc`). An unknown kind is a load WARNING and the ref is
//      kept verbatim and inert — never dropped, never an error.
//   3. AUTHORED vs SAVED. The group files are read-only at play time. A save
//      stores only DELTAS keyed by ref id ("door open", "npc spawned") as one
//      'REFS' record per ref in the region bucket of the ref's authored pos
//      (sim/worldio.h EntityScope::Region). A ref with no delta is exactly as
//      authored. A delta whose id no longer exists is dropped with a warning.
//   4. ACTIVATION FOLLOWS THE WINDOW, IN ID ORDER. A ref is ACTIVE while its
//      chunk is inside the residency window (by kActivateMarginChunks on every
//      face to switch on; wholly outside to switch off — the MobParking
//      hysteresis). Every activation pass walks refs in id order, so what
//      happens is a pure function of (window, refs, deltas). Voxel writes a
//      kind makes go through the tick's OpBatch (CLAUDE.md rule 3).
//
// WHAT LATER PACKAGES DO WITH THIS FILE (the stable API)
//
//   Register a kind:   refs::Kinds().Register(RefKind{...});   — from a
//                      function you add to refs::RegisterAllKinds()
//                      (refs_kinds.cpp), which main and the gates call.
//   Read refs:         store.Find(id), store.All() (sorted by id),
//                      store.ChildrenOf(id), store.InRegion(rc).
//   Remember state:    store.SetDelta(id, bytes) / store.Delta(id) /
//                      store.ClearDelta(id) — bytes are the kind's own.
//   Author (P5 wraps these as undoable commands; each writes the group file):
//                      refs::Place / Move / SetProp / SetField / Delete.
//   Use verb:          RefKind::usePrompt + RefKind::onUse (refs_use.h runs
//                      it from TickInput::useRef inside TickAuthority).
//
// NOTHING HERE IS HASHED SIM STATE. Refs are CPU gameplay/authoring state; a
// kind that changes voxels does it with ops, which are.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "math3d.h"

class MobSystem;
class World;
class ChunkStore;
class Physics;
class DebrisSystem;
struct ItemLibrary;
struct MaterialDef;
struct OpBatch;
struct PlayerSession;
namespace dialogue {
class Store;
}

namespace refs {

using Json = nlohmann::ordered_json;

class RefStore;

// ---- one reference -------------------------------------------------------
struct Ref {
  std::string id;     // "group/name" (children: "group/name/slot"); lowercase
  std::string kind;   // registry key; unknown kinds are kept, inert
  std::string base;   // what it is an instance OF (mob def, structure asset); "" = none
  IVec3 pos{};        // world voxels; the owning chunk/region derive from it
  int yaw = 0;        // degrees, heading 0 = +Z, 90 = +X (CLAUDE.md conventions)
  Json props = Json::object();  // free object, validated by the kind; unknown keys kept
  Json extra = Json::object();  // unknown TOP-LEVEL fields of this ref, kept in file order
  // ---- derived at load, never written ----
  std::string group;  // the group file it lives in
  uint32_t hash = 0;  // RefHash(id): what TickInput::useRef carries
};

// FNV-1a 32 of the id. Collisions are detected at load (a warning names both).
uint32_t RefHash(const std::string& id);
// "harrowby/smithy/door" -> "harrowby". "" when the id has no '/'.
std::string GroupOfId(const std::string& id);
// Ids are `group/name[/slot...]`, each part [a-z0-9_-]+. False + why.
bool ValidId(const std::string& id, std::string* why = nullptr);

// ---- one group file ------------------------------------------------------
struct RefGroupFile {
  std::string group;               // "group" field
  Json extra = Json::object();     // unknown top-level fields ("about", ...), file order
  std::vector<Ref> refs;           // FILE order (the author's), not id order
};

// Parse group JSON text. `label` names the file in warnings. Problems that do
// not stop the parse (a bad field on one ref) are pushed to `warn` as
// "<label>: <id>: <field>: <what>" and that ref is skipped; a malformed file
// returns false.
bool ParseGroup(const std::string& text, const std::string& label,
                RefGroupFile& out, std::vector<std::string>& warn);
// The canonical text: one ref per line, stable key order. See fact 1 above.
std::string WriteGroup(const RefGroupFile& g);
// The one-line text of one ref, as WriteGroup writes it (no indent, no comma):
// also the "is this ref's authored content unchanged" key used by Reload.
std::string RefLine(const Ref& r);
// Compact JSON with ", " / ": " separators (the props style in the files).
std::string JsonInline(const Json& j);

// ---- kinds ---------------------------------------------------------------

// Why a ref stops being active. A hook must handle each:
//   WindowLeft — its chunk left the window. The world owns what it made now
//                (an NPC wanders and is parked by MobSystem, not by this).
//   Edited     — its authored line changed (inspector, R reload, P5 command):
//                undo what activation did so it can re-activate from the file.
//   Deleted    — the ref is gone from its file: undo, and forget the delta.
//   Reset      — a save is being LOADED: every system is being reset by its
//                own save section; drop handles, touch nothing in the world.
enum class RefEvent : uint8_t { WindowLeft, Edited, Deleted, Reset };
const char* RefEventName(RefEvent e);

// What a hook may touch. Any pointer may be null (a headless tool, a gate
// that only exercises the store); a hook that needs one and finds it null
// returns Retry / does nothing.
struct RefCtx {
  RefStore* refs = nullptr;
  MobSystem* mobs = nullptr;
  World* world = nullptr;
  ChunkStore* store = nullptr;
  OpBatch* ops = nullptr;   // THIS tick's batch: every voxel write goes here (rule 3)
  uint32_t tick = 0;
  // P6 (doors): the leaf becomes a hinged body while open, so a door needs the
  // physics and the debris system that draws it, and the material table to
  // tell a solid leaf cell from a wisp of smoke. Containers name items.
  Physics* phys = nullptr;
  DebrisSystem* debris = nullptr;
  const std::vector<MaterialDef>* mats = nullptr;
  const ItemLibrary* items = nullptr;
  // P7 (NPC residents): the conversations (the use verb on an npc begins its
  // `dialogue`), and who is talking to whom this tick -- one entry per
  // session with a conversation open: the speaker's mob id and the talking
  // player's eye, so a villager can turn to face them.
  dialogue::Store* talk = nullptr;
  struct Talker {
    uint64_t mobId = 0;
    Vec3 eye{};
  };
  const std::vector<Talker>* talkers = nullptr;
};

// One use of one ref by one player (the §2.5 verb).
struct RefUse {
  uint32_t player = 0;                 // session index in the tick's span
  PlayerSession* session = nullptr;    // the user; null in a store-only gate
  std::string message;                 // what the HUD says back (kitMessage)
  // A container was opened: the ref id the using player's loot panel should
  // show (TickRefs hands it to the window's UI; "" = none).
  std::string openContainer;
};

enum class Activation : uint8_t { Done, Retry };

struct RefKind {
  std::string name;
  // Validate one ref's fields/props (load and every edit). Push problems as
  // "<field>: <what>"; the store prefixes file and id. Optional.
  std::function<void(const Ref&, std::vector<std::string>& problems)> validate;
  // The ref came into the window (or was re-applied after an edit). Retry =
  // "not now" (ground unknown, crowd full): asked again next Update. Optional.
  std::function<Activation(RefCtx&, const Ref&)> activate;
  // See RefEvent. Called only for refs whose activate returned Done. Optional.
  std::function<void(RefCtx&, const Ref&, RefEvent)> deactivate;
  // Before a SAVE, for every ACTIVE ref of this kind: bring its delta up to
  // date (store.SetDelta). For kinds whose state changes continuously
  // (a chest's contents); kinds that SetDelta at the moment of change need
  // none. Optional.
  std::function<void(RefCtx&, const Ref&)> flushDelta;
  // ---- the use verb (all optional; no usePrompt = not usable) ----
  // What the HUD offers: "Open door", "Talk to Osric". "" = not usable now.
  std::function<std::string(RefCtx&, const Ref&)> usePrompt;
  // Where the crosshair must point: the thing's centre in world voxels.
  // False = not usable now (an NPC that is not live). Default: pos + 0.5.
  std::function<bool(RefCtx&, const Ref&, Vec3& at)> usePoint;
  // Do it. Runs INSIDE TickAuthority from a TickInput, never from the frame.
  std::function<void(RefCtx&, const Ref&, RefUse&)> onUse;
  float useRadius = 6.0f;   // how near the look ray must pass usePoint, voxels
  // Bumped when the kind's delta bytes change meaning; carried in the record.
  uint32_t deltaVersion = 1;
  // ONCE PER TICK, per KIND (not per ref), after activation and the uses, for
  // a kind with moving parts (a door swinging). Must cost ~nothing when
  // nothing of its kind is moving (rule 2). Optional.
  std::function<void(RefCtx&)> tick;
};

class RefKindRegistry {
 public:
  // Adds, or REPLACES a kind of the same name (P7's npc replaces P1's).
  void Register(RefKind k);
  const RefKind* Find(const std::string& name) const;
  std::vector<std::string> Names() const;   // sorted
 private:
  std::map<std::string, RefKind> kinds_;
};
// The process registry. Kinds are behaviour, like the reaction table: one set
// per process, not per world.
RefKindRegistry& Kinds();
// Register every kind this build knows (idempotent). THE ONE PLACE later
// packages add a line: refs_kinds.cpp.
void RegisterAllKinds();

// ---- the store -----------------------------------------------------------

struct RefDelta {
  std::string kind;        // the kind that wrote it (a kind change drops it)
  uint32_t version = 1;    // RefKind::deltaVersion at write
  std::vector<uint8_t> bytes;
};

// One activation-state change, for gates and the References page log.
struct RefActivity {
  uint32_t tick = 0;
  std::string id;
  bool on = false;
  RefEvent why = RefEvent::WindowLeft;   // meaningful when !on
};

// One use of a ref (the §2.5 verb), as the gates and the References page
// read it back. Refusals are recorded too: a use that did nothing says why.
struct UseRecord {
  uint32_t tick = 0;
  uint32_t player = 0;
  std::string id;        // "" when the hash named no ref
  bool used = false;     // onUse ran
  std::string message;   // what onUse said, or why it was refused
};

class RefStore {
 public:
  RefStore() = default;
  RefStore(const RefStore&) = delete;
  RefStore& operator=(const RefStore&) = delete;
  // Tells anything holding this store's address between ticks (the P7
  // catch-up placer) that it is gone.
  ~RefStore();
  // Switch on at this margin inside the window; switch off wholly outside.
  static constexpr int kActivateMarginChunks = 1;

  // ---- loading -------------------------------------------------------------
  // Read every <assetDir>/worldmap/<map>/refs/*.json (sorted by file name).
  // Replaces the content; deltas and activation state are dropped (call on a
  // new world). Missing dir = zero refs, no warning. Always succeeds; every
  // problem is a warning (Warnings()).
  void LoadMap(const std::string& assetDir, const std::string& mapName);
  // Re-read the same directory (R in game). Refs whose line changed are
  // re-applied (Edited), vanished ones Deleted, new ones activate on the next
  // Update. `ctx` reaches the hooks.
  void Reload(RefCtx& ctx);
  const std::string& Dir() const { return dir_; }
  const std::string& MapName() const { return map_; }

  // ---- reading -------------------------------------------------------------
  const Ref* Find(const std::string& id) const;
  const Ref* FindByHash(uint32_t h) const;
  const std::map<std::string, Ref>& All() const { return refs_; }   // id order
  // Direct children: "a/b" -> "a/b/x" (not "a/b/x/y").
  std::vector<const Ref*> ChildrenOf(const std::string& id) const;
  // Refs whose authored pos is in region `rc` (ChunkStore regions), id order.
  std::vector<const Ref*> InRegion(IVec3 rc) const;
  const std::vector<std::string>& Groups() const { return groupOrder_; }
  const RefGroupFile* Group(const std::string& g) const;
  std::string GroupPath(const std::string& g) const;
  // Load + edit problems, newest last, each naming file, id and field.
  const std::vector<std::string>& Warnings() const { return warnings_; }
  void Warn(const std::string& w);
  // Bumped on every content change (load, reload, edit): the UI's cache key.
  uint64_t Revision() const { return revision_; }

  // ---- activation ----------------------------------------------------------
  // Once per tick with the window's origin in chunks. Cheap when nothing
  // changed: returns after one compare unless the window moved, an edit is
  // pending or a Retry is waiting.
  void Update(RefCtx& ctx, IVec3 windowOriginChunks);
  // Deactivate every active ref (id order) with `why`.
  void DeactivateAll(RefCtx& ctx, RefEvent why);
  bool IsActive(const std::string& id) const { return active_.count(id) != 0; }
  size_t ActiveCount() const { return active_.size(); }
  const std::vector<RefActivity>& Log() const { return log_; }
  void ClearLog() { log_.clear(); }
  const std::vector<UseRecord>& Uses() const { return uses_; }
  void NoteUse(UseRecord u);
  void ClearUses() { uses_.clear(); }
  // A DEV USE: the References page's "test open/close" button. Queued here and
  // run by the next TickRefs inside the tick (so its ops land in op order like
  // a player's), with no reach check and no player (RefUse::session null).
  void QueueDevUse(const std::string& id) { devUses_.push_back(id); }
  std::vector<std::string> TakeDevUses() {
    std::vector<std::string> v;
    v.swap(devUses_);
    return v;
  }

  // ---- deltas (the save's half) ---------------------------------------------
  // Bind the ChunkStore whose region buckets hold dormant 'REFS' records.
  // A ref's bucket is pulled into memory before its delta is first read or
  // written, so a delta is never held twice.
  void BindChunkStore(ChunkStore* s) { chunkStore_ = s; }
  const RefDelta* Delta(const std::string& id);
  void SetDelta(const std::string& id, const std::string& kind, uint32_t version,
                std::vector<uint8_t> bytes);
  void ClearDelta(const std::string& id);
  size_t DeltaCount() const { return deltas_.size(); }

  // The 'REFS' record: u32 fmt, id, kind, u32 kindVersion, bytes. Public for
  // the save section and the gates.
  static constexpr uint32_t kSaveVersion = 1;
  static void EncodeDelta(const std::string& id, const RefDelta& d,
                          std::vector<uint8_t>& out);
  static bool DecodeDelta(const uint8_t* p, size_t n, std::string& id, RefDelta& d);
  // Apply one decoded record (a load, or a pulled bucket). False = the id is
  // not in the store: the delta is DROPPED and a warning names it.
  bool AcceptDelta(const std::string& id, RefDelta d, const char* from);
  // Every delta as (authored pos, record bytes), id order — saveRecords.
  void SaveDeltas(RefCtx& ctx, std::vector<std::pair<Vec3, std::vector<uint8_t>>>& out);
  // A save is being loaded: deactivate all (Reset), drop deltas and the
  // pulled-bucket memory. Refs themselves (authored) are untouched.
  void ResetForLoad(RefCtx& ctx);

  // ---- authoring (called by refs::Place/Move/...) ----------------------------
  // Replace or insert one ref in memory and rewrite its group file. Marks it
  // for re-apply. `err` explains a refusal.
  // `fileIndex` places a NEW ref at that row of its group file (-1 = append),
  // so an undone delete puts the line back where it was.
  bool Upsert(const Ref& r, std::string* err, int fileIndex = -1);
  // `removed` / `fileIndex` (optional) receive the line and its row: what an
  // undo needs to put it back.
  bool Erase(const std::string& id, std::string* err, Ref* removed = nullptr,
             int* fileIndex = nullptr);

 private:
  bool WriteGroupFile(const std::string& group, std::string* err);
  void Index();
  void EnsurePulled(const Ref& r);
  void Activate(RefCtx& ctx, const Ref& r);
  void Deactivate(RefCtx& ctx, const std::string& id, RefEvent why);
  bool ValidateRef(const Ref& r, const std::string& label);

  std::string assetDir_, map_, dir_;
  std::map<std::string, RefGroupFile> groups_;
  std::vector<std::string> groupOrder_;
  std::map<std::string, Ref> refs_;
  std::map<uint32_t, std::string> byHash_;
  std::map<uint64_t, std::vector<std::string>> byRegion_;
  std::set<std::string> active_;       // activate returned Done
  std::set<std::string> retry_;        // activate returned Retry
  std::set<std::string> reapply_;      // edited: deactivate(Edited) + activate
  // The line an edited ref was ACTIVATED from, so Edited reaches the hook of
  // the kind that did the activating (an edit may change the kind).
  std::map<std::string, Ref> prev_;
  std::map<std::string, Ref> graveyard_;   // erased by an edit, not yet undone
  std::vector<std::string> warnings_;
  std::vector<RefActivity> log_;
  std::vector<UseRecord> uses_;
  std::vector<std::string> devUses_;
  std::map<std::string, RefDelta> deltas_;
  std::set<uint64_t> pulledRegions_;
  ChunkStore* chunkStore_ = nullptr;
  bool haveOrigin_ = false;
  IVec3 lastOrigin_{};
  uint64_t revision_ = 0;
  uint32_t lastTick_ = 0;
};

// ---- the four authoring actions (P5 wraps each as an undoable command) ----
//
// Each validates, rewrites the group JSON (creating the file for a new
// group), and marks the ref for re-apply in the live world. On refusal the
// file is untouched and `err` says why in words a person can act on.
// `fileIndex`: the row in the group file (-1 = append; an undo of Delete
// passes the row Delete reported).
bool Place(RefStore& s, const Ref& r, std::string* err = nullptr, int fileIndex = -1);
bool Move(RefStore& s, const std::string& id, IVec3 pos, int yaw,
          std::string* err = nullptr);
// `value` null = remove the prop.
bool SetProp(RefStore& s, const std::string& id, const std::string& key,
             const Json& value, std::string* err = nullptr);
// "kind" or "base" (strings). Other names are refused.
bool SetField(RefStore& s, const std::string& id, const std::string& field,
              const std::string& value, std::string* err = nullptr);
// `removed` / `fileIndex` receive the deleted line and its row (for undo).
bool Delete(RefStore& s, const std::string& id, std::string* err = nullptr,
            Ref* removed = nullptr, int* fileIndex = nullptr);

// A kind that keeps a RefStore's address between ticks (P7's catch-up placer)
// registers here to hear when that store is destroyed. Idempotent per fn.
void AddStoreGoneHook(void (*fn)(RefStore*));

// ---- helpers kinds share -----------------------------------------------------
// The live mob spawned from ref `id`, or null (MobSystem::FindMobByRef).
// Declared here so kinds need not include game/mob.h to ask.
uint64_t LiveMobOfRef(MobSystem& mobs, const std::string& id);

}  // namespace refs
