// dialogue.cpp — see dialogue.h for the schema and the three layers.

#include "game/dialogue.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/session.h"
#include "sim/tickinput.h"
#include "sim/world.h"
#include "test/support.h"

namespace dialogue {

using json = nlohmann::json;

// ---- small things ------------------------------------------------------------

const Node* Dialogue::Find(const std::string& id) const {
  for (const Node& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

std::string Problem::Line() const {
  std::string s = file;
  if (!node.empty()) s += ": " + node;
  if (!field.empty()) s += (node.empty() ? ": " : " ") + field;
  s += ": " + msg;
  return (error ? "error: " : "warning: ") + s;
}

const std::vector<std::string>& KnownActivities() {
  static const std::vector<std::string> k = {"sleep", "work",      "wander",
                                             "socialize", "eat", "goto"};
  return k;
}

namespace {

// "HH:MM" -> minutes after midnight, or -1.
int ParseClock(const std::string& s) {
  int h = 0, m = 0;
  char tail = 0;
  if (std::sscanf(s.c_str(), "%d:%d%c", &h, &m, &tail) != 2) return -1;
  if (h < 0 || h > 24 || m < 0 || m > 59 || (h == 24 && m != 0)) return -1;
  return h * 60 + m;
}

struct Where {
  const std::string& file;
  std::string node;
  std::vector<Problem>& out;
  void Err(const std::string& field, const std::string& msg) const {
    out.push_back(Problem{true, file, node, field, msg});
  }
  void Warn(const std::string& field, const std::string& msg) const {
    out.push_back(Problem{false, file, node, field, msg});
  }
};

bool ParseConds(const json& j, const Where& w, const std::string& field,
                std::vector<Cond>& out) {
  if (j.is_null()) return true;
  if (!j.is_array()) {
    w.Err(field, "must be a list of conditions, e.g. [ {\"flag\": \"x\"} ]");
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < j.size(); i++) {
    const json& c = j[i];
    const std::string f = field + "[" + std::to_string(i) + "]";
    if (!c.is_object()) {
      w.Err(f, "a condition is an object like {\"flag\": \"x\"}");
      ok = false;
      continue;
    }
    Cond cd;
    int kinds = 0;
    for (auto it = c.begin(); it != c.end(); ++it) {
      std::string k = it.key();
      if (k == "value" || k == "count") continue;
      bool neg = false;
      if (!k.empty() && k[0] == '!') {
        neg = true;
        k = k.substr(1);
      }
      kinds++;
      cd.negate = neg;
      const json& v = it.value();
      if (k == "flag") {
        cd.kind = Cond::Kind::Flag;
        if (!v.is_string() || v.get<std::string>().empty()) {
          w.Err(f + "." + it.key(), "names a flag (a non-empty string)");
          ok = false;
        } else {
          cd.arg = v.get<std::string>();
        }
      } else if (k == "time") {
        cd.kind = Cond::Kind::Time;
        std::string a, b;
        if (v.is_array() && v.size() == 2 && v[0].is_string() && v[1].is_string()) {
          a = v[0].get<std::string>();
          b = v[1].get<std::string>();
        } else if (v.is_string()) {
          const std::string s = v.get<std::string>();
          const size_t d = s.find('-');
          if (d != std::string::npos) {
            a = s.substr(0, d);
            b = s.substr(d + 1);
          }
        }
        cd.fromMin = ParseClock(a);
        cd.toMin = ParseClock(b);
        if (cd.fromMin < 0 || cd.toMin < 0) {
          w.Err(f + ".time", "is [\"HH:MM\", \"HH:MM\"] (from, to; wraps midnight)");
          ok = false;
        }
      } else if (k == "activity") {
        cd.kind = Cond::Kind::Activity;
        if (!v.is_string() || v.get<std::string>().empty()) {
          w.Err(f + "." + it.key(), "names an activity (sleep, work, wander, ...)");
          ok = false;
        } else {
          cd.arg = v.get<std::string>();
        }
      } else if (k == "has") {
        cd.kind = Cond::Kind::Has;
        if (!v.is_string() || v.get<std::string>().empty()) {
          w.Err(f + "." + it.key(), "names an item");
          ok = false;
        } else {
          cd.arg = v.get<std::string>();
        }
      } else if (k == "met") {
        cd.kind = Cond::Kind::Met;
        if (v.is_boolean()) {
          // {"met": false} is {"!met": true}: one spelling fewer to remember.
          if (!v.get<bool>()) cd.negate = !cd.negate;
          cd.arg.clear();
        } else if (v.is_string() && !v.get<std::string>().empty()) {
          cd.arg = v.get<std::string>();
        } else {
          w.Err(f + "." + it.key(), "is true (this dialogue) or a dialogue name");
          ok = false;
        }
      } else {
        w.Err(f + "." + it.key(),
              "unknown condition (flag, !flag, time, activity, has, met)");
        ok = false;
      }
    }
    if (kinds != 1) {
      w.Err(f, kinds == 0 ? "has no condition key" : "has more than one condition key; "
                                                       "use one object per condition");
      ok = false;
      continue;
    }
    if (c.contains("value")) {
      if (!c["value"].is_number_integer() || cd.kind != Cond::Kind::Flag) {
        w.Err(f + ".value", "is an integer, and only on a flag condition");
        ok = false;
      } else {
        cd.hasValue = true;
        cd.value = c["value"].get<int>();
      }
    }
    if (c.contains("count")) {
      if (!c["count"].is_number_integer() || cd.kind != Cond::Kind::Has ||
          c["count"].get<int>() < 1) {
        w.Err(f + ".count", "is a positive integer, and only on a has condition");
        ok = false;
      } else {
        cd.count = c["count"].get<int>();
      }
    }
    out.push_back(cd);
  }
  return ok;
}

bool ParseActs(const json& j, const Where& w, const std::string& field,
               std::vector<Act>& out) {
  if (j.is_null()) return true;
  if (!j.is_array()) {
    w.Err(field, "must be a list of actions, e.g. [ {\"set\": \"x\"} ]");
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < j.size(); i++) {
    const json& a = j[i];
    const std::string f = field + "[" + std::to_string(i) + "]";
    if (!a.is_object()) {
      w.Err(f, "an action is an object like {\"set\": \"x\"}");
      ok = false;
      continue;
    }
    Act ac;
    int kinds = 0;
    for (auto it = a.begin(); it != a.end(); ++it) {
      const std::string& k = it.key();
      if (k == "value" || k == "count") continue;
      kinds++;
      const json& v = it.value();
      if (k == "set" || k == "add" || k == "clear" || k == "give" || k == "take") {
        ac.kind = k == "set"     ? Act::Kind::Set
                  : k == "add"   ? Act::Kind::Add
                  : k == "clear" ? Act::Kind::Clear
                  : k == "give"  ? Act::Kind::Give
                                 : Act::Kind::Take;
        if (!v.is_string() || v.get<std::string>().empty()) {
          w.Err(f + "." + k, (k == "give" || k == "take") ? "names an item"
                                                           : "names a flag");
          ok = false;
        } else {
          ac.arg = v.get<std::string>();
        }
      } else if (k == "grant" || k == "learn") {
        // demons D2: a glyph by NAME (grant) / a name the player now knows
        // (learn -> the world flag "name:<arg>").
        ac.kind = k == "grant" ? Act::Kind::Grant : Act::Kind::Learn;
        if (!v.is_string() || v.get<std::string>().empty()) {
          w.Err(f + "." + k, k == "grant" ? "names a glyph (glyphs.json id)" : "names a name");
          ok = false;
        } else {
          ac.arg = v.get<std::string>();
        }
      } else if (k == "present_contract" || k == "release" || k == "dismiss") {
        // demons D5: a demon conversation's own verbs (Store::demonActs).
        ac.kind = k == "present_contract" ? Act::Kind::PresentContract
                  : k == "release"        ? Act::Kind::Release
                                          : Act::Kind::Dismiss;
        if (!v.is_boolean() || !v.get<bool>()) {
          w.Err(f + "." + k, "is true");
          ok = false;
        }
      } else if (k == "end") {
        ac.kind = Act::Kind::End;
        if (!v.is_boolean() || !v.get<bool>()) {
          w.Err(f + ".end", "is true");
          ok = false;
        }
      } else {
        w.Err(f + "." + k, "unknown action (set, add, clear, give, take, end, grant, learn, "
                           "present_contract, release, dismiss)");
        ok = false;
      }
    }
    if (kinds != 1) {
      w.Err(f, kinds == 0 ? "has no action key" : "has more than one action key");
      ok = false;
      continue;
    }
    const bool items = ac.kind == Act::Kind::Give || ac.kind == Act::Kind::Take;
    ac.value = 1;
    const char* numKey = items ? "count" : "value";
    const char* otherKey = items ? "value" : "count";
    if (a.contains(otherKey)) {
      w.Err(f + "." + otherKey, items ? "give/take use \"count\"" : "set/add use \"value\"");
      ok = false;
    }
    if (a.contains(numKey)) {
      if (!a[numKey].is_number_integer() || ac.kind == Act::Kind::Clear ||
          ac.kind == Act::Kind::End || ac.kind == Act::Kind::Grant ||
          ac.kind == Act::Kind::Learn || ac.kind == Act::Kind::PresentContract ||
          ac.kind == Act::Kind::Release || ac.kind == Act::Kind::Dismiss ||
          (items && a[numKey].get<int>() < 1)) {
        w.Err(f + "." + numKey, "is an integer (positive for give/take), not on clear/end");
        ok = false;
      } else {
        ac.value = a[numKey].get<int>();
      }
    }
    out.push_back(ac);
  }
  return ok;
}

std::string Str(const json& o, const char* key) {
  auto it = o.find(key);
  return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}  // namespace

// ---- parse -------------------------------------------------------------------

bool Library::Parse(const std::string& name, const std::string& text,
                    const std::string& file, Dialogue& out,
                    std::vector<Problem>& problems) const {
  const std::string fname = std::filesystem::path(file).filename().string();
  Where top{fname, "", problems};
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception& e) {
    top.Err("", std::string("not valid JSON: ") + e.what());
    return false;
  }
  if (!j.is_object()) {
    top.Err("", "the file must be one JSON object");
    return false;
  }
  const size_t before = problems.size();
  out = Dialogue{};
  out.name = name;
  out.file = file;
  if (j.contains("name") && Str(j, "name") != name)
    top.Warn("name", "\"" + Str(j, "name") + "\" differs from the file name \"" + name +
                         "\"; the FILE name is what everything refers to it by");
  out.speaker = Str(j, "speaker");
  if (j.contains("canLeave")) {
    if (!j["canLeave"].is_boolean())
      top.Err("canLeave", "is true or false");
    else
      out.canLeave = j["canLeave"].get<bool>();
  }
  static const std::unordered_set<std::string> kTop = {"name", "speaker", "canLeave",
                                                        "entry", "nodes", "notes"};
  for (auto it = j.begin(); it != j.end(); ++it)
    if (!kTop.count(it.key())) top.Warn(it.key(), "unknown key (kept, ignored)");

  const json* entry = j.contains("entry") ? &j["entry"] : nullptr;
  if (!entry || !entry->is_array() || entry->empty()) {
    top.Err("entry", "needs at least one entry, e.g. [ {\"goto\": \"start\"} ]");
  } else {
    for (size_t i = 0; i < entry->size(); i++) {
      const json& e = (*entry)[i];
      Where w{fname, "entry[" + std::to_string(i) + "]", problems};
      if (!e.is_object()) {
        w.Err("", "is an object {\"if\": [...], \"goto\": \"node\"}");
        continue;
      }
      Entry en;
      en.gotoId = Str(e, "goto");
      if (en.gotoId.empty()) w.Err("goto", "names the node this entry starts at");
      ParseConds(e.contains("if") ? e["if"] : json(), w, "if", en.conds);
      out.entries.push_back(std::move(en));
    }
  }

  const json* nodes = j.contains("nodes") ? &j["nodes"] : nullptr;
  if (!nodes || !nodes->is_array() || nodes->empty()) {
    top.Err("nodes", "needs at least one node");
  } else {
    static const std::unordered_set<std::string> kNode = {
        "id", "speaker", "text", "if", "else", "do", "choices", "goto", "canLeave", "notes",
        "pos"};
    static const std::unordered_set<std::string> kChoice = {"text", "goto", "if", "do",
                                                             "notes"};
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < nodes->size(); i++) {
      const json& n = (*nodes)[i];
      Where w{fname, "nodes[" + std::to_string(i) + "]", problems};
      if (!n.is_object()) {
        w.Err("", "a node is an object with an \"id\" and \"text\"");
        continue;
      }
      Node nd;
      nd.id = Str(n, "id");
      if (!nd.id.empty()) w.node = "node '" + nd.id + "'";
      if (nd.id.empty()) {
        w.Err("id", "every node needs an id");
        continue;
      }
      if (!seen.insert(nd.id).second) w.Err("id", "is used by two nodes");
      for (auto it = n.begin(); it != n.end(); ++it)
        if (!kNode.count(it.key())) w.Warn(it.key(), "unknown key (kept, ignored)");
      nd.speaker = Str(n, "speaker");
      nd.text = Str(n, "text");
      if (nd.text.empty()) w.Warn("text", "is empty");
      nd.elseId = Str(n, "else");
      nd.gotoId = Str(n, "goto");
      if (n.contains("canLeave")) {
        if (!n["canLeave"].is_boolean())
          w.Err("canLeave", "is true or false");
        else
          nd.canLeave = n["canLeave"].get<bool>() ? 1 : 0;
      }
      ParseConds(n.contains("if") ? n["if"] : json(), w, "if", nd.conds);
      ParseActs(n.contains("do") ? n["do"] : json(), w, "do", nd.acts);
      if (n.contains("choices")) {
        const json& cs = n["choices"];
        if (!cs.is_array()) {
          w.Err("choices", "is a list");
        } else {
          for (size_t k = 0; k < cs.size(); k++) {
            const json& c = cs[k];
            const std::string cf = "choices[" + std::to_string(k) + "]";
            if (!c.is_object()) {
              w.Err(cf, "a choice is an object with \"text\"");
              continue;
            }
            for (auto it = c.begin(); it != c.end(); ++it)
              if (!kChoice.count(it.key()))
                w.Warn(cf + "." + it.key(), "unknown key (kept, ignored)");
            Choice ch;
            ch.text = Str(c, "text");
            if (ch.text.empty()) w.Err(cf + ".text", "a choice needs text");
            ch.gotoId = Str(c, "goto");
            ParseConds(c.contains("if") ? c["if"] : json(), w, cf + ".if", ch.conds);
            ParseActs(c.contains("do") ? c["do"] : json(), w, cf + ".do", ch.acts);
            nd.choices.push_back(std::move(ch));
          }
          if (nd.choices.size() > 9)
            w.Warn("choices", "has more than 9; only 1-9 have keys (the rest are "
                              "reachable by mouse)");
        }
      }
      out.nodes.push_back(std::move(nd));
    }
  }
  for (size_t i = before; i < problems.size(); i++)
    if (problems[i].error) return false;
  return true;
}

// ---- validate ----------------------------------------------------------------

void Library::Validate(const ItemLibrary* items, std::vector<Problem>& problems,
                       const GlyphLibrary* glyphs) const {
  std::map<std::string, std::string> written, read;  // flag -> first place
  std::set<std::string> names;
  for (const Dialogue& d : all_) names.insert(d.name);
  std::vector<std::pair<std::string, std::string>> metRefs;  // (name, where)
  for (const Dialogue& d : all_) {
    const std::string fname = std::filesystem::path(d.file).filename().string();
    auto nodeWhere = [&](const std::string& id) {
      return Where{fname, id.empty() ? std::string() : "node '" + id + "'", problems};
    };
    auto target = [&](const Where& w, const std::string& field, const std::string& id) {
      if (!id.empty() && !d.Find(id))
        w.Err(field, "goes to '" + id + "', which is not a node in this file");
    };
    auto conds = [&](const Where& w, const std::string& field,
                     const std::vector<Cond>& cs) {
      for (size_t i = 0; i < cs.size(); i++) {
        const Cond& c = cs[i];
        const std::string f = field + "[" + std::to_string(i) + "]";
        if (c.kind == Cond::Kind::Flag && !read.count(c.arg))
          read[c.arg] = fname + " " + w.node;
        if (c.kind == Cond::Kind::Has && items && !items->Named(c.arg))
          w.Err(f + ".has", "'" + c.arg + "' is not an item (assets/items/items.json)");
        if (c.kind == Cond::Kind::Activity) {
          const auto& k = KnownActivities();
          if (std::find(k.begin(), k.end(), c.arg) == k.end())
            w.Warn(f + ".activity", "'" + c.arg + "' is not a known activity (sleep, "
                                                  "work, wander, socialize, eat, goto)");
        }
        if (c.kind == Cond::Kind::Met && !c.arg.empty())
          metRefs.push_back({c.arg, fname + " " + w.node + " " + f});
      }
    };
    auto acts = [&](const Where& w, const std::string& field, const std::vector<Act>& as) {
      for (size_t i = 0; i < as.size(); i++) {
        const Act& a = as[i];
        const std::string f = field + "[" + std::to_string(i) + "]";
        if ((a.kind == Act::Kind::Set || a.kind == Act::Kind::Add ||
             a.kind == Act::Kind::Clear) &&
            !written.count(a.arg))
          written[a.arg] = fname + " " + w.node;
        if ((a.kind == Act::Kind::Give || a.kind == Act::Kind::Take) && items &&
            !items->Named(a.arg))
          w.Err(f, "'" + a.arg + "' is not an item (assets/items/items.json)");
        // A grant naming no glyph is a WARNING, not an error: the book that
        // teaches a demon's name may ship before the glyph does (demons D2 /
        // D1), and a warning keeps the rest of the file loading. At run time
        // the grant is then a counted no-op (stats.refusedGrants).
        if (a.kind == Act::Kind::Grant && glyphs && glyphs->Find(a.arg) < 0)
          w.Warn(f + ".grant", "'" + a.arg + "' is not a glyph in assets/spells/glyphs.json "
                               "(the grant does nothing until it is)");
        if (a.kind == Act::Kind::Learn && !written.count("name:" + a.arg))
          written["name:" + a.arg] = fname + " " + w.node;
      }
    };
    for (size_t i = 0; i < d.entries.size(); i++) {
      Where w{fname, "entry[" + std::to_string(i) + "]", problems};
      target(w, "goto", d.entries[i].gotoId);
      conds(w, "if", d.entries[i].conds);
    }
    // Reachability: from every entry, over goto / else / choice.goto.
    std::set<std::string> reach;
    std::vector<std::string> todo;
    for (const Entry& e : d.entries) todo.push_back(e.gotoId);
    while (!todo.empty()) {
      const std::string id = todo.back();
      todo.pop_back();
      const Node* n = d.Find(id);
      if (!n || !reach.insert(id).second) continue;
      todo.push_back(n->gotoId);
      todo.push_back(n->elseId);
      for (const Choice& c : n->choices) todo.push_back(c.gotoId);
    }
    for (const Node& n : d.nodes) {
      Where w = nodeWhere(n.id);
      target(w, "goto", n.gotoId);
      target(w, "else", n.elseId);
      if (!n.elseId.empty() && n.conds.empty())
        w.Warn("else", "has no \"if\" to fail, so it is never taken");
      conds(w, "if", n.conds);
      acts(w, "do", n.acts);
      for (size_t k = 0; k < n.choices.size(); k++) {
        const std::string cf = "choices[" + std::to_string(k) + "]";
        target(w, cf + ".goto", n.choices[k].gotoId);
        conds(w, cf + ".if", n.choices[k].conds);
        acts(w, cf + ".do", n.choices[k].acts);
      }
      if (!reach.count(n.id)) w.Warn("", "cannot be reached from any entry");
    }
  }
  // FLAGS ARE WORLD-SCOPED, so these two checks are library-wide: a flag Wat
  // sets and Osric reads is one flag, and only the whole set can say it is
  // never read.
  for (const auto& [f, at] : written)
    if (!read.count(f))
      problems.push_back(Problem{false, "(all dialogue)", "", "flag '" + f + "'",
                                 "is set (first in " + at + ") but no condition reads it"});
  for (const auto& [f, at] : read)
    // `demon:*` flags are written by the ENGINE (demons D5, game/demon_talk.h
    // sets demon:bound / demon:present before a demon's conversation opens).
    if (!written.count(f) && f.rfind("demon:", 0) != 0)
      problems.push_back(Problem{false, "(all dialogue)", "", "flag '" + f + "'",
                                 "is read (first in " + at + ") but nothing sets it"});
  for (const auto& [n, at] : metRefs)
    if (!names.count(n))
      problems.push_back(Problem{false, "(all dialogue)", "", "met '" + n + "'",
                                 "names no dialogue file (" + at + ")"});
}

bool Library::Load(const std::string& dir, const ItemLibrary* items,
                   std::vector<Problem>& problems, const GlyphLibrary* glyphs) {
  all_.clear();
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    problems.push_back(Problem{false, dir, "", "", "no dialogue directory (nothing to load)"});
    return false;
  }
  std::vector<fs::path> files;
  for (const auto& de : fs::directory_iterator(dir, ec))
    if (de.is_regular_file() && de.path().extension() == ".json") files.push_back(de.path());
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Dialogue d;
    if (Parse(p.stem().string(), ss.str(), p.string(), d, problems))
      all_.push_back(std::move(d));
  }
  Validate(items, problems, glyphs);
  return true;
}

