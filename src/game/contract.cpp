// contract.cpp — the contract page, its language and its weight. See
// contract.h.
#include "game/contract.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace sandvox {
std::string AssetDir();   // test/support.cpp: the one asset-path chokepoint
}

using json = nlohmann::json;

namespace contract {

// ---- names ---------------------------------------------------------------------------

namespace {
constexpr const char* kKindNames[] = {"duty", "forbid", "penalty"};
constexpr const char* kVerbNames[] = {"follow", "guard", "goto",  "fetch", "spin",
                                      "attack", "cast",  "leave", "cause", "harm",
                                      "return", "none"};
constexpr const char* kThenNames[] = {"", "dismiss", "destroy", "pain"};
constexpr const char* kSelNames[] = {"me",        "self",    "all",  "attacked_by",
                                     "attacking", "hostile_to", "faction", "type",
                                     "&",         "|",       "!"};
}  // namespace

const char* KindName(Kind k) { return kKindNames[(int)k]; }
bool KindByName(const std::string& s, Kind& out) {
  for (int i = 0; i < 3; i++)
    if (s == kKindNames[i]) {
      out = (Kind)i;
      return true;
    }
  return false;
}
const char* VerbName(Verb v) { return kVerbNames[(int)v]; }
bool VerbByName(const std::string& s, Verb& out) {
  for (int i = 0; i < (int)Verb::None; i++)
    if (s == kVerbNames[i]) {
      out = (Verb)i;
      return true;
    }
  return false;
}
const std::vector<Verb>& VerbsFor(Kind k) {
  static const std::vector<Verb> duty = {Verb::Follow, Verb::Guard, Verb::Goto, Verb::Fetch,
                                         Verb::Spin,   Verb::Return};
  static const std::vector<Verb> forbid = {Verb::Attack, Verb::Cast, Verb::Leave, Verb::Cause};
  static const std::vector<Verb> penalty = {Verb::Attack, Verb::Leave, Verb::Harm};
  return k == Kind::Duty ? duty : k == Kind::Forbid ? forbid : penalty;
}
bool VerbFits(Kind k, Verb v) {
  for (Verb x : VerbsFor(k))
    if (x == v) return true;
  return false;
}
const char* ThenName(Then t) { return kThenNames[(int)t]; }
bool ThenByName(const std::string& s, Then& out) {
  for (int i = 1; i < 4; i++)
    if (s == kThenNames[i]) {
      out = (Then)i;
      return true;
    }
  if (s.empty()) {
    out = Then::None;
    return true;
  }
  return false;
}
const char* SelOpName(SelOp o) { return kSelNames[(int)o]; }

// ---- the selector parser ---------------------------------------------------------------
//
//   expr   := term ('|' term)*
//   term   := factor ('&' factor)*
//   factor := '!' factor | '(' expr ')' | word | word '(' expr ')' | word '(' name ')'

namespace {

struct Lexer {
  const std::string& s;
  size_t i = 0;
  void Skip() {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
  }
  bool Eat(char c) {
    Skip();
    if (i < s.size() && s[i] == c) {
      i++;
      return true;
    }
    return false;
  }
  bool AtEnd() {
    Skip();
    return i >= s.size();
  }
  std::string Word() {
    Skip();
    size_t j = i;
    while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-' ||
                            s[j] == ':'))
      j++;
    std::string w = s.substr(i, j - i);
    i = j;
    return w;
  }
};

