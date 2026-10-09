#include "ui/map_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include <imgui.h>

#include "sim/world.h"
#include "sim/worldmap.h"
#include "ui/theme.h"

namespace {

struct Rgb {
  float r, g, b;
};

Rgb Hex(uint32_t v) {
  return Rgb{(float)((v >> 16) & 255), (float)((v >> 8) & 255), (float)(v & 255)};
}

// assets/editor/climate.js BIOME_COLORS, so the in-game map and the tuner's
// map page say the same thing about the same ground. A biome not listed gets
// a stable hash colour, as the page does.
Rgb BiomeColour(const std::string& name) {
  static const struct { const char* n; uint32_t c; } kCols[] = {
      {"forest", 0x2f7a3a},   {"meadow", 0x8cc95a},   {"pine", 0x1f5a3a},
      {"desert", 0xd9c27a},   {"tundra", 0xd8e6f0},   {"swamp", 0x6a7a2f},
      {"alpine", 0x8f959c},   {"ocean", 0x2b5d9e},    {"ashlands", 0x4a4440},
      {"mesa", 0xb5532f},     {"moor", 0x7d5a7a},     {"glowfen", 0x3f8a86},
      {"glacier", 0xa9d8ec},  {"savanna", 0xc9a74a},  {"dunes", 0xe8d9a0},
      {"amberwood", 0xc8742c}, {"eldwood", 0x1c3b24}, {"crystal_barrens", 0xb9a3d6},
      {"steppe", 0xb7b37a},   {"jungle", 0x1f8a3c},   {"downs", 0xa9d18e},
      {"karst", 0x9aa39a},
  };
  for (const auto& k : kCols)
    if (name == k.n) return Hex(k.c);
  uint32_t h = 2166136261u;
  for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
  return Rgb{(float)(80 + (h & 127)), (float)(80 + ((h >> 8) & 127)),
             (float)(80 + ((h >> 16) & 127))};
}

Rgb Lerp(Rgb a, Rgb b, float t) {
  return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

void Put(std::vector<uint8_t>& px, int w, int x, int y, Rgb c) {
  uint8_t* p = &px[((size_t)y * w + x) * 4];
  p[0] = (uint8_t)std::clamp(c.r, 0.0f, 255.0f);
  p[1] = (uint8_t)std::clamp(c.g, 0.0f, 255.0f);
  p[2] = (uint8_t)std::clamp(c.b, 0.0f, 255.0f);
  p[3] = 255;
}

// A filled disc in picture pixels: rivers and roads are stamped along their
// lines, so a 1-pixel-wide river at this scale still reads as a line.
void Disc(std::vector<uint8_t>& px, int w, int h, float cx, float cy, float r, Rgb c) {
  r = std::max(r, 0.5f);
  const int x0 = std::max(0, (int)std::floor(cx - r)), x1 = std::min(w - 1, (int)std::ceil(cx + r));
  const int y0 = std::max(0, (int)std::floor(cy - r)), y1 = std::min(h - 1, (int)std::ceil(cy + r));
  const float r2 = std::max(r * r, 0.5f);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      if (dx * dx + dy * dy <= r2) Put(px, w, x, y, c);
    }
}

}  // namespace

