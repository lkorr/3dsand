// schedule.h — A VILLAGER'S DAY as data (docs/PLAN_world_editor.md §2.6, P7).
//
//   assets/schedules/<name>.json
//   {
//     "about": "Osric the smith: the anvil by day, the alehouse of an evening.",
//     "rows": [
//       { "from": "22:00", "to": "06:00", "do": "sleep",     "at": "bed" },
//       { "from": "07:00", "to": "18:00", "do": "work",      "at": "work" },
//       { "from": "18:00", "to": "22:00", "do": "socialize", "at": "gather", "radius": 3 }
//     ]
//   }
//
// A ROW is a span of the in-game clock and what to do in it:
//   from, to   "HH:MM", 24-hour, [from, to). `to` may be earlier than `from`
//              (the row wraps midnight) and may be "24:00".
//   do         the activity: sleep | work | wander | socialize | eat | goto.
//   at         WHERE, resolved against the NPC's ref at run time
//              (world/refs_npc.h ResolveAnchor):
//                a ROLE  -- any prop on the npc ref whose value names a ref
//                           ("bed", "work", "home", or one you invent:
//                           "tavern": "harrowby/alehouse/hearth");
//                a REF ID -- "harrowby/green_well" (anything with a '/');
//                a TAG   -- the marker nearest the npc's authored pos whose
//                           props.tags holds it ("gather").
//              Absent = "home".
//   radius     metres (wander: how far from the anchor it strays;
//              socialize: how far it looks for someone to face). Default 3.
//   note       free text for the author; kept, never read.
// Unknown keys on a row or at the top level are KEPT and written back.
//
// FIRST MATCH WINS where rows overlap (and Validate says so); a minute no row
// covers is a gap and the villager idles at home (Validate says that too).
//
// DETERMINISM. The clock is a pure function of the sim tick: minutes after
// midnight = dayPhase * 1440 / 65536 (world.h DayPhaseForTick, integer).
// Nothing here reads a wall clock or a float.

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace schedule {

using Json = nlohmann::ordered_json;

// The activities (the same list dialogue::KnownActivities answers with).
const std::vector<std::string>& Activities();
bool IsActivity(const std::string& s);

struct Row {
  int from = 0, to = 0;      // minutes after midnight; to == from = never
  std::string act = "goto";  // `do`
  std::string at;            // "" = home
  float radius = -1.0f;      // metres; < 0 = not authored (default 3)
  std::string note;
  Json extra = Json::object();   // unknown keys, file order
  float Radius() const { return radius < 0.0f ? 3.0f : radius; }
  bool Covers(int minute) const;
};

struct Schedule {
  std::string name;   // file stem
  std::string file;   // path it came from ("" = in memory)
  Json extra = Json::object();   // unknown top-level keys ("about", ...)
  std::vector<Row> rows;
  // The row covering `minute` (first match), or -1 (a gap).
  int RowAt(int minute) const;
  // The next row to START after `minute` (the one the NPC changes to next),
  // or -1 when there is only one row. `startsAt` gets its `from`.
  int NextRow(int minute, int* startsAt = nullptr) const;
};

// "HH:MM" <-> minutes. Parse returns -1 for anything else ("24:00" = 1440).
int ParseClock(const std::string& s);
std::string ClockText(int minute);

// Parse one file's text. Problems are pushed as "<label>: rows[2].from: ..."
// (the file, the row, the field — the owner's rule §1.5). A bad ROW is
// skipped and the rest load; unreadable JSON returns false.
bool Parse(const std::string& name, const std::string& text, const std::string& label,
           Schedule& out, std::vector<std::string>& problems);
// The canonical text: stable key order, one row per line. Parse(Write(s)) == s.
std::string Write(const Schedule& s);
// Overlaps and gaps, as warnings (the schedule still loads).
void Validate(const Schedule& s, std::vector<std::string>& problems);

class Library {
 public:
  // Every <dir>/*.json, sorted. A missing dir is zero schedules, no warning.
  void Load(const std::string& dir);
  const Schedule* Find(const std::string& name) const;
  const std::vector<Schedule>& All() const { return all_; }
  // Install / replace one (gates; the editor's save path re-reads instead).
  void Add(Schedule s);
  // Write one to <dir>/<name>.json (temp + rename) and re-read it. `err`
  // says why not.
  bool Save(const Schedule& s, std::string* err);
  const std::string& Dir() const { return dir_; }
  const std::vector<std::string>& Problems() const { return problems_; }
  uint64_t Revision() const { return rev_; }

 private:
  std::string dir_;
  std::vector<Schedule> all_;
  std::vector<std::string> problems_;
  uint64_t rev_ = 0;
};

// ---- the clock -------------------------------------------------------------
// Minutes after midnight for a day phase (0..kDayPhaseMask).
int MinuteOfPhase(uint32_t phase);
// THE SCHEDULE CLOCK for a sim tick: the override when set (a gate driving a
// fast day), else the game's day phase (sandvox::DayPhaseNow -- the celestial
// clock the sky and the dialogue `time` condition read).
int MinuteNow(uint32_t tick);
// -1 = off. Process-wide, like the celestial clock it stands in for.
void SetMinuteOverride(int minute);
int MinuteOverride();

}  // namespace schedule