struct SelParser {
  Lexer lx;
  Selector& out;
  std::string err;
  int Add(SelNode n) {
    if ((int)out.nodes.size() >= kMaxNodes) {
      if (err.empty()) err = "more than " + std::to_string(kMaxNodes) + " terms";
      return -1;
    }
    out.nodes.push_back(std::move(n));
    return (int)out.nodes.size() - 1;
  }
  int Expr() {
    int a = Term();
    while (a >= 0 && lx.Eat('|')) {
      const int b = Term();
      if (b < 0) return -1;
      SelNode n;
      n.op = SelOp::Or;
      n.a = (int16_t)a;
      n.b = (int16_t)b;
      a = Add(n);
    }
    return a;
  }
  int Term() {
    int a = Factor();
    while (a >= 0 && lx.Eat('&')) {
      const int b = Factor();
      if (b < 0) return -1;
      SelNode n;
      n.op = SelOp::And;
      n.a = (int16_t)a;
      n.b = (int16_t)b;
      a = Add(n);
    }
    return a;
  }
  int Factor() {
    if (lx.Eat('!')) {
      const int a = Factor();
      if (a < 0) return -1;
      SelNode n;
      n.op = SelOp::Not;
      n.a = (int16_t)a;
      return Add(n);
    }
    if (lx.Eat('(')) {
      const int a = Expr();
      if (a < 0) return -1;
      if (!lx.Eat(')')) {
        if (err.empty()) err = "missing ')'";
        return -1;
      }
      return a;
    }
    const std::string w = lx.Word();
    if (w.empty()) {
      if (err.empty())
        err = lx.AtEnd() ? "ends too soon" : std::string("unexpected '") + lx.s[lx.i] + "'";
      return -1;
    }
    SelNode n;
    if (w == "me") n.op = SelOp::Me;
    else if (w == "self") n.op = SelOp::Self;
    else if (w == "all" || w == "anyone") n.op = SelOp::All;
    else if (w == "attacked_by") n.op = SelOp::AttackedBy;
    else if (w == "attacking") n.op = SelOp::Attacking;
    else if (w == "hostile_to") n.op = SelOp::HostileTo;
    else if (w == "faction") n.op = SelOp::Faction;
    else if (w == "type") n.op = SelOp::Type;
    else {
      if (err.empty())
        err = "unknown word '" + w +
              "' (me, self, all, attacked_by, attacking, hostile_to, faction, type)";
      return -1;
    }
    const bool takesSet =
        n.op == SelOp::AttackedBy || n.op == SelOp::Attacking || n.op == SelOp::HostileTo;
    const bool takesName = n.op == SelOp::Faction || n.op == SelOp::Type;
    if (takesSet || takesName) {
      if (!lx.Eat('(')) {
        if (err.empty()) err = w + " needs (...)";
        return -1;
      }
      if (takesName) {
        n.name = lx.Word();
        if (n.name.empty()) {
          if (err.empty()) err = w + "( ) needs a name";
          return -1;
        }
      } else {
        const int a = Expr();
        if (a < 0) return -1;
        n.a = (int16_t)a;
      }
      if (!lx.Eat(')')) {
        if (err.empty()) err = "missing ')' after " + w;
        return -1;
      }
    }
    return Add(n);
  }
};

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

int CounterIndex(const std::vector<std::string>& counters, const std::string& n) {
  for (size_t i = 0; i < counters.size(); i++)
    if (counters[i] == n) return (int)i;
  return -1;
}

}  // namespace

bool ParseSelector(const std::string& text, Selector& out, std::string& err) {
  out = Selector{};
  if (Trim(text).empty()) return true;
  SelParser p{Lexer{text}, out, {}};
  const int root = p.Expr();
  if (root >= 0 && !p.lx.AtEnd() && p.err.empty())
    p.err = std::string("unexpected '") + text[p.lx.i] + "'";
  if (root < 0 || !p.err.empty()) {
    err = p.err.empty() ? "cannot read it" : p.err;
    out = Selector{};
    return false;
  }
  out.root = root;
  return true;
}

// ---- footprints (D6) ---------------------------------------------------------------------
//
//   fexpr  := fterm ('|' fterm)*
//   fterm  := ffact ('&' ffact)*
//   ffact  := '!' ffact | '(' fexpr ')' | atom
//   atom   := direct | affects_body ['(' who ')'] | creates '(' mat ')' | creates:mat
//           | alters '(' what ['(' who ')'] ')' | alters:what
//           | region_near '(' [who ','] N ')' | targets '(' x ')' | targets:x
//
// An atom's own actor (`(me)`) is read and dropped: the clause's `who` is the
// set the predicate is asked about.

