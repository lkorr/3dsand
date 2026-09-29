// selftest_dialogue.cpp — conversations (docs/PLAN_world_editor.md P3,
// game/dialogue.h).
//
// dialogue-graph  THE SHIPPED SAMPLE, THROUGH THE REAL TICK. Loads
//                 assets/dialogue/ and asserts the sample validates clean;
//                 parses a deliberately broken file and asserts every §2.7
//                 check names its file, node and field; then walks the sample
//                 with answers carried by TickInput::talk through
//                 TickAuthority (support::RunTicks) and asserts the EXACT end
//                 state: every flag, every item, the met set, which entry a
//                 second conversation takes, the day/night `else`, the
//                 activity hook, a refused Esc on a canLeave:false node, a
//                 speaker who vanishes, and a talking player who does not
//                 move while the controller is told to walk.
// dialogue-save   FLAGS AND MET SURVIVE SAVE/LOAD. A real SaveWorld/LoadWorld
//                 round trip with the 'DLGF' section registered through
//                 MakeEntityIO (world.sve), plus the section's own refusals
//                 (wrong version, truncated payload) and the rule that a
//                 zero flag is not written.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game/dialogue.h"
#include "game/persist.h"
#include "game/session.h"
#include "sim/worldgen_run.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

// persist.cpp's FourCC is file-local; the same packing.
constexpr uint32_t Tag4(char a, char b, char c, char d) {
  return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) |
         ((uint32_t)(uint8_t)d << 24);
}

struct Checker {
  const char* gate;
  bool ok = true;
  int checks = 0;
  void operator()(bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("%s: FAILED %s\n", gate, what.c_str());
    }
  }
};

bool HasProblem(const std::vector<dialogue::Problem>& ps, bool error, const char* node,
                const char* field, const char* msgPart) {
  for (const dialogue::Problem& p : ps)
    if (p.error == error && p.node.find(node) != std::string::npos &&
        p.field.find(field) != std::string::npos &&
        p.msg.find(msgPart) != std::string::npos)
      return true;
  return false;
}

