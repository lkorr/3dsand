// schedule.cpp — see schedule.h.

#include "game/schedule.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sim/world.h"
#include "test/support.h"

namespace schedule {

namespace {

int g_minuteOverride = -1;

std::string RowLabel(const std::string& label, size_t i, const char* field) {
  return label + ": rows[" + std::to_string(i) + "]." + field + ": ";
}

// Compact one-line JSON with ", " / ": " separators (the refs files' style).
std::string Inline(const Json& j) {
  if (j.is_object()) {
    std::string s = "{ ";
    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (!first) s += ", ";
      first = false;
      s += Json(it.key()).dump() + ": " + Inline(it.value());
    }
    return first ? "{}" : s + " }";
  }
  if (j.is_array()) {
    std::string s = "[";
    for (size_t i = 0; i < j.size(); i++) s += (i ? ", " : "") + Inline(j[i]);
    return s + "]";
  }
  return j.dump();
}

std::string Num(float v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

}  // namespace

const std::vector<std::string>& Activities() {
  static const std::vector<std::string> k = {"sleep", "work", "wander",
                                             "socialize", "eat", "goto"};
  return k;
}

bool IsActivity(const std::string& s) {
  const auto& k = Activities();
  return std::find(k.begin(), k.end(), s) != k.end();
}

int ParseClock(const std::string& s) {
  int h = 0, m = 0;
  char tail = 0;
  if (std::sscanf(s.c_str(), "%d:%d%c", &h, &m, &tail) != 2) return -1;
  if (h < 0 || h > 24 || m < 0 || m > 59 || (h == 24 && m != 0)) return -1;
  return h * 60 + m;
}

std::string ClockText(int minute) {
  minute = ((minute % 1440) + 1440) % 1440;
  char b[8];
  std::snprintf(b, sizeof b, "%02d:%02d", minute / 60, minute % 60);
  return b;
}

bool Row::Covers(int minute) const {
  if (from == to) return false;
  if (from < to) return minute >= from && minute < to;
  return minute >= from || minute < to;   // wraps midnight
}

int Schedule::RowAt(int minute) const {
  for (size_t i = 0; i < rows.size(); i++)
    if (rows[i].Covers(minute)) return (int)i;
  return -1;
}

int Schedule::NextRow(int minute, int* startsAt) const {
  const int cur = RowAt(minute);
  int best = -1, bestIn = 1 << 30;
  for (size_t i = 0; i < rows.size(); i++) {
    if ((int)i == cur) continue;
    const int in = ((rows[i].from - minute) % 1440 + 1440) % 1440;
    const int wait = in == 0 ? 1440 : in;
    if (wait < bestIn) {
      bestIn = wait;
      best = (int)i;
    }
  }
  if (best >= 0 && startsAt) *startsAt = rows[(size_t)best].from;
  return best;
}

bool Parse(const std::string& name, const std::string& text, const std::string& label,
           Schedule& out, std::vector<std::string>& problems) {
  out = Schedule{};
  out.name = name;
  out.file = label;
  Json j;
  try {
    j = Json::parse(text);
  } catch (const std::exception& e) {
    problems.push_back(label + ": not readable JSON (" + e.what() + ")");
    return false;
  }
  if (!j.is_object()) {
    problems.push_back(label + ": the file must be an object { \"rows\": [...] }");
    return false;
  }
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key() == "rows" || it.key() == "name") continue;
    out.extra[it.key()] = it.value();
  }
  if (j.contains("name") && j["name"].is_string() && j["name"].get<std::string>() != name)
    problems.push_back(label + ": name: says \"" + j["name"].get<std::string>() +
                       "\" but the file is " + name + ".json -- the file name wins");
  const Json rows = j.contains("rows") ? j["rows"] : Json::array();
  if (!rows.is_array()) {
    problems.push_back(label + ": rows: must be a list of rows");
    return false;
  }
  for (size_t i = 0; i < rows.size(); i++) {
    const Json& r = rows[i];
    if (!r.is_object()) {
      problems.push_back(label + ": rows[" + std::to_string(i) + "]: not an object -- skipped");
      continue;
    }
    Row row;
    bool ok = true;
    auto clock = [&](const char* k, int& dst) {
      if (!r.contains(k) || !r[k].is_string() || (dst = ParseClock(r[k].get<std::string>())) < 0) {
        problems.push_back(RowLabel(label, i, k) + "want a time like \"06:30\" -- row skipped");
        ok = false;
      }
    };
    clock("from", row.from);
    clock("to", row.to);
    if (ok && row.to == 1440) row.to = 0;
    if (ok && row.from == 1440) row.from = 0;
    if (!r.contains("do") || !r["do"].is_string()) {
      problems.push_back(RowLabel(label, i, "do") +
                         "want an activity (sleep, work, wander, socialize, eat, goto) -- row skipped");
      ok = false;
    } else {
      row.act = r["do"].get<std::string>();
      if (!IsActivity(row.act)) {
        problems.push_back(RowLabel(label, i, "do") + "'" + row.act +
                           "' is not an activity (sleep, work, wander, socialize, eat, goto) -- row skipped");
        ok = false;
      }
    }
    if (r.contains("at")) {
      if (r["at"].is_string()) row.at = r["at"].get<std::string>();
      else problems.push_back(RowLabel(label, i, "at") + "want a role, a ref id or a tag (text)");
    }
    if (r.contains("radius")) {
      if (r["radius"].is_number() && r["radius"].get<float>() >= 0.0f)
        row.radius = r["radius"].get<float>();
      else problems.push_back(RowLabel(label, i, "radius") + "want metres >= 0");
    }
    if (r.contains("note") && r["note"].is_string()) row.note = r["note"].get<std::string>();
    for (auto it = r.begin(); it != r.end(); ++it) {
      const std::string& k = it.key();
      if (k == "from" || k == "to" || k == "do" || k == "at" || k == "radius" || k == "note")
        continue;
      row.extra[k] = it.value();
    }
    if (ok && row.from == row.to)
      problems.push_back(RowLabel(label, i, "to") + "the row is empty (from == to)");
    if (ok) out.rows.push_back(std::move(row));
  }
  return true;
}

