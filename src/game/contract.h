// contract.h — DEMON CONTRACTS: the page, the language, the weight
// (docs/PLAN_demons.md D5; DESIGN.md §17 "Demons -- conversation, contracts").
//
// A CONTRACT IS A PAGE. Drafted beforehand in its own editor (ui/
// contract_ui.cpp), kept by the player like grimoire pages (PlayerCaster::
// contracts, saved in the player's file), and PRESENTED to a contained demon in
// conversation. It is pure content: names and short expressions, compiled
// here, never executed here (game/demon_talk.h runs a bound one in the tick).
//
// THE LANGUAGE (a small, fixed vocabulary; every name below is data-checked):
//
//   page      { "name", "hours" (term; 0 = indefinite), "counters": [..],
//               "clauses": [..] }
//   clause    { "kind": duty | forbid | penalty,
//               "verb": ...,                     (per kind, below)
//               "who":  <selector>,              the actors it is about
//               "when": <trigger>,               "" = always
//               "arg", "into",                   verb arguments (fetch, goto)
//               "then": dismiss | destroy | pain (a penalty's consequence)
//               "on":   "<counter>+N" | "<counter>-N" | "<counter>=N"
//                                                applied when the clause FIRES
//                                                (a duty done, a penalty paid) }
//   duty      follow (who)       keep near the nearest of `who`
//             guard (who)        fight the nearest of `who` in range
//             goto (arg)         go to "x y z" (world voxels) or "here" (where
//                                the contract was presented) and stand
//             fetch (arg, into)  fetch material `arg` into the summoner's
//                                vessel `into` (an item name; "" = any vessel
//                                that takes it), then come back
//             spin               turn on the spot
//   forbid    attack (who)       HARD FILTER: never targets / strikes `who`
//             cast (who, arg)    HARD FILTER: no spell at `who` (D4's caster
//                                asks demon::AllowCastAt); `arg` narrows it to
//                                spells whose footprint tags match: direct,
//                                affects_body, creates:<mat>, alters:<x>,
//                                targets:<x> ("" = any spell)
//             leave (who, arg)   never further than `arg` metres from `who`
//   penalty   attack (who) -> then      a blow ASKED at `who` fires `then`
//             leave (who, arg) -> then  further than `arg` m from `who`
//
//   selector  a set expression over ACTORS (the players and every live mob):
//               me                the summoner
//               self              the demon
//               all               everyone
//               attacked_by(S)    actors a member of S is attacking (a mob's
//                                 target; a player's recent strike)
//               attacking(S)      actors attacking a member of S
//               hostile_to(S)     actors hostile to a member of S (an aggro
//                                 hostile profile of another faction, or one
//                                 attacking it)
//               faction(name)     the actor's faction ("player" for players)
//               type(name)        the actor's mob def stem ("player")
//               S & S   S | S   !S   ( S )
//   trigger   conditions joined by &, ALL must hold:
//               <fact> <op> <int>,  op one of < <= > >= == !=
//             facts: dist (demon to summoner, metres), hp (summoner's life, %),
//                    demon_hp (%), term (seconds left), count(S) (actors of S
//                    within 24 m of the demon), <counter name>
//
// BOUNDED (rule 2): kMaxClauses clauses, kMaxNodes nodes per selector,
// kMaxConds conditions per trigger, kMaxCounters counters. Evaluation charges
// every node visited against a per-demon per-tick budget (demon_talk.h); a
// selector that runs it out answers "not a member" for a duty and "a member"
// for a forbid (fail closed: a prohibition the binding cannot evaluate holds).
//
// THE WEIGHT (assets/demons/contract_tariff.json, like glyphs.json's
// `budgets.rates`): a term bracket (one day is cheap, indefinite is dear),
// each clause's verb, its selector's breadth (per-node points x a per-kind
// rate), each trigger condition and each counter. Weigh() returns the total
// and one line per term, which the editor shows live. Integer, no floats.
//
// DETERMINISM: everything here is a pure function of the page text and the
// tariff. Content (the tariff, the stock pages) is read from disk at first use
// and on R, like glyphs.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace contract {

constexpr int kMaxClauses = 12;
constexpr int kMaxNodes = 24;
constexpr int kMaxConds = 4;
constexpr int kMaxCounters = 4;
constexpr int kMaxNameLen = 40;

enum class Kind : uint8_t { Duty = 0, Forbid, Penalty };
const char* KindName(Kind k);   // "duty" | "forbid" | "penalty"
bool KindByName(const std::string& s, Kind& out);

// The verbs, per kind. Fixed vocabulary: the order is the editor's menu order.
enum class Verb : uint8_t {
  Follow = 0, Guard, Goto, Fetch, Spin,   // duties
  Attack, Cast, Leave,                    // forbids (attack, leave also penalties)
  None,
};
const char* VerbName(Verb v);
bool VerbByName(const std::string& s, Verb& out);
// Is `v` a verb clauses of kind `k` may use?
bool VerbFits(Kind k, Verb v);
// The editor's menus.
const std::vector<Verb>& VerbsFor(Kind k);

enum class Then : uint8_t { None = 0, Dismiss, Destroy, Pain };
const char* ThenName(Then t);
bool ThenByName(const std::string& s, Then& out);

// ---- selectors --------------------------------------------------------------------

enum class SelOp : uint8_t {
  Me = 0, Self, All, AttackedBy, Attacking, HostileTo, Faction, Type, And, Or, Not,
};
const char* SelOpName(SelOp o);