namespace {

std::string NormAlters(std::string x) {
  const std::string suf = "_target";
  if (x.size() > suf.size() && x.compare(x.size() - suf.size(), suf.size(), suf) == 0)
    x.resize(x.size() - suf.size());
  return x;
}

struct FpParser {
  Lexer lx;
  FootPred& out;
  std::string err;
  int Add(FpNode n) {
    if ((int)out.nodes.size() >= kMaxNodes) {
      if (err.empty()) err = "more than " + std::to_string(kMaxNodes) + " terms";
      return -1;
    }
    out.nodes.push_back(std::move(n));
    return (int)out.nodes.size() - 1;
  }
  int Bin(FpOp op, int a, int b) {
    FpNode n;
    n.op = op;
    n.a = (int16_t)a;
    n.b = (int16_t)b;
    return Add(n);
  }
  int Expr() {
    int a = Term();
    while (a >= 0 && lx.Eat('|')) {
      const int b = Term();
      if (b < 0) return -1;
      a = Bin(FpOp::Or, a, b);
    }
    return a;
  }
  int Term() {
    int a = Factor();
    while (a >= 0 && lx.Eat('&')) {
      const int b = Factor();
      if (b < 0) return -1;
      a = Bin(FpOp::And, a, b);
    }
    return a;
  }
  // An optional actor argument, `(me)` / `(self)` / `(all)`: read and dropped.
  bool SkipActor() {
    const size_t at = lx.i;
    if (!lx.Eat('(')) return true;
    const std::string w = lx.Word();
    if (lx.Eat(')') && (w == "me" || w == "self" || w == "all" || w == "anyone")) return true;
    lx.i = at;
    return false;
  }
  int Factor() {
    if (lx.Eat('!')) {
      const int a = Factor();
      if (a < 0) return -1;
      FpNode n;
      n.op = FpOp::Not;
      n.a = (int16_t)a;
      return Add(n);
    }
    if (lx.Eat('(')) {
      const int a = Expr();
      if (a < 0) return -1;
      if (!lx.Eat(')')) {
        if (err.empty()) err = "missing ')'";
        return -1;
      }
      return a;
    }
    std::string w = lx.Word();
    if (w.empty()) {
      if (err.empty())
        err = lx.AtEnd() ? "ends too soon" : std::string("unexpected '") + lx.s[lx.i] + "'";
      return -1;
    }
    FpNode n;
    // D5's colon forms.
    const size_t colon = w.find(':');
    if (colon != std::string::npos) {
      const std::string k = w.substr(0, colon), v = w.substr(colon + 1);
      if (v.empty()) {
        if (err.empty()) err = "'" + w + "' names nothing";
        return -1;
      }
      if (k == "creates") n.op = FpOp::Creates;
      else if (k == "alters") n.op = FpOp::Alters;
      else if (k == "targets") n.op = FpOp::Targets;
      else {
        if (err.empty()) err = "unknown tag '" + w + "'";
        return -1;
      }
      n.name = n.op == FpOp::Alters ? NormAlters(v) : v;
      return Add(n);
    }
    if (w == "direct") {
      n.op = FpOp::Direct;
      if (!SkipActor() && err.empty()) err = "direct( ) takes an actor";
      return err.empty() ? Add(n) : -1;
    }
    if (w == "affects_body") {
      n.op = FpOp::AffectsBody;
      if (!SkipActor() && err.empty()) err = "affects_body( ) takes an actor";
      return err.empty() ? Add(n) : -1;
    }
    if (w == "creates" || w == "targets" || w == "alters") {
      n.op = w == "creates" ? FpOp::Creates : w == "targets" ? FpOp::Targets : FpOp::Alters;
      if (!lx.Eat('(')) {
        if (err.empty()) err = w + " needs (...)";
        return -1;
      }
      n.name = lx.Word();
      if (n.name.empty()) {
        if (err.empty()) err = w + "( ) needs a name";
        return -1;
      }
      if (n.op == FpOp::Alters) {
        n.name = NormAlters(n.name);
        if (!SkipActor()) {
          if (err.empty()) err = "alters(" + n.name + "( )) takes an actor";
          return -1;
        }
      }
      if (!lx.Eat(')')) {
        if (err.empty()) err = "missing ')' after " + w;
        return -1;
      }
      return Add(n);
    }
    if (w == "region_near") {
      n.op = FpOp::RegionNear;
      if (!lx.Eat('(')) {
        if (err.empty()) err = "region_near needs (metres)";
        return -1;
      }
      auto num = [&]() {
        lx.Skip();
        size_t j = lx.i;
        while (j < lx.s.size() && (std::isalnum((unsigned char)lx.s[j]) || lx.s[j] == '.' ||
                                   lx.s[j] == '_'))
          j++;
        std::string w2 = lx.s.substr(lx.i, j - lx.i);
        lx.i = j;
        return w2;
      };
      std::string first = num();
      if (lx.Eat(',')) first = num();   // region_near(me, 6)
      char* endp = nullptr;
      const double v = std::strtod(first.c_str(), &endp);
      if (first.empty() || endp == nullptr || *endp != 0 || v < 0) {
        if (err.empty()) err = "region_near( ) needs a distance in metres";
        return -1;
      }
      n.value = (float)v;
      if (!lx.Eat(')')) {
        if (err.empty()) err = "missing ')' after region_near";
        return -1;
      }
      return Add(n);
    }
    if (err.empty())
      err = "unknown footprint '" + w +
            "' (direct, affects_body, creates(x), alters(x), region_near(m), targets(x))";
    return -1;
  }
};

bool FpEval(const FootPred& p, int node, const FootFacts& f, int depth) {
  if (node < 0 || node >= (int)p.nodes.size() || depth > kMaxNodes) return false;
  const FpNode& n = p.nodes[(size_t)node];
  switch (n.op) {
    case FpOp::Direct: return f.direct && f.isTarget;
    case FpOp::AffectsBody: return f.affectsBody && f.hasActor && (f.isTarget || f.distM <= 0.0f);
    case FpOp::Creates:
      if (f.creates)
        for (const std::string& c : *f.creates)
          if (c == n.name) return true;
      return false;
    case FpOp::Alters: {
      if (f.alters == nullptr || f.alters->empty()) return false;
      const std::string a = *f.alters;
      const bool underTarget = NormAlters(a) != a;
      if (NormAlters(a) != n.name) return false;
      return !underTarget || f.isTarget;   // "under the target": about that actor
    }
    case FpOp::RegionNear: return f.hasActor && f.distM <= n.value;
    case FpOp::Targets: return f.targets != nullptr && *f.targets == n.name;
    case FpOp::And: return FpEval(p, n.a, f, depth + 1) && FpEval(p, n.b, f, depth + 1);
    case FpOp::Or: return FpEval(p, n.a, f, depth + 1) || FpEval(p, n.b, f, depth + 1);
    case FpOp::Not: return !FpEval(p, n.a, f, depth + 1);
  }
  return false;
}

}  // namespace