Status GateDialogueGraph(Ctx& c, std::string& detail) {
  Checker check{"dialogue-graph"};

  // ---- 1. load + validate the shipped content ------------------------------
  dialogue::Store store;
  store.dir = AssetDir() + "/dialogue";
  store.items = &c.items;
  store.Reload();
  const dialogue::Dialogue* sample = store.lib.Find("sample_stranger");
  check(sample != nullptr, "assets/dialogue/sample_stranger.json loads");
  int sampleProblems = 0;
  for (const dialogue::Problem& p : store.problems)
    if (p.file.find("sample_stranger") != std::string::npos || p.file == "(all dialogue)") {
      sampleProblems++;
      std::printf("dialogue-graph: sample problem: %s\n", p.Line().c_str());
    }
  check(sampleProblems == 0, Format("the sample validates with no errors or warnings (%d)",
                                    sampleProblems));
  if (!sample) {
    detail = "no sample";
    return Status::Fail;
  }

  // ---- 2. the §2.7 checks name file, node and field -------------------------
  {
    dialogue::Library lib;
    std::vector<dialogue::Problem> ps;
    const char* broken = R"({
      "speaker": "Broken",
      "entry": [ { "goto": "start" } ],
      "nodes": [
        { "id": "start", "text": "hi",
          "choices": [ { "text": "go", "goto": "nowhere" },
                       { "text": "take", "do": [ { "take": "no_such_item" } ] },
                       { "text": "bad", "if": [ { "flg": "x" } ] } ] },
        { "id": "island", "text": "nobody comes here", "do": [ { "set": "orphan_flag" } ] }
      ] })";
    dialogue::Dialogue d;
    lib.Parse("broken", broken, "broken.json", d, ps);
    check(HasProblem(ps, true, "node 'start'", "choices[2].if[0].flg", "unknown condition"),
          "an unknown condition key is an error naming node and field");
    // Re-parse leniently for the graph checks: drop the bad choice.
    dialogue::Dialogue d2;
    std::vector<dialogue::Problem> ps2;
    const char* graphOnly = R"({
      "entry": [ { "goto": "start" } ],
      "nodes": [
        { "id": "start", "text": "hi",
          "choices": [ { "text": "go", "goto": "nowhere" },
                       { "text": "take", "do": [ { "take": "no_such_item" } ] } ] },
        { "id": "island", "text": "nobody comes here", "do": [ { "set": "orphan_flag" } ] }
      ] })";
    check(lib.Parse("broken", graphOnly, "broken.json", d2, ps2), "the graph fixture parses");
    lib.Add(d2);
    lib.Validate(&c.items, ps2);
    for (const dialogue::Problem& p : ps2) std::printf("dialogue-graph:   (fixture) %s\n", p.Line().c_str());
    check(HasProblem(ps2, true, "node 'start'", "choices[0].goto", "not a node"),
          "a dangling goto is an error naming node and field");
    check(HasProblem(ps2, true, "node 'start'", "choices[1].do[0]", "not an item"),
          "an unknown item is an error");
    check(HasProblem(ps2, false, "node 'island'", "", "cannot be reached"),
          "an unreachable node is a warning");
    check(HasProblem(ps2, false, "", "flag 'orphan_flag'", "no condition reads it"),
          "a flag written but never read is a warning");
  }

  // ---- 3. the walk, through THE tick ---------------------------------------
  World& world = c.world;
  const IVec3 wo = world.WindowOrigin();
  const int sx = wo.x * (int)kChunk + 88, sz = wo.z * (int)kChunk + 88;
  const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
  support::TickRig rig(c, 9800, IVec3{sx >> 4, (h + 40) >> 4, sz >> 4});
  rig.Authority().talk = &store;
  PlayerSession& s = rig.Session();
  Kit& kit = s.kit();
  kit.Clear();
  {
    ItemStack f;
    f.name = "flask";
    f.count = 1;
    kit.bag.Add(f);
  }
  store.minuteOverride = 22 * 60;  // night: the treeline node's `time` holds

  // One tick carrying `code` (a TalkCommand) and, optionally, a walk order.
  auto tick = [&](int code, bool walk = false) {
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.input.talk = (int16_t)code;
      if (walk) o.input.forward = 1.0f;
    });
  };
  auto at = [&]() { return s.talk.active ? s.talk.node : std::string("(ended)"); };
  auto visible = [&]() { return (int)s.talk.visible.size(); };

  std::string why;
  check(dialogue::Begin(store, s, dialogue::Speaker{0, "", "gate"}, "sample_stranger",
                        rig.tick, &why),
        "Begin starts the sample (" + why + ")");
  check(at() == "hello", "a stranger never met enters at 'hello' (" + at() + ")");
  check(visible() == 0 && dialogue::MakeView(store, s.talk).canContinue,
        "'hello' has no choices: the panel offers [continue]");
  // A talking player does not move, whatever the command says.
  const Vec3 p0 = s.player.pos;
  tick(0, true);
  tick(0, true);
  const Vec3 p1 = s.player.pos;
  check(std::abs(p1.x - p0.x) + std::abs(p1.y - p0.y) + std::abs(p1.z - p0.z) < 1e-4f,
        "a talking player holds still with forward held");
  tick(kTalkContinue);
  check(at() == "ask", "[continue] follows the node's goto to 'ask' (" + at() + ")");
  check(visible() == 5, Format("'ask' shows 5 choices at first (%d): who, treeline, "
                               "flask, defend, goodbye; 'agnes' is hidden until met",
                               visible()));
  tick(2);  // "Have you seen anything at the treeline?"
  check(at() == "treeline", "at night the treeline node's time condition holds (" + at() + ")");
  check(store.Flag("asked_treeline") == 1, "the choice's `set` ran");
  tick(kTalkContinue);
  check(visible() == 4, Format("the treeline choice is hidden once asked (%d)", visible()));
  tick(2);  // "You look thirsty. Take my flask."
  check(at() == "thanks", "the flask choice leads to 'thanks' (" + at() + ")");
  check(dialogue::CountItem(kit, "flask") == 0, "`take` removed the flask");
  check(dialogue::CountItem(kit, "hood") == 1, "`give` put a hood in the pack");
  check(store.Flag("stranger_owed") == 2, "the node's `set value 2` overwrote the choice's 1");
  tick(kTalkLeave);
  check(at() == "thanks", "Esc is refused on a canLeave:false node");
  tick(kTalkContinue);
  check(visible() == 3, Format("with no flask the flask choice is hidden (%d)", visible()));
  tick(9);  // out of range: nothing happens
  check(at() == "ask", "a key past the visible choices does nothing");
  tick(2);  // "I have nothing to defend myself with."
  check(at() == "gift" && dialogue::CountItem(kit, "dagger") == 1 &&
            store.Flag("stranger_gave") == 1,
        "!has + !flag choice: gift node gives the dagger and sets the flag");
  tick(kTalkContinue);
  check(visible() == 2, Format("holding a dagger hides the defend choice (%d)", visible()));
  tick(2);  // "Enough talk. Good night." -> clear + end
  check(!s.talk.active, "the `end` action ends the conversation");
  check(store.Flag("asked_treeline") == 0 && store.flags.count("asked_treeline") == 0,
        "`clear` removed the flag");
  check(store.met.count("sample_stranger") == 1, "ending marks the dialogue met");
  // The exact end state.
  check(store.flags.size() == 2 && store.Flag("stranger_owed") == 2 &&
            store.Flag("stranger_gave") == 1,
        Format("flags are exactly {stranger_owed:2, stranger_gave:1} (%zu flags)",
               store.flags.size()));
  check(kit.bag.Count() + [&] {
          int n = 0;
          for (const ItemStack& st : kit.hotbar.slots) n += !st.Empty();
          return n;
        }() == 2,
        "the kit holds exactly two stacks (hood, dagger)");
  // A talking-free player walks: the control arm for "holds still".
  tick(0, true);
  tick(0, true);
  const Vec3 p2 = s.player.pos;
  check(std::abs(p2.x - p1.x) + std::abs(p2.z - p1.z) > 1e-3f,
        "the same walk order moves a player who is not talking (control)");

  // Second conversation: the owed==2 entry wins over `met`.
  check(dialogue::Begin(store, s, dialogue::Speaker{}, "sample_stranger", rig.tick, &why) &&
            at() == "settled",
        "a second conversation takes the first entry that holds: 'settled' (" + at() + ")");
  tick(kTalkContinue);
  check(!s.talk.active, "[continue] on a node with no goto ends it");

  // Daytime + the else chain, from a fresh world state.
  store.ResetState();
  kit.Clear();
  store.minuteOverride = 12 * 60;
  dialogue::Begin(store, s, dialogue::Speaker{}, "sample_stranger", rig.tick, &why);
  tick(kTalkContinue);
  tick(2);
  check(at() == "treeline_day", "at noon the treeline node's `if` fails to its `else` (" +
                                    at() + ")");
  tick(kTalkLeave);
  check(!s.talk.active, "Esc leaves where the file allows it");
  check(store.Flag("asked_treeline") == 1, "leaving keeps what was already set");

  // A second visit: met + add. And by the third, the `value 3` entry.
  store.ResetState();
  store.met.insert("sample_stranger");
  store.flags["stranger_visits"] = 2;
  dialogue::Begin(store, s, dialogue::Speaker{}, "sample_stranger", rig.tick, &why);
  check(at() == "again" && store.Flag("stranger_visits") == 3,
        "met: the 'again' entry, whose `add` counts the visit (" + at() + ")");
  tick(kTalkContinue);
  check(visible() == 5, Format("met by name reveals the 'agnes' choice (%d; no flask, so "
                               "the flask choice is hidden)",
                               visible()));
  dialogue::End(store, s);
  dialogue::Begin(store, s, dialogue::Speaker{}, "sample_stranger", rig.tick, &why);
  check(at() == "tired", "visits == 3 selects the 'tired' entry (" + at() + ")");
  dialogue::End(store, s);

  // The activity hook (P7 supplies it). Unset = false, set = consulted.
  store.activity = [](uint64_t, const std::string&) { return std::string("sleep"); };
  dialogue::Begin(store, s, dialogue::Speaker{}, "sample_stranger", rig.tick, &why);
  check(at() == "asleep", "an activity hook answering 'sleep' selects 'asleep' (" + at() + ")");
  dialogue::End(store, s);
  store.activity = nullptr;

  // A speaker who is not in the world ends the conversation on the next tick,
  // and IsInConversation follows it.
  const uint64_t ghost = 0x7fff'0000'0001ull;
  dialogue::Begin(store, s, dialogue::Speaker{ghost, "", "ghost"}, "sample_stranger",
                  rig.tick, &why);
  check(dialogue::IsInConversation(store, ghost), "IsInConversation names the speaker");
  tick(0);
  check(!s.talk.active && !dialogue::IsInConversation(store, ghost),
        "a speaker missing from the world ends the conversation");
  check(!dialogue::Begin(store, s, dialogue::Speaker{}, "no_such_dialogue", rig.tick, &why),
        "an unknown dialogue name refuses with a reason (" + why + ")");

  detail = Format("%d checks", check.checks);
  std::printf("dialogue-graph: %s (%d checks, %llu begun, %llu choices)\n",
              check.ok ? "PASS" : "FAIL", check.checks,
              (unsigned long long)store.stats.begun, (unsigned long long)store.stats.choices);
  return check.ok ? Status::Pass : Status::Fail;
}

