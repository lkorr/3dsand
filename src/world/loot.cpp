// loot.cpp — see loot.h.

#include "world/loot.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

#include "game/dye.h"
#include "game/equipment.h"
#include "game/item.h"
#include "sim/rng.h"
#include "test/support.h"

namespace loot {

namespace {

using Json = nlohmann::json;

struct Lib {
  bool loaded = false;
  std::map<std::string, Table> tables;
  std::vector<std::string> problems;
};
Lib& L() {
  static Lib l;
  return l;
}

uint32_t Milli(double v) { return (uint32_t)std::lround(std::max(0.0, v) * 1000.0); }

// `count`: a number or [min, max].
bool ReadCount(const Json& j, int& lo, int& hi) {
  if (j.is_number_integer()) {
    lo = hi = j.get<int>();
    return lo >= 1;
  }
  if (j.is_array() && j.size() == 2 && j[0].is_number_integer() && j[1].is_number_integer()) {
    lo = j[0].get<int>();
    hi = j[1].get<int>();
    return lo >= 1 && hi >= lo;
  }
  return false;
}

bool ReadRow(const Json& r, const std::string& at, bool weighted, Row& out,
             std::vector<std::string>& problems) {
  if (!r.is_object()) {
    problems.push_back(at + ": not an object -- skipped");
    return false;
  }
  if (r.contains("item")) {
    if (!r["item"].is_string()) {
      problems.push_back(at + ".item: want an item id (text) -- skipped");
      return false;
    }
    out.item = r["item"].get<std::string>();
  } else if (!weighted) {
    problems.push_back(at + ".item: an `always` row needs an item -- skipped");
    return false;
  }
  if (r.contains("count") && !ReadCount(r["count"], out.countMin, out.countMax)) {
    problems.push_back(at + ".count: want a number >= 1 or [min, max] -- 1 used");
    out.countMin = out.countMax = 1;
  }
  if (r.contains("weight")) {
    if (!r["weight"].is_number() || r["weight"].get<double>() <= 0.0) {
      problems.push_back(at + ".weight: want a number > 0 -- skipped");
      return false;
    }
    out.weight = std::max(1u, Milli(r["weight"].get<double>()));
  }
  if (r.contains("chance")) {
    if (!r["chance"].is_number()) problems.push_back(at + ".chance: want 0..1 -- 1 used");
    else out.chance = std::min(1000u, Milli(r["chance"].get<double>()));
  }
  if (r.contains("dye") && r["dye"].is_string()) out.dye = DyeParseHex(r["dye"].get<std::string>());
  return true;
}

uint32_t Draw(uint32_t seed, uint32_t a, uint32_t b) { return rng::Hash3(seed, a, b); }

int CountOf(const Row& r, uint32_t h) {
  if (r.countMax <= r.countMin) return r.countMin;
  return r.countMin + (int)(h % (uint32_t)(r.countMax - r.countMin + 1));
}

bool AddItem(const Row& r, int count, const ItemLibrary* lib, Bag& out,
             std::vector<std::string>* skipped) {
  if (r.item.empty() || count <= 0) return false;
  if (lib != nullptr && lib->Find(r.item) < 0) {
    if (skipped) skipped->push_back(r.item);
    return false;
  }
  ItemInstance it;
  it.name = r.item;
  it.count = count;
  it.dye = r.dye;
  return out.Add(it) >= 0;
}

}  // namespace

uint64_t Table::TotalWeight() const {
  uint64_t t = 0;
  for (const Row& r : rows) t += r.weight;
  return t;
}

bool Parse(const std::string& name, const std::string& text, const std::string& label,
           Table& out, std::vector<std::string>& problems) {
  out = Table{};
  out.name = name;
  Json j;
  try {
    j = Json::parse(text);
  } catch (const std::exception& e) {
    problems.push_back(label + ": not readable JSON (" + e.what() + ")");
    return false;
  }
  if (!j.is_object()) {
    problems.push_back(label + ": the file must be an object { \"rolls\", \"rows\" }");
    return false;
  }
  if (j.contains("rolls")) {
    int lo = 1, hi = 1;
    if (j["rolls"].is_number_integer() && j["rolls"].get<int>() >= 0) {
      lo = hi = j["rolls"].get<int>();
    } else if (j["rolls"].is_array() && j["rolls"].size() == 2 && j["rolls"][0].is_number_integer() &&
               j["rolls"][1].is_number_integer() && j["rolls"][0].get<int>() >= 0 &&
               j["rolls"][1].get<int>() >= j["rolls"][0].get<int>()) {
      lo = j["rolls"][0].get<int>();
      hi = j["rolls"][1].get<int>();
    } else {
      problems.push_back(label + ": rolls: want a number >= 0 or [min, max] -- 1 used");
    }
    out.rollsMin = std::min(lo, 64);
    out.rollsMax = std::min(hi, 64);
  }
  if (j.contains("rows")) {
    if (!j["rows"].is_array()) {
      problems.push_back(label + ": rows: want a list");
    } else {
      for (size_t i = 0; i < j["rows"].size(); i++) {
        Row r;
        if (ReadRow(j["rows"][i], label + ": rows[" + std::to_string(i) + "]", true, r, problems))
          out.rows.push_back(std::move(r));
      }
    }
  }
  if (j.contains("always")) {
    if (!j["always"].is_array()) {
      problems.push_back(label + ": always: want a list");
    } else {
      for (size_t i = 0; i < j["always"].size(); i++) {
        Row r;
        if (ReadRow(j["always"][i], label + ": always[" + std::to_string(i) + "]", false, r,
                    problems))
          out.always.push_back(std::move(r));
      }
    }
  }
  if (out.rows.empty() && out.always.empty())
    problems.push_back(label + ": no rows and no always: the table is always empty");
  return true;
}

void Reload() {
  namespace fs = std::filesystem;
  Lib& l = L();
  l.loaded = true;
  l.tables.clear();
  l.problems.clear();
  const std::string dir = sandvox::AssetDir() + "/loot";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> files;
  for (const auto& de : fs::directory_iterator(dir, ec))
    if (de.is_regular_file() && de.path().extension() == ".json") files.push_back(de.path());
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Table t;
    if (Parse(p.stem().string(), ss.str(), "loot/" + p.filename().string(), t, l.problems))
      l.tables[t.name] = std::move(t);
  }
}