bool BuildWorldMapImage(const worldmap::WorldMapData& m,
                        const std::vector<std::string>& biomeNameById, int maxSide,
                        std::vector<uint8_t>& rgba, int& imgW, int& imgH) {
  if (m.extentW <= 0 || m.extentH <= 0 || maxSide <= 0) return false;
  const float vpp = (float)std::max(m.extentW, m.extentH) / (float)maxSide;  // voxels per pixel
  imgW = std::max(1, (int)std::lround(m.extentW / vpp));
  imgH = std::max(1, (int)std::lround(m.extentH / vpp));
  const int W = imgW, H = imgH;
  rgba.assign((size_t)W * H * 4, 0);

  // ---- heights, at each pixel centre: the expensive part, split by rows ----
  std::vector<int> hgt((size_t)W * H);
  {
    const unsigned nt = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < nt; t++)
      ts.emplace_back([&, t] {
        for (int y = (int)t; y < H; y += (int)nt)
          for (int x = 0; x < W; x++) {
            const int wx = m.extentX0 + (int)((x + 0.5f) * vpp);
            const int wz = m.extentZ0 + (int)((y + 0.5f) * vpp);
            hgt[(size_t)y * W + x] = worldmap::AuthoredHeightAt(wx, wz);
          }
      });
    for (std::thread& th : ts) th.join();
  }

  // ---- biome colour x hillshade, the sea by depth ----
  std::vector<Rgb> biomeCol(biomeNameById.size());
  for (size_t i = 0; i < biomeNameById.size(); i++) biomeCol[i] = BiomeColour(biomeNameById[i]);
  const Rgb kShallow = Hex(0x4f8cc4), kDeep = Hex(0x14305e), kSnow = Hex(0xf2f4f6);
  const int bl = m.biomeLog2;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const int h0 = hgt[(size_t)y * W + x];
      if (h0 < m.seaLevelY) {
        const float d = std::min((m.seaLevelY - h0) / 400.0f, 1.0f);
        Put(rgba, W, x, y, Lerp(kShallow, kDeep, std::sqrt(d)));
        continue;
      }
      const int wx = m.extentX0 + (int)((x + 0.5f) * vpp);
      const int wz = m.extentZ0 + (int)((y + 0.5f) * vpp);
      int id = m.biomeDefault;
      if (!m.biome.empty()) {
        const int bi = std::clamp((wx - m.extentX0) >> bl, 0, m.biomePostsX - 1);
        const int bk = std::clamp((wz - m.extentZ0) >> bl, 0, m.biomePostsZ - 1);
        id = m.biome[(size_t)bk * m.biomePostsX + bi];
      }
      Rgb c = id < (int)biomeCol.size() ? biomeCol[id] : Rgb{128, 128, 128};
      // Hillshade from the north-west (the tuner page's light): the slope
      // toward the light, in voxels per voxel, central differences.
      const int hl = hgt[(size_t)y * W + std::max(x - 1, 0)];
      const int hr = hgt[(size_t)y * W + std::min(x + 1, W - 1)];
      const int hu = hgt[(size_t)std::max(y - 1, 0) * W + x];
      const int hd = hgt[(size_t)std::min(y + 1, H - 1) * W + x];
      const float gx = (hr - hl) / (2.0f * vpp), gz = (hd - hu) / (2.0f * vpp);
      const float shade = std::clamp(1.0f + (gx + gz) * -2.2f, 0.5f, 1.45f);
      // A little altitude lift, then snow on the highest ground.
      const float alt = std::clamp((h0 - m.seaLevelY) / 6000.0f, 0.0f, 1.0f);
      c = Lerp(c, kSnow, std::max(0.0f, alt - 0.6f) * 1.5f);
      c = Rgb{c.r * shade, c.g * shade, c.b * shade};
      // the coast: one shade of sand-dark at the waterline
      if (h0 < m.seaLevelY + 8) c = Lerp(c, Hex(0xd8c99a), 0.5f);
      Put(rgba, W, x, y, c);
    }

  auto toPx = [&](float wx, float wz, float& px, float& py) {
    px = (wx - m.extentX0) / vpp;
    py = (wz - m.extentZ0) / vpp;
  };

  // ---- lakes: their baked polygon, even-odd scanline ----
  const Rgb kLake = Hex(0x3a78b8);
  for (const auto& lk : m.lakes) {
    const auto& poly = lk.poly.empty() ? lk.points : lk.poly;
    if (poly.size() < 3) continue;
    std::vector<std::pair<float, float>> pp;
    float ymin = 1e9f, ymax = -1e9f;
    for (const auto& p : poly) {
      float px, py;
      toPx((float)p.first, (float)p.second, px, py);
      pp.push_back({px, py});
      ymin = std::min(ymin, py);
      ymax = std::max(ymax, py);
    }
    const int ya = std::max(0, (int)std::floor(ymin)), yb = std::min(H - 1, (int)std::ceil(ymax));
    std::vector<float> xs;
    for (int y = ya; y <= yb; y++) {
      const float sy = y + 0.5f;
      xs.clear();
      for (size_t i = 0, j = pp.size() - 1; i < pp.size(); j = i++) {
        const auto& a = pp[i];
        const auto& b = pp[j];
        if ((a.second > sy) != (b.second > sy))
          xs.push_back(a.first + (sy - a.second) * (b.first - a.first) / (b.second - a.second));
      }
      std::sort(xs.begin(), xs.end());
      for (size_t i = 0; i + 1 < xs.size(); i += 2) {
        const int xa = std::max(0, (int)std::lround(xs[i])), xb = std::min(W - 1, (int)std::lround(xs[i + 1]) - 1);
        for (int x = xa; x <= xb; x++) Put(rgba, W, x, y, kLake);
      }
    }
    // a lake smaller than a pixel still shows
    if (yb - ya < 1) {
      float px, py;
      toPx((float)(lk.x0 + lk.x1) * 0.5f, (float)(lk.z0 + lk.z1) * 0.5f, px, py);
      Disc(rgba, W, H, px, py, 0.7f, kLake);
    }
  }

  // ---- rivers: the baked centreline (x, z, surface, half-width) ----
  const Rgb kRiver = Hex(0x3f7fc0);
  for (const auto& rv : m.rivers) {
    float lx = -1e9f, ly = -1e9f;
    for (const auto& q : rv.line) {
      float px, py;
      toPx((float)q[0], (float)q[1], px, py);
      if (std::fabs(px - lx) + std::fabs(py - ly) < 0.5f) continue;
      lx = px;
      ly = py;
      Disc(rgba, W, H, px, py, q[3] / vpp, kRiver);
    }
  }

  // ---- roads: the authored polyline, stepped every half pixel ----
  const Rgb kRoad = Hex(0x7a5a38);
  for (const auto& rd : m.roads) {
    const float r = std::max(0.5f, rd.width * 0.5f / vpp);
    for (size_t i = 0; i + 1 < rd.points.size(); i++) {
      float ax, ay, bx, by;
      toPx((float)rd.points[i].first, (float)rd.points[i].second, ax, ay);
      toPx((float)rd.points[i + 1].first, (float)rd.points[i + 1].second, bx, by);
      const int n = std::max(1, (int)std::ceil(std::hypot(bx - ax, by - ay) * 2.0f));
      for (int k = 0; k <= n; k++) {
        const float t = (float)k / n;
        Disc(rgba, W, H, ax + (bx - ax) * t, ay + (by - ay) * t, r, kRoad);
      }
    }
  }
  return true;
}

