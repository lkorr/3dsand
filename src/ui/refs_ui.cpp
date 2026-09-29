// refs_ui.cpp — see refs_ui.h.

#include "ui/refs_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ui/overlay.h"
#include "ui/theme.h"
#include "world/refs.h"
#include "world/refs_doors.h"

namespace {

ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// Page-local state: what is selected and the edit buffers. UI state, not game
// state -- the truth is the group file.
struct PageState {
  char search[64] = {};
  int kindFilter = 0;              // 0 = all kinds
  std::string selected;
  uint64_t loadedRev = ~0ull;      // edit buffers were filled at this revision
  std::string loadedId;
  char kind[48] = {};
  char base[64] = {};
  int pos[3] = {};
  int yaw = 0;
  struct PropEdit {
    std::string key;
    char value[256] = {};
    bool bad = false;
  };
  std::vector<PropEdit> props;
  char newKey[48] = {};
  char newValue[256] = {};
  char newId[96] = {};
  int newKind = 0;
  char newBase[64] = {};
  std::string status;              // the last action's result
  bool statusBad = false;
  bool confirmDelete = false;
};

PageState& P() {
  static PageState p;
  return p;
}

void Status(const std::string& msg, bool bad) {
  P().status = msg;
  P().statusBad = bad;
}

void Copy(char* dst, size_t n, const std::string& src) {
  std::snprintf(dst, n, "%s", src.c_str());
}

void LoadBuffers(const refs::Ref& r, uint64_t rev) {
  PageState& p = P();
  p.loadedRev = rev;
  p.loadedId = r.id;
  Copy(p.kind, sizeof p.kind, r.kind);
  Copy(p.base, sizeof p.base, r.base);
  p.pos[0] = r.pos.x;
  p.pos[1] = r.pos.y;
  p.pos[2] = r.pos.z;
  p.yaw = r.yaw;
  p.props.clear();
  for (auto it = r.props.begin(); it != r.props.end(); ++it) {
    PageState::PropEdit e;
    e.key = it.key();
    Copy(e.value, sizeof e.value, refs::JsonInline(it.value()));
    p.props.push_back(std::move(e));
  }
  p.confirmDelete = false;
}

// A prop value typed by a person: JSON if it parses (numbers, true, [..],
// {..}, "quoted"), otherwise the bare text as a string -- so typing Osric
// works without quotes.
bool ParseValue(const char* text, refs::Json& out) {
  try {
    out = refs::Json::parse(text);
    return true;
  } catch (...) {
  }
  if (text[0] == '\0') return false;
  if (std::strchr(text, '{') || std::strchr(text, '[')) return false;   // broken JSON
  out = std::string(text);
  return true;
}

bool Report(bool ok, const std::string& what, const std::string& err) {
  Status(ok ? what : what + " refused: " + err, !ok);
  return ok;
}

// ---- P6: the per-kind fields (world/refs_doors.h) ------------------------------

int PropI(const refs::Ref& r, const char* k, int def) {
  return r.props.contains(k) && r.props[k].is_number() ? r.props[k].get<int>() : def;
}
float PropF(const refs::Ref& r, const char* k, float def) {
  return r.props.contains(k) && r.props[k].is_number() ? r.props[k].get<float>() : def;
}

// A world-space polyline through the main camera (UIState::projectWorld),
// drawn over the scene. Segments behind the eye are skipped.
void WorldLine(UIState& s, const std::vector<Vec3>& pts, ImU32 col, float thick) {
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

// THE SWING ARC: the closed leaf's outline, where it stands fully open, and
// the path its latch edge sweeps -- so a wall in the way is visible before
// anyone opens the door.
void DrawDoorPreview(UIState& s, const refs::DoorGeom& g) {
  if (!g.ok) return;
  const float y0 = g.hinge.y, y1 = g.hinge.y + (float)g.height;
  const float target = g.openSign * g.openRad;
  auto leaf = [&](float rad) {
    const Vec3 l = g.LatchAt(rad);
    return std::vector<Vec3>{{g.hinge.x, y0, g.hinge.z}, {l.x, y0, l.z}, {l.x, y1, l.z},
                             {g.hinge.x, y1, g.hinge.z}, {g.hinge.x, y0, g.hinge.z}};
  };
  WorldLine(s, leaf(0.0f), ui::ColGoldHi(), 2.0f);
  WorldLine(s, leaf(target), ui::ColEmber(), 2.0f);
  for (const float y : {y0 + 0.05f, 0.5f * (y0 + y1)}) {
    std::vector<Vec3> arc;
    for (int i = 0; i <= 16; i++) {
      const Vec3 l = g.LatchAt(target * (float)i / 16.0f);
      arc.push_back({l.x, y, l.z});
    }
    WorldLine(s, arc, ui::ColEmber(), 1.5f);
  }
  // The hinge line itself.
  WorldLine(s, {{g.hinge.x, y0, g.hinge.z}, {g.hinge.x, y1, g.hinge.z}}, ui::ColBloodHi(), 3.0f);
}

void DrawDoorFields(UIState& s, refs::RefStore& st, const refs::Ref& r) {
  std::string err;
  const std::string id = r.id;
  const refs::DoorGeom g = refs::DoorGeometry(r);
  ImGui::TextColored(V4(ui::ColGoldDim()), "door");
  if (!g.ok) ImGui::TextColored(V4(ui::ColBloodHi()), "%s", g.why.c_str());
  // hinge
  {
    const std::string h = r.props.contains("hinge") && r.props["hinge"].is_string()
                              ? r.props["hinge"].get<std::string>()
                              : "left";
    int hi = h == "right" ? 1 : 0;
    const char* opts[] = {"left", "right"};
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("hinge", &hi, opts, 2) && opts[hi] != h)
      Report(refs::SetProp(st, id, "hinge", refs::Json(opts[hi]), &err),
             "hinge -> " + std::string(opts[hi]), err);
    ImGui::SetItemTooltip(
        "Seen from the side the door opens toward, facing the\n"
        "door: which hand the hinge is on. The arc drawn in the\n"
        "world shows it; if it is on the wrong side, flip this.");
  }
  // open angle: applied when the slider is let go.
  {
    static int angle = 95;
    static std::string angleFor;
    if (angleFor != id || !ImGui::IsAnyItemActive()) angle = (int)PropF(r, "openAngle", 95.0f);
    angleFor = id;
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("open angle", &angle, 5, 175, "%d deg");
    if (ImGui::IsItemDeactivatedAfterEdit())
      Report(refs::SetProp(st, id, "openAngle", refs::Json(angle), &err), "openAngle set", err);
  }
  {
    int wht[3] = {PropI(r, "width", 9), PropI(r, "height", 20), PropI(r, "thickness", 1)};
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputInt3("w / h / thick", wht, ImGuiInputTextFlags_EnterReturnsTrue)) {
      const bool ok = refs::SetProp(st, id, "width", refs::Json(wht[0]), &err) &&
                      refs::SetProp(st, id, "height", refs::Json(wht[1]), &err) &&
                      refs::SetProp(st, id, "thickness", refs::Json(wht[2]), &err);
      Report(ok, "leaf size set", err);
    }
    ImGui::SetItemTooltip(
        "The leaf box in cells: width along the leaf from the hinge,\n"
        "height up from pos, thickness BEHIND the front face.\n"
        "pos = the hinge-side bottom cell of the front layer;\n"
        "yaw = the way it opens. Enter applies.");
  }
  {
    bool locked = r.props.contains("locked") && r.props["locked"].is_boolean() &&
                  r.props["locked"].get<bool>();
    if (ImGui::Checkbox("locked", &locked))
      Report(refs::SetProp(st, id, "locked", refs::Json(locked), &err),
             locked ? "locked" : "unlocked", err);
    ImGui::SameLine();
    float ac = PropF(r, "autoClose", 0.0f);
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputFloat("auto-close s", &ac, 0, 0, "%.1f",
                          ImGuiInputTextFlags_EnterReturnsTrue))
      Report(refs::SetProp(st, id, "autoClose", refs::Json(std::max(0.0f, ac)), &err),
             "autoClose set", err);
    ImGui::SetItemTooltip("Seconds it stays open before it swings shut. 0 = never.");
  }
  if (ImGui::Button("test open/close")) {
    st.QueueDevUse(id);
    Status("asked " + id + " to open/close (next tick)", false);
  }
  ImGui::SetItemTooltip(
      "Uses the door as a player would, from anywhere (no reach\n"
      "check). The ref must be active (in the window).");
  refs::DoorStatus ds;
  if (refs::DoorStatusOf(id, ds))
    ImGui::TextDisabled("%s  %.0f deg  %u cells%s%s", refs::DoorPhaseName(ds.phase),
                        ds.angle * 57.2958f, ds.leafCells, ds.note.empty() ? "" : "  - ",
                        ds.note.c_str());
  DrawDoorPreview(s, g);
}