bool ParseFootPred(const std::string& text, FootPred& out, std::string& err) {
  out = FootPred{};
  if (Trim(text).empty()) return true;
  FpParser p{Lexer{text}, out, {}};
  const int root = p.Expr();
  if (root >= 0 && !p.lx.AtEnd() && p.err.empty())
    p.err = std::string("unexpected '") + text[p.lx.i] + "'";
  if (root < 0 || !p.err.empty()) {
    err = p.err.empty() ? "cannot read it" : p.err;
    out = FootPred{};
    return false;
  }
  out.root = root;
  return true;
}

bool FootHolds(const FootPred& p, const FootFacts& f) {
  if (p.Empty()) return true;
  return FpEval(p, p.root, f, 0);
}

std::string FetchMaterial(const std::string& arg) {
  const std::string t = Trim(arg);
  const size_t sp = t.find_first_of(" \t");
  return sp == std::string::npos ? t : t.substr(0, sp);
}

// ---- triggers --------------------------------------------------------------------------

bool CmpHolds(Cmp op, int32_t a, int32_t b) {
  switch (op) {
    case Cmp::Lt: return a < b;
    case Cmp::Le: return a <= b;
    case Cmp::Gt: return a > b;
    case Cmp::Ge: return a >= b;
    case Cmp::Eq: return a == b;
    case Cmp::Ne: return a != b;
  }
  return false;
}

bool ParseTrigger(const std::string& text, const std::vector<std::string>& counters,
                  std::vector<Cond>& out, std::string& err) {
  out.clear();
  const std::string t = Trim(text);
  if (t.empty()) return true;
  // Split on '&' at paren depth 0 (count(...) may hold a selector with '&').
  std::vector<std::string> parts;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i <= t.size(); i++) {
    if (i == t.size() || (t[i] == '&' && depth == 0)) {
      parts.push_back(Trim(t.substr(start, i - start)));
      start = i + 1;
      continue;
    }
    if (t[i] == '(') depth++;
    if (t[i] == ')') depth--;
  }
  if ((int)parts.size() > kMaxConds) {
    err = "more than " + std::to_string(kMaxConds) + " conditions";
    return false;
  }
  for (const std::string& p : parts) {
    // <fact> <op> <int>: the op is the LAST run of <>=! outside parens.
    int d = 0;
    size_t opAt = std::string::npos, opLen = 0;
    for (size_t i = 0; i < p.size(); i++) {
      if (p[i] == '(') d++;
      if (p[i] == ')') d--;
      if (d == 0 && (p[i] == '<' || p[i] == '>' || p[i] == '=' || p[i] == '!')) {
        opAt = i;
        opLen = (i + 1 < p.size() && p[i + 1] == '=') ? 2 : 1;
        break;
      }
    }
    if (opAt == std::string::npos) {
      err = "'" + p + "' needs a comparison (dist < 6, count(hostile_to(me)) > 0, ...)";
      return false;
    }
    const std::string lhs = Trim(p.substr(0, opAt)), ops = p.substr(opAt, opLen),
                      rhs = Trim(p.substr(opAt + opLen));
    Cond c;
    if (ops == "<") c.op = Cmp::Lt;
    else if (ops == "<=") c.op = Cmp::Le;
    else if (ops == ">") c.op = Cmp::Gt;
    else if (ops == ">=") c.op = Cmp::Ge;
    else if (ops == "==" || ops == "=") c.op = Cmp::Eq;
    else if (ops == "!=") c.op = Cmp::Ne;
    else {
      err = "'" + ops + "' is not a comparison";
      return false;
    }
    char* endp = nullptr;
    const long v = std::strtol(rhs.c_str(), &endp, 10);
    if (rhs.empty() || endp == nullptr || *endp != 0) {
      err = "'" + rhs + "' is not a whole number";
      return false;
    }
    c.value = (int32_t)std::clamp<long>(v, -1000000, 1000000);
    if (lhs == "dist") c.fact = FactKind::Dist;
    else if (lhs == "hp") c.fact = FactKind::Hp;
    else if (lhs == "demon_hp") c.fact = FactKind::DemonHp;
    else if (lhs == "term") c.fact = FactKind::Term;
    else if (lhs.rfind("count(", 0) == 0 && lhs.back() == ')') {
      c.fact = FactKind::Count;
      std::string e;
      if (!ParseSelector(lhs.substr(6, lhs.size() - 7), c.sel, e)) {
        err = "count(): " + e;
        return false;
      }
    } else if (CounterIndex(counters, lhs) >= 0) {
      c.fact = FactKind::Counter;
      c.counter = CounterIndex(counters, lhs);
    } else {
      err = "unknown fact '" + lhs + "' (dist, hp, demon_hp, term, count(...), or a counter)";
      return false;
    }
    out.push_back(std::move(c));
  }
  return true;
}

