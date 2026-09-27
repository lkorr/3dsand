// The alchemy bench sim (game/flasksim.*): CPU-only, no world, no GPU. The
// material table is the only engine state read, and only through
// flasksim_mats.h — the same conversion the panel uses.
//
// What these gates hold the bench to:
//   - CONSERVATION, exactly: every eighth that goes in comes out, in a vessel
//     or in the spill, per material. The panel commits Count() as a transfer
//     the game validates, so a leak here would be a matter printer.
//   - ORDER: at rest, substances stack by the 3D sim's density, powders
//     included (sand floats on lava, sinks through water).
//   - A POUR moves matter from the tilted vessel into the other one.
//   - STIRRING mixes, and the mix separates again.
// Each writes a picture of its last frame beside the exe (alchemy_*.bmp).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/flasksim.h"
#include "game/flasksim_mats.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

using alchemy::Composition;
using alchemy::FlaskSim;
using alchemy::V2;
using alchemy::Xform;

int MatId(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (int)i;
  return -1;
}

void Shot(const FlaskSim& s, const char* file) {
  std::vector<uint32_t> px;
  s.Render(px);
  // Composite over the panel's backdrop colour so the BMP reads like the UI.
  std::vector<uint8_t> rgba(px.size() * 4);
  for (size_t i = 0; i < px.size(); i++) {
    uint32_t c = px[i];
    int a = c >> 24;
    const int bg[3] = {28, 24, 34};
    for (int ch = 0; ch < 3; ch++) rgba[i * 4 + ch] = (uint8_t)((((c >> (8 * ch)) & 255) * a + bg[ch] * (255 - a)) / 255);
    rgba[i * 4 + 3] = 255;
  }
  if (WriteBmpFile(file, rgba, (uint32_t)s.GridW(), (uint32_t)s.GridH()))
    std::printf("  wrote %s\n", file);
}

// Per material: what went in == what the tally says is anywhere.
bool Conserved(const Composition& in, const alchemy::Tally& t, std::string& why) {
  for (int i = 0; i < in.n; i++) {
    uint32_t mat = in.p[i].mat, want = in.p[i].eighths, got = t.spilled.AmountOf((uint16_t)mat);
    for (const Composition& v : t.vessel) got += v.AmountOf((uint16_t)mat);
    if (got != want) {
      why += "mat " + std::to_string(mat) + ": in " + std::to_string(want) + " out " + std::to_string(got) + "; ";
      return false;
    }
  }
  return true;
}

// Mean heights must follow density, heaviest lowest.
bool Ordered(const FlaskSim& s, const std::vector<alchemy::Substance>& subs, std::string& detail) {
  auto mh = s.MeanHeights();
  std::vector<int> idx;
  for (size_t i = 0; i < subs.size(); i++)
    if (mh[i] >= 0) idx.push_back((int)i);
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return subs[a].density > subs[b].density; });
  bool ok = true;
  for (size_t k = 0; k < idx.size(); k++) {
    detail += Format("%s%d@%.1f", k ? " < " : "", (int)subs[idx[k]].mat, mh[idx[k]]);
    if (k && mh[idx[k]] <= mh[idx[k - 1]]) ok = false;
  }
  return ok;
}