struct SelNode {
  SelOp op = SelOp::Me;
  int16_t a = -1, b = -1;   // child nodes (And/Or: a, b; Not/AttackedBy/...: a)
  std::string name;         // Faction / Type
};

struct Selector {
  std::vector<SelNode> nodes;
  int root = -1;
  bool Empty() const { return root < 0; }
};
// Parse `text` ("" -> empty selector). False with `err` on a syntax error,
// an unknown word or more than kMaxNodes nodes.
bool ParseSelector(const std::string& text, Selector& out, std::string& err);

// ---- triggers ---------------------------------------------------------------------

enum class FactKind : uint8_t { Dist = 0, Hp, DemonHp, Term, Count, Counter };
enum class Cmp : uint8_t { Lt = 0, Le, Gt, Ge, Eq, Ne };

struct Cond {
  FactKind fact = FactKind::Dist;
  Cmp op = Cmp::Lt;
  int32_t value = 0;
  Selector sel;          // Count
  int counter = -1;      // Counter: index into Page::counters
};
// Parse `text` against the page's counter names.
bool ParseTrigger(const std::string& text, const std::vector<std::string>& counters,
                  std::vector<Cond>& out, std::string& err);
bool CmpHolds(Cmp op, int32_t a, int32_t b);

// ---- the page ---------------------------------------------------------------------

struct Effect {
  int counter = -1;     // -1 = none
  char op = '+';        // '+', '-', '='
  int32_t value = 0;
};
bool ParseEffect(const std::string& text, const std::vector<std::string>& counters, Effect& out,
                 std::string& err);

struct Clause {
  // ---- authored (the source of truth, saved) ----
  Kind kind = Kind::Duty;
  Verb verb = Verb::Follow;
  std::string who;    // selector text
  std::string when;   // trigger text
  std::string arg;    // fetch: material; goto: "x y z" | "here"; leave: metres
  std::string into;   // fetch: vessel item name ("" = any)
  Then then = Then::None;
  std::string on;     // effect text
  // ---- compiled (Compile) ----
  Selector sel;
  std::vector<Cond> conds;
  Effect effect;
};

struct Page {
  std::string name;
  int32_t hours = 24;                 // term in game hours; 0 = indefinite
  std::vector<std::string> counters;  // named counters (<= kMaxCounters)
  std::vector<Clause> clauses;        // <= kMaxClauses
  bool stock = false;                 // shipped with the game, read-only
};

// Compile every clause. False with one line per problem in `errs` (clause
// index first). A page that does not compile cannot be presented.
bool Compile(Page& p, std::vector<std::string>& errs);

// The id a TickInput carries for a page: FNV-1a of its NAME (nonzero).
uint32_t PageHash(const std::string& name);

// ---- the tariff -------------------------------------------------------------------

struct Tariff {
  // Term brackets, ascending: a term of h hours costs the first bracket whose
  // `hours` >= h. Longer than every bracket and not indefinite: the last.
  std::vector<std::pair<int32_t, int32_t>> term{{24, 1}, {72, 4}, {168, 8}};
  int32_t indefinite = 30;
  // Verb base weights, per kind.
  int32_t duty[5] = {1, 3, 1, 3, 0};          // follow guard goto fetch spin
  int32_t forbid[3] = {3, 3, 2};              // attack cast leave
  int32_t penaltyAct[3] = {0, 0, 0};          // attack cast leave (watched act)
  int32_t then[4] = {0, 0, 1, 1};             // none dismiss destroy pain
  // Selector breadth: points per node op (SelOp order).
  int32_t selOp[11] = {0, 0, 6, 1, 1, 1, 2, 1, 0, 1, 2};
  // Breadth points -> weight, per kind (duty / forbid / penalty), in percent.
  int32_t breadthPct[3] = {100, 100, 0};
  int32_t perCondition = 1;
  int32_t perCounter = 1;
  // Upkeep: mana reserved while bound = power x upkeepPerPowerPct / 100.
  int32_t upkeepPerPowerPct = 100;
};

struct WeightLine {
  std::string label;
  int32_t weight = 0;
};
struct Weight {
  int32_t total = 0;
  std::vector<WeightLine> lines;
};
// Pure. Clauses that do not compile still weigh by their verb.
Weight Weigh(const Page& p, const Tariff& t);
int32_t SelectorBreadth(const Selector& s, const Tariff& t);
int32_t UpkeepFor(int32_t power, const Tariff& t);

// ---- content: assets/demons/contract_tariff.json + contracts.json -------------------

struct Content {
  bool loaded = false;
  Tariff tariff;
  std::vector<Page> stock;   // compiled, stock = true
  std::string log;           // what the last load had to say
  const Page* FindStock(const std::string& name) const;
};
// Loaded at first use; `reload` re-reads (the R key).
const Content& GetContent(bool reload = false);
// Parse a page from JSON text (the stock file's entries, a test's page).
bool PageFromJson(const std::string& text, Page& out, std::string& err);
std::string PageToJson(const Page& p);

// ---- persistence (a page's bytes; demon_talk.cpp frames them) ------------------------

void PutPage(std::vector<uint8_t>& out, const Page& p);
bool GetPage(const uint8_t*& d, const uint8_t* end, Page& out);

// A one-line English reading of a clause, for the editor and the HUD
// ("forbid: attack me", "duty: guard attacked_by(me) & hostile_to(me)").
std::string Describe(const Clause& c);

}  // namespace contract
