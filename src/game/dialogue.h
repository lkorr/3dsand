// dialogue.h — BRANCHING CONVERSATIONS (docs/PLAN_world_editor.md §2.7, P3).
//
// THREE LAYERS, and the split is the design:
//
//   CONTENT   assets/dialogue/<name>.json, one conversation graph per file,
//             loaded and VALIDATED into a Library. Plain JSON a person edits
//             in the tuner's Dialogue tab or in a text editor, then presses R.
//   WORLD     the Store: the library plus the world-scoped FLAGS
//             (string -> int) and the MET set, saved in world.sve ('DLGF',
//             game/persist.h). One per world, owned by main() and borrowed by
//             the tick through TickAuthorityCtx::dialogue.
//   PLAYER    a Conversation per PlayerSession: which file, which node, who
//             is speaking, and the choices the panel shows. Not saved (a save
//             taken mid-sentence loads with nobody talking).
//
// THE FILE (the whole schema; docs/EDITOR_GUIDE.md is the tutorial):
//
//   { "name": "osric", "speaker": "Osric", "canLeave": true,
//     "entry": [ { "if": [ {"met": true} ], "goto": "again" },
//                { "goto": "hello" } ],
//     "nodes": [
//       { "id": "hello", "text": "Forge is hot. Mind your sleeves.",
//         "do": [ {"set": "osric_greeted"} ],
//         "choices": [
//           { "text": "Wat wants to learn the trade.", "goto": "wat",
//             "if": [ {"flag": "wat_asked"} ] },
//           { "text": "Goodbye." } ] },
//       { "id": "again", "text": "You again.", "goto": "hello" } ] }
//
//   entry    tried in order; the FIRST whose `if` holds picks the start node.
//   node     id (unique in the file), text, optional speaker (default: the
//            file's), `if` + `else` (arriving at a node whose `if` fails goes
//            to its `else` node, or ends), `do` (run on arrival), `choices`,
//            `goto` (where [continue] leads when there are no choices; absent
//            = the conversation ends), `canLeave` (Esc allowed here).
//   choice   text, `goto` (absent = end), `if` (hidden unless it holds),
//            `do` (run when chosen, before the goto).
//   `if` is a LIST of conditions, ALL of which must hold:
//     {"flag": "f"}  f is non-zero      {"flag": "f", "value": 2}  f == 2
//     {"!flag": "f"} f is zero (unset)
//     {"time": ["06:00", "18:00"]}      in-game clock in [from, to), wraps
//     {"activity": "work"} / {"!activity": ...}  the speaker's schedule row
//     {"has": "dagger", "count": 1} / {"!has": "dagger"}  in the pack/hotbar
//     {"met": true} / {"!met": true}    spoken to THIS dialogue before
//     {"met": "agnes"} / {"!met": ...}  spoken to THAT dialogue before
//   `do` is a LIST of actions, run in order:
//     {"set": "f"} (=1)  {"set": "f", "value": 3}  {"add": "f", "value": 1}
//     {"clear": "f"}     {"give": "dagger", "count": 1}
//     {"take": "dagger", "count": 1}     {"end": true}
//
// DETERMINISM. Nothing here writes a voxel. What it writes is flags (world
// state, integer) and the player's kit (items by name), and it writes them
// only from TickAuthority, on the tick a TickInput carried the answer — never
// from the panel. Evaluation reads the sim tick's day phase, never a clock.
//
// "met" IS KEYED BY DIALOGUE NAME, not by NPC: one conversation file is one
// person's voice, and keying by name is what lets `{"met": "osric"}` be
// written by a human without knowing a ref id. Two NPCs sharing one file
// share one `met`.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Kit;
struct ItemLibrary;
struct TickInput;
struct PlayerSession;
class MobSystem;