Status GateDialogueSave(Ctx& c, std::string& detail) {
  Checker check{"dialogue-save"};
  dialogue::Store store;
  store.flags["wat_asked"] = 1;
  store.flags["osric_mood"] = -3;
  store.flags["cleared_one"] = 0;  // a zero flag is not written
  store.met.insert("osric");
  store.met.insert("sample_stranger");

  // ---- the section's own contract ------------------------------------------
  {
    std::vector<uint8_t> bytes;
    store.SaveState(bytes);
    dialogue::Store b;
    check(b.LoadState(bytes.data(), bytes.size(), dialogue::Store::kSaveVersion),
          "the payload loads");
    check(b.flags.size() == 2 && b.Flag("osric_mood") == -3 && b.flags.count("cleared_one") == 0,
          "flags round-trip (negative values too) and a zero flag is not written");
    check(!b.LoadState(bytes.data(), bytes.size(), dialogue::Store::kSaveVersion + 1),
          "a future version is refused");
    check(!b.LoadState(bytes.data(), bytes.size() - 3, dialogue::Store::kSaveVersion),
          "a truncated payload is refused");
    check(b.flags.size() == 2 && b.met.size() == 2, "a refused load leaves the store untouched");
  }

  // ---- a real save and load --------------------------------------------------
  const char* kPath = "selftest_dialogue.svd";
  std::filesystem::remove_all(kPath);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  EntityIO eio = MakeEntityIO(c.debris, c.mobs, nullptr, nullptr, nullptr, &store);
  bool registered = false;
  for (const EntitySection& sec : eio.sections)
    if (sec.id == Tag4('D', 'L', 'G', 'F'))
      registered = sec.scope == EntityScope::World;
  check(registered, "'DLGF' is registered by MakeEntityIO as a WORLD section");
  const bool saved = SaveWorld(c.ctx, c.world, c.stream, kPath, c.mats, &eio);
  check(saved, "SaveWorld succeeds");
  {
    std::ifstream f(std::string(kPath) + "/world.sve", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check(bytes.find("osric_mood") != std::string::npos, "the flags are in world.sve");
  }
  const std::map<std::string, int> flagsBefore = store.flags;
  const std::set<std::string> metBefore = store.met;
  // Wreck the live state so a pass cannot be leftovers.
  store.flags.clear();
  store.flags["stale"] = 7;
  store.met.clear();
  const bool loaded = LoadWorld(c.ctx, c.world, c.sim, c.stream, kPath, c.mats, &eio);
  check(loaded, "LoadWorld succeeds");
  std::map<std::string, int> expect = flagsBefore;
  expect.erase("cleared_one");
  check(store.flags == expect, Format("flags survive save/load exactly (%zu, want %zu)",
                                      store.flags.size(), expect.size()));
  check(store.met == metBefore, "the met set survives save/load exactly");

  // A save WITHOUT the section (an older world) loads with nobody met. Into
  // the SAME dir: a session's store is bound to one world dir (worldio), and
  // re-saving it without the section is exactly what an older build writes.
  EntityIO bare = MakeEntityIO(c.debris, c.mobs, nullptr);
  const bool savedBare = SaveWorld(c.ctx, c.world, c.stream, kPath, c.mats, &bare);
  const bool loadedBare = LoadWorld(c.ctx, c.world, c.sim, c.stream, kPath, c.mats, &eio);
  check(savedBare && loadedBare && store.flags.empty() && store.met.empty(),
        "a save without 'DLGF' loads a world where nobody has been spoken to");
  std::filesystem::remove_all(kPath);

  detail = Format("%d checks", check.checks);
  std::printf("dialogue-save: %s (%d checks)\n", check.ok ? "PASS" : "FAIL", check.checks);
  return check.ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DialogueGates() {
  static const std::vector<Gate> g = {
      {"dialogue-graph", "dialogue", {}, false, GateDialogueGraph},
      {"dialogue-save", "dialogue", {}, false, GateDialogueSave},
  };
  return g;
}

}  // namespace selftest
