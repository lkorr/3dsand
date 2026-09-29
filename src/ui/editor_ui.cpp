// editor_ui.cpp — see editor_ui.h. The in-game editor: camera, picking,
// tools, the world overlays and the panels. Every change goes through
// editor::Session (editor/commands.h); nothing here writes a file itself.

#include "ui/editor_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "editor/commands.h"
#include "editor/struct_edit.h"
#include "ui/overlay.h"
#include "ui/refs_ui.h"
#include "ui/theme.h"
#include "world/refs.h"
#include "world/refs_doors.h"
#include "world/refs_npc.h"
#include "world/structures.h"

namespace editor_ui {

namespace {

using Json = editor::Json;

ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 Alpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)a << 24); }

constexpr float kPi = 3.14159265f;

// ---- the tools ----------------------------------------------------------------------
enum Tool : int {
  kSelect = 1,
  kPencil,
  kBrush,
  kBox,
  kLine,
  kEyedrop,
  kPlace,
  kLink,
  kToolCount
};
const char* kToolName[kToolCount] = {"",      "Select", "Pencil",     "Brush",
                                     "Box",   "Line",   "Eyedropper", "Place",
                                     "Link"};
const char* kToolTip[kToolCount] = {
    "",
    "Select a reference or slot (click), move it with the arrows,\nturn it with the ring. Double-click a house to open it.",
    "Place one voxel on the face you point at (click, drag to draw).\nShift+click erases. Alt+click picks the material.",
    "Paint a sphere or cube of voxels (B toggles). [ and ] size.\nShift erases. Alt+click picks the material.",
    "Drag across the house to select a box. PgUp/PgDn raise the top\n(Shift: the bottom). Enter fill, Shift+Enter walls, Backspace hollow,\nDelete clear, T replace, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste.",
    "Click a start voxel, then an end voxel: a beam between them.\n[ and ] set the thickness.",
    "Click a voxel to take its material.",
    "Pick a kind in the palette, click to place it. With a house open\nit adds SLOTS to the house; closed, references in the map.",
    "Click a waynode, then another, to link them. Shift+click the\nsecond to unlink."};

bool VoxelTool(int t) { return t == kPencil || t == kBrush || t == kBox || t == kLine || t == kEyedrop; }