const Dialogue* Library::Find(const std::string& name) const {
  for (const Dialogue& d : all_)
    if (d.name == name) return &d;
  return nullptr;
}

void Library::Add(Dialogue d) {
  for (Dialogue& e : all_)
    if (e.name == d.name) {
      e = std::move(d);
      return;
    }
  all_.push_back(std::move(d));
}

// ---- store -------------------------------------------------------------------

bool Store::Reload() {
  problems.clear();
  lib.Load(dir, items, problems, glyphs);
  bool ok = true;
  for (const Problem& p : problems) {
    std::fprintf(stderr, "dialogue: %s\n", p.Line().c_str());
    if (p.error) ok = false;
  }
  std::printf("dialogue: %zu conversation(s) from %s\n", lib.All().size(), dir.c_str());
  return ok;
}

int Store::Flag(const std::string& f) const {
  auto it = flags.find(f);
  return it == flags.end() ? 0 : it->second;
}

bool Store::IsInConversation(uint64_t mobId) const {
  return mobId != 0 && talking_.count(mobId) > 0;
}

namespace {
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
}
void PutStr(std::vector<uint8_t>& out, const std::string& s) {
  PutU32(out, (uint32_t)s.size());
  out.insert(out.end(), s.begin(), s.end());
}
struct Rd {
  const uint8_t* d;
  size_t n, at = 0;
  bool ok = true;
  uint32_t U32() {
    if (at + 4 > n) {
      ok = false;
      return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)d[at + i] << (8 * i);
    at += 4;
    return v;
  }
  std::string Str() {
    const uint32_t len = U32();
    if (!ok || len > (1u << 16) || at + len > n) {
      ok = false;
      return {};
    }
    std::string s((const char*)d + at, len);
    at += len;
    return s;
  }
};
}  // namespace