void DrawContainerFields(UIState& s, refs::RefStore& st, const refs::Ref& r) {
  static char search[48] = {};
  std::string err;
  const std::string id = r.id;
  ImGui::TextColored(V4(ui::ColGoldDim()), "contents (props.items: what it starts with)");
  refs::Json items = r.props.contains("items") && r.props["items"].is_array()
                         ? r.props["items"]
                         : refs::Json::array();
  bool changed = false;
  int erase = -1;
  for (size_t i = 0; i < items.size(); i++) {
    refs::Json& e = items[i];
    if (!e.is_object()) continue;
    ImGui::PushID((int)i);
    const std::string name =
        e.contains("item") && e["item"].is_string() ? e["item"].get<std::string>() : "?";
    const bool known =
        std::find(s.itemLibraryNames.begin(), s.itemLibraryNames.end(), name) != s.itemLibraryNames.end();
    if (!known) ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColBloodHi()));
    ImGui::TextUnformatted(name.c_str());
    if (!known) ImGui::PopStyleColor();
    if (!known) ImGui::SetItemTooltip("no item by this name in the library: it is skipped");
    ImGui::SameLine(150);
    const int had = e.contains("count") && e["count"].is_number_integer() ? e["count"].get<int>() : 1;
    int n = had;
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##n", &n, 1, 5);
    if (n != had) {
      e["count"] = std::max(1, n);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) erase = (int)i;
    ImGui::PopID();
  }
  if (erase >= 0) {
    items.erase(items.begin() + erase);
    changed = true;
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##itemsearch", "add an item: type to search...", search,
                           sizeof search);
  if (search[0] != 0) {
    ImGui::BeginChild("##itempick", ImVec2(0, 90), ImGuiChildFlags_Borders);
    int shown = 0;
    for (const std::string& nm : s.itemLibraryNames) {
      if (nm.find(search) == std::string::npos) continue;
      if (ImGui::Selectable(nm.c_str())) {
        bool merged = false;
        for (refs::Json& e : items)
          if (e.is_object() && e.value("item", std::string()) == nm) {
            e["count"] = e.value("count", 1) + 1;
            merged = true;
          }
        if (!merged) items.push_back(refs::Json{{"item", nm}, {"count", 1}});
        changed = true;
        search[0] = 0;
      }
      if (++shown >= 40) break;
    }
    ImGui::EndChild();
  }
  if (changed)
    Report(refs::SetProp(st, id, "items", items.empty() ? refs::Json() : items, &err),
           "contents of " + id + " set", err);
  if (const refs::RefDelta* d = st.Delta(id); d != nullptr && d->kind == "container") {
    ImGui::TextColored(V4(ui::ColEmber()), "this playthrough has changed it (a save delta)");
    ImGui::SetItemTooltip(
        "Something was taken or put: from then on the chest holds\n"
        "what it holds now, and these authored contents are only\n"
        "where a NEW game starts.");
    if (ImGui::SmallButton("reset to authored")) {
      st.ClearDelta(id);
      Status(id + ": back to its authored contents", false);
    }
  }
}