bool ParseEffect(const std::string& text, const std::vector<std::string>& counters, Effect& out,
                 std::string& err) {
  out = Effect{};
  const std::string t = Trim(text);
  if (t.empty()) return true;
  const size_t at = t.find_first_of("+-=");
  if (at == std::string::npos || at == 0) {
    err = "'" + t + "' is <counter>+N, <counter>-N or <counter>=N";
    return false;
  }
  const std::string name = Trim(t.substr(0, at));
  const int ci = CounterIndex(counters, name);
  if (ci < 0) {
    err = "no counter '" + name + "' on this page";
    return false;
  }
  char* endp = nullptr;
  const std::string num = Trim(t.substr(at + 1));
  const long v = std::strtol(num.c_str(), &endp, 10);
  if (num.empty() || *endp != 0) {
    err = "'" + num + "' is not a whole number";
    return false;
  }
  out.counter = ci;
  out.op = t[at];
  out.value = (int32_t)std::clamp<long>(v, -1000000, 1000000);
  return true;
}

// ---- compile ----------------------------------------------------------------------------

bool Compile(Page& p, std::vector<std::string>& errs) {
  errs.clear();
  if (p.name.empty()) errs.push_back("the page has no name");
  if ((int)p.name.size() > kMaxNameLen)
    errs.push_back("the name is longer than " + std::to_string(kMaxNameLen) + " letters");
  if ((int)p.clauses.size() > kMaxClauses)
    errs.push_back("more than " + std::to_string(kMaxClauses) + " clauses");
  if ((int)p.counters.size() > kMaxCounters)
    errs.push_back("more than " + std::to_string(kMaxCounters) + " counters");
  if (p.hours < 0) p.hours = 0;
  for (size_t i = 0; i < p.clauses.size(); i++) {
    Clause& c = p.clauses[i];
    const std::string at = "clause " + std::to_string(i + 1) + ": ";
    std::string e;
    if (!VerbFits(c.kind, c.verb))
      errs.push_back(at + "a " + KindName(c.kind) + " cannot be '" + VerbName(c.verb) + "'");
    if (!ParseSelector(c.who, c.sel, e)) errs.push_back(at + "who: " + e);
    e.clear();
    if (!ParseTrigger(c.when, p.counters, c.conds, e)) errs.push_back(at + "when: " + e);
    e.clear();
    if (!ParseEffect(c.on, p.counters, c.effect, e)) errs.push_back(at + "on: " + e);
    const bool needsWho = c.verb == Verb::Follow || c.verb == Verb::Guard ||
                          c.verb == Verb::Attack || c.verb == Verb::Cast ||
                          c.verb == Verb::Leave || c.verb == Verb::Harm || c.verb == Verb::Return;
    if (needsWho && c.sel.Empty()) errs.push_back(at + VerbName(c.verb) + " needs a who");
    if (c.verb == Verb::Fetch && FetchMaterial(c.arg).empty())
      errs.push_back(at + "fetch needs a material");
    if (c.kind == Kind::Penalty && c.then == Then::None)
      errs.push_back(at + "a penalty needs a consequence (dismiss, destroy, pain)");
    c.fp = FootPred{};
    c.argNum = 0;
    if (c.verb == Verb::Cast || c.verb == Verb::Cause) {
      e.clear();
      if (!ParseFootPred(c.arg, c.fp, e)) errs.push_back(at + VerbName(c.verb) + ": " + e);
      else if (c.verb == Verb::Cause && c.fp.Empty())
        errs.push_back(at + "cause needs a footprint (alters(ground_under), creates(lava), ...)");
    }
    if (c.verb == Verb::Leave && std::atoi(c.arg.c_str()) <= 0)
      errs.push_back(at + "leave needs a distance in metres");
    if (c.verb == Verb::Leave || c.verb == Verb::Follow) c.argNum = std::max(0, std::atoi(c.arg.c_str()));
    if (c.verb == Verb::Harm || c.verb == Verb::Return) {
      const int v = Trim(c.arg).empty() ? (c.verb == Verb::Harm ? 10 : 30) : std::atoi(c.arg.c_str());
      if (v <= 0) errs.push_back(at + VerbName(c.verb) + " needs a time in seconds");
      c.argNum = std::max(1, v);
    }
  }
  return errs.empty();
}