// 'DLGF' v1: u32 flagCount, {str name, i32 value}*, u32 metCount, {str}*.
// Flags at zero are not written: "never set" and "cleared" read the same, and
// a save that grew by one row per cleared flag forever would be a leak.
void Store::SaveState(std::vector<uint8_t>& out) const {
  uint32_t n = 0;
  for (const auto& [k, v] : flags) n += v != 0;
  PutU32(out, n);
  for (const auto& [k, v] : flags) {
    if (v == 0) continue;
    PutStr(out, k);
    PutU32(out, (uint32_t)v);
  }
  PutU32(out, (uint32_t)met.size());
  for (const std::string& m : met) PutStr(out, m);
}

bool Store::LoadState(const uint8_t* data, size_t len, uint32_t version) {
  if (version != kSaveVersion) return false;
  Rd rd{data, len};
  std::map<std::string, int> f;
  std::set<std::string> m;
  const uint32_t nf = rd.U32();
  for (uint32_t i = 0; rd.ok && i < nf; i++) {
    std::string k = rd.Str();
    const int v = (int)rd.U32();
    if (rd.ok) f[k] = v;
  }
  const uint32_t nm = rd.U32();
  for (uint32_t i = 0; rd.ok && i < nm; i++) {
    std::string k = rd.Str();
    if (rd.ok) m.insert(k);
  }
  // Refused whole, never half-applied (the 'TIME' rule).
  if (!rd.ok || rd.at != len) return false;
  flags = std::move(f);
  met = std::move(m);
  return true;
}

