// refs_npc_ui.cpp — the References page's NPC and WAYNODE panels (P7).
//
// With an npc selected: what it is doing now (row, activity, anchor, the
// phase of its walk, the door it is waiting at), the NEXT row, its route drawn
// in the world (waynode path, door crossings marked), the schedule TABLE
// editor (rows with time pickers, an activity dropdown, an anchor picker from
// its roles, tags and refs; save writes assets/schedules/<name>.json), and
// "jump clock to" buttons for testing a row without waiting for it.
//
// With a waynode selected: its links (resolved or not, written on this node
// or on the other end -- a link is two-way), add a link by picking another
// waynode from a list sorted by distance, remove one, autoLink metres, and
// every edge drawn in the world (door edges in ember).
//
// Every write goes through refs::SetProp (the group file) or
// schedule::Library::Save (the schedule file): plain text a person can also
// edit by hand and reload with R.

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ui/overlay.h"
#include "ui/refs_ui.h"
#include "ui/theme.h"
#include "world/refs.h"
#include "world/refs_doors.h"
#include "world/refs_npc.h"

namespace {

ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void Line(UIState& s, const std::vector<Vec3>& pts, ImU32 col, float thick) {
  if (!s.projectWorld || pts.size() < 2) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  bool havePrev = false;
  ImVec2 prev{};
  for (const Vec3& p : pts) {
    const float w[3] = {p.x, p.y, p.z};
    float sc[2];
    if (!s.projectWorld(w, sc)) {
      havePrev = false;
      continue;
    }
    const ImVec2 cur(sc[0], sc[1]);
    if (havePrev) dl->AddLine(prev, cur, col, thick);
    prev = cur;
    havePrev = true;
  }
}

void Label(UIState& s, const Vec3& p, ImU32 col, const std::string& text) {
  if (!s.projectWorld) return;
  const float w[3] = {p.x, p.y, p.z};
  float sc[2];
  if (!s.projectWorld(w, sc)) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddCircleFilled(ImVec2(sc[0], sc[1]), 3.0f, col);
  if (!text.empty()) dl->AddText(ImVec2(sc[0] + 5, sc[1] - 7), col, text.c_str());
}

// A post standing at a foot point, so a route reads in 3D.
void Post(UIState& s, const Vec3& p, ImU32 col, float h = 12.0f) {
  Line(s, {p, Vec3{p.x, p.y + h, p.z}}, col, 2.0f);
}

struct NpcPage {
  std::string schedFor;           // the schedule name the buffer holds
  uint64_t schedRev = ~0ull;
  schedule::Schedule buf;
  bool dirty = false;
  char newName[64] = {};
  std::string status;
  bool statusBad = false;
  char linkSearch[64] = {};
};
NpcPage& NP() {
  static NpcPage p;
  return p;
}

void Say(const std::string& m, bool bad) {
  NP().status = m;
  NP().statusBad = bad;
}
// An authoring call's outcome, in words.
void Result(bool ok, const std::string& okMsg, const std::string& err) {
  Say(ok ? okMsg : "refused: " + err, !ok);
}

std::string PropS(const refs::Ref& r, const char* k) {
  return r.props.contains(k) && r.props[k].is_string() ? r.props[k].get<std::string>()
                                                       : std::string();
}

// Every anchor spelling this npc could use in a row's `at`: its roles (props
// whose value names a ref), "home", every tag in the map, every placeable ref.
std::vector<std::string> AnchorChoices(const refs::RefStore& st, const refs::Ref& npc) {
  std::vector<std::string> out{"home"};
  for (auto it = npc.props.begin(); it != npc.props.end(); ++it)
    if (it.value().is_string() && st.Find(it.value().get<std::string>()) != nullptr &&
        it.key() != "home")
      out.push_back(it.key());
  std::set<std::string> tags;
  for (const auto& [id, r] : st.All())
    if (r.props.contains("tags") && r.props["tags"].is_array())
      for (const refs::Json& t : r.props["tags"])
        if (t.is_string()) tags.insert(t.get<std::string>());
  for (const std::string& t : tags) out.push_back(t);
  for (const auto& [id, r] : st.All())
    if (r.kind == "marker" || r.kind == "bed" || r.kind == "waynode" || r.kind == "structure")
      out.push_back(id);
  return out;
}

// HH:MM as two small drags. True when changed.
bool TimePicker(const char* id, int& minute) {
  int h = minute / 60, m = minute % 60;
  bool ch = false;
  ImGui::PushID(id);
  ImGui::SetNextItemWidth(34);
  ch |= ImGui::DragInt("##h", &h, 0.1f, 0, 24, "%02d");
  ImGui::SameLine(0, 1);
  ImGui::TextUnformatted(":");
  ImGui::SameLine(0, 1);
  ImGui::SetNextItemWidth(34);
  ch |= ImGui::DragInt("##m", &m, 0.2f, 0, 59, "%02d");
  ImGui::PopID();
  if (ch) {
    h = std::clamp(h, 0, 24);
    m = h == 24 ? 0 : std::clamp(m, 0, 59);
    minute = (h * 60 + m) % 1440;
  }
  return ch;
}

void DrawSchedule(UIState& s, refs::RefStore& st, const refs::Ref& r) {
  NpcPage& p = NP();
  schedule::Library& lib = refs::Schedules();
  const std::string name = PropS(r, "schedule");
  // (Re)load the buffer when the npc's schedule or the library changed and
  // the buffer holds no unsaved edit.
  if (p.schedFor != name || (!p.dirty && p.schedRev != lib.Revision())) {
    p.schedFor = name;
    p.schedRev = lib.Revision();
    p.dirty = false;
    const schedule::Schedule* sc = lib.Find(name);
    p.buf = sc != nullptr ? *sc : schedule::Schedule{};
    p.buf.name = name;
  }
  // Which file.
  {
    std::string err;
    std::vector<std::string> names{"(none)"};
    for (const schedule::Schedule& sc : lib.All()) names.push_back(sc.name);
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("schedule", name.empty() ? "(none)" : name.c_str())) {
      for (const std::string& n : names)
        if (ImGui::Selectable(n.c_str(), n == name) && n != name) {
          const bool none = n == "(none)";
          if (refs::SetProp(st, r.id, "schedule", none ? refs::Json() : refs::Json(n), &err))
            Say(r.id + " now follows " + (none ? std::string("no schedule") : n), false);
          else Say("schedule refused: " + err, true);
        }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("assets/schedules/<name>.json. Several villagers may share one.");
  }
  if (!name.empty() && lib.Find(name) == nullptr)
    ImGui::TextColored(V4(ui::ColEmber()), "no schedule named '%s' yet: save below to create it",
                       name.c_str());

  // The table.
  if (ImGui::BeginTable("##sched", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("from");
    ImGui::TableSetupColumn("to");
    ImGui::TableSetupColumn("do");
    ImGui::TableSetupColumn("at", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("m");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    const std::vector<std::string> anchors = AnchorChoices(st, r);
    int erase = -1, up = -1;
    for (size_t i = 0; i < p.buf.rows.size(); i++) {
      schedule::Row& row = p.buf.rows[i];
      ImGui::PushID((int)i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      p.dirty |= TimePicker("f", row.from);
      ImGui::TableNextColumn();
      p.dirty |= TimePicker("t", row.to);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(84);
      if (ImGui::BeginCombo("##do", row.act.c_str())) {
        for (const std::string& a : schedule::Activities())
          if (ImGui::Selectable(a.c_str(), a == row.act)) {
            row.act = a;
            p.dirty = true;
          }
        ImGui::EndCombo();
      }
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      const std::string shown = row.at.empty() ? std::string("home") : row.at;
      if (ImGui::BeginCombo("##at", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const std::string& a : anchors)
          if (ImGui::Selectable(a.c_str(), a == shown)) {
            row.at = a == "home" ? std::string() : a;
            p.dirty = true;
          }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip(
          "WHERE: a role (a prop on this npc naming a ref: bed, work,\n"
          "home...), a tag (the nearest marker wearing it), or a ref id.");
      ImGui::TableNextColumn();
      float rad = row.Radius();
      ImGui::SetNextItemWidth(40);
      if (ImGui::DragFloat("##r", &rad, 0.05f, 0.0f, 30.0f, "%.1f")) {
        row.radius = rad;
        p.dirty = true;
      }
      ImGui::SetItemTooltip("radius, metres: how far a wander strays / a socialize looks");
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("^")) up = (int)i;
      ImGui::SameLine(0, 2);
      if (ImGui::SmallButton("x")) erase = (int)i;
      ImGui::SameLine(0, 2);
      if (ImGui::SmallButton("jump")) {
        s.jumpClockMinute = (row.from + 1) % 1440;
        Say("clock -> " + schedule::ClockText(row.from + 1) + " (row " + std::to_string(i) + ")", false);
      }
      ImGui::SetItemTooltip("jump the in-game clock to just after this row starts");
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (erase >= 0) {
      p.buf.rows.erase(p.buf.rows.begin() + erase);
      p.dirty = true;
    }
    if (up > 0) {
      std::swap(p.buf.rows[(size_t)up], p.buf.rows[(size_t)up - 1]);
      p.dirty = true;
    }
  }
  if (ImGui::SmallButton("+ row")) {
    schedule::Row row;
    row.from = p.buf.rows.empty() ? 8 * 60 : p.buf.rows.back().to;
    row.to = (row.from + 120) % 1440;
    row.act = "goto";
    p.buf.rows.push_back(row);
    p.dirty = true;
  }
  ImGui::SameLine();
  // Save: to this schedule, or as a new name (and point the npc at it).
  ImGui::SetNextItemWidth(120);
  ImGui::InputTextWithHint("##newsched", name.empty() ? "name" : name.c_str(), p.newName,
                           sizeof p.newName);
  ImGui::SetItemTooltip("empty = save over this npc's schedule; a new name = save a copy\n"
                        "under that name and switch this npc to it");
  ImGui::SameLine();
  if (!p.dirty && p.newName[0] == 0) ImGui::BeginDisabled();
  if (ImGui::Button(p.dirty ? "save *" : "save")) {
    schedule::Schedule out = p.buf;
    out.name = p.newName[0] != 0 ? std::string(p.newName) : name;
    std::string err;
    if (out.name.empty()) {
      Say("give the schedule a name first", true);
    } else if (lib.Save(out, &err)) {
      std::string e2;
      if (out.name != name) refs::SetProp(st, r.id, "schedule", refs::Json(out.name), &e2);
      p.dirty = false;
      p.newName[0] = 0;
      p.schedFor.clear();   // reload the buffer from the file
      Say("saved assets/schedules/" + out.name + ".json", false);
    } else {
      Say("save refused: " + err, true);
    }
  }
  if (!p.dirty && p.newName[0] == 0) ImGui::EndDisabled();
  if (p.dirty) {
    ImGui::SameLine();
    if (ImGui::SmallButton("revert")) p.schedFor.clear();
  }
  // What the table would load with (gaps, overlaps).
  std::vector<std::string> probs;
  schedule::Validate(p.buf, probs);
  for (const std::string& pr : probs) ImGui::TextColored(V4(ui::ColEmber()), "%s", pr.c_str());
}

void DrawNpcLive(UIState& s, refs::RefStore& st, const refs::Ref& r) {
  refs::ResidentStatus rs;
  const bool live = refs::ResidentStatusOf(r.id, rs) && rs.mobId != 0;
  const schedule::Schedule* sc = refs::Schedules().Find(PropS(r, "schedule"));
  ImGui::TextColored(V4(ui::ColGoldDim()), "now");
  if (s.clockMinute >= 0)
    ImGui::TextDisabled("clock %s%s", schedule::ClockText(s.clockMinute).c_str(),
                        s.clockFrozen ? "  (FROZEN by tuning dayNight.freeze: jumps do nothing)" : "");
  if (!live) {
    ImGui::TextDisabled(st.IsActive(r.id) ? "no live body (spawning, parked or dead)"
                                          : "dormant: outside the window");
  } else {
    auto rowText = [&](int i) {
      if (sc == nullptr || i < 0 || i >= (int)sc->rows.size()) return std::string("(gap: idles at home)");
      const schedule::Row& rw = sc->rows[(size_t)i];
      return schedule::ClockText(rw.from) + "-" + schedule::ClockText(rw.to) + "  " + rw.act +
             " at " + (rw.at.empty() ? std::string("home") : rw.at);
    };
    ImGui::Text("%s", rowText(rs.row).c_str());
    ImGui::TextDisabled("-> %s (%s)%s", rs.anchor.ok ? rs.anchor.refId.c_str() : "?",
                        rs.anchor.ok ? rs.anchor.how.c_str() : rs.anchor.why.c_str(),
                        rs.anchor.bed ? ", lies down" : "");
    const char* ph = rs.talking ? "talking" : rs.busy ? "busy (fight/flight)"
                                                     : refs::ResidentPhaseName(rs.phase);
    ImGui::Text("%s%s%s", ph, rs.phase == refs::ResidentPhase::DoorWait ? ": " : "",
                rs.phase == refs::ResidentPhase::DoorWait ? rs.door.c_str() : "");
    if (rs.arrivedTick != 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(arrived after %u ticks)", rs.arrivedTick - rs.rowSince);
    }
    if (rs.nextRow >= 0) {
      ImGui::TextDisabled("next: %s", rowText(rs.nextRow).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("jump there")) s.jumpClockMinute = (rs.nextFrom + 1) % 1440;
    }
    if (!rs.note.empty()) ImGui::TextColored(V4(ui::ColEmber()), "%s", rs.note.c_str());
    ImGui::TextDisabled("doors opened %u closed %u, arrivals %u, re-plans %u", rs.doorOpens,
                        rs.doorCloses, rs.arrivals, rs.replans);
    // THE ROUTE, in the world: from the feet through what is left of it.
    std::vector<Vec3> line{rs.foot};
    for (size_t i = rs.cursor; i < rs.route.pts.size(); i++) line.push_back(rs.route.pts[i]);
    for (Vec3& v : line) v.y += 1.0f;
    Line(s, line, ui::ColMana(), 3.0f);
    for (size_t i = rs.cursor; i < rs.route.pts.size(); i++) {
      const bool door = i < rs.route.doorBefore.size() && !rs.route.doorBefore[i].empty();
      const bool last = i + 1 == rs.route.pts.size();
      Post(s, rs.route.pts[i], last ? ui::ColGoldHi() : ui::ColMana());
      if (door) {
        // Mark the crossing at the door itself.
        if (const refs::Ref* d = st.Find(rs.route.doorBefore[i])) {
          const refs::DoorGeom g = refs::DoorGeometry(*d);
          if (g.ok) {
            const Vec3 c = g.Center();
            Line(s, {Vec3{c.x, (float)g.lo.y, c.z}, Vec3{c.x, (float)g.hi.y + 1, c.z}},
                 ui::ColEmber(), 4.0f);
            Label(s, Vec3{c.x, (float)g.hi.y + 2, c.z}, ui::ColEmber(), "door");
          }
        }
      }
      if (last) Label(s, Vec3{rs.route.pts[i].x, rs.route.pts[i].y + 13, rs.route.pts[i].z},
                      ui::ColGoldHi(), rs.activity);
    }
  }
  // Jump buttons for every row (also in the table): quick testing.
  if (sc != nullptr && !sc->rows.empty()) {
    ImGui::TextDisabled("jump clock to:");
    for (size_t i = 0; i < sc->rows.size(); i++) {
      ImGui::SameLine();
      ImGui::PushID((int)i + 1000);
      const std::string b = schedule::ClockText(sc->rows[i].from);
      if (ImGui::SmallButton(b.c_str())) s.jumpClockMinute = (sc->rows[i].from + 1) % 1440;
      ImGui::SetItemTooltip("%s at %s", sc->rows[i].act.c_str(),
                            sc->rows[i].at.empty() ? "home" : sc->rows[i].at.c_str());
      ImGui::PopID();
    }
  }
}

// A role prop (home / bed / work): a combo of the refs that fit it.
void RolePicker(refs::RefStore& st, const refs::Ref& r, const char* role,
                const std::vector<std::string>& kinds) {
  const std::string cur = PropS(r, role);
  const bool bad = !cur.empty() && st.Find(cur) == nullptr;
  if (bad) ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColBloodHi()));
  ImGui::SetNextItemWidth(220);
  if (ImGui::BeginCombo(role, cur.empty() ? "(none)" : cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    std::string err;
    std::string pick;
    bool clear = false;
    if (ImGui::Selectable("(none)", cur.empty()) && !cur.empty()) clear = true;
    for (const auto& [id, o] : st.All()) {
      if (std::find(kinds.begin(), kinds.end(), o.kind) == kinds.end()) continue;
      if (ImGui::Selectable(id.c_str(), id == cur) && id != cur) pick = id;
    }
    // Written after the list is drawn: SetProp rewrites the store.
    if (clear) {
      const bool ok = refs::SetProp(st, r.id, role, refs::Json(), &err);
      Result(ok, std::string(role) + " cleared", err);
    } else if (!pick.empty()) {
      const bool ok = refs::SetProp(st, r.id, role, refs::Json(pick), &err);
      Result(ok, std::string(role) + " -> " + pick, err);
    }
    ImGui::EndCombo();
  }
  if (bad) {
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("no ref with this id");
  }
}

}  // namespace

void DrawNpcFields(UIState& s, refs::RefStore& st, const refs::Ref& r) {
  ImGui::TextColored(V4(ui::ColGoldDim()), "villager");
  RolePicker(st, r, "home", {"structure", "marker", "waynode"});
  RolePicker(st, r, "bed", {"bed"});
  RolePicker(st, r, "work", {"marker", "waynode", "structure"});
  ImGui::SetItemTooltip("Roles a schedule row's `at` can name. Add others as plain\n"
                        "props (\"tavern\": \"harrowby/alehouse/hearth\").");
  DrawNpcLive(s, st, r);
  ImGui::Spacing();
  DrawSchedule(s, st, r);
  NpcPage& p = NP();
  if (!p.status.empty())
    ImGui::TextColored(V4(p.statusBad ? ui::ColBloodHi() : ui::ColParchDim()), "%s",
                       p.status.c_str());
}

void DrawWaynodeFields(UIState& s, refs::RefStore& st, const refs::Ref& r) {
  NpcPage& p = NP();
  const refs::WayGraph& g = refs::Graph(st);
  ImGui::TextColored(V4(ui::ColGoldDim()), "waynode links (two-way)");
  const int me = g.Find(r.id);
  // Every edge touching this node: where it was written, door or not.
  std::string err;
  if (me >= 0) {
    for (int ei : g.adj[(size_t)me]) {
      const refs::WayGraph::Edge& e = g.edges[(size_t)ei];
      const int other = e.a == me ? e.b : e.a;
      const std::string& oid = g.nodes[(size_t)other].id;
      ImGui::PushID(ei);
      ImGui::TextUnformatted(oid.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%.0f vox%s%s%s", e.cost, e.autoLinked ? ", auto" : "",
                          e.door.empty() ? "" : ", door ", e.door.c_str());
      ImGui::SameLine();
      if (!e.autoLinked && ImGui::SmallButton("x")) {
        // Remove it wherever it is written: this node's links or the other's.
        auto strip = [&](const std::string& holder, const std::string& target) {
          const refs::Ref* h = st.Find(holder);
          if (h == nullptr || !h->props.contains("links") || !h->props["links"].is_array())
            return false;
          refs::Json links = h->props["links"];
          const size_t cut = target.rfind('/');
          const std::string slot = cut == std::string::npos ? target : target.substr(cut + 1);
          bool hit = false;
          for (size_t i = links.size(); i-- > 0;)
            if (links[i].is_string() &&
                (links[i].get<std::string>() == target || links[i].get<std::string>() == slot)) {
              links.erase(links.begin() + (ptrdiff_t)i);
              hit = true;
            }
          if (hit) refs::SetProp(st, holder, "links", links.empty() ? refs::Json() : links, &err);
          return hit;
        };
        const bool a = strip(r.id, oid), b = strip(oid, r.id);
        Say(a || b ? "unlinked " + oid : "that link is not written on either node", !(a || b));
      }
      ImGui::PopID();
      // Drawn in the world: this node's edges bright.
      Line(s, {Vec3{g.nodes[(size_t)me].p.x, g.nodes[(size_t)me].p.y + 2, g.nodes[(size_t)me].p.z},
               Vec3{g.nodes[(size_t)other].p.x, g.nodes[(size_t)other].p.y + 2,
                    g.nodes[(size_t)other].p.z}},
           e.door.empty() ? ui::ColGoldHi() : ui::ColEmber(), 3.0f);
    }
  }
  // Every other edge, faint, so the whole graph reads.
  for (const refs::WayGraph::Edge& e : g.edges) {
    if (e.a == me || e.b == me) continue;
    const Vec3 a = g.nodes[(size_t)e.a].p, b = g.nodes[(size_t)e.b].p;
    Line(s, {Vec3{a.x, a.y + 2, a.z}, Vec3{b.x, b.y + 2, b.z}},
         e.door.empty() ? IM_COL32(120, 110, 80, 160) : IM_COL32(200, 120, 40, 160), 1.5f);
  }
  for (const refs::WayGraph::Node& n : g.nodes) Post(s, n.p, n.id == r.id ? ui::ColGoldHi() : ui::ColParchDim(), 8.0f);
  // Unresolved names written on this node.
  if (r.props.contains("links") && r.props["links"].is_array())
    for (const refs::Json& l : r.props["links"])
      if (l.is_string()) {
        const std::string nm = l.get<std::string>();
        bool found = false;
        for (const std::string& pr : g.problems)
          if (pr.find(r.id + ":") != std::string::npos && pr.find("\"" + nm + "\"") != std::string::npos)
            found = true;
        if (found) ImGui::TextColored(V4(ui::ColBloodHi()), "unresolved: %s", nm.c_str());
      }
  // Add a link: pick another waynode, nearest first.
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##linksearch", "link to: type to filter waynodes...", p.linkSearch,
                           sizeof p.linkSearch);
  {
    std::vector<std::pair<float, std::string>> cands;
    const Vec3 at{(float)r.pos.x, (float)r.pos.y, (float)r.pos.z};
    for (const refs::WayGraph::Node& n : g.nodes) {
      if (n.id == r.id) continue;
      if (p.linkSearch[0] != 0 && n.id.find(p.linkSearch) == std::string::npos) continue;
      const float dx = n.p.x - at.x, dy = n.p.y - at.y, dz = n.p.z - at.z;
      cands.push_back({std::sqrt(dx * dx + dy * dy + dz * dz), n.id});
    }
    std::sort(cands.begin(), cands.end());
    ImGui::BeginChild("##linkpick", ImVec2(0, 80), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < cands.size() && i < 30; i++) {
      char line[160];
      std::snprintf(line, sizeof line, "%s  (%.1f m)", cands[i].second.c_str(), cands[i].first / 10.0f);
      if (ImGui::Selectable(line)) {
        refs::Json links = r.props.contains("links") && r.props["links"].is_array()
                               ? r.props["links"]
                               : refs::Json::array();
        links.push_back(cands[i].second);
        const bool ok = refs::SetProp(st, r.id, "links", links, &err);
        Result(ok, "linked to " + cands[i].second, err);
        p.linkSearch[0] = 0;
        break;
      }
    }
    ImGui::EndChild();
  }
  {
    float al = r.props.contains("autoLink") && r.props["autoLink"].is_number()
                   ? r.props["autoLink"].get<float>()
                   : 0.0f;
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputFloat("autoLink m", &al, 0, 0, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
      const bool ok =
          refs::SetProp(st, r.id, "autoLink", al > 0.0f ? refs::Json(al) : refs::Json(), &err);
      Result(ok, "autoLink set", err);
    }
    ImGui::SetItemTooltip(
        "Join every waynode within this many metres (height within 2 m).\n"
        "No line-of-sight test: an auto edge an NPC fails to walk is\n"
        "dropped for that NPC and it re-plans. 0 = off.");
  }
  if (!g.problems.empty() && ImGui::TreeNode("graph problems")) {
    for (const std::string& pr : g.problems) ImGui::TextWrapped("%s", pr.c_str());
    ImGui::TreePop();
  }
  if (!p.status.empty())
    ImGui::TextColored(V4(p.statusBad ? ui::ColBloodHi() : ui::ColParchDim()), "%s",
                       p.status.c_str());
}