void FillWorldMapMirror(const worldmap::WorldMapData& m, UIState::WorldMapUI& out) {
  out.mapName = m.name;
  out.x0 = m.extentX0;
  out.z0 = m.extentZ0;
  out.w = m.extentW;
  out.h = m.extentH;
  out.spawnX = m.spawnX;
  out.spawnZ = m.spawnZ;
  out.labels.clear();
  for (const auto& r : m.regions)
    if (!r.name.empty())
      out.labels.push_back({r.name, (r.x0 + r.x1) / 2, (r.z0 + r.z1) / 2, 2});
  for (const auto& l : m.locales)
    out.labels.push_back({l.name.empty() ? l.id : l.name, (l.x0 + l.x1) / 2, (l.z0 + l.z1) / 2, 0});
  for (const auto& mk : m.markers)
    if (!mk.name.empty()) out.labels.push_back({mk.name, mk.x, mk.z, 1});
}

// ============================================================================
// THE PANEL
// ============================================================================

void DrawWorldMapScreen(UIState& s) {
  UIState::WorldMapUI& M = s.worldMap;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float margin = 24.0f;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(disp);
  ImGui::SetNextWindowBgAlpha(0.0f);
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  if (!ImGui::Begin("##worldmap", nullptr, fl)) {
    ImGui::End();
    ImGui::PopStyleVar(2);
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), disp, ui::Fade(ui::ColInk(), 0.72f));

  const ImVec2 pa(margin, margin), pb(disp.x - margin, disp.y - margin);
  ui::PanelBody(dl, pa, pb);
  ui::Draw9(dl, "panel", pa, pb);
  char right[160];
  std::snprintf(right, sizeof(right), "%s   M / Esc closes", M.mapName.c_str());
  const float headB = ui::HeaderBar(dl, ImVec2(pa.x + 8, pa.y + 8), pb.x - pa.x - 16, "WORLD MAP", right);

  // ---- the button row along the bottom ----
  const float rowH = 40.0f;
  const ImVec2 area0(pa.x + 16, headB + 10);
  const ImVec2 area1(pb.x - 16, pb.y - 16 - rowH - 8);
  const float aw = area1.x - area0.x, ah = area1.y - area0.y;
  const ImVec2 ac((area0.x + area1.x) * 0.5f, (area0.y + area1.y) * 0.5f);

  if (M.w <= 0 || M.h <= 0 || aw < 32 || ah < 32) {
    ui::ShadowText(dl, ImVec2(area0.x + 8, area0.y + 8), ui::ColParch(),
                   "This map has no extent to draw (the harness map?).");
  } else {
    const float fitVpp = std::max(M.w / aw, M.h / ah) * 1.02f;
    if (!M.viewSet) {
      M.viewSet = true;
      M.cx = M.x0 + M.w * 0.5f;
      M.cz = M.z0 + M.h * 0.5f;
      M.voxPerPx = fitVpp;
    }
    auto toScreen = [&](float wx, float wz) {
      return ImVec2(ac.x + (wx - M.cx) / M.voxPerPx, ac.y + (wz - M.cz) / M.voxPerPx);
    };
    auto toWorld = [&](ImVec2 p, float& wx, float& wz) {
      wx = M.cx + (p.x - ac.x) * M.voxPerPx;
      wz = M.cz + (p.y - ac.y) * M.voxPerPx;
    };

    // ---- input over the map area ----
    ImGui::SetCursorScreenPos(area0);
    ImGui::InvisibleButton("##maparea", ImVec2(aw, ah),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hover = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    if (hover && io.MouseWheel != 0.0f) {
      // zoom about the cursor: the world point under it stays under it
      float wx, wz;
      toWorld(io.MousePos, wx, wz);
      const float f = std::pow(0.8f, io.MouseWheel);
      M.voxPerPx = std::clamp(M.voxPerPx * f, 2.0f, fitVpp * 2.0f);
      M.cx = wx - (io.MousePos.x - ac.x) * M.voxPerPx;
      M.cz = wz - (io.MousePos.y - ac.y) * M.voxPerPx;
    }
    const bool panLeft = !M.teleportArmed;
    if (ImGui::IsItemActive()) {
      const bool drag = (panLeft && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) ||
                        ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) ||
                        ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f);
      if (drag) {
        M.cx -= io.MouseDelta.x * M.voxPerPx;
        M.cz -= io.MouseDelta.y * M.voxPerPx;
      }
    }
    if (M.teleportArmed && hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      float wx, wz;
      toWorld(io.MousePos, wx, wz);
      M.wantTeleport = true;
      M.tpX = (int)std::floor(wx);
      M.tpZ = (int)std::floor(wz);
      M.teleportArmed = false;
    }

    // ---- the picture ----
    dl->PushClipRect(area0, area1, true);
    dl->AddRectFilled(area0, area1, IM_COL32(20, 48, 94, 255));   // the sea past the edge
    if (M.tex && M.texReady && M.texW > 0 && M.texH > 0) {
      dl->AddImage((ImTextureID)M.tex, toScreen((float)M.x0, (float)M.z0),
                   toScreen((float)(M.x0 + M.w), (float)(M.z0 + M.h)), ImVec2(0, 0),
                   ImVec2((float)M.imgW / M.texW, (float)M.imgH / M.texH));
    } else {
      ui::ShadowText(dl, ImVec2(area0.x + 8, area0.y + 8), ui::ColParch(), "drawing the map...");
    }

    // ---- named places ----
    ImGui::PushFont(ui::FontSmall());
    for (const UIState::WorldMapUI::Label& L : M.labels) {
      const ImVec2 p = toScreen((float)L.x, (float)L.z);
      if (p.x < area0.x - 200 || p.x > area1.x + 200 || p.y < area0.y - 40 || p.y > area1.y + 40) continue;
      const ImVec2 ts = ImGui::CalcTextSize(L.name.c_str());
      if (L.kind == 2) {
        ui::ShadowText(dl, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f),
                       ui::Fade(ui::ColParchDim(), 0.8f), L.name.c_str());
      } else {
        const ImU32 dot = L.kind == 0 ? ui::ColGold() : ui::ColParch();
        dl->AddRectFilled(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + 3, p.y + 3), ui::ColInk());
        dl->AddRectFilled(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + 2, p.y + 2), dot);
        ui::ShadowText(dl, ImVec2(p.x + 6, p.y - ts.y * 0.5f),
                       L.kind == 0 ? ui::ColGoldPale() : ui::ColParch(), L.name.c_str());
      }
    }
    // the spawn: a small ring
    {
      const ImVec2 p = toScreen((float)M.spawnX, (float)M.spawnZ);
      dl->AddCircle(p, 6.0f, ui::ColInk(), 12, 4.0f);
      dl->AddCircle(p, 6.0f, ui::ColBloodHi(), 12, 2.0f);
    }
    // the player: an arrow along the camera's flat forward
    {
      const ImVec2 p = toScreen(M.playerX, M.playerZ);
      float fx = M.fwdX, fz = M.fwdZ;
      const float fl2 = std::sqrt(fx * fx + fz * fz);
      if (fl2 > 1e-4f) { fx /= fl2; fz /= fl2; } else { fx = 1; fz = 0; }
      const ImVec2 tip(p.x + fx * 13, p.y + fz * 13);
      const ImVec2 l(p.x - fx * 7 - fz * 7, p.y - fz * 7 + fx * 7);
      const ImVec2 r(p.x - fx * 7 + fz * 7, p.y - fz * 7 - fx * 7);
      dl->AddTriangleFilled(tip, l, r, ui::ColInk());
      const ImVec2 tip2(p.x + fx * 10, p.y + fz * 10);
      const ImVec2 l2(p.x - fx * 5 - fz * 5, p.y - fz * 5 + fx * 5);
      const ImVec2 r2(p.x - fx * 5 + fz * 5, p.y - fz * 5 - fx * 5);
      dl->AddTriangleFilled(tip2, l2, r2, ui::ColGoldHi());
    }
    // the armed cursor: crosshair + where it would put you
    if (hover) {
      float wx, wz;
      toWorld(io.MousePos, wx, wz);
      char buf[96];
      std::snprintf(buf, sizeof(buf), "x %d  z %d   (%.0f m, %.0f m)", (int)std::floor(wx),
                    (int)std::floor(wz), wx * kVoxelMeters, wz * kVoxelMeters);
      const ImVec2 mp = io.MousePos;
      if (M.teleportArmed) {
        const ImU32 c = ui::ColBloodHi();
        dl->AddLine(ImVec2(mp.x - 14, mp.y), ImVec2(mp.x - 4, mp.y), c, 2.0f);
        dl->AddLine(ImVec2(mp.x + 4, mp.y), ImVec2(mp.x + 14, mp.y), c, 2.0f);
        dl->AddLine(ImVec2(mp.x, mp.y - 14), ImVec2(mp.x, mp.y - 4), c, 2.0f);
        dl->AddLine(ImVec2(mp.x, mp.y + 4), ImVec2(mp.x, mp.y + 14), c, 2.0f);
      }
      ui::ShadowText(dl, ImVec2(mp.x + 16, mp.y + 14),
                     M.teleportArmed ? ui::ColBloodHi() : ui::ColParch(), buf);
    }
    // ---- the scale bar, bottom-left: a round number of metres ~120 px long ----
    {
      const float target = M.voxPerPx * 120.0f * kVoxelMeters;
      float metres = std::pow(10.0f, std::floor(std::log10(std::max(target, 1.0f))));
      if (metres * 5 <= target) metres *= 5;
      else if (metres * 2 <= target) metres *= 2;
      const float px = metres / kVoxelMeters / M.voxPerPx;
      const ImVec2 b0(area0.x + 14, area1.y - 18);
      dl->AddRectFilled(ImVec2(b0.x - 1, b0.y - 1), ImVec2(b0.x + px + 1, b0.y + 5), ui::ColInk());
      dl->AddRectFilled(b0, ImVec2(b0.x + px, b0.y + 4), ui::ColParch());
      char sb[32];
      if (metres >= 1000) std::snprintf(sb, sizeof(sb), "%g km", metres / 1000.0f);
      else std::snprintf(sb, sizeof(sb), "%g m", metres);
      ui::ShadowText(dl, ImVec2(b0.x, b0.y - 18), ui::ColParch(), sb);
    }
    ImGui::PopFont();
    dl->PopClipRect();
    ui::InnerShadow(dl, area0, area1, 8.0f, 0.5f);

    // ---- the button row ----
    ImGui::SetCursorScreenPos(ImVec2(area0.x, area1.y + 8));
    if (ImGui::Button("Centre on me", ImVec2(0, rowH))) {
      M.cx = M.playerX;
      M.cz = M.playerZ;
      M.voxPerPx = std::min(M.voxPerPx, std::max(fitVpp / 8.0f, 2.0f));
    }
    ImGui::SameLine();
    if (ImGui::Button("Whole map", ImVec2(0, rowH))) M.viewSet = false;
    ImGui::SameLine();
    if (!M.canTeleport) {
      ImGui::BeginDisabled();
      ImGui::Button("DEV teleport (single player only)", ImVec2(0, rowH));
      ImGui::EndDisabled();
    } else {
      const bool armed = M.teleportArmed;
      if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(ui::ColBlood()));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(ui::ColBloodHi()));
      }
      if (ImGui::Button(armed ? "DEV teleport: click the map (cancel)" : "DEV teleport",
                        ImVec2(0, rowH)))
        M.teleportArmed = !M.teleportArmed;
      if (armed) ImGui::PopStyleColor(2);
    }
    if (!M.status.empty()) {
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColParchDim()), "%s", M.status.c_str());
    }
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(area1.x - 110, area1.y + 8));
    if (ImGui::Button("Close", ImVec2(110, rowH))) M.wantClose = true;
  }
  if (M.w <= 0 || M.h <= 0) {
    ImGui::SetCursorScreenPos(ImVec2(pb.x - 16 - 110, pb.y - 16 - rowH));
    if (ImGui::Button("Close", ImVec2(110, rowH))) M.wantClose = true;
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}
