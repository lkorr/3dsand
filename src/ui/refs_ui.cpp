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