void Store::ResetState() {
  flags.clear();
  met.clear();
}

// ---- evaluation --------------------------------------------------------------

int CountItem(const Kit& kit, const std::string& item) {
  int n = 0;
  for (const ItemStack& s : kit.bag.slots)
    if (!s.Empty() && s.name == item) n += s.count;
  for (const ItemStack& s : kit.hotbar.slots)
    if (!s.Empty() && s.name == item) n += s.count;
  return n;
}

int MinuteNow(const Store& store, uint32_t tick) {
  if (store.minuteOverride >= 0) return store.minuteOverride % 1440;
  const uint32_t phase = sandvox::DayPhaseNow(tick);
  return (int)(((uint64_t)phase * 1440u) / kDayPhaseMax);
}

bool Holds(const Cond& c, const Env& env) {
  bool r = false;
  switch (c.kind) {
    case Cond::Kind::Flag: {
      const int v = env.store ? env.store->Flag(c.arg) : 0;
      r = c.hasValue ? v == c.value : v != 0;
      break;
    }
    case Cond::Kind::Time:
      r = c.fromMin <= c.toMin ? (env.minute >= c.fromMin && env.minute < c.toMin)
                               : (env.minute >= c.fromMin || env.minute < c.toMin);
      break;
    case Cond::Kind::Activity: {
      // No hook (P7 has not wired schedules) = nobody is doing anything.
      std::string now;
      if (env.store && env.store->activity) now = env.store->activity(env.mobId, env.refId);
      r = !now.empty() && now == c.arg;
      break;
    }
    case Cond::Kind::Has:
      r = env.kit && CountItem(*env.kit, c.arg) >= c.count;
      break;
    case Cond::Kind::Met: {
      const std::string& who = c.arg.empty() ? env.self : c.arg;
      r = env.store && env.store->met.count(who) > 0;
      break;
    }
  }
  return c.negate ? !r : r;
}