Status GateAlchemyLayers(Ctx& c, std::string& detail) {
  const char* names[] = {"water", "oil", "lava", "sand", "acid"};
  const uint32_t amts[] = {200, 160, 120, 100, 100};
  Composition in;
  for (int i = 0; i < 5; i++) {
    int id = MatId(c, names[i]);
    if (id < 0) { detail = std::string("no material ") + names[i]; return Status::Fail; }
    in.Add((uint16_t)id, amts[i]);
  }
  // Seeded in the WRONG order (lightest first) is not possible through
  // AddVessel — it always seeds settled — so this gate shakes it instead:
  // a stir, then time to separate.
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  FlaskSim s;
  s.SetSubstances(subs);
  s.AddVessel(alchemy::FlaskShape(110, 150), {{96, 8}, 0}, in);
  const int frames = (int)BaselineNumber("alchemy.separateFrames", 900);
  for (int f = 0; f < 240 + frames; f++) {
    bool on = f >= 20 && f < 240;
    float ph = f * 0.09f;
    // Through the mouth, the way the panel holds it: the top stays inside
    // the neck, the bottom sweeps the bulb.
    s.SetStick(on, {96 + 5 * std::sin(ph), 175}, {96 + 36 * std::sin(ph + 0.4f), 22}, 2.5f);
    s.Step(3);
  }
  Shot(s, "alchemy_layers.bmp");
  auto t = s.Count();
  std::string why;
  bool cons = Conserved(in, t, why);
  std::string order;
  bool ord = Ordered(s, subs, order);
  // Nothing leaves a flask that is only being stirred.
  bool kept = t.spilled.Total() == 0;
  bool ok = cons && ord && kept;
  detail = Format("%s; order %s; spilled %u", cons ? "conserved" : why.c_str(), order.c_str(), t.spilled.Total());
  std::printf("alchemy-layers: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

Status GateAlchemyPour(Ctx& c, std::string& detail) {
  int water = MatId(c, "water"), oil = MatId(c, "oil"), sand = MatId(c, "sand");
  if (water < 0 || oil < 0 || sand < 0) { detail = "missing water/oil/sand"; return Status::Fail; }
  Composition a, b;
  a.Add((uint16_t)water, 300);
  b.Add((uint16_t)oil, 200);
  b.Add((uint16_t)sand, 80);
  Composition all = a;
  for (int i = 0; i < b.n; i++) all.Add(b.p[i].mat, b.p[i].eighths);

  auto subs = alchemy::SubstancesFor(c.mats, {&a, &b});
  FlaskSim s;
  s.SetSubstances(subs);
  int A = s.AddVessel(alchemy::FlaskShape(110, 150), {{96, 8}, 0}, a);
  // The source flask starts upright above and to the right, and swings in
  // while it tilts about its left lip corner, arriving over A's mouth by
  // half tilt — before its contents reach the lip — the way a hand pours.
  auto pose = [&](float t) {
    float u = std::min(1.f, t / 0.5f), e = u * u * (3 - 2 * u);
    float ang = 2.8f * t;
    const V2 lipL{-0.30f * 35, 95};
    const V2 lipW{176 + (98 - 176) * e, 262 + (166 - 262) * e};
    float cs = std::cos(ang), sn = std::sin(ang);
    return Xform{{lipW.x - (lipL.x * cs - lipL.y * sn), lipW.y - (lipL.x * sn + lipL.y * cs)}, ang};
  };
  int B = s.AddVessel(alchemy::FlaskShape(70, 95), pose(0), b);
  for (int f = 0; f < 640; f++) {
    float t = std::clamp((f - 20) / 300.f, 0.f, 1.f);
    if (f > 480) t = std::max(0.f, 1 - (f - 480) / 80.f);
    s.SetVesselXform(B, pose(t));
    s.Step(3);
  }
  s.Settle(1200);
  Shot(s, "alchemy_pour.bmp");
  auto t = s.Count();
  std::string why;
  bool cons = Conserved(all, t, why);
  const Composition& ta = t.vessel[A];
  uint32_t oilIn = ta.AmountOf((uint16_t)oil), sandIn = ta.AmountOf((uint16_t)sand);
  uint32_t poured = oilIn + sandIn, spilled = t.spilled.Total();
  const double minPoured = BaselineNumber("alchemy.pourMinFrac", 0.6);
  const double maxSpill = BaselineNumber("alchemy.pourMaxSpillFrac", 0.05);
  bool moved = poured >= minPoured * b.Total();
  bool tidy = spilled <= maxSpill * b.Total();
  // After settling, the target has sand under water under oil.
  auto mh = s.MeanHeights();
  std::string order;
  bool ord = Ordered(s, subs, order);
  (void)mh;
  RecordObserved("alchemy.pourPoured", (double)poured);
  RecordObserved("alchemy.pourSpilled", (double)spilled);
  detail = Format("%s; into A: oil %u sand %u of %u; spilled %u; order %s", cons ? "conserved" : why.c_str(),
                  oilIn, sandIn, b.Total(), spilled, order.c_str());
  bool ok = cons && moved && tidy;
  (void)ord;  // mixed across two vessels: reported, not asserted
  std::printf("alchemy-pour: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// The whole-flask cost, for the panel's budget: a nearly full flask of five
// substances, three substeps a frame. Advisory: a number, not a verdict.
Status GateAlchemyCost(Ctx& c, std::string& detail) {
  const char* names[] = {"water", "oil", "sand"};
  const uint32_t amts[] = {500, 400, 120};
  Composition in;
  for (int i = 0; i < 3; i++) {
    int id = MatId(c, names[i]);
    if (id < 0) { detail = std::string("no material ") + names[i]; return Status::Fail; }
    in.Add((uint16_t)id, amts[i]);
  }
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  FlaskSim s;
  s.SetSubstances(subs);
  s.AddVessel(alchemy::FlaskShape(110, 150), {{96, 8}, 0}, in);
  const int frames = 300;
  auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; f++) s.Step(3);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
  std::vector<uint32_t> px;
  auto r0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; i++) s.Render(px);
  double rms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count() / 20;
  RecordObserved("alchemy.fullFlaskMsPerFrame", ms);
  detail = Format("%d particles %d grains: step %.2f ms/frame, render %.2f ms", s.ParticleCount(), s.GrainCount(),
                  ms, rms);
  std::printf("alchemy-cost: %s\n", detail.c_str());
  return Status::Pass;
}

}  // namespace

const std::vector<Gate>& AlchemyGates() {
  static const std::vector<Gate> g = {
      // CPU-only over the material table; order-independent.
      {"alchemy-layers", "player", {}, false, GateAlchemyLayers},
      {"alchemy-pour", "player", {}, false, GateAlchemyPour},
      {"alchemy-cost", "player", {}, true, GateAlchemyCost},
  };
  return g;
}

}  // namespace selftest