// ---- the camera's view of the world (the inverse of raymarch's primary ray) --------
struct View {
  Vec3 eye, f, r, u;
  float th = 1, asp = 1, W = 1, H = 1;
  bool P(Vec3 p, ImVec2& out) const {
    const Vec3 d = p - eye;
    const float z = d.dot(f);
    if (z <= 0.1f) return false;
    out = ImVec2((0.5f + 0.5f * d.dot(r) / (z * th * asp)) * W, (0.5f - 0.5f * d.dot(u) / (z * th)) * H);
    return true;
  }
  // A segment clipped to the near plane.
  void Seg(ImDrawList* dl, Vec3 a, Vec3 b, ImU32 c, float t) const {
    const float za = (a - eye).dot(f), zb = (b - eye).dot(f);
    const float n = 0.15f;
    if (za < n && zb < n) return;
    if (za < n) a = a + (b - a) * ((n - za) / (zb - za));
    if (zb < n) b = b + (a - b) * ((n - zb) / (za - zb));
    ImVec2 pa, pb;
    if (!P(a, pa) || !P(b, pb)) return;
    dl->AddLine(pa, pb, c, t);
  }
  Vec3 Ray(float mx, float my) const {
    const float nx = mx / W * 2.0f - 1.0f, ny = 1.0f - my / H * 2.0f;
    return (f + r * (nx * th * asp) + u * (ny * th)).normalized();
  }
  void Box(ImDrawList* dl, Vec3 lo, Vec3 hi, ImU32 c, float t) const {
    const Vec3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
                       {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    for (int i = 0; i < 4; i++) {
      Seg(dl, p[i], p[(i + 1) % 4], c, t);
      Seg(dl, p[4 + i], p[4 + (i + 1) % 4], c, t);
      Seg(dl, p[i], p[4 + i], c, t);
    }
  }
  void Label(ImDrawList* dl, Vec3 p, ImU32 c, const char* text) const {
    ImVec2 s;
    if (!P(p, s)) return;
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 a(s.x - sz.x * 0.5f, s.y - sz.y - 2);
    dl->AddRectFilled(ImVec2(a.x - 3, a.y - 1), ImVec2(a.x + sz.x + 3, a.y + sz.y + 1), IM_COL32(9, 8, 14, 170));
    dl->AddText(a, c, text);
  }
  // A face of a unit cell, filled (hover, ghost). `axis` 0..2, `side` 0/1.
  bool Face(ImDrawList* dl, IVec3 c, int axis, int side, ImU32 fill) const {
    Vec3 q[4];
    const float x0 = (float)c.x, y0 = (float)c.y, z0 = (float)c.z;
    const float s = (float)side;
    if (axis == 0) {
      q[0] = {x0 + s, y0, z0}; q[1] = {x0 + s, y0 + 1, z0}; q[2] = {x0 + s, y0 + 1, z0 + 1}; q[3] = {x0 + s, y0, z0 + 1};
    } else if (axis == 1) {
      q[0] = {x0, y0 + s, z0}; q[1] = {x0 + 1, y0 + s, z0}; q[2] = {x0 + 1, y0 + s, z0 + 1}; q[3] = {x0, y0 + s, z0 + 1};
    } else {
      q[0] = {x0, y0, z0 + s}; q[1] = {x0 + 1, y0, z0 + s}; q[2] = {x0 + 1, y0 + 1, z0 + s}; q[3] = {x0, y0 + 1, z0 + s};
    }
    ImVec2 p[4];
    for (int i = 0; i < 4; i++)
      if (!P(q[i], p[i])) return false;
    dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
    return true;
  }
};

float SegDist(ImVec2 p, ImVec2 a, ImVec2 b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
  return std::sqrt(ex * ex + ey * ey);
}

bool RayBox(Vec3 o, Vec3 d, Vec3 lo, Vec3 hi, float& t0, float& t1) {
  t0 = 0.0f;
  t1 = 1e30f;
  const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z},
              h[3] = {hi.x, hi.y, hi.z};
  for (int a = 0; a < 3; a++) {
    if (std::fabs(dd[a]) < 1e-9f) {
      if (oo[a] < l[a] || oo[a] > h[a]) return false;
      continue;
    }
    float ta = (l[a] - oo[a]) / dd[a], tb = (h[a] - oo[a]) / dd[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

struct Hit {
  bool ok = false;
  IVec3 cell{};      // the solid cell hit
  IVec3 normal{};    // the face it was entered through
  float t = 1e30f;
  std::string on;    // the structure ref it belongs to ("" = the ground)
  bool open = false; // ...and it is the open one
  IVec3 Adjacent() const { return {cell.x + normal.x, cell.y + normal.y, cell.z + normal.z}; }
};

// Amanatides-Woo through unit cells from o along d for t in [tA, tB].
template <class F>
bool March(Vec3 o, Vec3 d, float tA, float tB, F solid, Hit& h) {
  Vec3 p = o + d * (tA + 1e-4f);
  IVec3 c{(int)std::floor(p.x), (int)std::floor(p.y), (int)std::floor(p.z)};
  const int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
  auto next = [](float pp, float dd, int cc) {
    if (std::fabs(dd) < 1e-9f) return 1e30f;
    const float b = dd > 0 ? (float)(cc + 1) : (float)cc;
    return (b - pp) / dd;
  };
  float tx = tA + next(p.x, d.x, c.x), ty = tA + next(p.y, d.y, c.y), tz = tA + next(p.z, d.z, c.z);
  const float dx = std::fabs(d.x) < 1e-9f ? 1e30f : std::fabs(1.0f / d.x);
  const float dy = std::fabs(d.y) < 1e-9f ? 1e30f : std::fabs(1.0f / d.y);
  const float dz = std::fabs(d.z) < 1e-9f ? 1e30f : std::fabs(1.0f / d.z);
  IVec3 n{0, 0, 0};
  float t = tA;
  for (int i = 0; i < 4096 && t <= tB; i++) {
    if (solid(c)) {
      h.ok = true;
      h.cell = c;
      h.normal = n;
      h.t = t;
      return true;
    }
    if (tx < ty && tx < tz) {
      t = tx;
      tx += dx;
      c.x += sx;
      n = {-sx, 0, 0};
    } else if (ty < tz) {
      t = ty;
      ty += dy;
      c.y += sy;
      n = {0, -sy, 0};
    } else {
      t = tz;
      tz += dz;
      c.z += sz;
      n = {0, 0, -sz};
    }
  }
  return false;
}

// A placed house's voxels, for picking (house frame keys).
struct Shape {
  bool ok = false;
  std::unordered_set<uint64_t> cells;
  IVec3 lo{}, hi{};
};

// World AABB (cell corners) of a house-frame box under (pos, yaw).
void HouseBoxWorld(IVec3 lo, IVec3 hi, IVec3 pos, int yaw, Vec3& wlo, Vec3& whi) {
  const IVec3 a = editor::HouseToWorld(lo, pos, yaw), b = editor::HouseToWorld(hi, pos, yaw);
  wlo = {(float)std::min(a.x, b.x), (float)std::min(a.y, b.y), (float)std::min(a.z, b.z)};
  whi = {(float)std::max(a.x, b.x) + 1, (float)std::max(a.y, b.y) + 1, (float)std::max(a.z, b.z) + 1};
}

ImU32 KindColor(const std::string& k) {
  if (k == "npc") return ui::ColGoldHi();
  if (k == "door") return ui::ColEmber();
  if (k == "container") return IM_COL32(190, 150, 90, 255);
  if (k == "bed") return ui::ColMana();
  if (k == "marker") return IM_COL32(120, 200, 120, 255);
  if (k == "waynode") return IM_COL32(90, 210, 210, 255);
  if (k == "structure") return IM_COL32(190, 130, 220, 255);
  return ui::ColSteel();
}

const char* kPlaceKinds[] = {"npc", "door", "container", "bed", "marker", "waynode", "structure"};
const char* kSlotKinds[] = {"door", "bed", "container", "marker", "waynode"};

std::string Tail(const std::string& id) {
  const size_t s = id.rfind('/');
  return s == std::string::npos ? id : id.substr(s + 1);
}

// A prop value typed by a person: JSON if it parses, else the bare text.
bool ParseValue(const char* text, Json& out) {
  try {
    out = Json::parse(text);
    return true;
  } catch (...) {
  }
  if (text[0] == '\0') return false;
  if (std::strchr(text, '{') || std::strchr(text, '[')) return false;
  out = std::string(text);
  return true;
}

Json V3(IVec3 v) { return Json::array({v.x, v.y, v.z}); }

}  // namespace

// =====================================================================================

struct EditorMode::Impl {
  // ---- binding ----
  refs::RefStore* store = nullptr;
  std::string assetDir;
  std::function<int(int, int)> groundAt;
  editor::Session session;

  // ---- mode ----
  bool active = false;
  bool askExit = false;
  bool exited = false;
  Vec3 camPos{};
  float yaw = 0, pitch = 0;
  float speed = 60.0f;   // voxels per second (6 m/s)
  float fovY = 1.2f;

  // ---- tools ----
  int tool = kSelect;
  int mat = 1;
  int replaceFrom = 0;
  char matSearch[48] = {};
  bool matPickerOpen = false;
  int brushR = 2;
  bool brushCube = false;
  int lineR = 0;
  // selection
  std::string selRef, selSlot;
  bool haveBox = false;
  IVec3 boxA{}, boxB{};
  bool boxDrag = false;
  // line
  bool lineHave = false;
  IVec3 lineA{};
  // paste
  bool pasting = false;
  int pasteRot = 0, pasteMirror = 0;
  bool pasteSkipAir = false;
  // gizmo
  int dragAxis = -1;   // 0..2 an axis, 3 the yaw ring
  Vec3 dragC{};
  float dragT0 = 0, dragA0 = 0;
  IVec3 dragDelta{};
  int dragYaw = 0;
  // stroke (pencil / brush drag = one undo step)
  bool stroking = false;
  IVec3 lastStroke{INT32_MIN, 0, 0};
  // place palette
  int placeKind = 0, slotKind = 0;
  int placeYaw = 0;
  char placeGroup[48] = "village";
  char placeBase[64] = "human";
  std::vector<std::string> structAssets;
  int structPick = 0;
  // link
  std::string linkFrom;   // ref id, or "slot:<name>"
  // messages
  std::string msg;
  bool msgBad = false;
  // world preview
  std::vector<std::pair<IVec3, uint16_t>> worldCells;
  std::map<std::string, std::string> bufInstance;   // asset -> the copy it was last opened by
  bool wantsTick = false;
  // scripted cursor
  float sx = -1, sy = -1;
  bool showHelp = false;
  // caches
  std::map<std::string, std::shared_ptr<Shape>> shapes;
  uint64_t boundsRev = ~0ull;
  bool boundsOk = false;
  IVec3 bLo{}, bHi{};
  // the paste ghost: exposed faces of the transformed clipboard
  struct GhostFace {
    IVec3 c;
    int axis, side;
    uint16_t m;
  };
  std::vector<GhostFace> ghost;
  IVec3 ghostSize{};
  int ghostKey = -1;
  // the inspector's edit buffers
  std::string inspFor;
  uint64_t inspRev = ~0ull;
  int inspPos[3] = {};
  int inspYaw = 0;
  char inspBase[64] = {};
  struct PropRow {
    std::string key;
    char value[256] = {};
  };
  std::vector<PropRow> props;
  char newKey[48] = {}, newValue[256] = {};
  char slotRename[48] = {};

  // ---------------------------------------------------------------------------------
  void Say(const std::string& m, bool bad) {
    msg = m;
    msgBad = bad;
  }
  bool Run(const std::string& name, Json args, const std::string& okMsg = std::string()) {
    std::string err;
    const bool ok = session.Run(name, std::move(args), &err);
    if (ok) {
      if (!okMsg.empty()) Say(okMsg, false);
      wantsTick = true;
    } else {
      Say(name + ": " + err, true);
    }
    return ok;
  }
  editor::StructEdit* Open() { return session.ctx.Active(); }
  const refs::Ref* Instance() {
    return store && !session.ctx.instance.empty() ? store->Find(session.ctx.instance) : nullptr;
  }
  bool InstanceFrame(IVec3& pos, int& y) {
    const refs::Ref* r = Instance();
    if (r == nullptr) return false;
    pos = r->pos;
    y = r->yaw;
    return true;
  }

  Shape* ShapeOf(const std::string& base) {
    auto it = shapes.find(base);
    if (it != shapes.end()) return it->second.get();
    auto s = std::make_shared<Shape>();
    structures::Asset a;
    std::string err;
    std::vector<std::string> w;
    if (structures::LoadAsset(assetDir, base, true, a, err, w)) {
      s->ok = true;
      s->lo = {INT32_MAX, INT32_MAX, INT32_MAX};
      s->hi = {INT32_MIN, INT32_MIN, INT32_MIN};
      for (const PrefabModel& m : a.prefab.models)
        for (const PrefabVoxel& v : m.voxels) {
          const IVec3 h{m.offset.x + v.x - a.origin.x, m.offset.y + v.y - a.origin.y,
                        m.offset.z + v.z - a.origin.z};
          s->cells.insert(editor::StructEdit::Key(h));
          s->lo = {std::min(s->lo.x, h.x), std::min(s->lo.y, h.y), std::min(s->lo.z, h.z)};
          s->hi = {std::max(s->hi.x, h.x), std::max(s->hi.y, h.y), std::max(s->hi.z, h.z)};
        }
    }
    shapes[base] = s;
    return s.get();
  }

  // The open buffer's occupied box (house frame), cached per revision.
  bool OpenBounds(IVec3& lo, IVec3& hi) {
    editor::StructEdit* se = Open();
    if (se == nullptr) return false;
    if (boundsRev != se->Revision()) {
      boundsOk = se->Bounds(bLo, bHi);
      boundsRev = se->Revision();
    }
    lo = bLo;
    hi = bHi;
    return boundsOk;
  }

  // ---- picking --------------------------------------------------------------------
  Hit PickWorld(const View& v, Vec3 d) {
    Hit best;
    const Vec3 o = v.eye;
    // Houses: the open one from its live buffer, the rest from their assets.
    if (store != nullptr) {
      for (const auto& [id, r] : store->All()) {
        if (r.kind != "structure" || r.base.empty() || !structures::ValidYaw(r.yaw)) continue;
        const bool isOpen = id == session.ctx.instance && Open() != nullptr;
        IVec3 lo, hi;
        const editor::StructEdit* se = isOpen ? Open() : nullptr;
        const Shape* sh = nullptr;
        if (isOpen) {
          if (!OpenBounds(lo, hi)) continue;
          // Let the cursor reach a few voxels past the house (building out).
        } else {
          sh = ShapeOf(r.base);
          if (!sh->ok) continue;
          lo = sh->lo;
          hi = sh->hi;
        }
        Vec3 wlo, whi;
        HouseBoxWorld(lo, hi, r.pos, r.yaw, wlo, whi);
        float t0, t1;
        if (!RayBox(o, d, wlo, whi, t0, t1) || t0 > best.t || t0 > 3000.0f) continue;
        Hit h;
        auto solid = [&](IVec3 c) {
          const IVec3 hc = editor::WorldToHouse(c, r.pos, r.yaw);
          return se ? se->Get(hc) != 0 : sh->cells.count(editor::StructEdit::Key(hc)) != 0;
        };
        if (March(o, d, t0, t1, solid, h) && h.t < best.t) {
          best = h;
          best.on = id;
          best.open = isOpen;
        }
      }
    }
    // The ground (the analytic terrain column; a house's pad is its floor - 1).
    if (groundAt) {
      std::unordered_map<int64_t, int> col;
      auto solid = [&](IVec3 c) {
        const int64_t k = ((int64_t)c.x << 32) ^ (uint32_t)c.z;
        auto it = col.find(k);
        int g;
        if (it == col.end()) {
          g = groundAt(c.x, c.z);
          col.emplace(k, g);
        } else {
          g = it->second;
        }
        return c.y <= g;
      };
      Hit h;
      if (March(o, d, 0.0f, std::min(best.t, 1500.0f), solid, h) && h.t < best.t) best = h;
    }
    // With a house open and nothing else in the way: the plane of its floor,
    // so a voxel can be placed over empty pad.
    IVec3 ipos;
    int iyaw;
    if (!best.ok && Open() != nullptr && InstanceFrame(ipos, iyaw) && std::fabs(d.y) > 1e-4f) {
      const float t = ((float)ipos.y - o.y) / d.y;
      if (t > 0 && t < 1500.0f) {
        const Vec3 p = o + d * t;
        best.ok = true;
        best.t = t;
        best.cell = {(int)std::floor(p.x), ipos.y - 1, (int)std::floor(p.z)};
        best.normal = {0, 1, 0};
      }
    }
    return best;
  }

  // Refs within reach, and a box to pick / draw each by.
  bool RefBox(const refs::Ref& r, Vec3& lo, Vec3& hi) {
    const Vec3 p{(float)r.pos.x, (float)r.pos.y, (float)r.pos.z};
    if (r.kind == "npc") {
      lo = {p.x - 2.5f, p.y, p.z - 2.5f};
      hi = {p.x + 3.5f, p.y + 18.0f, p.z + 3.5f};
      return true;
    }
    if (r.kind == "door") {
      const refs::DoorGeom g = refs::DoorGeometry(r);
      if (g.ok) {
        lo = {(float)g.lo.x, (float)g.lo.y, (float)g.lo.z};
        hi = {(float)g.hi.x + 1, (float)g.hi.y + 1, (float)g.hi.z + 1};
        return true;
      }
    }
    if (r.props.contains("box") && r.props["box"].is_object()) {
      const Json& b = r.props["box"];
      if (b.contains("min") && b.contains("max") && b["min"].is_array() && b["max"].is_array() &&
          b["min"].size() == 3 && b["max"].size() == 3) {
        lo = {b["min"][0].get<float>(), b["min"][1].get<float>(), b["min"][2].get<float>()};
        hi = {b["max"][0].get<float>() + 1, b["max"][1].get<float>() + 1, b["max"][2].get<float>() + 1};
        return true;
      }
    }
    if (r.kind == "bed") {
      refs::BedAnchor a;
      if (refs::BedAnchorOf(r, a)) {
        lo = {std::min(a.head.x, a.foot.x) - 2.5f, a.head.y - 0.5f, std::min(a.head.z, a.foot.z) - 2.5f};
        hi = {std::max(a.head.x, a.foot.x) + 2.5f, a.head.y + 1.5f, std::max(a.head.z, a.foot.z) + 2.5f};
        return true;
      }
    }
    if (r.kind == "structure") {
      if (!structures::ValidYaw(r.yaw)) return false;
      const Shape* s = ShapeOf(r.base);
      if (!s->ok) return false;
      HouseBoxWorld(s->lo, s->hi, r.pos, r.yaw, lo, hi);
      return true;
    }
    lo = {p.x - 1, p.y, p.z - 1};
    hi = {p.x + 2, p.y + 3, p.z + 2};
    return true;
  }

  // The refs and slots under the ray: nearest non-structure first; a house
  // box only when nothing else is hit.
  std::string PickRef(Vec3 o, Vec3 d, bool* isSlot) {
    *isSlot = false;
    std::string best, bestStruct;
    float bt = 1e30f, bs = 1e30f;
    if (store != nullptr)
      for (const auto& [id, r] : store->All()) {
        if (!session.ctx.instance.empty() && r.derivedFrom == session.ctx.instance) continue;
        Vec3 lo, hi;
        if (!RefBox(r, lo, hi)) continue;
        float t0, t1;
        if (!RayBox(o, d, lo, hi, t0, t1)) continue;
        if (r.kind == "structure") {
          if (t0 < bs) {
            bs = t0;
            bestStruct = id;
          }
        } else if (t0 < bt) {
          bt = t0;
          best = id;
        }
      }
    // Slots of the open house (drawn from the buffer, not the store).
    for (const refs::Ref& c : OpenSlotRefs()) {
      Vec3 lo, hi;
      if (!RefBox(c, lo, hi)) continue;
      float t0, t1;
      if (RayBox(o, d, lo, hi, t0, t1) && t0 < bt) {
        bt = t0;
        best = "slot:" + c.props.value("slot", std::string());
        *isSlot = true;
      }
    }
    if (!best.empty()) return best;
    return bestStruct;
  }

  // The open house's slots as world refs (the same DeriveChildren the game uses).
  std::vector<refs::Ref> OpenSlotRefs() {
    std::vector<refs::Ref> out;
    editor::StructEdit* se = Open();
    const refs::Ref* inst = Instance();
    if (se == nullptr || inst == nullptr) return out;
    structures::DeriveChildren(se->AsAsset(), *inst, out, nullptr);
    return out;
  }

  // ---- the gizmo target ----------------------------------------------------------
  bool Target(Vec3& c, int& yawOut, bool& quarter) {
    quarter = false;
    if (!selSlot.empty()) {
      for (const refs::Ref& s : OpenSlotRefs())
        if (s.props.value("slot", std::string()) == selSlot) {
          const refs::Ref* inst = Instance();
          editor::StructEdit* se = Open();
          const int i = se ? se->FindSlot(selSlot) : -1;
          if (inst == nullptr || i < 0) return false;
          const IVec3 w = editor::HouseToWorld(se->Slots()[(size_t)i].pos, inst->pos, inst->yaw);
          c = {w.x + 0.5f, w.y + 0.5f, w.z + 0.5f};
          yawOut = se->Slots()[(size_t)i].yaw;
          quarter = true;
          return true;
        }
      return false;
    }
    if (!selRef.empty() && store != nullptr) {
      const refs::Ref* r = store->Find(selRef);
      if (r == nullptr) return false;
      c = {r->pos.x + 0.5f, r->pos.y + 0.5f, r->pos.z + 0.5f};
      yawOut = r->yaw;
      quarter = r->kind == "structure";
      return true;
    }
    return false;
  }
  bool TargetEditable() {
    if (!selSlot.empty()) return true;
    const refs::Ref* r = store && !selRef.empty() ? store->Find(selRef) : nullptr;
    return r != nullptr && r->derivedFrom.empty();
  }

  // Move / turn the selection by a world delta (one command).
  void MoveSelection(IVec3 by, int yawAbs, bool setYaw) {
    if (!selSlot.empty()) {
      Json a{{"name", selSlot}, {"frame", "world"}, {"by", V3(by)}};
      if (setYaw) a["yaw"] = yawAbs;
      if (!session.ctx.instance.empty()) Run("slot.move", a, "moved slot " + selSlot);
      else Say("open the house through a placed copy to move slots with the gizmo", true);
      return;
    }
    if (selRef.empty()) return;
    Json a{{"id", selRef}, {"by", V3(by)}};
    if (setYaw) a["yaw"] = yawAbs;
    Run("ref.move", a, "moved " + selRef);
  }

  // Edits thrown away: the world was showing them (preview ops), and the
  // asset did not change, so no re-stamp will put the house back. Write the
  // on-disk voxels back over every cell the buffer differs in.
  void RevertPreview(const std::string& asset) {
    auto it = session.ctx.structs.find(asset);
    IVec3 pos;
    int y;
    if (it == session.ctx.structs.end() || asset != session.ctx.active || !InstanceFrame(pos, y)) return;
    editor::StructEdit disk;
    std::string err;
    std::vector<std::string> w;
    if (!disk.Load(assetDir, asset, err, w)) return;
    const editor::StructEdit& buf = *it->second;
    for (const auto& [k, m] : buf.Cells())
      if (disk.Get(editor::StructEdit::Unkey(k)) != m)
        worldCells.push_back({editor::HouseToWorld(editor::StructEdit::Unkey(k), pos, y),
                              disk.Get(editor::StructEdit::Unkey(k))});
    for (const auto& [k, m] : disk.Cells())
      if (buf.Get(editor::StructEdit::Unkey(k)) == 0)
        worldCells.push_back({editor::HouseToWorld(editor::StructEdit::Unkey(k), pos, y), m});
    wantsTick = true;
  }

  // ---- the clipboard ghost ----------------------------------------------------------
  void BuildGhost() {
    const editor::Clipboard& cb = session.ctx.clip;
    const int key = (pasteRot & 3) | (pasteMirror << 2) | ((int)cb.cells.size() << 4);
    if (key == ghostKey) return;
    ghostKey = key;
    ghost.clear();
    if (!cb.valid) return;
    const int W = cb.size.x, D = cb.size.z;
    auto map = [&](int x, int z, int* ox, int* oz) {
      int mx = pasteMirror == 1 ? W - 1 - x : x, mz = pasteMirror == 2 ? D - 1 - z : z;
      int w = W, d = D;
      for (int k = 0; k < (pasteRot & 3); k++) {
        const int nx = mz, nz = w - 1 - mx;
        mx = nx;
        mz = nz;
        std::swap(w, d);
      }
      *ox = mx;
      *oz = mz;
    };
    ghostSize = (pasteRot & 1) ? IVec3{cb.size.z, cb.size.y, cb.size.x} : cb.size;
    auto at = [&](int x, int y, int z) -> uint16_t {
      if (x < 0 || y < 0 || z < 0 || x >= cb.size.x || y >= cb.size.y || z >= cb.size.z) return 0;
      return cb.At(x, y, z);
    };
    const int dn[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (int z = 0; z < cb.size.z && ghost.size() < 60000; z++)
      for (int y = 0; y < cb.size.y; y++)
        for (int x = 0; x < cb.size.x; x++) {
          const uint16_t m = at(x, y, z);
          if (m == 0) continue;
          int ox, oz;
          map(x, z, &ox, &oz);
          for (int f = 0; f < 6; f++) {
            if (at(x + dn[f][0], y + dn[f][1], z + dn[f][2]) != 0) continue;
            // The face's direction through the same transform.
            int fx0, fz0, fx1, fz1;
            map(x, z, &fx0, &fz0);
            map(x + dn[f][0], z + dn[f][2], &fx1, &fz1);
            const int ddx = fx1 - fx0, ddz = fz1 - fz0, ddy = dn[f][1];
            const int axis = ddx != 0 ? 0 : ddy != 0 ? 1 : 2;
            const int side = (ddx + ddy + ddz) > 0 ? 1 : 0;
            ghost.push_back({{ox, y, oz}, axis, side, m});
          }
        }
  }

  // Where a paste would land (house frame min corner) for the cursor hit.
  bool PasteAnchor(const Hit& h, IVec3& at) {
    IVec3 ipos;
    int iyaw;
    if (!h.ok || !InstanceFrame(ipos, iyaw)) return false;
    const IVec3 a = editor::WorldToHouse(h.Adjacent(), ipos, iyaw);
    at = {a.x - ghostSize.x / 2, a.y, a.z - ghostSize.z / 2};
    return true;
  }
};

// =====================================================================================

EditorMode::EditorMode() : p_(new Impl) {}
EditorMode::~EditorMode() = default;

void EditorMode::Init(refs::RefStore* store, const std::string& assetDir,
                      std::function<int(int, int)> groundAt) {
  p_->store = store;
  p_->assetDir = assetDir;
  p_->groundAt = std::move(groundAt);
  editor::Context& c = p_->session.ctx;
  c.refs = store;
  c.assetDir = assetDir;
  c.mats = editor::LoadMaterialNames(assetDir);
  // Start the voxel tools on a building material, not stone.
  for (size_t i = 1; i < c.mats.size() && i <= 255; i++)
    if (c.mats[i] == "plank") p_->mat = (int)i;
  Impl* im = p_.get();
  c.onAssetSaved = [im](const std::string& asset) {
    structures::Reload(im->store, asset);
    im->shapes.erase(asset);
    im->wantsTick = true;
  };
}

void EditorMode::SetStore(refs::RefStore* store) {
  p_->store = store;
  p_->session.ctx.refs = store;
}

bool EditorMode::Active() const { return p_->active; }

void EditorMode::Enter(Vec3 eye, float yaw, float pitch) {
  Impl& p = *p_;
  p.active = true;
  p.askExit = false;
  p.camPos = eye;
  p.yaw = yaw;
  p.pitch = pitch;
  if (p.session.JournalPath().empty()) {
    const std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof buf, "build/editor/session_%Y%m%d_%H%M%S.jsonl", std::localtime(&now));
    p.session.OpenJournal(buf);
  }
  p.Say("editor on: the world is paused. Right mouse + WASD to fly, H for the keys", false);
}

void EditorMode::RequestExit() {
  Impl& p = *p_;
  if (!p.session.UnsavedStructures().empty()) {
    p.askExit = true;
    return;
  }
  p.active = false;
  p.exited = true;
}

bool EditorMode::TakeExited() {
  const bool e = p_->exited;
  p_->exited = false;
  return e;
}

Vec3 EditorMode::CamPos() const { return p_->camPos; }
float EditorMode::CamYaw() const { return p_->yaw; }
float EditorMode::CamPitch() const { return p_->pitch; }
void EditorMode::SetCamera(Vec3 pos, float yaw, float pitch) {
  p_->camPos = pos;
  p_->yaw = yaw;
  p_->pitch = pitch;
}

void EditorMode::OpenStructureRef(const std::string& refId) {
  Impl& p = *p_;
  if (p.Run("struct.open", Json{{"ref", refId}}, "opened " + refId + " for editing (Ctrl+S saves the house)")) {
    p.selRef.clear();
    p.selSlot.clear();
    p.haveBox = false;
    if (p.tool == kSelect || p.tool == kPlace || p.tool == kLink) p.tool = kPencil;
    // Fly to a three-quarter view of it.
    const refs::Ref* r = p.Instance();
    IVec3 lo, hi;
    if (r != nullptr && p.OpenBounds(lo, hi)) {
      Vec3 wlo, whi;
      HouseBoxWorld(lo, hi, r->pos, r->yaw, wlo, whi);
      const Vec3 c = (wlo + whi) * 0.5f;
      const float ext = std::max({whi.x - wlo.x, whi.y - wlo.y, whi.z - wlo.z});
      const Vec3 dir = Vec3{-0.62f, 0.45f, -0.64f}.normalized();
      p.camPos = c + dir * (ext * 1.15f + 20.0f);
      const Vec3 f = (c - p.camPos).normalized();
      p.yaw = std::atan2(f.z, f.x);
      p.pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
    }
  }
}

std::vector<std::pair<IVec3, uint16_t>> EditorMode::TakeWorldCells() {
  std::vector<std::pair<IVec3, uint16_t>> v;
  v.swap(p_->worldCells);
  return v;
}

bool EditorMode::TakeWantsTick() {
  const bool w = p_->wantsTick;
  p_->wantsTick = false;
  return w;
}

editor::Session& EditorMode::Session() { return p_->session; }

void EditorMode::SetScriptedCursor(float x, float y) {
  p_->sx = x;
  p_->sy = y;
}
void EditorMode::SetTool(int key) { p_->tool = std::clamp(key, 1, (int)kToolCount - 1); }
void EditorMode::SetSelection(IVec3 lo, IVec3 hi) {
  p_->haveBox = true;
  p_->boxA = lo;
  p_->boxB = hi;
}
void EditorMode::SelectRef(const std::string& id) {
  p_->selRef = id;
  p_->selSlot.clear();
}
void EditorMode::StartPaste() {
  p_->pasting = p_->session.ctx.clip.valid;
  p_->ghostKey = -1;
}
std::string EditorMode::LastMessage() const { return p_->msg; }

// =====================================================================================
// THE FRAME

void EditorMode::Frame(UIState& ui, float dt, float fovY) {
  Impl& p = *p_;
  if (!p.active) return;
  p.fovY = fovY;
  ImGuiIO& io = ImGui::GetIO();
  editor::Session& S = p.session;

  // ---- the view --------------------------------------------------------------------
  View v;
  {
    const float cp = std::cos(p.pitch);
    v.eye = p.camPos;
    v.f = Vec3{std::cos(p.yaw) * cp, std::sin(p.pitch), std::sin(p.yaw) * cp};
    v.r = v.f.cross(Vec3{0, 1, 0}).normalized();
    v.u = v.r.cross(v.f).normalized();
    v.th = std::tan(fovY * 0.5f);
    v.W = std::max(io.DisplaySize.x, 1.0f);
    v.H = std::max(io.DisplaySize.y, 1.0f);
    v.asp = v.W / v.H;
  }
  const bool mouseFree = !io.WantCaptureMouse;
  const bool keysFree = !io.WantCaptureKeyboard;
  const ImVec2 mouse = p.sx >= 0 ? ImVec2(p.sx, p.sy) : io.MousePos;
  const bool shift = io.KeyShift, ctrl = io.KeyCtrl, alt = io.KeyAlt;

  // ---- camera: RMB look, WASD / Q E fly, wheel speed --------------------------------
  if (mouseFree && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    p.yaw += io.MouseDelta.x * 0.0035f;
    p.pitch = std::clamp(p.pitch - io.MouseDelta.y * 0.0035f, -1.55f, 1.55f);
  }
  if (mouseFree && io.MouseWheel != 0.0f)
    p.speed = std::clamp(p.speed * std::pow(1.25f, io.MouseWheel), 5.0f, 800.0f);
  if (keysFree && !ctrl) {
    Vec3 mv{0, 0, 0};
    const Vec3 flat = Vec3{v.f.x, 0, v.f.z}.normalized();
    if (ImGui::IsKeyDown(ImGuiKey_W)) mv += v.f;
    if (ImGui::IsKeyDown(ImGuiKey_S)) mv = mv - v.f;
    if (ImGui::IsKeyDown(ImGuiKey_D)) mv += v.r;
    if (ImGui::IsKeyDown(ImGuiKey_A)) mv = mv - v.r;
    if (ImGui::IsKeyDown(ImGuiKey_E)) mv += Vec3{0, 1, 0};
    if (ImGui::IsKeyDown(ImGuiKey_Q)) mv = mv - Vec3{0, 1, 0};
    (void)flat;
    if (mv.len() > 1e-4f) p.camPos += mv.normalized() * (p.speed * (shift ? 4.0f : 1.0f) * dt);
  }

  editor::StructEdit* se = p.Open();
  const refs::Ref* inst = p.Instance();
  IVec3 ipos{};
  int iyaw = 0;
  const bool haveInst = p.InstanceFrame(ipos, iyaw);
  if (se == nullptr && VoxelTool(p.tool)) p.tool = kSelect;

  // ---- picking -----------------------------------------------------------------
  const Vec3 rd = v.Ray(mouse.x, mouse.y);
  Hit hit = mouseFree ? p.PickWorld(v, rd) : Hit{};
  bool hoverIsSlot = false;
  const std::string hoverRef = mouseFree ? p.PickRef(v.eye, rd, &hoverIsSlot) : std::string();

  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  // ---- keyboard ------------------------------------------------------------------
  if (keysFree) {
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
      std::string m;
      const bool ok = shift ? S.Redo(&m) : S.Undo(&m);
      p.Say(m, !ok);
      p.wantsTick = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
      std::string m;
      const bool ok = S.Redo(&m);
      p.Say(m, !ok);
      p.wantsTick = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      if (se != nullptr) {
        if (!se->Dirty()) p.Say(se->name + ": no unsaved changes", false);
        else p.Run("struct.save", Json::object(), "saved " + se->name + " (.vox + .struct.json); every copy re-stamped");
      } else {
        p.Say("references are written as you edit them; open a house to have something to save", false);
      }
    }
    if (!ctrl) {
      for (int k = 1; k < kToolCount; k++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k - 1), false)) {
          if (VoxelTool(k) && se == nullptr) {
            p.Say(std::string(kToolName[k]) + " edits a house: open one first (double-click it with Select)", true);
          } else {
            p.tool = k;
            p.lineHave = false;
            p.linkFrom.clear();
          }
        }
      if (ImGui::IsKeyPressed(ImGuiKey_H, false)) p.showHelp = !p.showHelp;
      if (ImGui::IsKeyPressed(ImGuiKey_B, false)) p.brushCube = !p.brushCube;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
        if (p.tool == kLine) p.lineR = std::max(0, p.lineR - 1);
        else p.brushR = std::max(0, p.brushR - 1);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
        if (p.tool == kLine) p.lineR = std::min(8, p.lineR + 1);
        else p.brushR = std::min(32, p.brushR + 1);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (p.pasting) p.pasting = false;
        else if (p.lineHave) p.lineHave = false;
        else if (!p.linkFrom.empty()) p.linkFrom.clear();
        else if (p.dragAxis >= 0) p.dragAxis = -1;
        else if (p.haveBox) p.haveBox = false;
        else {
          p.selRef.clear();
          p.selSlot.clear();
        }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        Vec3 c;
        int yy;
        bool q;
        float ext = 20.0f;
        bool any = false;
        if (p.haveBox && haveInst) {
          Vec3 lo, hi;
          const IVec3 a{std::min(p.boxA.x, p.boxB.x), std::min(p.boxA.y, p.boxB.y), std::min(p.boxA.z, p.boxB.z)};
          const IVec3 b{std::max(p.boxA.x, p.boxB.x), std::max(p.boxA.y, p.boxB.y), std::max(p.boxA.z, p.boxB.z)};
          HouseBoxWorld(a, b, ipos, iyaw, lo, hi);
          c = (lo + hi) * 0.5f;
          ext = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
          any = true;
        } else if (p.Target(c, yy, q)) {
          any = true;
        } else if (inst != nullptr) {
          IVec3 lo, hi;
          if (p.OpenBounds(lo, hi)) {
            Vec3 wl, wh;
            HouseBoxWorld(lo, hi, ipos, iyaw, wl, wh);
            c = (wl + wh) * 0.5f;
            ext = std::max({wh.x - wl.x, wh.y - wl.y, wh.z - wl.z});
            any = true;
          }
        }
        if (any) {
          p.camPos = c - v.f * (ext * 1.2f + 12.0f);
          p.Say("framed the selection", false);
        }
      }
    }
  }

  // ---- the gizmo (Select tool, a ref or slot selected) -----------------------------
  Vec3 gc{};
  int gyaw = 0;
  bool gquarter = false;
  const bool haveTarget = p.tool == kSelect && p.Target(gc, gyaw, gquarter) && p.TargetEditable();
  const float gdist = haveTarget ? (gc - v.eye).len() : 1.0f;
  const float gL = std::max(6.0f, gdist * 0.12f);
  const float gR = gL * 0.8f;
  int hoverAxis = -1;
  if (haveTarget && mouseFree) {
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    ImVec2 a, b;
    float bestD = 8.0f;
    for (int i = 0; i < 3; i++)
      if (v.P(gc, a) && v.P(gc + axes[i] * gL, b)) {
        const float d = SegDist(mouse, a, b);
        if (d < bestD) {
          bestD = d;
          hoverAxis = i;
        }
      }
    for (int k = 0; k < 48; k++) {
      const float t0 = (float)k / 48.0f * 2 * kPi, t1 = (float)(k + 1) / 48.0f * 2 * kPi;
      if (v.P(gc + Vec3{std::sin(t0) * gR, 0, std::cos(t0) * gR}, a) &&
          v.P(gc + Vec3{std::sin(t1) * gR, 0, std::cos(t1) * gR}, b)) {
        const float d = SegDist(mouse, a, b);
        if (d < bestD) {
          bestD = d;
          hoverAxis = 3;
        }
      }
    }
  }
  auto axisParam = [&](int ax) {
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const Vec3 a = axes[ax];
    const Vec3 w0 = p.dragC - v.eye;
    const float b = a.dot(rd);
    const float den = 1.0f - b * b;
    if (std::fabs(den) < 1e-4f) return 0.0f;
    return (b * rd.dot(w0) - a.dot(w0)) / den;
  };
  auto ringAngle = [&]() {
    if (std::fabs(rd.y) < 1e-4f) return 0.0f;
    const float t = (p.dragC.y - v.eye.y) / rd.y;
    const Vec3 q = v.eye + rd * t;
    return std::atan2(q.x - p.dragC.x, q.z - p.dragC.z);
  };

  const bool lmbClick = mouseFree && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool lmbUp = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  const bool lmbDouble = mouseFree && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

  if (p.dragAxis >= 0) {
    if (p.dragAxis < 3) {
      const float dtv = axisParam(p.dragAxis) - p.dragT0;
      const int step = shift ? 10 : 1;
      const int n = (int)std::lround(dtv / (float)step) * step;
      p.dragDelta = {p.dragAxis == 0 ? n : 0, p.dragAxis == 1 ? n : 0, p.dragAxis == 2 ? n : 0};
    } else {
      const float da = (ringAngle() - p.dragA0) * 180.0f / kPi;
      const int snap = gquarter ? 90 : 15;
      const int d = (int)std::lround(da / (float)snap) * snap;
      p.dragYaw = ((gyaw + d) % 360 + 360) % 360;
    }
    if (lmbUp) {
      if (p.dragAxis < 3 && (p.dragDelta.x | p.dragDelta.y | p.dragDelta.z) != 0)
        p.MoveSelection(p.dragDelta, 0, false);
      else if (p.dragAxis == 3 && p.dragYaw != gyaw)
        p.MoveSelection({0, 0, 0}, p.dragYaw, true);
      p.dragAxis = -1;
    }
  } else if (lmbClick && hoverAxis >= 0 && !alt) {
    p.dragAxis = hoverAxis;
    p.dragC = gc;
    p.dragDelta = {0, 0, 0};
    p.dragYaw = gyaw;
    if (hoverAxis < 3) p.dragT0 = axisParam(hoverAxis);
    else p.dragA0 = ringAngle();
  } else if (keysFree && !ctrl && (p.tool == kSelect) && haveTarget) {
    // Keyboard nudges: arrows x / z, PgUp / PgDn y, R turns.
    const int step = shift ? 10 : 1;
    IVec3 by{0, 0, 0};
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) by.x -= step;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) by.x += step;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) by.z += step;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) by.z -= step;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) by.y += step;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) by.y -= step;
    if ((by.x | by.y | by.z) != 0) p.MoveSelection(by, 0, false);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
      const int st = gquarter ? 90 : 15;
      p.MoveSelection({0, 0, 0}, ((gyaw + (shift ? -st : st)) % 360 + 360) % 360, true);
    }
  }

  // ---- delete / duplicate the selection ---------------------------------------------
  if (keysFree && p.tool == kSelect && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
    if (!p.selSlot.empty()) {
      if (p.Run("slot.remove", Json{{"name", p.selSlot}}, "removed slot " + p.selSlot + " (Ctrl+Z to undo)"))
        p.selSlot.clear();
    } else if (!p.selRef.empty()) {
      if (p.Run("ref.delete", Json{{"id", p.selRef}}, "deleted " + p.selRef + " (Ctrl+Z to undo)"))
        p.selRef.clear();
    }
  }
  if (keysFree && ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !p.selRef.empty() && p.store) {
    std::string nid;
    for (int n = 2; n < 1000; n++) {
      nid = p.selRef + "_" + std::to_string(n);
      if (p.store->Find(nid) == nullptr) break;
    }
    if (p.Run("ref.duplicate", Json{{"id", p.selRef}, {"as", nid}, {"by", V3({10, 0, 0})}},
              "duplicated as " + nid))
      p.selRef = nid;
  }

  // ---- the tools ------------------------------------------------------------------
  const bool gizmoBusy = p.dragAxis >= 0 || (hoverAxis >= 0 && lmbClick);
  const uint16_t curMat = (uint16_t)p.mat;
  auto toHouse = [&](IVec3 w) { return editor::WorldToHouse(w, ipos, iyaw); };
  const bool voxelReady = se != nullptr && haveInst;
  if (VoxelTool(p.tool) && se != nullptr && !haveInst && lmbClick)
    p.Say("this house was opened by asset name: open a PLACED copy to edit it in the world", true);

  // Alt+click = eyedropper in any voxel tool.
  if (voxelReady && lmbClick && (alt || p.tool == kEyedrop) && hit.ok && !gizmoBusy) {
    uint16_t m = 0;
    if (hit.open) m = se->Get(toHouse(hit.cell));
    else if (!hit.on.empty() && p.store) {
      const refs::Ref* r = p.store->Find(hit.on);
      structures::Asset a;
      std::string err;
      std::vector<std::string> w;
      if (r && structures::LoadAsset(p.assetDir, r->base, true, a, err, w)) {
        const IVec3 hc = editor::WorldToHouse(hit.cell, r->pos, r->yaw);
        for (const PrefabModel& mm : a.prefab.models)
          for (const PrefabVoxel& vv : mm.voxels)
            if (mm.offset.x + vv.x - a.origin.x == hc.x && mm.offset.y + vv.y - a.origin.y == hc.y &&
                mm.offset.z + vv.z - a.origin.z == hc.z)
              m = vv.material;
      }
    }
    if (m != 0) {
      p.mat = m;
      p.Say("picked " + (m < ui.materialNames.size() ? ui.materialNames[m] : std::to_string(m)), false);
    } else {
      p.Say("that is the ground, not the house: the eyedropper reads house voxels", true);
    }
  } else if (voxelReady && !alt && !gizmoBusy) {
    switch (p.tool) {
      case kPencil:
      case kBrush: {
        if (lmbClick && hit.ok) {
          S.BeginGroup(p.tool == kPencil ? (shift ? "erase voxels" : "draw voxels")
                                         : (shift ? "erase brush" : "paint brush"));
          p.stroking = true;
          p.lastStroke = {INT32_MIN, 0, 0};
        }
        if (p.stroking && lmbDown && hit.ok) {
          const bool erase = shift;
          const IVec3 wc = erase || p.tool == kBrush ? hit.cell : hit.Adjacent();
          if (wc.x != p.lastStroke.x || wc.y != p.lastStroke.y || wc.z != p.lastStroke.z) {
            p.lastStroke = wc;
            const IVec3 h = toHouse(wc);
            std::string err;
            bool ok;
            if (p.tool == kPencil)
              ok = S.Run("vox.set", Json{{"pos", V3(h)}, {"mat", erase ? 0 : (int)curMat}}, &err);
            else
              ok = S.Run("vox.brush", Json{{"pos", V3(h)}, {"radius", p.brushR},
                                           {"shape", p.brushCube ? "cube" : "sphere"},
                                           {"mat", erase ? 0 : (int)curMat}},
                         &err);
            if (!ok) p.Say(err, true);
            p.wantsTick = true;
          }
        }
        if (p.stroking && lmbUp) {
          S.EndGroup();
          p.stroking = false;
        }
        break;
      }
      case kBox: {
        if (p.pasting) {
          IVec3 at;
          if (lmbClick && p.PasteAnchor(hit, at)) {
            p.Run("vox.paste",
                  Json{{"pos", V3(at)}, {"rot", p.pasteRot},
                       {"mirror", p.pasteMirror == 1 ? "x" : p.pasteMirror == 2 ? "z" : "none"},
                       {"air", p.pasteSkipAir ? "skip" : "keep"}},
                  "pasted (Ctrl+Z to undo); click again to paste another, Esc to stop");
          }
          break;
        }
        if (lmbClick && hit.ok) {
          p.boxDrag = true;
          p.haveBox = true;
          p.boxA = p.boxB = toHouse(hit.cell);
        }
        if (p.boxDrag && lmbDown && hit.ok) p.boxB = toHouse(hit.cell);
        if (p.boxDrag && lmbUp) p.boxDrag = false;
        break;
      }
      case kLine: {
        if (lmbClick && hit.ok) {
          const IVec3 h = toHouse(shift ? hit.cell : hit.Adjacent());
          if (!p.lineHave) {
            p.lineHave = true;
            p.lineA = h;
            p.Say("line: now click the other end (Esc cancels)", false);
          } else {
            p.Run("vox.line", Json{{"from", V3(p.lineA)}, {"to", V3(h)}, {"mat", (int)curMat},
                                   {"radius", p.lineR}},
                  "beam drawn");
            p.lineHave = false;
          }
        }
        break;
      }
      default:
        break;
    }
  }

  // Box tool keys.
  if (p.tool == kBox && se != nullptr && keysFree) {
    const IVec3 lo{std::min(p.boxA.x, p.boxB.x), std::min(p.boxA.y, p.boxB.y), std::min(p.boxA.z, p.boxB.z)};
    const IVec3 hi{std::max(p.boxA.x, p.boxB.x), std::max(p.boxA.y, p.boxB.y), std::max(p.boxA.z, p.boxB.z)};
    const Json box{{"min", V3(lo)}, {"max", V3(hi)}};
    auto withBox = [&](Json extra) {
      Json a = box;
      for (auto it = extra.begin(); it != extra.end(); ++it) a[it.key()] = it.value();
      return a;
    };
    if (p.haveBox && !p.pasting) {
      if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        p.Run(shift ? "vox.box_shell" : "vox.box_fill", withBox(Json{{"mat", (int)curMat}}),
              shift ? "built the box's walls" : "filled the box");
      if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        p.Run("vox.box_hollow", box, "hollowed the box (inside emptied, faces kept)");
      if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) p.Run("vox.clear", box, "cleared the box");
      if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_T, false))
        p.Run("vox.replace", withBox(Json{{"from", p.replaceFrom}, {"to", (int)curMat}}),
              "replaced " + (p.replaceFrom < (int)ui.materialNames.size() ? ui.materialNames[p.replaceFrom] : std::string("?")));
      if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        p.Run("vox.copy", box, "copied the box (Ctrl+V to paste)");
      if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        S.BeginGroup("cut");
        if (p.Run("vox.copy", box)) p.Run("vox.clear", box, "cut the box (Ctrl+V to paste)");
        S.EndGroup();
      }
      const int step = shift ? 10 : 1;
      IVec3 by{0, 0, 0};
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) by.x -= step;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) by.x += step;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) by.z += step;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) by.z -= step;
      if ((by.x | by.z) != 0) {
        // Arrows are WORLD directions on screen; turned into the house frame.
        const IVec3 z0 = editor::WorldToHouse({0, 0, 0}, {0, 0, 0}, iyaw);
        const IVec3 d = editor::WorldToHouse(by, {0, 0, 0}, iyaw);
        const IVec3 hb{d.x - z0.x, d.y - z0.y, d.z - z0.z};
        if (ctrl) p.Run("vox.move", withBox(Json{{"by", V3(hb)}}), "moved the voxels");
        p.boxA = {p.boxA.x + hb.x, p.boxA.y, p.boxA.z + hb.z};
        p.boxB = {p.boxB.x + hb.x, p.boxB.y, p.boxB.z + hb.z};
      }
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        const int d = ImGui::IsKeyPressed(ImGuiKey_PageUp) ? 1 : -1;
        IVec3& top = p.boxA.y >= p.boxB.y ? p.boxA : p.boxB;
        IVec3& bot = p.boxA.y >= p.boxB.y ? p.boxB : p.boxA;
        if (shift) bot.y += d;
        else top.y += d;
      }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
      if (S.ctx.clip.valid) {
        p.pasting = true;
        p.ghostKey = -1;
        p.Say("paste: click to land it. R turns, M mirrors, Shift = skip air, Esc stops", false);
      } else {
        p.Say("nothing copied yet: select a box and Ctrl+C", true);
      }
    }
    if (p.pasting && !ctrl) {
      if (ImGui::IsKeyPressed(ImGuiKey_R, false)) p.pasteRot = (p.pasteRot + 1) & 3;
      if (ImGui::IsKeyPressed(ImGuiKey_M, false)) p.pasteMirror = (p.pasteMirror + 1) % 3;
      p.pasteSkipAir = shift;
    }
  }

  // Select / Place / Link clicks.
  if (lmbClick && !gizmoBusy && !alt) {
    if (p.tool == kSelect) {
      if (lmbDouble && !hoverRef.empty() && !hoverIsSlot && p.store) {
        const refs::Ref* r = p.store->Find(hoverRef);
        if (r && r->kind == "structure") OpenStructureRef(r->id);
      } else if (hoverIsSlot) {
        p.selSlot = hoverRef.substr(5);
        p.selRef.clear();
      } else if (!hoverRef.empty()) {
        p.selRef = hoverRef;
        p.selSlot.clear();
      } else if (hit.ok && !hit.on.empty()) {
        p.selRef = hit.on;
        p.selSlot.clear();
      } else {
        p.selRef.clear();
        p.selSlot.clear();
      }
    } else if (p.tool == kPlace && hit.ok) {
      const IVec3 at = hit.Adjacent();
      if (se != nullptr && haveInst) {
        const std::string kind = kSlotKinds[p.slotKind];
        std::string name;
        for (int n = 0; n < 1000; n++) {
          name = kind + "_" + std::to_string(n);
          if (se->FindSlot(name) < 0) break;
        }
        const IVec3 h = toHouse(at);
        const int y = p.placeYaw;
        Json props = Json::object();
        // P2 slot vocabulary defaults (what housegen writes).
        const bool alongX = (y % 180) == 0;
        if (kind == "door") {
          const IVec3 lo = alongX ? IVec3{h.x - 4, h.y, h.z} : IVec3{h.x, h.y, h.z - 4};
          const IVec3 hi = alongX ? IVec3{h.x + 4, h.y + 19, h.z} : IVec3{h.x, h.y + 19, h.z + 4};
          props["leaf"] = Json{{"min", V3(lo)}, {"max", V3(hi)}};
          const int inner = (y == 0 || y == 90) ? 0 : 1;
          props["hingeLine"] = alongX ? Json::array({lo.x, h.z + inner}) : Json::array({h.x + inner, lo.z});
          props["opens"] = "in";
          props["openAngle"] = 95;
        } else if (kind == "bed") {
          const IVec3 lo = alongX ? IVec3{h.x - 4, h.y, h.z - 9} : IVec3{h.x - 9, h.y, h.z - 4};
          const IVec3 hi = alongX ? IVec3{h.x + 4, h.y + 3, h.z + 9} : IVec3{h.x + 9, h.y + 3, h.z + 4};
          props["box"] = Json{{"min", V3(lo)}, {"max", V3(hi)}};
        } else if (kind == "container") {
          props["box"] = Json{{"min", V3({h.x - 3, h.y, h.z - 2})}, {"max", V3({h.x + 3, h.y + 4, h.z + 2})}};
          props["items"] = Json::array();
        } else if (kind == "marker") {
          props["tags"] = Json::array();
        } else if (kind == "waynode") {
          props["links"] = Json::array();
        }
        IVec3 pos = h;
        if (kind == "bed") pos = {h.x, h.y + 4, h.z};
        if (p.Run("slot.add", Json{{"name", name}, {"kind", kind}, {"pos", V3(pos)}, {"yaw", y}, {"props", props}},
                  "added slot " + name + " to " + se->name + " (Ctrl+S saves the house)")) {
          p.selSlot = name;
          p.selRef.clear();
          p.tool = kSelect;
        }
      } else if (p.store != nullptr) {
        const std::string kind = kPlaceKinds[p.placeKind];
        std::string group = p.placeGroup;
        std::string stem = kind;
        if (kind == "structure" && !p.structAssets.empty())
          stem = Tail(p.structAssets[(size_t)std::clamp(p.structPick, 0, (int)p.structAssets.size() - 1)]);
        std::string id;
        for (int n = 1; n < 10000; n++) {
          id = group + "/" + stem + "_" + std::to_string(n);
          if (p.store->Find(id) == nullptr) break;
        }
        Json a{{"id", id}, {"kind", kind}, {"pos", V3(at)}, {"yaw", kind == "structure" ? (p.placeYaw / 90) * 90 : p.placeYaw}};
        if (kind == "npc") {
          a["base"] = std::string(p.placeBase[0] ? p.placeBase : "human");
          a["props"] = Json{{"name", "Villager"}};
        } else if (kind == "structure") {
          if (p.structAssets.empty()) {
            p.Say("no structures in assets/structures (make one on the tuner's Structures page)", true);
            a.clear();
          } else {
            a["base"] = p.structAssets[(size_t)std::clamp(p.structPick, 0, (int)p.structAssets.size() - 1)];
          }
        } else if (kind == "marker") {
          a["props"] = Json{{"tags", Json::array()}};
        } else if (kind == "container") {
          a["props"] = Json{{"items", Json::array()}};
        }
        if (!a.empty() && p.Run("ref.place", a, "placed " + id + " (Ctrl+Z to undo)")) {
          p.selRef = id;
          p.selSlot.clear();
          p.tool = kSelect;
        }
      } else {
        p.Say("no references are loaded for this map (a harness run)", true);
      }
    } else if (p.tool == kLink) {
      if (hoverRef.empty()) {
        p.Say("link: click a waynode", true);
      } else if (p.linkFrom.empty()) {
        p.linkFrom = hoverRef;
        p.Say("link: now click the waynode to join it to (Shift+click to unlink)", false);
      } else {
        const std::string a = p.linkFrom, b = hoverRef;
        p.linkFrom.clear();
        const bool aSlot = a.rfind("slot:", 0) == 0, bSlot = b.rfind("slot:", 0) == 0;
        if (aSlot && bSlot && se != nullptr) {
          const std::string an = a.substr(5), bn = b.substr(5);
          const int ia = se->FindSlot(an);
          if (ia >= 0) {
            Json links = se->Slots()[(size_t)ia].props.contains("links") &&
                                 se->Slots()[(size_t)ia].props["links"].is_array()
                             ? se->Slots()[(size_t)ia].props["links"]
                             : Json::array();
            bool had = false;
            for (size_t i = links.size(); i-- > 0;)
              if (links[i] == bn) {
                had = true;
                if (shift) links.erase(links.begin() + (ptrdiff_t)i);
              }
            if (shift && !had) p.Say(an + " does not link to " + bn + " (links are written on the first one clicked)", true);
            else if (!shift && had) p.Say(an + " already links to " + bn, true);
            else {
              if (!shift) links.push_back(bn);
              p.Run("slot.set_prop", Json{{"name", an}, {"key", "links"}, {"value", links}},
                    (shift ? "unlinked " : "linked ") + an + " - " + bn);
            }
          }
        } else if (aSlot || bSlot) {
          // A slot of the OPEN house is not in the store under its id until saved.
          const std::string sa = aSlot ? S.ctx.instance + "/" + a.substr(5) : a;
          const std::string sb = bSlot ? S.ctx.instance + "/" + b.substr(5) : b;
          p.Run(shift ? "ref.unlink" : "ref.link", Json{{"a", sa}, {"b", sb}},
                (shift ? "unlinked " : "linked ") + sa + " - " + sb);
        } else {
          p.Run(shift ? "ref.unlink" : "ref.link", Json{{"a", a}, {"b", b}},
                (shift ? "unlinked " : "linked ") + a + " - " + b);
        }
      }
    }
  }
  if (lmbUp && p.stroking) {
    S.EndGroup();
    p.stroking = false;
  }

  // ---- the world preview of the open house -----------------------------------------
  if (se != nullptr) {
    std::vector<std::pair<IVec3, uint16_t>> ch = se->TakeChanged();
    if (haveInst)
      for (const auto& [h, m] : ch) p.worldCells.push_back({editor::HouseToWorld(h, ipos, iyaw), m});
    if (!ch.empty()) p.wantsTick = true;
  }
  // Other buffers (an undo that reached a house since closed) preview through
  // the copy they were last opened by.
  if (!S.ctx.active.empty() && !S.ctx.instance.empty()) p.bufInstance[S.ctx.active] = S.ctx.instance;
  for (auto& [n, b] : S.ctx.structs) {
    if (b.get() == se) continue;
    std::vector<std::pair<IVec3, uint16_t>> ch = b->TakeChanged();
    auto bi = p.bufInstance.find(n);
    const refs::Ref* r = bi != p.bufInstance.end() && p.store ? p.store->Find(bi->second) : nullptr;
    if (r == nullptr || ch.empty()) continue;
    for (const auto& [h, m] : ch) p.worldCells.push_back({editor::HouseToWorld(h, r->pos, r->yaw), m});
    p.wantsTick = true;
  }

  // =================================================================================
  // DRAWING: the world overlays
  // =================================================================================

  // All refs within reach: box, label, links, door arcs.
  if (p.store != nullptr) {
    const refs::WayGraph& g = refs::Graph(*p.store);
    for (const refs::WayGraph::Edge& e : g.edges) {
      if (e.a < 0 || e.b < 0 || e.a >= (int)g.nodes.size() || e.b >= (int)g.nodes.size()) continue;
      const Vec3 a = g.nodes[(size_t)e.a].p + Vec3{0.5f, 0.5f, 0.5f}, b = g.nodes[(size_t)e.b].p + Vec3{0.5f, 0.5f, 0.5f};
      if ((a - v.eye).len() > 1500.0f) continue;
      v.Seg(dl, a, b, e.autoLinked ? IM_COL32(90, 210, 210, 110) : IM_COL32(90, 210, 210, 230),
            e.door.empty() ? 2.0f : 3.0f);
    }
    for (const auto& [id, r] : p.store->All()) {
      if (!S.ctx.instance.empty() && r.derivedFrom == S.ctx.instance) continue;
      const Vec3 c{r.pos.x + 0.5f, r.pos.y + 0.5f, r.pos.z + 0.5f};
      const float dist = (c - v.eye).len();
      if (dist > 1500.0f) continue;
      Vec3 lo, hi;
      if (!p.RefBox(r, lo, hi)) continue;
      const bool sel = id == p.selRef;
      const bool hov = id == hoverRef;
      ImU32 col = KindColor(r.kind);
      if (r.kind == "structure") {
        if (id == S.ctx.instance) continue;   // drawn below, as the open house
        v.Box(dl, lo, hi, Alpha(col, sel ? 255 : hov ? 200 : 90), sel ? 2.5f : 1.0f);
      } else {
        v.Box(dl, lo, hi, Alpha(col, sel || hov ? 255 : 170), sel ? 2.5f : 1.5f);
      }
      if (r.kind == "door") {
        const refs::DoorGeom dg = refs::DoorGeometry(r);
        if (dg.ok && dist < 400.0f) {
          const float y = dg.hinge.y + 0.05f;
          Vec3 prev = dg.LatchAt(0.0f);
          for (int i = 1; i <= 12; i++) {
            const Vec3 q = dg.LatchAt(dg.openSign * dg.openRad * (float)i / 12.0f);
            v.Seg(dl, {prev.x, y, prev.z}, {q.x, y, q.z}, Alpha(ui::ColEmber(), 180), 1.5f);
            prev = q;
          }
          v.Seg(dl, dg.hinge, dg.hinge + Vec3{0, (float)dg.height, 0}, ui::ColBloodHi(), 2.5f);
        }
      }
      // Labels declutter: always for the selected / hovered one; waynodes
      // only while linking; a house's slots only up close.
      const bool slot = !r.derivedFrom.empty();
      const bool wantLabel = sel || hov ||
                             (r.kind == "waynode" ? p.tool == kLink && dist < 300.0f
                                                  : dist < (slot ? 150.0f : 500.0f));
      if (wantLabel) {
        const std::string t = slot ? Tail(id) : r.kind + ": " + Tail(id);
        v.Label(dl, {(lo.x + hi.x) * 0.5f, hi.y + 0.5f, (lo.z + hi.z) * 0.5f},
                sel ? ui::ColGoldPale() : Alpha(col, slot ? 190 : 235), t.c_str());
      }
    }
  }

  // The open house: its box, its origin, its front, its slots.
  if (se != nullptr && haveInst) {
    IVec3 lo, hi;
    if (p.OpenBounds(lo, hi)) {
      Vec3 wl, wh;
      HouseBoxWorld(lo, hi, ipos, iyaw, wl, wh);
      v.Box(dl, wl, wh, IM_COL32(232, 212, 154, 230), 2.0f);
      const IVec3 f0 = editor::HouseToWorld({0, 0, hi.z + 4}, ipos, iyaw);
      const Vec3 o{ipos.x + 0.5f, (float)ipos.y, ipos.z + 0.5f};
      v.Seg(dl, o, Vec3{f0.x + 0.5f, (float)ipos.y, f0.z + 0.5f}, ui::ColBloodHi(), 3.0f);
      v.Label(dl, Vec3{f0.x + 0.5f, (float)ipos.y + 1, f0.z + 0.5f}, ui::ColBloodHi(), "front");
      char t[160];
      std::snprintf(t, sizeof t, "editing %s%s  (%.1f x %.1f x %.1f m)", se->name.c_str(), se->Dirty() ? " *" : "",
                    (hi.x - lo.x + 1) * 0.1f, (hi.y - lo.y + 1) * 0.1f, (hi.z - lo.z + 1) * 0.1f);
      v.Label(dl, Vec3{(wl.x + wh.x) * 0.5f, wh.y + 2, (wl.z + wh.z) * 0.5f}, ui::ColGoldPale(), t);
    }
    for (const refs::Ref& s : p.OpenSlotRefs()) {
      Vec3 blo, bhi;
      if (!p.RefBox(s, blo, bhi)) continue;
      const std::string nm = s.props.value("slot", std::string());
      const bool sel = nm == p.selSlot;
      const bool hov = hoverIsSlot && hoverRef == "slot:" + nm;
      const ImU32 col = KindColor(s.kind);
      v.Box(dl, blo, bhi, Alpha(col, sel || hov ? 255 : 190), sel ? 2.5f : 1.5f);
      if (s.kind == "door") {
        const refs::DoorGeom dg = refs::DoorGeometry(s);
        if (dg.ok) {
          const float y = dg.hinge.y + 0.05f;
          Vec3 prev = dg.LatchAt(0.0f);
          for (int i = 1; i <= 16; i++) {
            const Vec3 q = dg.LatchAt(dg.openSign * dg.openRad * (float)i / 16.0f);
            v.Seg(dl, {prev.x, y, prev.z}, {q.x, y, q.z}, ui::ColEmber(), 2.0f);
            prev = q;
          }
          const Vec3 l = dg.LatchAt(dg.openSign * dg.openRad);
          v.Seg(dl, {dg.hinge.x, y, dg.hinge.z}, {l.x, y, l.z}, Alpha(ui::ColEmber(), 150), 1.5f);
          v.Seg(dl, dg.hinge, dg.hinge + Vec3{0, (float)dg.height, 0}, ui::ColBloodHi(), 3.0f);
        }
      }
      if (s.kind == "waynode" && s.props.contains("links") && s.props["links"].is_array()) {
        for (const refs::Ref& o : p.OpenSlotRefs())
          for (const Json& l : s.props["links"])
            if (l.is_string() && l.get<std::string>() == o.id)
              v.Seg(dl, Vec3{s.pos.x + 0.5f, s.pos.y + 0.5f, s.pos.z + 0.5f},
                    Vec3{o.pos.x + 0.5f, o.pos.y + 0.5f, o.pos.z + 0.5f}, IM_COL32(90, 210, 210, 230), 2.0f);
      }
      if (s.kind != "waynode" || sel || hov || p.tool == kLink)
        v.Label(dl, {(blo.x + bhi.x) * 0.5f, bhi.y + 0.5f, (blo.z + bhi.z) * 0.5f},
                sel ? ui::ColGoldPale() : Alpha(col, 240), nm.c_str());
    }
  }

  // The box selection.
  if (p.haveBox && se != nullptr && haveInst) {
    const IVec3 lo{std::min(p.boxA.x, p.boxB.x), std::min(p.boxA.y, p.boxB.y), std::min(p.boxA.z, p.boxB.z)};
    const IVec3 hi{std::max(p.boxA.x, p.boxB.x), std::max(p.boxA.y, p.boxB.y), std::max(p.boxA.z, p.boxB.z)};
    Vec3 wl, wh;
    HouseBoxWorld(lo, hi, ipos, iyaw, wl, wh);
    // A faint fill on the top face so the box reads as a volume.
    ImVec2 q[4];
    const Vec3 tq[4] = {{wl.x, wh.y, wl.z}, {wh.x, wh.y, wl.z}, {wh.x, wh.y, wh.z}, {wl.x, wh.y, wh.z}};
    bool okq = true;
    for (int i = 0; i < 4; i++) okq = okq && v.P(tq[i], q[i]);
    if (okq) dl->AddQuadFilled(q[0], q[1], q[2], q[3], IM_COL32(90, 170, 255, 40));
    v.Box(dl, wl, wh, IM_COL32(110, 190, 255, 255), 2.5f);
    char t[96];
    std::snprintf(t, sizeof t, "%d x %d x %d", hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1);
    v.Label(dl, {(wl.x + wh.x) * 0.5f, wh.y + 0.5f, (wl.z + wh.z) * 0.5f}, IM_COL32(150, 210, 255, 255), t);
  }

  // The cursor: which cell a click acts on.
  if (hit.ok && voxelReady && !p.pasting && mouseFree && p.dragAxis < 0 && hoverAxis < 0) {
    const bool erase = shift && (p.tool == kPencil || p.tool == kBrush);
    const bool onCell = erase || alt || p.tool == kEyedrop || p.tool == kBox || p.tool == kBrush;
    const IVec3 c = onCell ? hit.cell : hit.Adjacent();
    const ImU32 col = erase ? ui::ColBloodHi() : alt || p.tool == kEyedrop ? ui::ColGoldHi() : IM_COL32(255, 255, 255, 230);
    if (p.tool == kBrush) {
      const float r = (float)p.brushR + 0.5f;
      const Vec3 cc{c.x + 0.5f, c.y + 0.5f, c.z + 0.5f};
      if (p.brushCube) {
        v.Box(dl, cc - Vec3{r, r, r}, cc + Vec3{r, r, r}, col, 1.5f);
      } else {
        for (int k = 0; k < 3; k++) {
          Vec3 prev{};
          for (int i = 0; i <= 32; i++) {
            const float a = (float)i / 32.0f * 2 * kPi;
            const Vec3 q = k == 0 ? Vec3{std::cos(a) * r, std::sin(a) * r, 0}
                         : k == 1 ? Vec3{std::cos(a) * r, 0, std::sin(a) * r}
                                  : Vec3{0, std::cos(a) * r, std::sin(a) * r};
            if (i > 0) v.Seg(dl, cc + prev, cc + q, col, 1.5f);
            prev = q;
          }
        }
      }
    } else {
      v.Box(dl, {(float)c.x, (float)c.y, (float)c.z}, {(float)c.x + 1, (float)c.y + 1, (float)c.z + 1}, col, 2.0f);
      if (p.tool == kPencil && !erase && p.mat < (int)ui.materialColors.size())
        for (int a = 0; a < 3; a++)
          for (int sd = 0; sd < 2; sd++) v.Face(dl, c, a, sd, Alpha(ui.materialColors[(size_t)p.mat], 110));
    }
    if (p.tool == kLine && p.lineHave) {
      const IVec3 a = editor::HouseToWorld(p.lineA, ipos, iyaw);
      v.Seg(dl, {a.x + 0.5f, a.y + 0.5f, a.z + 0.5f}, {c.x + 0.5f, c.y + 0.5f, c.z + 0.5f}, ui::ColGoldHi(), 3.0f);
    }
  }

  // The paste ghost.
  if (p.pasting && se != nullptr && haveInst) {
    p.BuildGhost();
    IVec3 at;
    if (p.PasteAnchor(hit, at)) {
      struct F {
        float d;
        const Impl::GhostFace* g;
      };
      std::vector<F> fs;
      fs.reserve(p.ghost.size());
      for (const Impl::GhostFace& g : p.ghost) {
        const IVec3 w = editor::HouseToWorld({at.x + g.c.x, at.y + g.c.y, at.z + g.c.z}, ipos, iyaw);
        const Vec3 cc{w.x + 0.5f, w.y + 0.5f, w.z + 0.5f};
        fs.push_back({(cc - v.eye).len(), &g});
      }
      std::sort(fs.begin(), fs.end(), [](const F& a, const F& b) { return a.d > b.d; });
      size_t drawn = 0;
      for (const F& f : fs) {
        if (drawn++ > 20000) break;
        const Impl::GhostFace& g = *f.g;
        const IVec3 w = editor::HouseToWorld({at.x + g.c.x, at.y + g.c.y, at.z + g.c.z}, ipos, iyaw);
        // The face axis/side turn with the instance too.
        IVec3 n{g.axis == 0 ? (g.side ? 1 : -1) : 0, g.axis == 1 ? (g.side ? 1 : -1) : 0,
                g.axis == 2 ? (g.side ? 1 : -1) : 0};
        const IVec3 z0 = editor::HouseToWorld({0, 0, 0}, {0, 0, 0}, iyaw);
        const IVec3 nw = editor::HouseToWorld(n, {0, 0, 0}, iyaw);
        n = {nw.x - z0.x, nw.y - z0.y, nw.z - z0.z};
        const int ax = n.x != 0 ? 0 : n.y != 0 ? 1 : 2;
        const int sd = (n.x + n.y + n.z) > 0 ? 1 : 0;
        const Vec3 fc{w.x + 0.5f + n.x * 0.5f, w.y + 0.5f + n.y * 0.5f, w.z + 0.5f + n.z * 0.5f};
        if (Vec3{(float)n.x, (float)n.y, (float)n.z}.dot(v.eye - fc) <= 0) continue;
        const ImU32 mc = g.m < ui.materialColors.size() ? ui.materialColors[g.m] : IM_COL32(200, 200, 200, 255);
        v.Face(dl, w, ax, sd, Alpha(mc, 150));
      }
      const IVec3 hiG{at.x + p.ghostSize.x - 1, at.y + p.ghostSize.y - 1, at.z + p.ghostSize.z - 1};
      Vec3 wl, wh;
      HouseBoxWorld(at, hiG, ipos, iyaw, wl, wh);
      v.Box(dl, wl, wh, IM_COL32(120, 230, 140, 255), 2.0f);
      char t[96];
      std::snprintf(t, sizeof t, "paste  turn %d deg  mirror %s%s", p.pasteRot * 90,
                    p.pasteMirror == 0 ? "none" : p.pasteMirror == 1 ? "x" : "z", p.pasteSkipAir ? "  (skip air)" : "");
      v.Label(dl, {(wl.x + wh.x) * 0.5f, wh.y + 0.5f, (wl.z + wh.z) * 0.5f}, IM_COL32(150, 240, 160, 255), t);
    }
  }

  // Place tool preview.
  if (p.tool == kPlace && hit.ok && mouseFree) {
    const IVec3 at = hit.Adjacent();
    if (se == nullptr && std::string(kPlaceKinds[p.placeKind]) == "structure" && !p.structAssets.empty()) {
      structures::Asset a;
      std::string err;
      std::vector<std::string> w;
      const std::string base = p.structAssets[(size_t)std::clamp(p.structPick, 0, (int)p.structAssets.size() - 1)];
      if (structures::LoadAsset(p.assetDir, base, false, a, err, w)) {
        const structures::Frame f = structures::MakeFrame(a, at, (p.placeYaw / 90) * 90);
        IVec3 lo, hi;
        f.Box(lo, hi);
        v.Box(dl, {(float)lo.x, (float)lo.y, (float)lo.z}, {(float)hi.x + 1, (float)hi.y + 1, (float)hi.z + 1},
              IM_COL32(190, 130, 220, 255), 2.0f);
        const IVec3 fr = f.Cell({a.origin.x, a.origin.y, a.size.z + 4});
        v.Label(dl, {fr.x + 0.5f, fr.y + 0.5f, fr.z + 0.5f}, ui::ColBloodHi(), "front");
      }
    } else {
      v.Box(dl, {(float)at.x - 1, (float)at.y, (float)at.z - 1}, {(float)at.x + 2, (float)at.y + 3, (float)at.z + 2},
            ui::ColGoldHi(), 2.0f);
      // The heading it will face.
      const float a = (float)p.placeYaw * kPi / 180.0f;
      const Vec3 c{at.x + 0.5f, at.y + 0.5f, at.z + 0.5f};
      v.Seg(dl, c, c + Vec3{std::sin(a) * 5, 0, std::cos(a) * 5}, ui::ColBloodHi(), 2.5f);
    }
  }

  // The link in progress.
  if (p.tool == kLink && !p.linkFrom.empty()) {
    Vec3 a{};
    bool ok = false;
    if (p.linkFrom.rfind("slot:", 0) == 0) {
      for (const refs::Ref& s : p.OpenSlotRefs())
        if ("slot:" + s.props.value("slot", std::string()) == p.linkFrom) {
          a = {s.pos.x + 0.5f, s.pos.y + 0.5f, s.pos.z + 0.5f};
          ok = true;
        }
    } else if (p.store) {
      if (const refs::Ref* r = p.store->Find(p.linkFrom)) {
        a = {r->pos.x + 0.5f, r->pos.y + 0.5f, r->pos.z + 0.5f};
        ok = true;
      }
    }
    ImVec2 s;
    if (ok && v.P(a, s)) dl->AddLine(s, mouse, IM_COL32(90, 210, 210, 255), 2.0f);
  }

  // The gizmo.
  if (haveTarget) {
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const ImU32 ac[3] = {IM_COL32(230, 80, 70, 255), IM_COL32(110, 220, 90, 255), IM_COL32(80, 140, 240, 255)};
    Vec3 c = gc;
    if (p.dragAxis >= 0 && p.dragAxis < 3)
      c = gc + Vec3{(float)p.dragDelta.x, (float)p.dragDelta.y, (float)p.dragDelta.z};
    for (int i = 0; i < 3; i++) {
      const bool h = hoverAxis == i || p.dragAxis == i;
      v.Seg(dl, c, c + axes[i] * gL, h ? IM_COL32(255, 240, 120, 255) : ac[i], h ? 4.0f : 3.0f);
      ImVec2 tip;
      if (v.P(c + axes[i] * gL, tip)) dl->AddCircleFilled(tip, h ? 6.0f : 5.0f, h ? IM_COL32(255, 240, 120, 255) : ac[i]);
    }
    const bool rh = hoverAxis == 3 || p.dragAxis == 3;
    Vec3 prev{};
    for (int k = 0; k <= 48; k++) {
      const float t = (float)k / 48.0f * 2 * kPi;
      const Vec3 q = c + Vec3{std::sin(t) * gR, 0, std::cos(t) * gR};
      if (k > 0) v.Seg(dl, prev, q, rh ? IM_COL32(255, 240, 120, 255) : IM_COL32(230, 200, 90, 200), rh ? 3.0f : 2.0f);
      prev = q;
    }
    const int showYaw = p.dragAxis == 3 ? p.dragYaw : gyaw;
    const float ya = (float)showYaw * kPi / 180.0f;
    v.Seg(dl, c, c + Vec3{std::sin(ya) * gR, 0, std::cos(ya) * gR}, ui::ColBloodHi(), 3.0f);
    char t[96];
    if (p.dragAxis >= 0 && p.dragAxis < 3)
      std::snprintf(t, sizeof t, "%+d %c  (%s)", p.dragDelta.x + p.dragDelta.y + p.dragDelta.z, "xyz"[p.dragAxis],
                    shift ? "1 m steps" : "hold Shift: 1 m steps");
    else if (p.dragAxis == 3)
      std::snprintf(t, sizeof t, "yaw %d deg", p.dragYaw);
    else
      std::snprintf(t, sizeof t, "yaw %d", gyaw);
    v.Label(dl, c + Vec3{0, gL + 1.5f, 0}, IM_COL32(255, 240, 160, 255), t);
  }

  // =================================================================================
  // PANELS
  // =================================================================================
  const float W = v.W, H = v.H;
  const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

  // ---- the toolbar (left) ----
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(330, 0), ImVec2(330, H - 90));
  if (ImGui::Begin("EDITOR###sv_editor_tools", nullptr, fixed | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(V4(ui::ColEmber()), "WORLD PAUSED");
    ImGui::SameLine();
    ImGui::TextDisabled("- each edit runs one tick");
    ImGui::SetItemTooltip("The simulation is stopped while you edit, so nothing burns,\n"
                          "flows or walks off. Every edit advances it by exactly ONE tick\n"
                          "so the change reaches the world. F8 leaves the editor.");
    ImGui::Separator();
    // Tools: two rows of four.
    for (int k = 1; k < kToolCount; k++) {
      const bool needHouse = VoxelTool(k) && se == nullptr;
      if (needHouse) ImGui::BeginDisabled();
      const bool on = p.tool == k;
      if (on) ImGui::PushStyleColor(ImGuiCol_Button, V4(ui::ColGoldDim()));
      char lbl[48];
      std::snprintf(lbl, sizeof lbl, "%d %s", k, kToolName[k]);
      if (ImGui::Button(lbl, ImVec2(74, 0))) {
        p.tool = k;
        p.lineHave = false;
        p.linkFrom.clear();
      }
      if (on) ImGui::PopStyleColor();
      if (needHouse) ImGui::EndDisabled();
      ImGui::SetItemTooltip("%s%s", kToolTip[k], needHouse ? "\n\n(open a house first)" : "");
      if (k % 4 != 0 && k != kToolCount - 1) ImGui::SameLine();
    }
    ImGui::Separator();

    // ---- tool options ----
    auto matLine = [&](const char* what) {
      OverlayCurrentMaterial(ui, p.mat, what);
      ImGui::SameLine();
      if (ImGui::SmallButton(p.matPickerOpen ? "hide##mp" : "change##mp")) p.matPickerOpen = !p.matPickerOpen;
      if (p.matPickerOpen) {
        std::vector<int> ids;
        for (int i = 1; i < (int)ui.materialNames.size() && i <= 255; i++) ids.push_back(i);
        if (OverlayMaterialPicker(ui, "edmat", ids, p.mat, p.matSearch, sizeof p.matSearch, 220.0f))
          p.matPickerOpen = false;
      }
    };
    switch (p.tool) {
      case kSelect:
        ImGui::TextWrapped("Click a thing to select it. Drag an ARROW to move it (Shift: 1 m steps), "
                           "drag the RING to turn it. Arrows / PgUp / PgDn nudge, R turns, Delete removes, "
                           "Ctrl+D duplicates. Double-click a house to open it.");
        break;
      case kPencil:
        matLine("draw with");
        ImGui::TextDisabled("click: place on a face   Shift: erase   Alt: pick");
        break;
      case kBrush:
        matLine("paint with");
        ImGui::SetNextItemWidth(140);
        ImGui::SliderInt("radius  [ ]", &p.brushR, 0, 16);
        ImGui::RadioButton("sphere", !p.brushCube) ? (void)(p.brushCube = false) : (void)0;
        ImGui::SameLine();
        ImGui::RadioButton("cube  (B)", p.brushCube) ? (void)(p.brushCube = true) : (void)0;
        ImGui::TextDisabled("Shift: erase   Alt: pick");
        break;
      case kBox: {
        matLine("fill with");
        if (!p.haveBox) {
          ImGui::TextWrapped("Drag across the house to select a box.");
        } else {
          const IVec3 lo{std::min(p.boxA.x, p.boxB.x), std::min(p.boxA.y, p.boxB.y), std::min(p.boxA.z, p.boxB.z)};
          const IVec3 hi{std::max(p.boxA.x, p.boxB.x), std::max(p.boxA.y, p.boxB.y), std::max(p.boxA.z, p.boxB.z)};
          int a[3] = {lo.x, lo.y, lo.z}, b[3] = {hi.x, hi.y, hi.z};
          ImGui::SetNextItemWidth(240);
          if (ImGui::InputInt3("min", a)) {
            p.boxA = {a[0], a[1], a[2]};
            p.boxB = hi;
          }
          ImGui::SetNextItemWidth(240);
          if (ImGui::InputInt3("max", b)) {
            p.boxA = lo;
            p.boxB = {b[0], b[1], b[2]};
          }
          ImGui::SetItemTooltip("house frame: 0 0 0 is the front-centre floor cell,\ny up, +z toward the front");
          const Json box{{"min", V3(lo)}, {"max", V3(hi)}};
          auto with = [&](Json e) {
            Json r = box;
            for (auto it = e.begin(); it != e.end(); ++it) r[it.key()] = it.value();
            return r;
          };
          const float bw = 90;
          if (ImGui::Button("Fill", ImVec2(bw, 0))) p.Run("vox.box_fill", with(Json{{"mat", p.mat}}), "filled the box");
          ImGui::SetItemTooltip("Enter");
          ImGui::SameLine();
          if (ImGui::Button("Walls", ImVec2(bw, 0))) p.Run("vox.box_shell", with(Json{{"mat", p.mat}}), "built the box's walls");
          ImGui::SetItemTooltip("Shift+Enter: the six faces in the material, inside untouched");
          ImGui::SameLine();
          if (ImGui::Button("Hollow", ImVec2(bw, 0))) p.Run("vox.box_hollow", box, "hollowed the box");
          ImGui::SetItemTooltip("Backspace: empty the inside, keep the faces");
          if (ImGui::Button("Clear", ImVec2(bw, 0))) p.Run("vox.clear", box, "cleared the box");
          ImGui::SetItemTooltip("Delete");
          ImGui::SameLine();
          if (ImGui::Button("Copy", ImVec2(bw, 0))) p.Run("vox.copy", box, "copied (Ctrl+V to paste)");
          ImGui::SetItemTooltip("Ctrl+C");
          ImGui::SameLine();
          if (ImGui::Button("Paste...", ImVec2(bw, 0))) {
            p.pasting = S.ctx.clip.valid;
            p.ghostKey = -1;
            if (!p.pasting) p.Say("nothing copied yet", true);
          }
          ImGui::SetItemTooltip("Ctrl+V: a ghost follows the cursor; click lands it");
          // Replace: from (eyedropped) -> the current material.
          ImGui::TextDisabled("replace");
          ImGui::SameLine();
          ImGui::SetNextItemWidth(110);
          const std::string fromName =
              p.replaceFrom > 0 && p.replaceFrom < (int)ui.materialNames.size() ? ui.materialNames[(size_t)p.replaceFrom] : "(pick)";
          if (ImGui::BeginCombo("##from", fromName.c_str())) {
            std::set<uint16_t> inHouse;
            for (int z = lo.z; z <= hi.z && inHouse.size() < 64; z++)
              for (int y = lo.y; y <= hi.y; y++)
                for (int x = lo.x; x <= hi.x; x++)
                  if (const uint16_t m = se ? se->Get({x, y, z}) : 0) inHouse.insert(m);
            for (uint16_t m : inHouse)
              if (m < ui.materialNames.size() && ImGui::Selectable(ui.materialNames[m].c_str(), m == p.replaceFrom))
                p.replaceFrom = m;
            ImGui::EndCombo();
          }
          ImGui::SetItemTooltip("the materials inside the box");
          ImGui::SameLine();
          ImGui::TextDisabled("->");
          ImGui::SameLine();
          if (ImGui::Button("current (T)"))
            p.Run("vox.replace", with(Json{{"from", p.replaceFrom}, {"to", p.mat}}), "replaced");
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
          ImGui::TextWrapped("PgUp / PgDn raise the top (Shift: the bottom). Arrows slide the box, Ctrl+arrows move its voxels.");
          ImGui::PopStyleColor();
        }
        if (p.pasting) {
          ImGui::TextColored(V4(IM_COL32(150, 240, 160, 255)), "pasting: click to land");
          if (ImGui::Button("turn 90 (R)")) p.pasteRot = (p.pasteRot + 1) & 3;
          ImGui::SameLine();
          if (ImGui::Button("mirror (M)")) p.pasteMirror = (p.pasteMirror + 1) % 3;
          ImGui::SameLine();
          if (ImGui::Button("stop (Esc)")) p.pasting = false;
        }
        break;
      }
      case kLine:
        matLine("beam of");
        ImGui::SetNextItemWidth(140);
        ImGui::SliderInt("thickness  [ ]", &p.lineR, 0, 8);
        ImGui::TextDisabled(p.lineHave ? "click the other end (Esc cancels)" : "click the first end");
        break;
      case kEyedrop:
        matLine("current");
        ImGui::TextDisabled("click a voxel of a house to take its material");
        break;
      case kPlace: {
        if (se != nullptr) {
          ImGui::TextWrapped("Adding a SLOT to %s (saved with the house):", se->name.c_str());
          for (int i = 0; i < 5; i++) {
            ImGui::RadioButton(kSlotKinds[i], &p.slotKind, i);
            if (i != 2 && i != 4) ImGui::SameLine();
          }
        } else {
          ImGui::TextDisabled("place into group");
          ImGui::SameLine();
          ImGui::SetNextItemWidth(-FLT_MIN);
          ImGui::InputText("##grp", p.placeGroup, sizeof p.placeGroup);
          ImGui::SetItemTooltip("refs/<group>.json in this map; ids are <group>/<kind>_<n>");
          for (int i = 0; i < 7; i++) {
            ImGui::RadioButton(kPlaceKinds[i], &p.placeKind, i);
            if (i % 3 != 2 && i != 6) ImGui::SameLine();
          }
          const std::string k = kPlaceKinds[p.placeKind];
          if (k == "npc") {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##base", "mob def (human)", p.placeBase, sizeof p.placeBase);
          } else if (k == "structure") {
            if (p.structAssets.empty()) p.structAssets = structures::ListAssets(p.assetDir);
            std::vector<const char*> items;
            for (const std::string& s : p.structAssets) items.push_back(s.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!items.empty()) ImGui::Combo("##sp", &p.structPick, items.data(), (int)items.size());
          }
        }
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("facing (deg)", &p.placeYaw, 15, 90);
        p.placeYaw = ((p.placeYaw % 360) + 360) % 360;
        ImGui::SetItemTooltip("heading: 0 faces +Z, 90 faces +X (red line in the world).\n"
                              "Structures and slots use quarter turns.");
        ImGui::TextDisabled("click in the world to place");
        break;
      }
      case kLink:
        ImGui::TextWrapped("%s", p.linkFrom.empty() ? "Click a waynode to start a link."
                                                    : ("from " + p.linkFrom + ": click the other end "
                                                       "(Shift: unlink, Esc: cancel)").c_str());
        break;
      default:
        break;
    }

    // ---- houses ----
    ImGui::Separator();
    if (se != nullptr) {
      ImGui::TextColored(V4(ui::ColGoldPale()), "House: %s%s", se->name.c_str(), se->Dirty() ? "  * unsaved" : "");
      if (inst) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("the copy %s at %d %d %d, facing %d", inst->id.c_str(), inst->pos.x, inst->pos.y, inst->pos.z, inst->yaw);
        ImGui::PopStyleColor();
      }
      else ImGui::TextDisabled("opened by asset name (no copy in the world)");
      if (!se->Dirty()) ImGui::BeginDisabled();
      if (ImGui::Button("Save  (Ctrl+S)", ImVec2(130, 0)))
        p.Run("struct.save", Json::object(), "saved " + se->name + "; every copy re-stamped");
      if (!se->Dirty()) ImGui::EndDisabled();
      ImGui::SetItemTooltip("writes assets/structures/%s.vox + .struct.json (handEdited)\n"
                            "and re-stamps every placed copy of it", se->name.c_str());
      ImGui::SameLine();
      if (ImGui::Button("Close house", ImVec2(130, 0))) {
        if (se->Dirty()) ImGui::OpenPopup("Close with unsaved edits?");
        else p.Run("struct.close", Json::object(), "closed the house");
      }
      if (ImGui::BeginPopupModal("Close with unsaved edits?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved edits.", se->name.c_str());
        if (ImGui::Button("Save, then close")) {
          if (p.Run("struct.save", Json::object())) p.Run("struct.close", Json::object(), "saved and closed");
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard edits")) {
          p.RevertPreview(se->name);
          p.Run("struct.close", Json{{"discard", true}}, "closed; the edits were thrown away");
          S.ClearHistory();
          p.wantsTick = true;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    } else if (p.store != nullptr) {
      ImGui::TextDisabled("Houses in this map (double-click to open):");
      ImGui::BeginChild("##houses", ImVec2(0, 90), ImGuiChildFlags_Borders);
      for (const auto& [id, r] : p.store->All()) {
        if (r.kind != "structure") continue;
        if (ImGui::Selectable((id + "  (" + r.base + ")").c_str(), p.selRef == id,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
          p.selRef = id;
          p.selSlot.clear();
          if (ImGui::IsMouseDoubleClicked(0)) OpenStructureRef(id);
        }
      }
      ImGui::EndChild();
    }
    ImGui::Separator();
    if (ImGui::SmallButton(p.showHelp ? "hide keys (H)" : "all keys (H)")) p.showHelp = !p.showHelp;
    ImGui::SameLine();
    ImGui::TextDisabled("speed %.0f m/s (wheel)", p.speed * 0.1f);
  }
  ImGui::End();

  // A button above may have closed / discarded the house.
  se = p.Open();
  inst = p.Instance();

  // ---- the inspector (right) ----
  const bool haveSel = !p.selRef.empty() || !p.selSlot.empty();
  if (haveSel) {
    ImGui::SetNextWindowPos(ImVec2(W - 380, 52), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(370, 0), ImVec2(370, H - 120));
    if (ImGui::Begin("INSPECTOR###sv_editor_insp", nullptr, fixed | ImGuiWindowFlags_AlwaysAutoResize)) {
      if (!p.selSlot.empty() && se != nullptr) {
        const int i = se->FindSlot(p.selSlot);
        if (i < 0) {
          p.selSlot.clear();
        } else {
          const structures::Slot sl = se->Slots()[(size_t)i];
          ImGui::TextColored(V4(ui::ColGoldPale()), "slot %s", sl.name.c_str());
          ImGui::TextDisabled("%s, part of %s (saved with the house)", sl.kind.c_str(), se->name.c_str());
          const std::string key = "slot:" + sl.name;
          if (p.inspFor != key || p.inspRev != se->Revision()) {
            p.inspFor = key;
            p.inspRev = se->Revision();
            p.inspPos[0] = sl.pos.x;
            p.inspPos[1] = sl.pos.y;
            p.inspPos[2] = sl.pos.z;
            p.inspYaw = sl.yaw;
            p.props.clear();
            for (auto it = sl.props.begin(); it != sl.props.end(); ++it) {
              Impl::PropRow r;
              r.key = it.key();
              std::snprintf(r.value, sizeof r.value, "%s", refs::JsonInline(it.value()).c_str());
              p.props.push_back(r);
            }
          }
          ImGui::SetNextItemWidth(200);
          ImGui::InputInt3("pos (house)", p.inspPos);
          ImGui::SetNextItemWidth(90);
          ImGui::InputInt("yaw", &p.inspYaw, 90, 90);
          ImGui::SameLine();
          if (ImGui::Button("apply##sl"))
            p.Run("slot.move", Json{{"name", sl.name}, {"pos", V3({p.inspPos[0], p.inspPos[1], p.inspPos[2]})}, {"yaw", p.inspYaw}},
                  "moved slot " + sl.name);
          ImGui::TextColored(V4(ui::ColGoldDim()), "props (house frame)");
          for (size_t k = 0; k < p.props.size(); k++) {
            ImGui::PushID((int)k);
            ImGui::TextUnformatted(p.props[k].key.c_str());
            ImGui::SameLine(100);
            ImGui::SetNextItemWidth(-30);
            if (ImGui::InputText("##v", p.props[k].value, sizeof p.props[k].value, ImGuiInputTextFlags_EnterReturnsTrue)) {
              Json val;
              if (ParseValue(p.props[k].value, val))
                p.Run("slot.set_prop", Json{{"name", sl.name}, {"key", p.props[k].key}, {"value", val}}, "set " + p.props[k].key);
              else
                p.Say(p.props[k].key + ": not valid JSON", true);
            }
            ImGui::SetItemTooltip("JSON: 3, true, \"text\", [\"a\"], {\"min\": [..], \"max\": [..]}. Enter applies.");
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
              p.Run("slot.set_prop", Json{{"name", sl.name}, {"key", p.props[k].key}, {"value", nullptr}},
                    "removed " + p.props[k].key);
            ImGui::PopID();
          }
          ImGui::SetNextItemWidth(90);
          ImGui::InputTextWithHint("##nk", "new prop", p.newKey, sizeof p.newKey);
          ImGui::SameLine(100);
          ImGui::SetNextItemWidth(-40);
          ImGui::InputTextWithHint("##nv", "value", p.newValue, sizeof p.newValue);
          ImGui::SameLine();
          if (ImGui::SmallButton("add")) {
            Json val;
            if (ParseValue(p.newValue, val) &&
                p.Run("slot.set_prop", Json{{"name", sl.name}, {"key", p.newKey}, {"value", val}}, "added a prop"))
              p.newKey[0] = p.newValue[0] = 0;
          }
          if (ImGui::Button("remove slot (Delete)"))
            if (p.Run("slot.remove", Json{{"name", sl.name}}, "removed slot " + sl.name)) p.selSlot.clear();
          if (sl.kind == "door")
            ImGui::TextDisabled("orange: the swing; red: the hinge. Change\nprops.opens (in/out) or hingeLine to flip it.");
        }
      } else if (!p.selRef.empty() && p.store != nullptr) {
        const refs::Ref* r = p.store->Find(p.selRef);
        if (r == nullptr) {
          p.selRef.clear();
        } else if (!r->derivedFrom.empty()) {
          ImGui::TextColored(V4(ui::ColGoldPale()), "%s", r->id.c_str());
          ImGui::TextDisabled("a %s slot of %s", r->kind.c_str(), r->derivedFrom.c_str());
          ImGui::TextWrapped("Slots come from the house's asset: open the house to move or change it.");
          if (ImGui::Button("open its house")) OpenStructureRef(r->derivedFrom);
        } else {
          ImGui::TextColored(V4(ui::ColGoldPale()), "%s", r->id.c_str());
          ImGui::TextDisabled("%s  -  refs/%s.json", r->kind.c_str(), r->group.c_str());
          const std::string key = "ref:" + r->id;
          if (p.inspFor != key || p.inspRev != p.store->Revision()) {
            p.inspFor = key;
            p.inspRev = p.store->Revision();
            p.inspPos[0] = r->pos.x;
            p.inspPos[1] = r->pos.y;
            p.inspPos[2] = r->pos.z;
            p.inspYaw = r->yaw;
            std::snprintf(p.inspBase, sizeof p.inspBase, "%s", r->base.c_str());
            p.props.clear();
            for (auto it = r->props.begin(); it != r->props.end(); ++it) {
              Impl::PropRow row;
              row.key = it.key();
              std::snprintf(row.value, sizeof row.value, "%s", refs::JsonInline(it.value()).c_str());
              p.props.push_back(row);
            }
          }
          const std::string id = r->id;
          {
            std::vector<std::string> kinds = refs::Kinds().Names();
            for (const char* k : kPlaceKinds)
              if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) kinds.push_back(k);
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("kind", r->kind.c_str())) {
              for (const std::string& k : kinds)
                if (ImGui::Selectable(k.c_str(), k == r->kind) && k != r->kind)
                  p.Run("ref.set_field", Json{{"id", id}, {"field", "kind"}, {"value", k}}, "kind -> " + k);
              ImGui::EndCombo();
            }
          }
          ImGui::SetNextItemWidth(150);
          if (ImGui::InputText("base", p.inspBase, sizeof p.inspBase, ImGuiInputTextFlags_EnterReturnsTrue))
            p.Run("ref.set_field", Json{{"id", id}, {"field", "base"}, {"value", std::string(p.inspBase)}}, "base set");
          ImGui::SetItemTooltip("a mob def for an npc, a structure asset for a structure. Enter applies.");
          ImGui::SetNextItemWidth(200);
          ImGui::InputInt3("pos", p.inspPos);
          ImGui::SetNextItemWidth(90);
          ImGui::InputInt("yaw", &p.inspYaw, r->kind == "structure" ? 90 : 15, 90);
          ImGui::SameLine();
          if (ImGui::Button("apply"))
            p.Run("ref.move", Json{{"id", id}, {"pos", V3({p.inspPos[0], p.inspPos[1], p.inspPos[2]})}, {"yaw", p.inspYaw}},
                  "moved " + id);
          const float bw = 110;
          if (ImGui::Button("duplicate", ImVec2(bw, 0))) {
            std::string nid;
            for (int n = 2; n < 1000; n++) {
              nid = id + "_" + std::to_string(n);
              if (p.store->Find(nid) == nullptr) break;
            }
            if (p.Run("ref.duplicate", Json{{"id", id}, {"as", nid}}, "duplicated as " + nid)) p.selRef = nid;
          }
          ImGui::SetItemTooltip("Ctrl+D");
          ImGui::SameLine();
          if (ImGui::Button("delete", ImVec2(bw, 0)))
            if (p.Run("ref.delete", Json{{"id", id}}, "deleted " + id + " (Ctrl+Z to undo)")) p.selRef.clear();
          ImGui::SetItemTooltip("Delete (Ctrl+Z puts it back)");
          if (r->kind == "structure") {
            ImGui::SameLine();
            if (ImGui::Button("open house", ImVec2(bw, 0))) OpenStructureRef(id);
          }
          if (p.store->Find(id) != nullptr) {
            const refs::Ref* cur = p.store->Find(id);
            ImGui::TextColored(V4(ui::ColGoldDim()), "props");
            for (size_t k = 0; k < p.props.size(); k++) {
              ImGui::PushID((int)k);
              ImGui::TextUnformatted(p.props[k].key.c_str());
              ImGui::SameLine(100);
              ImGui::SetNextItemWidth(-30);
              if (ImGui::InputText("##v", p.props[k].value, sizeof p.props[k].value, ImGuiInputTextFlags_EnterReturnsTrue)) {
                Json val;
                if (ParseValue(p.props[k].value, val))
                  p.Run("ref.set_prop", Json{{"id", id}, {"key", p.props[k].key}, {"value", val}}, "set " + p.props[k].key);
                else
                  p.Say(p.props[k].key + ": not valid JSON", true);
              }
              ImGui::SetItemTooltip("JSON: 3, true, \"text\", [\"a\", \"b\"]. A bare word is text. Enter applies.");
              ImGui::SameLine();
              if (ImGui::SmallButton("x"))
                p.Run("ref.set_prop", Json{{"id", id}, {"key", p.props[k].key}, {"value", nullptr}}, "removed " + p.props[k].key);
              ImGui::PopID();
            }
            ImGui::SetNextItemWidth(90);
            ImGui::InputTextWithHint("##nk", "new prop", p.newKey, sizeof p.newKey);
            ImGui::SameLine(100);
            ImGui::SetNextItemWidth(-40);
            ImGui::InputTextWithHint("##nv", "value", p.newValue, sizeof p.newValue);
            ImGui::SameLine();
            if (ImGui::SmallButton("add")) {
              Json val;
              if (ParseValue(p.newValue, val) &&
                  p.Run("ref.set_prop", Json{{"id", id}, {"key", p.newKey}, {"value", val}}, "added a prop"))
                p.newKey[0] = p.newValue[0] = 0;
            }
            // The kind's own panel (P6 doors / chests / beds, P7 npc / waynode),
            // the SAME code as the References page. It writes refs itself, so
            // what it changes is diffed here and recorded as an undo step.
            if (cur->kind == "door" || cur->kind == "container" || cur->kind == "bed" || cur->kind == "npc" ||
                cur->kind == "waynode" || cur->kind == "structure") {
              ImGui::Separator();
              std::map<std::string, refs::Ref> before;
              for (const auto& [rid, rr] : p.store->All())
                if (rr.derivedFrom.empty()) before.emplace(rid, rr);
              const uint64_t rev0 = p.store->Revision();
              DrawRefKindFields(ui, *p.store, *cur);
              if (p.store->Revision() != rev0) {
                std::vector<editor::Command> fwd, inv, journal;
                for (const auto& [rid, old] : before) {
                  const refs::Ref* now = p.store->Find(rid);
                  if (now == nullptr || refs::RefLine(*now) == refs::RefLine(old)) continue;
                  auto pa = std::make_shared<editor::Payload>();
                  pa->ref = std::make_shared<refs::Ref>(old);
                  inv.push_back(editor::Command{"_ref.put", Json{{"id", rid}, {"index", -1}}, pa});
                  auto pb = std::make_shared<editor::Payload>();
                  pb->ref = std::make_shared<refs::Ref>(*now);
                  fwd.push_back(editor::Command{"_ref.put", Json{{"id", rid}, {"index", -1}}, pb});
                  // The journal: the public commands a replay would run.
                  std::set<std::string> keys;
                  for (auto it = old.props.begin(); it != old.props.end(); ++it) keys.insert(it.key());
                  for (auto it = now->props.begin(); it != now->props.end(); ++it) keys.insert(it.key());
                  for (const std::string& k : keys) {
                    const Json a = old.props.contains(k) ? old.props[k] : Json();
                    const Json b = now->props.contains(k) ? now->props[k] : Json();
                    if (a != b)
                      journal.push_back(editor::Command{"ref.set_prop", Json{{"id", rid}, {"key", k}, {"value", b}}, nullptr});
                  }
                }
                S.Record("edit " + id, std::move(fwd), std::move(inv), journal);
                p.wantsTick = true;
              }
            }
          }
        }
      }
    }
    ImGui::End();
  }

  // ---- the status bar (bottom) ----
  const float barH = ImGui::GetTextLineHeightWithSpacing() * 2 + ImGui::GetStyle().WindowPadding.y * 2;
  ImGui::SetNextWindowPos(ImVec2(0, H - barH), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, barH), ImGuiCond_Always);
  if (ImGui::Begin("##sv_editor_status", nullptr,
                   fixed | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar)) {
    ImGui::TextColored(V4(ui::ColEmber()), "EDITOR");
    ImGui::SameLine();
    ImGui::TextColored(V4(ui::ColGoldPale()), "%s", kToolName[p.tool]);
    ImGui::SameLine();
    if (p.haveBox && se != nullptr) {
      const int sx2 = std::abs(p.boxA.x - p.boxB.x) + 1, sy2 = std::abs(p.boxA.y - p.boxB.y) + 1,
                sz2 = std::abs(p.boxA.z - p.boxB.z) + 1;
      ImGui::TextDisabled("| sel %d x %d x %d (%.1f x %.1f x %.1f m)", sx2, sy2, sz2, sx2 * 0.1f, sy2 * 0.1f, sz2 * 0.1f);
    } else if (!p.selRef.empty()) {
      ImGui::TextDisabled("| %s", p.selRef.c_str());
    } else if (!p.selSlot.empty()) {
      ImGui::TextDisabled("| slot %s", p.selSlot.c_str());
    } else {
      ImGui::TextDisabled("| nothing selected");
    }
    ImGui::SameLine();
    const std::vector<std::string> unsaved = S.UnsavedStructures();
    if (!unsaved.empty()) {
      ImGui::TextColored(V4(ui::ColBloodHi()), "| * unsaved: %s", unsaved[0].c_str());
      ImGui::SameLine();
    }
    ImGui::TextDisabled("| undo %zu  redo %zu", S.UndoDepth(), S.RedoDepth());
    // The last thing that happened, right-aligned on the first row (red when
    // something was refused, with the reason).
    if (!p.msg.empty()) {
      ImGui::SameLine();
      const float x0 = ImGui::GetCursorPosX() + 24;
      const float tw = ImGui::CalcTextSize(p.msg.c_str()).x;
      ImGui::SameLine(std::max(x0, W - tw - 16));
      ImGui::TextColored(V4(p.msgBad ? ui::ColBloodHi() : IM_COL32(150, 220, 150, 255)), "%s", p.msg.c_str());
    }
    const char* hint = "";
    switch (p.tool) {
      case kSelect: hint = "click select | drag arrows/ring | Del | Ctrl+D | dbl-click house: open"; break;
      case kPencil: hint = "click draw | Shift erase | Alt pick | Ctrl+Z undo"; break;
      case kBrush: hint = "click paint | Shift erase | [ ] size | B shape | Alt pick"; break;
      case kBox: hint = p.pasting ? "click land | R turn | M mirror | Shift skip air | Esc stop"
                                  : "drag select | Enter fill | Shift+Enter walls | Bksp hollow | Del clear | T replace | Ctrl+C/X/V"; break;
      case kLine: hint = "click start, click end | [ ] thickness"; break;
      case kEyedrop: hint = "click a house voxel"; break;
      case kPlace: hint = "click to place | set facing in the panel"; break;
      case kLink: hint = "click node, click node | Shift unlink"; break;
      default: break;
    }
    ImGui::TextColored(V4(ui::ColParchDim()), "keys: %s   |   F8 leave   H all keys   Ctrl+Z undo   Ctrl+S save", hint);
  }
  ImGui::End();

  // ---- the keys (H) ----
  if (p.showHelp) {
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f - 260, 60), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
    if (ImGui::Begin("EDITOR KEYS (H closes)###sv_editor_help", &p.showHelp, fixed | ImGuiWindowFlags_AlwaysAutoResize)) {
      const char* rows[][2] = {
          {"F8", "leave the editor (asks first about unsaved houses)"},
          {"right mouse + drag", "look around"},
          {"W A S D / Q E", "fly / down, up (Shift: 4x faster)"},
          {"mouse wheel", "flying speed"},
          {"F", "frame the selection"},
          {"1 .. 8", "Select, Pencil, Brush, Box, Line, Eyedropper, Place, Link"},
          {"Ctrl+Z / Ctrl+Y", "undo / redo (Ctrl+Shift+Z also redoes)"},
          {"Ctrl+S", "save the open house (.vox + .struct.json)"},
          {"Esc", "cancel the current action, then clear the selection"},
          {"Select: drag arrow", "move along it (Shift: 1 m steps)"},
          {"Select: drag ring", "turn (15 deg; houses and slots 90)"},
          {"Select: arrows, PgUp/PgDn, R", "nudge x/z, y; turn (Shift: 10 voxels / back)"},
          {"Select: Delete, Ctrl+D", "delete, duplicate"},
          {"Pencil/Brush: Shift, Alt", "erase, pick the material"},
          {"Brush: [ ] and B", "size, sphere/cube"},
          {"Box: Enter / Shift+Enter", "fill / walls"},
          {"Box: Backspace / Delete / T", "hollow / clear / replace"},
          {"Box: Ctrl+C, Ctrl+X, Ctrl+V", "copy, cut, paste (R turn, M mirror)"},
          {"Box: arrows, Ctrl+arrows", "move the box, move the voxels"},
          {"Box: PgUp/PgDn (+Shift)", "raise / lower the top (the bottom)"},
      };
      if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg)) {
        for (auto& r : rows) {
          ImGui::TableNextColumn();
          ImGui::TextColored(V4(ui::ColGoldHi()), "%s", r[0]);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
      }
      if (!S.JournalPath().empty()) ImGui::TextDisabled("this session is journaled to %s", S.JournalPath().c_str());
    }
    ImGui::End();
  }

  // ---- leaving with unsaved houses ----
  if (p.askExit) {
    ImGui::OpenPopup("Leave the editor?");
    p.askExit = false;
  }
  if (ImGui::BeginPopupModal("Leave the editor?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const std::vector<std::string> unsaved = S.UnsavedStructures();
    ImGui::Text("These houses have unsaved edits:");
    for (const std::string& n : unsaved) ImGui::BulletText("%s", n.c_str());
    if (ImGui::Button("Save all and leave")) {
      bool ok = true;
      for (const std::string& n : unsaved) ok = ok && p.Run("struct.save", Json{{"struct", n}});
      if (ok) {
        p.active = false;
        p.exited = true;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard and leave")) {
      if (!S.ctx.active.empty()) p.RevertPreview(S.ctx.active);
      for (const std::string& n : unsaved) S.ctx.structs.erase(n);
      S.ctx.active.clear();
      S.ctx.instance.clear();
      S.ClearHistory();
      p.active = false;
      p.exited = true;
      p.wantsTick = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep editing")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace editor_ui