namespace dialogue {

// ---- content -----------------------------------------------------------------

struct Cond {
  enum class Kind : uint8_t { Flag, Time, Activity, Has, Met };
  Kind kind = Kind::Flag;
  bool negate = false;   // the "!key" spelling
  std::string arg;       // flag / activity / item / dialogue name ("" = this one)
  bool hasValue = false; // flag: compare to `value` instead of non-zero
  int value = 0;
  int count = 1;         // has: at least this many
  int fromMin = 0, toMin = 0;  // time: minutes after midnight, [from, to)
};

struct Act {
  enum class Kind : uint8_t { Set, Add, Clear, Give, Take, End };
  Kind kind = Kind::Set;
  std::string arg;  // flag or item name
  int value = 1;    // set/add value, give/take count
};

struct Choice {
  std::string text;
  std::string gotoId;  // "" = the conversation ends
  std::vector<Cond> conds;
  std::vector<Act> acts;
};

struct Node {
  std::string id;
  std::string speaker;  // "" = the file's speaker
  std::string text;
  std::vector<Cond> conds;
  std::string elseId;   // where to go when `conds` fail on arrival ("" = end)
  std::vector<Act> acts;
  std::vector<Choice> choices;
  std::string gotoId;   // [continue] target when no choice is shown
  int canLeave = -1;    // -1 = the file's setting
};

struct Entry {
  std::vector<Cond> conds;
  std::string gotoId;
};

struct Dialogue {
  std::string name;  // file stem, the name everything refers to it by
  std::string file;  // path it was loaded from (for messages)
  std::string speaker;
  bool canLeave = true;
  std::vector<Entry> entries;
  std::vector<Node> nodes;
  const Node* Find(const std::string& id) const;
};

// One thing wrong with the content, naming WHERE (owner's rule §1.5): the
// file, the node (or "entry[2]" / "(file)"), and the field.
struct Problem {
  bool error = true;  // false = a warning: it loads, but look at it
  std::string file, node, field, msg;
  std::string Line() const;  // "osric.json: node 'hello' choices[1].goto: ..."
};

// The schedule activities P7 defines (§2.6); an `activity` condition naming
// anything else is a warning, not an error, so a new activity can be written
// before the code for it exists.
const std::vector<std::string>& KnownActivities();

class Library {
 public:
  // Every <dir>/*.json, sorted by name. Returns false only when the directory
  // could not be read; a malformed FILE is skipped with an error Problem and
  // the rest still load (one typo must not silence a whole village).
  // `items`, when given, checks every give/take/has names a real item.
  bool Load(const std::string& dir, const ItemLibrary* items,
            std::vector<Problem>& problems);
  // Parse one file's text (the gate and the tuner-mirror path use this).
  bool Parse(const std::string& name, const std::string& text,
             const std::string& file, Dialogue& out,
             std::vector<Problem>& problems) const;
  // The §2.7 checks over one dialogue (dangling goto, unreachable node,
  // unknown item/activity) and, library-wide, flags written but never read
  // and read but never written.
  void Validate(const ItemLibrary* items, std::vector<Problem>& problems) const;

  const Dialogue* Find(const std::string& name) const;
  const std::vector<Dialogue>& All() const { return all_; }
  void Add(Dialogue d);  // tests: install a parsed dialogue
  void Clear() { all_.clear(); }

 private:
  std::vector<Dialogue> all_;
};

// ---- the world: library + flags + met ----------------------------------------

struct Speaker;

class Store {
 public:
  Library lib;
  std::string dir;                      // assets/dialogue, for Reload
  const ItemLibrary* items = nullptr;   // for validation and give/take
  std::vector<Problem> problems;        // the last Reload's, for the dev panel

  // THE ACTIVITY HOOK (P7). Answers "what is this speaker doing now" (the
  // current schedule row's `do`), or "" for nothing. Unset until P7 wires
  // schedules: an `activity` condition is then simply false — no warning,
  // because "nobody has a schedule yet" is the ordinary state of this slice.
  std::function<std::string(uint64_t mobId, const std::string& refId)> activity;

  // The in-game clock the `time` condition reads, in minutes after midnight,
  // or -1 = derive it from the sim tick's day phase (the game). A gate pins it.
  int minuteOverride = -1;

  // Re-read `dir`. Keeps the flags and the met set (they are world state, not
  // content). Returns false when a file failed; `problems` says which.
  bool Reload();

  // ---- flags: world-scoped string -> int. std::map, so the save writes them
  // in one order on every machine.
  std::map<std::string, int> flags;
  std::set<std::string> met;  // dialogue names spoken to (ended) before
  int Flag(const std::string& f) const;

  // ---- who is being talked to (IsInConversation) ----
  bool IsInConversation(uint64_t mobId) const;