uint32_t PageHash(const std::string& name) {
  uint32_t h = 2166136261u;
  for (char ch : name) h = (h ^ (uint8_t)ch) * 16777619u;
  return h == 0 ? 1u : h;
}

// ---- weight -----------------------------------------------------------------------------

int32_t SelectorBreadth(const Selector& s, const Tariff& t) {
  int32_t b = 0;
  for (const SelNode& n : s.nodes) b += t.selOp[(int)n.op];
  return b;
}

int32_t UpkeepFor(int32_t power, const Tariff& t) {
  return std::max(0, (int32_t)(((int64_t)power * t.upkeepPerPowerPct + 99) / 100));
}

Weight Weigh(const Page& p, const Tariff& t) {
  Weight w;
  auto line = [&](std::string label, int32_t v) {
    w.lines.push_back({std::move(label), v});
    w.total += v;
  };
  // Term.
  if (p.hours <= 0) {
    line("term: indefinite", t.indefinite);
  } else {
    int32_t v = t.term.empty() ? 0 : t.term.back().second;
    for (const auto& [h, cost] : t.term)
      if (p.hours <= h) {
        v = cost;
        break;
      }
    char buf[48];
    if (p.hours % 24 == 0)
      std::snprintf(buf, sizeof buf, "term: %d day%s", p.hours / 24, p.hours == 24 ? "" : "s");
    else
      std::snprintf(buf, sizeof buf, "term: %d hours", p.hours);
    line(buf, v);
  }
  for (size_t i = 0; i < p.clauses.size(); i++) {
    const Clause& c = p.clauses[i];
    int32_t v = 0;
    const int vi = (int)c.verb;
    // The tables' columns (contract_tariff.json): duty follow..spin, return;
    // forbid / penalty attack cast leave, then cause (forbid) / harm (penalty).
    const int dutyCol = vi <= (int)Verb::Spin ? vi : c.verb == Verb::Return ? 5 : -1;
    const int actCol = (vi >= (int)Verb::Attack && vi <= (int)Verb::Leave) ? vi - (int)Verb::Attack
                       : (c.verb == Verb::Cause || c.verb == Verb::Harm)  ? 3
                                                                           : -1;
    if (c.kind == Kind::Duty && dutyCol >= 0) v = t.duty[dutyCol];
    else if (c.kind == Kind::Forbid && actCol >= 0) v = t.forbid[actCol];
    else if (c.kind == Kind::Penalty && actCol >= 0)
      v = t.penaltyAct[actCol] + t.then[(int)c.then];
    const int32_t breadth = SelectorBreadth(c.sel, t);
    v += (int32_t)(((int64_t)breadth * t.breadthPct[(int)c.kind]) / 100);
    v += (int32_t)c.conds.size() * t.perCondition;
    line(Describe(c), std::max(0, v));
  }
  if (!p.counters.empty()) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%zu counter%s", p.counters.size(),
                  p.counters.size() == 1 ? "" : "s");
    line(buf, (int32_t)p.counters.size() * t.perCounter);
  }
  return w;
}

// ---- describing ------------------------------------------------------------------------

std::string Describe(const Clause& c) {
  std::string s = KindName(c.kind);
  s += ": ";
  s += VerbName(c.verb);
  if (!Trim(c.arg).empty() && (c.verb == Verb::Fetch || c.verb == Verb::Goto ||
                                c.verb == Verb::Cast || c.verb == Verb::Cause))
    s += " " + Trim(c.arg);
  if (!Trim(c.into).empty() && c.verb == Verb::Fetch) s += " into " + Trim(c.into);
  if (!Trim(c.who).empty()) s += " " + Trim(c.who);
  if (c.verb == Verb::Leave && !Trim(c.arg).empty()) s += " > " + Trim(c.arg) + " m";
  if (c.verb == Verb::Follow && !Trim(c.arg).empty()) s += " within " + Trim(c.arg) + " m";
  if ((c.verb == Verb::Harm || c.verb == Verb::Return) && !Trim(c.arg).empty())
    s += (c.verb == Verb::Harm ? " within " : " by ") + Trim(c.arg) + " s";
  if (c.kind == Kind::Penalty && c.then != Then::None) s += std::string(" -> ") + ThenName(c.then);
  if (!Trim(c.when).empty()) s += " when " + Trim(c.when);
  return s;
}