const Table* Find(const std::string& name) {
  if (!L().loaded) Reload();
  auto it = L().tables.find(name);
  return it == L().tables.end() ? nullptr : &it->second;
}

const std::vector<std::string>& Problems() {
  if (!L().loaded) Reload();
  return L().problems;
}

void Add(Table t) {
  if (!L().loaded) Reload();
  L().tables[t.name] = std::move(t);
}

int Roll(const Table& t, uint32_t seed, const ItemLibrary* lib, Bag& out,
         std::vector<std::string>* skipped) {
  int added = 0;
  // Separate keys for "how many", "which", "how much", "whether", so a lucky
  // row is not also a big row (the mob pack's rule).
  for (size_t i = 0; i < t.always.size(); i++) {
    const Row& r = t.always[i];
    if (Draw(seed, 0xA1, (uint32_t)i) % 1000u >= r.chance) continue;
    if (AddItem(r, CountOf(r, Draw(seed, 0xA2, (uint32_t)i)), lib, out, skipped)) added++;
  }
  const uint64_t total = t.TotalWeight();
  if (total == 0) return added;
  int rolls = t.rollsMin;
  if (t.rollsMax > t.rollsMin)
    rolls += (int)(Draw(seed, 0xB0, 0) % (uint32_t)(t.rollsMax - t.rollsMin + 1));
  for (int k = 0; k < rolls; k++) {
    uint64_t pick = Draw(seed, 0xB1, (uint32_t)k) % total;
    size_t row = 0;
    for (; row < t.rows.size(); row++) {
      if (pick < t.rows[row].weight) break;
      pick -= t.rows[row].weight;
    }
    if (row >= t.rows.size()) continue;
    const Row& r = t.rows[row];
    if (AddItem(r, CountOf(r, Draw(seed, 0xB2, (uint32_t)k)), lib, out, skipped)) added++;
  }
  return added;
}

}  // namespace loot