std::string Write(const Schedule& s) {
  std::string o = "{\n";
  for (auto it = s.extra.begin(); it != s.extra.end(); ++it)
    o += "  " + Json(it.key()).dump() + ": " + Inline(it.value()) + ",\n";
  o += "  \"rows\": [";
  for (size_t i = 0; i < s.rows.size(); i++) {
    const Row& r = s.rows[i];
    o += i ? ",\n    " : "\n    ";
    o += "{ \"from\": \"" + ClockText(r.from) + "\", \"to\": \"" +
         (r.to == 0 && r.from != 0 ? std::string("24:00") : ClockText(r.to)) + "\", \"do\": " +
         Json(r.act).dump();
    if (!r.at.empty()) o += ", \"at\": " + Json(r.at).dump();
    if (r.radius >= 0.0f) o += ", \"radius\": " + Num(r.radius);
    if (!r.note.empty()) o += ", \"note\": " + Json(r.note).dump();
    for (auto it = r.extra.begin(); it != r.extra.end(); ++it)
      o += ", " + Json(it.key()).dump() + ": " + Inline(it.value());
    o += " }";
  }
  o += s.rows.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return o;
}

void Validate(const Schedule& s, std::vector<std::string>& problems) {
  const std::string label = s.file.empty() ? s.name : s.file;
  // Gaps: runs of minutes no row covers.
  int gapFrom = -1;
  for (int m = 0; m <= 1440; m++) {
    const bool covered = m < 1440 && s.RowAt(m) >= 0;
    if (!covered && m < 1440 && gapFrom < 0) gapFrom = m;
    if ((covered || m == 1440) && gapFrom >= 0) {
      problems.push_back(label + ": gap " + ClockText(gapFrom) + "-" +
                         (m == 1440 ? std::string("24:00") : ClockText(m)) +
                         ": no row covers it, so the villager idles at home");
      gapFrom = -1;
    }
  }
  // Overlaps: a minute two rows cover (the first one wins).
  for (size_t a = 0; a < s.rows.size(); a++)
    for (size_t b = a + 1; b < s.rows.size(); b++) {
      int first = -1;
      for (int m = 0; m < 1440 && first < 0; m++)
        if (s.rows[a].Covers(m) && s.rows[b].Covers(m)) first = m;
      if (first >= 0)
        problems.push_back(label + ": rows[" + std::to_string(b) + "] overlaps rows[" +
                           std::to_string(a) + "] (from " + ClockText(first) +
                           "): the earlier row wins there");
    }
}

// ---- library -------------------------------------------------------------------

void Library::Load(const std::string& dir) {
  namespace fs = std::filesystem;
  dir_ = dir;
  all_.clear();
  problems_.clear();
  rev_++;
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
    Schedule s;
    const std::string label = "schedules/" + p.filename().string();
    if (Parse(p.stem().string(), ss.str(), label, s, problems_)) {
      Validate(s, problems_);
      all_.push_back(std::move(s));
    }
  }
}

const Schedule* Library::Find(const std::string& name) const {
  for (const Schedule& s : all_)
    if (s.name == name) return &s;
  return nullptr;
}

void Library::Add(Schedule s) {
  rev_++;
  for (Schedule& e : all_)
    if (e.name == s.name) {
      e = std::move(s);
      return;
    }
  all_.push_back(std::move(s));
}

bool Library::Save(const Schedule& s, std::string* err) {
  namespace fs = std::filesystem;
  if (s.name.empty() || s.name.find_first_of("/\\.:") != std::string::npos) {
    if (err) *err = "a schedule needs a plain name (letters, digits, _ and -)";
    return false;
  }
  if (dir_.empty()) {
    if (err) *err = "no schedules directory";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const std::string path = dir_ + "/" + s.name + ".json";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (err) *err = "could not write " + tmp;
      return false;
    }
    f << Write(s);
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    if (err) *err = "could not replace " + path + ": " + ec.message();
    return false;
  }
  Load(dir_);
  return true;
}

// ---- clock ----------------------------------------------------------------------

int MinuteOfPhase(uint32_t phase) {
  return (int)(((uint64_t)(phase & kDayPhaseMask) * 1440u) / kDayPhaseMax);
}

int MinuteNow(uint32_t tick) {
  if (g_minuteOverride >= 0) return g_minuteOverride % 1440;
  return MinuteOfPhase(sandvox::DayPhaseNow(tick));
}

void SetMinuteOverride(int minute) { g_minuteOverride = minute < 0 ? -1 : minute % 1440; }
int MinuteOverride() { return g_minuteOverride; }

}  // namespace schedule