// ---- JSON ------------------------------------------------------------------------------

namespace {

bool PageFromJ(const json& j, Page& p, std::string& err) {
  p = Page{};
  if (!j.is_object()) {
    err = "a page is an object";
    return false;
  }
  p.name = j.value("name", std::string());
  p.hours = j.value("hours", 24);
  for (const json& c : j.value("counters", json::array()))
    if (c.is_string()) p.counters.push_back(c.get<std::string>());
  for (const json& c : j.value("clauses", json::array())) {
    if (!c.is_object()) continue;
    Clause cl;
    const std::string k = c.value("kind", std::string("duty"));
    if (!KindByName(k, cl.kind)) {
      err = "kind '" + k + "' is not duty | forbid | penalty";
      return false;
    }
    const std::string v = c.value("verb", std::string());
    if (!VerbByName(v, cl.verb)) {
      err = "verb '" + v + "' is unknown";
      return false;
    }
    cl.who = c.value("who", std::string());
    cl.when = c.value("when", std::string());
    cl.arg = c.value("arg", std::string());
    cl.into = c.value("into", std::string());
    const std::string th = c.value("then", std::string());
    if (!ThenByName(th, cl.then)) {
      err = "then '" + th + "' is not dismiss | destroy | pain";
      return false;
    }
    cl.on = c.value("on", std::string());
    p.clauses.push_back(std::move(cl));
  }
  std::vector<std::string> errs;
  if (!Compile(p, errs)) {
    err = p.name + ": " + errs.front();
    return false;
  }
  return true;
}

json PageToJ(const Page& p) {
  json j;
  j["name"] = p.name;
  j["hours"] = p.hours;
  if (!p.counters.empty()) j["counters"] = p.counters;
  json cs = json::array();
  for (const Clause& c : p.clauses) {
    json o;
    o["kind"] = KindName(c.kind);
    o["verb"] = VerbName(c.verb);
    if (!c.who.empty()) o["who"] = c.who;
    if (!c.when.empty()) o["when"] = c.when;
    if (!c.arg.empty()) o["arg"] = c.arg;
    if (!c.into.empty()) o["into"] = c.into;
    if (c.then != Then::None) o["then"] = ThenName(c.then);
    if (!c.on.empty()) o["on"] = c.on;
    cs.push_back(o);
  }
  j["clauses"] = cs;
  return j;
}

json ReadJson(const std::string& path, std::string& log) {
  try {
    std::ifstream f(path);
    if (!f) {
      log += "contracts: cannot read " + path + "\n";
      return json();
    }
    return json::parse(f, nullptr, true, true);
  } catch (const std::exception& ex) {
    log += "contracts: " + path + ": " + ex.what() + "\n";
    return json();
  }
}

void LoadTariff(const json& j, Tariff& t) {
  if (!j.is_object()) return;
  if (j.contains("term") && j["term"].is_array()) {
    t.term.clear();
    for (const json& b : j["term"])
      if (b.is_object())
        t.term.push_back({std::max(1, b.value("hours", 24)), std::max(0, b.value("weight", 0))});
    std::sort(t.term.begin(), t.term.end());
  }
  t.indefinite = std::max(0, j.value("indefinite", t.indefinite));
  auto table = [&](const char* key, int32_t* dst, std::initializer_list<const char*> names) {
    if (!j.contains(key) || !j[key].is_object()) return;
    int i = 0;
    for (const char* n : names) {
      dst[i] = std::max(0, j[key].value(n, dst[i]));
      i++;
    }
  };
  table("duty", t.duty, {"follow", "guard", "goto", "fetch", "spin", "return"});
  table("forbid", t.forbid, {"attack", "cast", "leave", "cause"});
  table("penalty", t.penaltyAct, {"attack", "cast", "leave", "harm"});
  table("then", t.then, {"none", "dismiss", "destroy", "pain"});
  table("selector", t.selOp,
        {"me", "self", "all", "attacked_by", "attacking", "hostile_to", "faction", "type", "and",
         "or", "not"});
  table("breadthPct", t.breadthPct, {"duty", "forbid", "penalty"});
  t.perCondition = std::max(0, j.value("perCondition", t.perCondition));
  t.perCounter = std::max(0, j.value("perCounter", t.perCounter));
  t.upkeepPerPowerPct = std::clamp(j.value("upkeepPerPowerPct", t.upkeepPerPowerPct), 0, 10000);
}

}  // namespace