bool AllHold(const std::vector<Cond>& cs, const Env& env) {
  for (const Cond& c : cs)
    if (!Holds(c, env)) return false;
  return true;
}

// ---- the conversation --------------------------------------------------------

namespace {

// End, saying why on stdout: the one line an author reading the console
// needs to know whether the file or the world ended it.
void EndBecause(Store& store, PlayerSession& s, const char* why) {
  std::printf("dialogue: '%s' ended at '%s': %s\n", s.talk.dialogue.c_str(),
              s.talk.node.c_str(), why);
  End(store, s);
}

Env EnvFor(const Store& store, PlayerSession& s, const Conversation& c, uint32_t tick) {
  Env e;
  e.store = &store;
  e.kit = &s.kit();
  e.minute = MinuteNow(store, tick);
  e.mobId = c.speaker.mobId;
  e.refId = c.speaker.refId;
  e.self = c.dialogue;
  return e;
}

// Run a list of actions. Returns true when one of them was `end`.
bool RunActs(Store& store, PlayerSession& s, const std::vector<Act>& acts) {
  bool end = false;
  Kit& kit = s.kit();
  for (const Act& a : acts) {
    switch (a.kind) {
      case Act::Kind::Set: store.flags[a.arg] = a.value; break;
      case Act::Kind::Add: store.flags[a.arg] += a.value; break;
      case Act::Kind::Clear: store.flags.erase(a.arg); break;
      case Act::Kind::Give: {
        // BY NAME, through the kit's own merge rule: the bag first (it is
        // storage), the hotbar when the bag is full.
        ItemStack st;
        const ItemDef* d = store.items ? store.items->Named(a.arg) : nullptr;
        if (store.items && !d) {
          store.stats.refusedGives++;
          std::fprintf(stderr, "dialogue: give '%s': no such item\n", a.arg.c_str());
          break;
        }
        st.name = a.arg;
        st.count = a.value;
        if (kit.bag.Add(st) < 0 && kit.hotbar.Add(st) < 0) {
          store.stats.refusedGives++;
          std::fprintf(stderr, "dialogue: give '%s': the pack is full\n", a.arg.c_str());
        }
        break;
      }
      case Act::Kind::Take: {
        int left = a.value;
        auto takeFrom = [&](ItemStack* slots, int n) {
          for (int i = 0; i < n && left > 0; i++) {
            ItemStack& st = slots[i];
            if (st.Empty() || st.name != a.arg) continue;
            const int k = std::min(left, st.count);
            st.count -= k;
            left -= k;
            if (st.count <= 0) st = ItemStack{};
          }
        };
        takeFrom(kit.bag.slots, Bag::kSlots);
        takeFrom(kit.hotbar.slots, kItemSlots);
        break;
      }
      case Act::Kind::End: end = true; break;
      case Act::Kind::Grant: {
        // BY NAME, resolved now: glyph indices die on every R reload, names
        // do not. Ownership is the kit's (GlyphInventory::owned, written by
        // name in the save's kit section), so a granted glyph survives a save.
        const int gi = store.glyphs ? store.glyphs->Find(a.arg) : -1;
        if (gi < 0) {
          store.stats.refusedGrants++;
          std::fprintf(stderr, "dialogue: grant '%s': no such glyph\n", a.arg.c_str());
          break;
        }
        s.caster.inventory.Grant(gi);
        store.stats.grants++;
        break;
      }
      case Act::Kind::Learn:
        store.flags["name:" + a.arg] = 1;
        store.stats.learned++;
        break;
      case Act::Kind::PresentContract:
      case Act::Kind::Release:
      case Act::Kind::Dismiss:
        // demons D5: queued for the demon tick (Store::demonActs).
        store.demonActs.push_back(Store::DemonAct{s.index, s.talk.speaker.mobId, a.kind});
        store.stats.demonActs++;
        break;
    }
  }
  return end;
}

// Arrive at node `id`: guard (else chain), run its actions, fix the visible
// choices. Ends the conversation when the chain runs out or an action says so.
void Enter(Store& store, PlayerSession& s, std::string id, uint32_t tick) {
  Conversation& c = s.talk;
  const Dialogue* d = store.lib.Find(c.dialogue);
  // A bounded chain: `else` cycles are an authoring mistake, not a hang.
  for (int hop = 0; hop < 32; hop++) {
    const Node* n = d ? d->Find(id) : nullptr;
    if (!n) {
      if (!id.empty())
        std::fprintf(stderr, "dialogue: %s: no node '%s'; conversation ends\n",
                     c.dialogue.c_str(), id.c_str());
      EndBecause(store, s, "goto names no node");
      return;
    }
    const Env env = EnvFor(store, s, c, tick);
    if (!AllHold(n->conds, env)) {
      if (n->elseId.empty()) {
        EndBecause(store, s, "a node's `if` failed and it has no `else`");
        return;
      }
      id = n->elseId;
      continue;
    }
    c.node = n->id;
    c.steps++;
    if (RunActs(store, s, n->acts)) {
      EndBecause(store, s, "an `end` action");
      return;
    }
    // Visible choices are decided AFTER the node's own actions, so a node
    // that sets a flag can reveal a choice that reads it.
    const Env env2 = EnvFor(store, s, c, tick);
    c.visible.clear();
    for (int k = 0; k < (int)n->choices.size(); k++)
      if (AllHold(n->choices[k].conds, env2)) c.visible.push_back(k);
    return;
  }
  std::fprintf(stderr, "dialogue: %s: 'else' chain longer than 32 at '%s'\n",
               c.dialogue.c_str(), id.c_str());
  EndBecause(store, s, "else chain too long");
}

bool CanLeave(const Dialogue* d, const Node* n) {
  if (n && n->canLeave >= 0) return n->canLeave != 0;
  return d ? d->canLeave : true;
}

}  // namespace