  // Telemetry the gates read.
  struct Stats {
    uint64_t begun = 0, ended = 0, choices = 0, refusedGives = 0;
  } stats;

  // ---- persistence ('DLGF' in world.sve, game/persist.cpp) ----
  static constexpr uint32_t kSaveVersion = 1;
  void SaveState(std::vector<uint8_t>& out) const;
  bool LoadState(const uint8_t* data, size_t len, uint32_t version);
  void ResetState();  // flags + met cleared; content kept

 private:
  friend bool Begin(Store&, PlayerSession&, const Speaker&,
                    const std::string&, uint32_t, std::string*);
  friend void End(Store&, PlayerSession&);
  std::multiset<uint64_t> talking_;
};

// ---- the player: one conversation --------------------------------------------

// Who is talking. Any of the three may be empty: the dev hook talks to a mob
// with no ref, P7's NPCs carry both, and `name` is the header bar's fallback
// when the node and the file name no speaker.
struct Speaker {
  uint64_t mobId = 0;   // 0 = nobody in the world (a voice, a sign, a test)
  std::string refId;    // P1's stable ref id, when the speaker is authored
  std::string name;
};

struct Conversation {
  bool active = false;
  std::string dialogue;  // file name
  std::string node;      // current node id
  Speaker speaker;
  // The choice indices (into node.choices) the panel SHOWS, fixed when the
  // node was entered: key n picks visible[n-1]. Fixed rather than re-derived
  // per frame so the number on the screen and the tick that applies it can
  // never disagree about what "2" meant.
  std::vector<int> visible;
  uint32_t steps = 0;    // bumps on every node change (UI change detection)
  uint32_t startTick = 0;
};

// The dev hook's request (Spawn page -> "talk"): a UI transaction queued on
// the session and started by the next tick, like castAtPartQueued. The use
// verb (P1) and NPCs (P7) call Begin directly from inside the tick.
struct BeginRequest {
  bool pending = false;
  Speaker speaker;
  std::string dialogue;
};

// START a conversation: pick the first entry whose conditions hold and enter
// its node. False (with `why`) when the file is unknown, no entry matches, or
// the session is already talking. Call from inside the tick (TickAuthority,
// a kind's OnUse) — it may run the entry node's actions.
bool Begin(Store& store, PlayerSession& s, const Speaker& speaker,
           const std::string& dialogueName, uint32_t tick,
           std::string* why = nullptr);
// END it now (marks `met`). Safe when nothing is active.
void End(Store& store, PlayerSession& s);

// Is anyone talking to this mob? P7's arbiter asks this to turn the NPC to
// face the player and hold its schedule.
inline bool IsInConversation(const Store& store, uint64_t mobId) {
  return store.IsInConversation(mobId);
}

// THE TICK HOOK. TickAuthority calls it once per session, first thing: starts
// a queued dev-hook request, applies `ti.talk`, ends a conversation whose
// speaker died or vanished, and — while talking — SUPPRESSES the player's
// movement and hands (the command is zeroed in place, so every later phase
// sees a player standing still).
void TickSession(Store& store, PlayerSession& s, TickInput& ti, uint32_t tick,
                 MobSystem* mobs);

// ---- evaluation (exposed for the gates and P7) --------------------------------

struct Env {
  const Store* store = nullptr;
  const Kit* kit = nullptr;       // has
  int minute = 0;                 // time: minutes after midnight
  uint64_t mobId = 0;             // activity
  std::string refId;              // activity
  std::string self;               // met: true -> this dialogue name
};
bool Holds(const Cond& c, const Env& env);
bool AllHold(const std::vector<Cond>& cs, const Env& env);
// How many of `item` a kit holds in its bag and hotbar (worn gear does not
// count: a conversation that takes your sword off your belt is a bug).
int CountItem(const Kit& kit, const std::string& item);
// Minutes after midnight for the sim tick (the store's override if set).
int MinuteNow(const Store& store, uint32_t tick);

// ---- what the panel draws ------------------------------------------------------

struct View {
  bool open = false;
  std::string dialogue;
  std::string speaker;
  std::string text;
  std::vector<std::string> choices;  // visible, in key order (1..9)
  bool canContinue = false;          // no choices: [continue]
  bool canLeave = true;
  uint32_t steps = 0;
};
View MakeView(const Store& store, const Conversation& c);

}  // namespace dialogue