void DrawBedFields(UIState& s, const refs::Ref& r) {
  refs::BedAnchor a;
  ImGui::TextColored(V4(ui::ColGoldDim()), "bed");
  if (!refs::BedAnchorOf(r, a)) {
    ImGui::TextColored(V4(ui::ColBloodHi()), "yaw must be 0/90/180/270, length >= 1");
    return;
  }
  ImGui::TextDisabled("head at pos, feet toward yaw, %d cells (props.length)",
                      PropI(r, "length", 18));
  WorldLine(s, {a.head, a.foot}, ui::ColMana(), 3.0f);
}

}  // namespace

void DrawRefsPage(UIState& s) {
  PageState& p = P();
  if (s.refs == nullptr) {
    ImGui::TextDisabled("no reference store (a harness run: the harness map's");
    ImGui::TextDisabled("refs are gate fixtures and are not loaded here)");
    return;
  }
  refs::RefStore& st = *s.refs;
  const std::vector<std::string> kinds = refs::Kinds().Names();

  // ---- header ----
  ImGui::TextColored(V4(ui::ColParch()), "map '%s': %zu refs in %zu groups, %zu active",
                     st.MapName().c_str(), st.All().size(), st.Groups().size(),
                     st.ActiveCount());
  ImGui::SetItemTooltip("Refs live in %s\\<group>.json.\nEdit them here, or in any text "
                        "editor and press R.",
                        st.Dir().c_str());
  if (ImGui::Button("reload refs (R)")) s.reloadMaterials = true;
  ImGui::SetItemTooltip("R reloads materials, reactions AND the map's refs");

  // ---- filter ----
  {
    std::vector<const char*> items{"all kinds"};
    for (const std::string& k : kinds) items.push_back(k.c_str());
    if (p.kindFilter >= (int)items.size()) p.kindFilter = 0;
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##kindfilter", &p.kindFilter, items.data(), (int)items.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##refsearch", "search id / base / props...", p.search,
                             sizeof p.search);
  }
  const std::string wantKind = p.kindFilter > 0 ? kinds[p.kindFilter - 1] : std::string();
  const std::string needle = p.search;

  // ---- the list, grouped by group file ----
  ImGui::BeginChild("##reflist", ImVec2(0, 200), ImGuiChildFlags_Borders);
  for (const std::string& g : st.Groups()) {
    const refs::RefGroupFile* gf = st.Group(g);
    if (gf == nullptr) continue;
    std::vector<const refs::Ref*> rows;
    for (const refs::Ref& fr : gf->refs) {
      const refs::Ref* r = st.Find(fr.id);
      if (r == nullptr) continue;
      if (!wantKind.empty() && r->kind != wantKind) continue;
      if (!needle.empty()) {
        const std::string hay = r->id + " " + r->base + " " + refs::JsonInline(r->props);
        if (hay.find(needle) == std::string::npos) continue;
      }
      rows.push_back(r);
    }
    char head[128];
    std::snprintf(head, sizeof head, "%s  (%zu)###grp_%s", g.c_str(), rows.size(), g.c_str());
    if (!ImGui::TreeNodeEx(head, ImGuiTreeNodeFlags_DefaultOpen)) continue;
    for (const refs::Ref* r : rows) {
      const bool act = st.IsActive(r->id);
      const bool known = refs::Kinds().Find(r->kind) != nullptr;
      char line[256];
      std::snprintf(line, sizeof line, "%s %s  [%s]  %d %d %d###row_%s", act ? "*" : " ",
                    r->id.c_str(), r->kind.c_str(), r->pos.x, r->pos.y, r->pos.z,
                    r->id.c_str());
      if (!known) ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColBloodHi()));
      if (ImGui::Selectable(line, p.selected == r->id, ImGuiSelectableFlags_AllowDoubleClick)) {
        p.selected = r->id;
        if (ImGui::IsMouseDoubleClicked(0)) {
          s.refFlyTo = true;
          s.refFlyPos[0] = (float)r->pos.x;
          s.refFlyPos[1] = (float)r->pos.y;
          s.refFlyPos[2] = (float)r->pos.z;
        }
      }
      if (!known) ImGui::PopStyleColor();
      ImGui::SetItemTooltip("%s\n* = active (in the window)\ndouble-click: fly there%s",
                            r->id.c_str(), known ? "" : "\nUNKNOWN KIND: kept, inert");
    }
    ImGui::TreePop();
  }
  ImGui::EndChild();

  // ---- the inspector ----
  const refs::Ref* sel = p.selected.empty() ? nullptr : st.Find(p.selected);
  if (sel != nullptr) {
    if (p.loadedId != sel->id || p.loadedRev != st.Revision()) LoadBuffers(*sel, st.Revision());
    ImGui::Spacing();
    ImGui::TextColored(V4(ui::ColGoldPale()), "%s", sel->id.c_str());
    ImGui::TextDisabled("refs/%s.json   %s", sel->group.c_str(),
                        st.IsActive(sel->id) ? "active" : "dormant");
    const float bw = (ImGui::GetContentRegionAvail().x - 8) / 2;
    if (ImGui::Button("fly to it", ImVec2(bw, 0))) {
      s.refFlyTo = true;
      s.refFlyPos[0] = (float)sel->pos.x;
      s.refFlyPos[1] = (float)sel->pos.y;
      s.refFlyPos[2] = (float)sel->pos.z;
    }
    ImGui::SameLine();
    if (!p.confirmDelete) {
      if (ImGui::Button("delete...", ImVec2(bw, 0))) p.confirmDelete = true;
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, V4(ui::ColBlood()));
      if (ImGui::Button("really delete?", ImVec2(bw, 0))) {
        std::string err;
        const std::string id = sel->id;
        if (Report(refs::Delete(st, id, &err), "deleted " + id, err)) p.selected.clear();
        p.confirmDelete = false;
      }
      ImGui::PopStyleColor();
    }
    sel = p.selected.empty() ? nullptr : st.Find(p.selected);
  }
  if (sel != nullptr) {
    const std::string id = sel->id;
    std::string err;
    // kind: a combo of the registered kinds (plus the current one if unknown).
    {
      std::vector<std::string> opts = kinds;
      if (std::find(opts.begin(), opts.end(), sel->kind) == opts.end()) opts.push_back(sel->kind);
      ImGui::SetNextItemWidth(160);
      if (ImGui::BeginCombo("kind", sel->kind.c_str())) {
        for (const std::string& k : opts)
          if (ImGui::Selectable(k.c_str(), k == sel->kind) && k != sel->kind)
            Report(refs::SetField(st, id, "kind", k, &err), "kind -> " + k, err);
        ImGui::EndCombo();
      }
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("base", p.base, sizeof p.base, ImGuiInputTextFlags_EnterReturnsTrue))
      Report(refs::SetField(st, id, "base", p.base, &err), "base -> " + std::string(p.base), err);
    ImGui::SetItemTooltip("what it is an instance of: a mob def for an npc, a\n"
                          "structure for a structure. Enter applies.");
    ImGui::SetNextItemWidth(200);
    ImGui::InputInt3("pos", p.pos);
    ImGui::SetItemTooltip("world voxels (0.1 m). Enter a value, then 'apply'.");
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("yaw", &p.yaw, 15, 90);
    ImGui::SetItemTooltip("degrees; 0 faces +Z, 90 faces +X. Structures: multiples of 90.");
    ImGui::SameLine();
    const bool moved = p.pos[0] != sel->pos.x || p.pos[1] != sel->pos.y ||
                       p.pos[2] != sel->pos.z || p.yaw != sel->yaw;
    if (!moved) ImGui::BeginDisabled();
    if (ImGui::Button("apply pos/yaw"))
      Report(refs::Move(st, id, {p.pos[0], p.pos[1], p.pos[2]}, p.yaw, &err), "moved " + id, err);
    if (!moved) ImGui::EndDisabled();
    if (ImGui::Button("move to my feet")) {
      const IVec3 at{(int)std::floor(s.playerPos[0]), (int)std::floor(s.playerPos[1]),
                     (int)std::floor(s.playerPos[2])};
      Report(refs::Move(st, id, at, sel->yaw, &err), "moved " + id + " to your feet", err);
    }
    ImGui::SameLine();
    if (ImGui::Button("move to crosshair") && s.hoverMat > 0) {
      const IVec3 at{s.hoverCell[0], s.hoverCell[1] + 1, s.hoverCell[2]};
      Report(refs::Move(st, id, at, sel->yaw, &err), "moved " + id + " to the crosshair", err);
    }

    // props: one row each, value as JSON (bare words are strings).
    ImGui::TextColored(V4(ui::ColGoldDim()), "props");
    int eraseRow = -1;
    for (size_t i = 0; i < p.props.size(); i++) {
      PageState::PropEdit& e = p.props[i];
      ImGui::PushID((int)i);
      ImGui::SetNextItemWidth(90);
      ImGui::TextUnformatted(e.key.c_str());
      ImGui::SameLine(100);
      if (e.bad) ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColBloodHi()));
      ImGui::SetNextItemWidth(-30);
      if (ImGui::InputText("##v", e.value, sizeof e.value, ImGuiInputTextFlags_EnterReturnsTrue)) {
        refs::Json v;
        e.bad = !ParseValue(e.value, v);
        if (!e.bad) Report(refs::SetProp(st, id, e.key, v, &err), "props." + e.key + " set", err);
        else Status("props." + e.key + ": not valid JSON", true);
      }
      if (e.bad) ImGui::PopStyleColor();
      ImGui::SetItemTooltip("JSON: 3, true, \"text\", [\"a\", \"b\"], { \"k\": 1 }.\n"
                            "A bare word is text. Enter applies.");
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) eraseRow = (int)i;
      ImGui::SetItemTooltip("remove this prop");
      ImGui::PopID();
    }
    if (eraseRow >= 0) {
      const std::string k = p.props[(size_t)eraseRow].key;
      Report(refs::SetProp(st, id, k, refs::Json(), &err), "props." + k + " removed", err);
    }
    ImGui::SetNextItemWidth(90);
    ImGui::InputTextWithHint("##nk", "new prop", p.newKey, sizeof p.newKey);
    ImGui::SameLine(100);
    ImGui::SetNextItemWidth(-40);
    ImGui::InputTextWithHint("##nv", "value", p.newValue, sizeof p.newValue);
    ImGui::SameLine();
    if (ImGui::SmallButton("add")) {
      refs::Json v;
      if (!ParseValue(p.newValue, v)) {
        Status("the value is not valid JSON", true);
      } else if (Report(refs::SetProp(st, id, p.newKey, v, &err),
                        "props." + std::string(p.newKey) + " added", err)) {
        p.newKey[0] = p.newValue[0] = '\0';
      }
    }
    // The kind's own fields (P6): friendlier than raw JSON, same file.
    if (const refs::Ref* cur = st.Find(id)) {
      ImGui::Spacing();
      if (cur->kind == "door") DrawDoorFields(s, st, *cur);
      else if (cur->kind == "container") DrawContainerFields(s, st, *cur);
      else if (cur->kind == "bed") DrawBedFields(s, *cur);
    }
  }

  // ---- place a new one ----
  ImGui::Spacing();
  ImGui::TextColored(V4(ui::ColGoldDim()), "new reference");
  {
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##newid", "group/name  (e.g. harrowby/well)", p.newId,
                             sizeof p.newId);
    std::vector<const char*> items;
    for (const std::string& k : kinds) items.push_back(k.c_str());
    if (p.newKind >= (int)items.size()) p.newKind = 0;
    ImGui::SetNextItemWidth(120);
    if (!items.empty()) ImGui::Combo("##newkind", &p.newKind, items.data(), (int)items.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##newbase", "base (optional)", p.newBase, sizeof p.newBase);
    auto place = [&](IVec3 at, const char* where) {
      refs::Ref r;
      r.id = p.newId;
      r.kind = items.empty() ? "marker" : kinds[(size_t)p.newKind];
      r.base = p.newBase;
      r.pos = at;
      std::string err;
      if (Report(refs::Place(st, r, &err), "placed " + r.id + " " + where, err)) {
        p.selected = r.id;
        p.newId[0] = '\0';
      }
    };
    const float bw = (ImGui::GetContentRegionAvail().x - 8) / 2;
    if (ImGui::Button("place at crosshair", ImVec2(bw, 0))) {
      if (s.hoverMat > 0) place({s.hoverCell[0], s.hoverCell[1] + 1, s.hoverCell[2]}, "at the crosshair");
      else Status("the crosshair is not on anything", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("place at my feet", ImVec2(bw, 0)))
      place({(int)std::floor(s.playerPos[0]), (int)std::floor(s.playerPos[1]),
             (int)std::floor(s.playerPos[2])},
            "at your feet");
  }

  if (!p.status.empty())
    ImGui::TextColored(V4(p.statusBad ? ui::ColBloodHi() : ui::ColParchDim()), "%s",
                       p.status.c_str());

  // ---- warnings: every one names the file, the id and the field ----
  const std::vector<std::string>& w = st.Warnings();
  char wh[64];
  std::snprintf(wh, sizeof wh, "warnings (%zu)###refwarn", w.size());
  if (!w.empty()) ImGui::PushStyleColor(ImGuiCol_Text, V4(ui::ColEmber()));
  const bool open = ImGui::TreeNodeEx(wh, w.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
  if (!w.empty()) ImGui::PopStyleColor();
  if (open) {
    for (size_t i = w.size(); i-- > 0;) ImGui::TextWrapped("%s", w[i].c_str());
    ImGui::TreePop();
  }
}