bool Begin(Store& store, PlayerSession& s, const Speaker& speaker,
           const std::string& dialogueName, uint32_t tick, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (s.talk.active) return fail("already in a conversation");
  const Dialogue* d = store.lib.Find(dialogueName);
  if (!d) return fail("no dialogue named '" + dialogueName + "'");
  Conversation& c = s.talk;
  c = Conversation{};
  c.dialogue = d->name;
  c.speaker = speaker;
  c.startTick = tick;
  const Env env = EnvFor(store, s, c, tick);
  const Entry* pick = nullptr;
  for (const Entry& e : d->entries)
    if (AllHold(e.conds, env)) {
      pick = &e;
      break;
    }
  if (!pick) {
    c = Conversation{};
    return fail("no entry of '" + dialogueName + "' holds right now");
  }
  c.active = true;
  if (speaker.mobId) store.talking_.insert(speaker.mobId);
  store.stats.begun++;
  Enter(store, s, pick->gotoId, tick);
  return true;
}

void End(Store& store, PlayerSession& s) {
  Conversation& c = s.talk;
  if (!c.active) return;
  store.met.insert(c.dialogue);
  if (c.speaker.mobId) {
    auto it = store.talking_.find(c.speaker.mobId);
    if (it != store.talking_.end()) store.talking_.erase(it);
  }
  store.stats.ended++;
  c = Conversation{};
}