bool PageFromJson(const std::string& text, Page& out, std::string& err) {
  try {
    return PageFromJ(json::parse(text, nullptr, true, true), out, err);
  } catch (const std::exception& ex) {
    err = ex.what();
    return false;
  }
}

std::string PageToJson(const Page& p) { return PageToJ(p).dump(); }

const Page* Content::FindStock(const std::string& name) const {
  for (const Page& p : stock)
    if (p.name == name) return &p;
  return nullptr;
}

const Content& GetContent(bool reload) {
  static Content c;
  if (c.loaded && !reload) return c;
  Content n;
  n.loaded = true;
  const std::string dir = sandvox::AssetDir() + "/demons/";
  LoadTariff(ReadJson(dir + "contract_tariff.json", n.log), n.tariff);
  const json s = ReadJson(dir + "contracts.json", n.log);
  for (const json& pj : s.value("stock", json::array())) {
    Page p;
    std::string err;
    if (!PageFromJ(pj, p, err)) {
      n.log += "contracts: contracts.json: " + err + "\n";
      continue;
    }
    p.stock = true;
    n.stock.push_back(std::move(p));
  }
  if (!n.log.empty() && n.log != c.log) std::fprintf(stderr, "%s", n.log.c_str());
  c = std::move(n);
  return c;
}

// ---- bytes -----------------------------------------------------------------------------

namespace {
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
}
void PutStr(std::vector<uint8_t>& out, const std::string& s) {
  PutU32(out, (uint32_t)s.size());
  out.insert(out.end(), s.begin(), s.end());
}
bool GetU32(const uint8_t*& d, const uint8_t* end, uint32_t& v) {
  if (end - d < 4) return false;
  v = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
  d += 4;
  return true;
}
bool GetStr(const uint8_t*& d, const uint8_t* end, std::string& s) {
  uint32_t n = 0;
  if (!GetU32(d, end, n) || n > 4096 || (size_t)(end - d) < n) return false;
  s.assign((const char*)d, n);
  d += n;
  return true;
}
}  // namespace

void PutPage(std::vector<uint8_t>& out, const Page& p) {
  PutStr(out, p.name);
  PutU32(out, (uint32_t)p.hours);
  PutU32(out, p.stock ? 1u : 0u);
  PutU32(out, (uint32_t)p.counters.size());
  for (const std::string& c : p.counters) PutStr(out, c);
  PutU32(out, (uint32_t)p.clauses.size());
  for (const Clause& c : p.clauses) {
    PutU32(out, (uint32_t)c.kind);
    PutU32(out, (uint32_t)c.verb);
    PutU32(out, (uint32_t)c.then);
    PutStr(out, c.who);
    PutStr(out, c.when);
    PutStr(out, c.arg);
    PutStr(out, c.into);
    PutStr(out, c.on);
  }
}

bool GetPage(const uint8_t*& d, const uint8_t* end, Page& p) {
  p = Page{};
  uint32_t hours = 0, stock = 0, n = 0;
  if (!GetStr(d, end, p.name) || !GetU32(d, end, hours) || !GetU32(d, end, stock) ||
      !GetU32(d, end, n) || n > (uint32_t)kMaxCounters)
    return false;
  p.hours = (int32_t)hours;
  p.stock = stock != 0;
  p.counters.resize(n);
  for (std::string& c : p.counters)
    if (!GetStr(d, end, c)) return false;
  if (!GetU32(d, end, n) || n > (uint32_t)kMaxClauses) return false;
  p.clauses.resize(n);
  for (Clause& c : p.clauses) {
    uint32_t k = 0, v = 0, t = 0;
    if (!GetU32(d, end, k) || !GetU32(d, end, v) || !GetU32(d, end, t) || k > 2 ||
        v >= (uint32_t)Verb::None || t > 3)
      return false;
    c.kind = (Kind)k;
    c.verb = (Verb)v;
    c.then = (Then)t;
    if (!GetStr(d, end, c.who) || !GetStr(d, end, c.when) || !GetStr(d, end, c.arg) ||
        !GetStr(d, end, c.into) || !GetStr(d, end, c.on))
      return false;
  }
  std::vector<std::string> errs;
  Compile(p, errs);   // a page that no longer compiles is kept, flagged by Compile's caller
  return true;
}

}  // namespace contract