void TickSession(Store& store, PlayerSession& s, TickInput& ti, uint32_t tick,
                 MobSystem* mobs) {
  // The dev hook's queued request (BeginRequest): started here, on the tick,
  // like every other way into a conversation.
  if (s.talkBegin.pending) {
    const BeginRequest req = s.talkBegin;
    s.talkBegin = BeginRequest{};
    std::string why;
    if (s.talk.active) End(store, s);
    if (!Begin(store, s, req.speaker, req.dialogue, tick, &why))
      std::fprintf(stderr, "dialogue: could not start '%s': %s\n", req.dialogue.c_str(),
                   why.c_str());
  }
  Conversation& c = s.talk;
  if (!c.active) return;
  // The speaker died or was taken out of the world: there is nobody to talk to.
  if (c.speaker.mobId && mobs) {
    const Mob* m = mobs->FindMobById(c.speaker.mobId);
    if (!m || !m->Alive()) {
      EndBecause(store, s, "the speaker is dead or gone");
      return;
    }
  }
  const Dialogue* d = store.lib.Find(c.dialogue);
  const Node* n = d ? d->Find(c.node) : nullptr;
  if (!n) {  // a reload removed it under us
    EndBecause(store, s, "the node is gone (reloaded?)");
    return;
  }
  const int code = ti.talk;
  if (code >= 1 && code <= 9 && code <= (int)c.visible.size()) {
    const int k = c.visible[code - 1];
    if (k >= 0 && k < (int)n->choices.size()) {
      const Choice ch = n->choices[k];  // copy: Enter may End and reset `c`
      store.stats.choices++;
      if (RunActs(store, s, ch.acts) || ch.gotoId.empty())
        EndBecause(store, s, "the choice ends it");
      else
        Enter(store, s, ch.gotoId, tick);
    }
  } else if (code == kTalkContinue && c.visible.empty()) {
    const std::string next = n->gotoId;
    if (next.empty())
      EndBecause(store, s, "[continue] with no goto");
    else
      Enter(store, s, next, tick);
  } else if (code == kTalkLeave && CanLeave(d, n)) {
    EndBecause(store, s, "the player left (Esc)");
  }
  // WHILE TALKING THE PLAYER STANDS STILL. The command is zeroed in place, so
  // the controller, the hands and every later phase see a player who asked
  // for nothing — the same thing a replay of this command will see.
  if (s.talk.active) {
    const int16_t tool = ti.tool, hotbar = ti.hotbar, talk = ti.talk;
    const Vec3 ff = ti.flatFwd, rt = ti.right, lf = ti.lookFwd;
    ti = TickInput{};
    ti.talk = talk;  // kept: it is what this tick DID, for a record of it
    ti.tool = tool;
    ti.hotbar = hotbar;
    ti.flatFwd = ff;
    ti.right = rt;
    ti.lookFwd = lf;
  }
}

View MakeView(const Store& store, const Conversation& c) {
  View v;
  if (!c.active) return v;
  const Dialogue* d = store.lib.Find(c.dialogue);
  const Node* n = d ? d->Find(c.node) : nullptr;
  if (!n) return v;
  v.open = true;
  v.dialogue = c.dialogue;
  v.speaker = !n->speaker.empty()   ? n->speaker
              : !d->speaker.empty() ? d->speaker
                                    : c.speaker.name;
  v.text = n->text;
  for (int k : c.visible)
    if (k >= 0 && k < (int)n->choices.size()) v.choices.push_back(n->choices[k].text);
  v.canContinue = v.choices.empty();
  v.canLeave = CanLeave(d, n);
  v.steps = c.steps;
  return v;
}

}  // namespace dialogue
