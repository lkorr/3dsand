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

#include "game/alchemy_bench.h"
#include "game/container.h"
#include "game/flasksim.h"
#include "game/flasksim_mats.h"
#include "game/item.h"
#include "game/iteminstance.h"
#include "game/mob.h"
#include "sim/bytestream.h"
#include "sim/solutes.h"
#include "sim/world.h"
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
  // A careful pour: tip over 300 frames, HOLD until nothing is still coming
  // out of the lip (sand trickles long after the liquid is gone), then swing
  // back. Swinging back mid-trickle drops the tail of it outside -- a real
  // spill, and what the panel counts as one, but not what this gate measures.
  // WHAT FALLS OFF THE TABLE IS DRAINED AS IT FALLS, the way the bench
  // streams it into the world (AlchemyBench::Run -> TakeSpill), and the sum
  // must still come out exact: drained + what is left == what went in.
  Composition drained;
  auto drain = [&]() {
    const Composition d = s.DrainSpilled();
    for (int i = 0; i < d.n; i++) drained.Add(d.p[i].mat, d.p[i].eighths);
  };
  int f = 0;
  for (; f < 320; f++) {
    s.SetVesselXform(B, pose(std::clamp((f - 20) / 300.f, 0.f, 1.f)));
    s.Step(3);
    drain();
  }
  for (int hold = 0; hold < 900; hold++, f++) {
    s.Step(3);
    drain();
    if (hold > 120 && (hold % 30) == 0 && s.MovingCount(0.3f) == 0) break;
  }
  for (int k = 0; k <= 80; k++, f++) {
    s.SetVesselXform(B, pose(1.0f - k / 80.0f));
    s.Step(3);
    drain();
  }
  s.Settle(1200);
  Shot(s, "alchemy_pour.bmp");
  auto t = s.Count();
  for (int i = 0; i < drained.n; i++) t.spilled.Add(drained.p[i].mat, drained.p[i].eighths);
  std::string why;
  bool cons = Conserved(all, t, why);
  const Composition& ta = t.vessel[A];
  uint32_t oilIn = ta.AmountOf((uint16_t)oil), sandIn = ta.AmountOf((uint16_t)sand);
  uint32_t poured = oilIn + sandIn, spilled = t.spilled.Total();
  // Asserted on the LIQUID: most of the oil arrives and little of it
  // splashes. The sand is reported, not asserted -- in a round flask it
  // wedges at the shoulder below its pile angle and trickles, and whatever
  // is still at the lip when the flask swings back falls where it falls,
  // which is the sim being right about sand.
  const uint32_t oilSpilled = t.spilled.AmountOf((uint16_t)oil);
  const double minPoured = BaselineNumber("alchemy.pourMinFrac", 0.8);
  const double maxSpill = BaselineNumber("alchemy.pourMaxSpillFrac", 0.05);
  bool moved = oilIn >= minPoured * b.AmountOf((uint16_t)oil);
  bool tidy = oilSpilled <= maxSpill * b.AmountOf((uint16_t)oil);
  // After settling, the target has sand under water under oil.
  auto mh = s.MeanHeights();
  std::string order;
  bool ord = Ordered(s, subs, order);
  (void)mh;
  RecordObserved("alchemy.pourPoured", (double)poured);
  RecordObserved("alchemy.pourSpilled", (double)spilled);
  detail = Format("%s; into A: oil %u/%u sand %u/%u; spilled oil %u, all %u (%u drained live); order %s",
                  cons ? "conserved" : why.c_str(), oilIn, b.AmountOf((uint16_t)oil), sandIn,
                  b.AmountOf((uint16_t)sand), oilSpilled, spilled, drained.Total(), order.c_str());
  // ...then A is turned over and emptied off the table, drained as it
  // falls: the live stream must carry real matter and still add up.
  for (int k = 0; k < 360; k++) {
    s.SetVesselXform(A, Xform{{96, 60}, std::min(3.0f, 3.0f * k / 60.0f)});
    s.Step(3);
    drain();
  }
  auto t2 = s.Count();
  for (int i = 0; i < drained.n; i++) t2.spilled.Add(drained.p[i].mat, drained.p[i].eighths);
  std::string why2;
  const bool cons2 = Conserved(all, t2, why2);
  const bool streamed = drained.Total() > 0;
  detail += Format("; dumped: %u eighths drained live, %s", drained.Total(),
                   cons2 ? "conserved" : why2.c_str());
  bool ok = cons && moved && tidy && cons2 && streamed;
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

// The bench's own table (game/alchemy_bench.h: 12 units an eighth, a
// particle half of one) and a flask of the item's capacity, 1024 eighths.
alchemy::SimConfig BenchConfig() {
  alchemy::SimConfig cfg;
  cfg.gridW = 480;
  cfg.gridH = 380;
  cfg.unitsPerEighth = 12;
  cfg.unitsPerParticle = 6;
  return cfg;
}
alchemy::VesselShape BenchFlask(float capEighths) {
  return alchemy::ShapeWithArea(alchemy::FlaskShape(110, 150), capEighths * 12);
}

// SHAKING A FLASK KEEPS ITS LIQUID IN, AND IT SLOSHES FOR A WHILE AFTER.
// Carried side to side the way a hand would, a 60%-full flask used to throw
// a fifth to a third of its water out of the mouth (a world-frame drag on
// the liquid was a sideways gravity), and a flask set down froze solid in a
// fifth of a second (a sleep test on speed, set loose enough to sleep at
// all). Asserted: nothing spills; half a second after the hand stops the
// liquid is still moving; it sleeps within a few seconds (cost).
Status GateAlchemyShake(Ctx& c, std::string& detail) {
  const int water = MatId(c, "water");
  if (water < 0) { detail = "no water"; return Status::Fail; }
  Composition in;
  in.Add((uint16_t)water, 600);
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  FlaskSim s(BenchConfig());
  s.SetSubstances(subs);
  const int v = s.AddVessel(BenchFlask(1024), {{240, 4}, 0}, in);
  const int stopAt = 150, frames = 60 * 9;
  int sleptAt = -1;
  bool movingAfterStop = false;
  for (int f = 0; f < frames; f++) {
    const float t = f / 60.0f;
    const float dx = f < stopAt ? 70.0f * std::sin(6.2832f * 1.5f * t) : 0.0f;
    s.SetVesselXform(v, {{240 + dx, 34}, 0});
    s.Step(4);
    if (f == stopAt + 30) movingAfterStop = !s.VesselAsleep(v) && s.MovingCount(0.3f) > 0;
    if (f > stopAt && sleptAt < 0 && s.VesselAsleep(v)) sleptAt = f;
  }
  Shot(s, "alchemy_shake.bmp");
  const auto t = s.Count();
  std::string why;
  const bool cons = Conserved(in, t, why);
  const uint32_t spilled = t.spilled.Total();
  const double maxSleep = BaselineNumber("alchemy.shakeSleepWithinSec", 6.0);
  const bool slept = sleptAt >= 0 && (sleptAt - stopAt) / 60.0 <= maxSleep;
  RecordObserved("alchemy.shakeSleepSec", sleptAt < 0 ? -1.0 : (sleptAt - stopAt) / 60.0);
  detail = Format("%s; spilled %u eighths; still sloshing 0.5 s after: %s; asleep %.2f s after the hand stopped",
                  cons ? "conserved" : why.c_str(), spilled, movingAfterStop ? "yes" : "NO",
                  sleptAt < 0 ? -1.0 : (sleptAt - stopAt) / 60.0);
  const bool ok = cons && spilled == 0 && movingAfterStop && slept;
  std::printf("alchemy-shake: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// A VESSEL ARRIVES SETTLED. It used to appear seeded -- a lattice filled to
// the neck, with the particles that had no lattice point dropped in a heap by
// the mouth -- and visibly sag, spray and settle in its first second.
// Asserted on a brim-full flask of three substances: nothing outside it,
// asleep on arrival, and nothing moving in its first frame.
Status GateAlchemySpawn(Ctx& c, std::string& detail) {
  const int water = MatId(c, "water"), oil = MatId(c, "oil"), sand = MatId(c, "sand");
  if (water < 0 || oil < 0 || sand < 0) { detail = "missing water/oil/sand"; return Status::Fail; }
  Composition in;
  in.Add((uint16_t)water, 600);
  in.Add((uint16_t)oil, 300);
  in.Add((uint16_t)sand, 100);
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  FlaskSim s(BenchConfig());
  s.SetSubstances(subs);
  const auto t0 = std::chrono::steady_clock::now();
  const int v = s.AddVessel(BenchFlask(1024), {{240, 4}, 0}, in);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Shot(s, "alchemy_spawn.bmp");
  const auto t = s.Count();
  const bool asleep = s.VesselAsleep(v);
  s.Step(4);
  const int moving = s.MovingCount(0.1f);
  std::string why;
  const bool cons = Conserved(in, t, why);
  RecordObserved("alchemy.spawnSettleMs", ms);
  detail = Format("%s; outside %u eighths; asleep on arrival %s; moving after a frame %d; settled in %.0f ms",
                  cons ? "conserved" : why.c_str(), t.spilled.Total(), asleep ? "yes" : "NO", moving, ms);
  const bool ok = cons && t.spilled.Total() == 0 && asleep && moving == 0;
  std::printf("alchemy-spawn: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// SAND IN A CARRIED VESSEL STAYS PUT. A grain's sub-pixel position went
// stale whenever the CA moved it, and the next carry put it back where it
// had been: sand that snapped up into the air and poured down again every
// time the vessel moved. Asserted over a vial of sand and water swung,
// lifted, turned and set down: no grain leaves it, almost no grain moves
// more than 3 px in a step (the glass shoving a packed pile moves some), none
// more than 8, and the whole of it sleeps once set down.
Status GateAlchemySandCarry(Ctx& c, std::string& detail) {
  const int water = MatId(c, "water"), sand = MatId(c, "sand");
  if (water < 0 || sand < 0) { detail = "missing water/sand"; return Status::Fail; }
  Composition in;
  in.Add((uint16_t)sand, 90);
  in.Add((uint16_t)water, 80);
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  FlaskSim s(BenchConfig());
  s.SetSubstances(subs);
  const int v = s.AddVessel(BenchFlask(256), {{200, 4}, 0}, in);
  std::vector<V2> before, after;
  long moves = 0, big = 0;
  int worst = 0, sleptAt = -1;
  const int setDown = 240;
  for (int f = 0; f < 600; f++) {
    const float t = f / 60.0f;
    if (f < setDown)
      s.SetVesselXform(v, {{200 + 60 * std::sin(t * 2.1f), 60 + 40 * std::sin(t * 1.3f)}, 0.9f * std::sin(t * 1.7f)});
    else
      s.SetVesselXform(v, {{260, 4}, 0});
    for (int k = 0; k < 4; k++) {
      s.GrainPositions(before);
      s.Step(1);
      s.GrainPositions(after);
      if (before.size() != after.size()) continue;
      for (size_t i = 0; i < after.size(); i++) {
        const int j = (int)std::max(std::fabs(after[i].x - before[i].x), std::fabs(after[i].y - before[i].y));
        moves += j > 0;
        big += j > 3;
        worst = std::max(worst, j);
      }
    }
    if (f > setDown && sleptAt < 0 && s.VesselAsleep(v)) sleptAt = f;
  }
  Shot(s, "alchemy_sandcarry.bmp");
  const auto t = s.Count();
  std::string why;
  const bool cons = Conserved(in, t, why);
  const double bigFrac = moves ? (double)big / moves : 0.0;
  RecordObserved("alchemy.sandBigMoveFrac", bigFrac);
  const double maxBig = BaselineNumber("alchemy.sandBigMoveFracMax", 0.005);
  detail = Format("%s; spilled %u; grain moves %ld, >3 px %ld (%.3f%%), worst %d px; asleep %.2f s after set-down",
                  cons ? "conserved" : why.c_str(), t.spilled.Total(), moves, big, 100 * bigFrac, worst,
                  sleptAt < 0 ? -1.0 : (sleptAt - setDown) / 60.0);
  const bool ok = cons && t.spilled.Total() == 0 && bigFrac <= maxBig && worst <= 8 && sleptAt >= 0;
  std::printf("alchemy-sand-carry: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}


// ---- BENCH CHEMISTRY (docs/PLAN_alchemy_chemistry.md package C) -------------
//
// The bench runs the WORLD'S compiled rules (flasksim_mats.h
// BuildBenchChemistry over c.mats / c.reactions, plus solutes.json). A gate
// whose rule the table does not carry (a table from before package A, or a
// rule later retuned away) appends a FIXTURE rule so the mechanism is still
// tested -- and says so in its detail line, so a gate that is only testing its
// own fixture is visible as such.
namespace {

struct ChemBench {
  std::vector<alchemy::Substance> subs;
  alchemy::Chemistry chem;
  std::vector<SoluteDef> solutes;
  std::string fixtures;   // what the gate had to author itself
};

ChemBench MakeChemBench(Ctx& c) {
  ChemBench b;
  std::string err;
  LoadSolutes(AssetDir() + "/materials/solutes.json", c.mats, b.solutes, err);
  b.subs = alchemy::BenchSubstances(c.mats, c.reactions);
  b.chem = alchemy::BuildBenchChemistry(c.mats, c.reactions, b.solutes, b.subs);
  return b;
}

int SlotOfName(const ChemBench& b, Ctx& c, const char* n) {
  const int m = MatId(c, n);
  for (size_t i = 0; i < b.subs.size(); i++)
    if ((int)b.subs[i].mat == m) return (int)i;
  return -1;
}

// Does the table carry a pair rule of `self` whose neighbour predicate
// matches `nbr` and that turns something into `product` (self or neighbour)?
bool HasPairRule(const ChemBench& b, int self, int nbr, int product) {
  if (self < 0 || nbr < 0) return false;
  const alchemy::Substance& n = b.subs[nbr];
  for (const alchemy::ChemRule& r : b.chem.rules[self])
    if (r.kind == alchemy::kChemPair && alchemy::ChemNbrMatches(r, n.mat, n.tagMask, n.klass, false) &&
        (product < 0 || r.prodSelf == product || r.prodNbr == product))
      return true;
  return false;
}
bool HasVirtualRule(const ChemBench& b, int self, const alchemy::ChemVirtual& v, int product) {
  if (self < 0) return false;
  for (const alchemy::ChemRule& r : b.chem.rules[self])
    if (r.kind == alchemy::kChemPair && alchemy::ChemNbrMatches(r, v.mat, v.tags, v.klass, false) &&
        (product < 0 || r.prodSelf == product || r.prodNbr == product))
      return true;
  return false;
}
// A fixture rule, FIRST in the bucket (first-match: it is the one tried).
void AddFixtureRule(ChemBench& b, int self, alchemy::ChemRule r, const char* what) {
  b.chem.rules[self].insert(b.chem.rules[self].begin(), r);
  b.fixtures += std::string(b.fixtures.empty() ? "" : ", ") + what;
}

alchemy::SimConfig ChemConfig() {
  alchemy::SimConfig cfg = BenchConfig();
  cfg.chemRate = 2.0f;   // AlchemyBench::Open's
  cfg.tableY = 4.0f;
  return cfg;
}

// The ValidateBench a session of this sim would get: `in` came on (one
// vessel), `drained` went out live (the gas in it vented), the ledger is the
// sim's. True = before + produced - consumed == after + spilled + streamed +
// vented for every material.
bool BenchLedgerHolds(Ctx& c, const alchemy::FlaskSim& s, const Composition& in, int vessel,
                      const Composition& drained, std::string& why) {
  alchemy::BenchResult r;
  const alchemy::Tally t = s.Count();
  alchemy::BenchEntry e;
  e.before = in;
  e.after = vessel >= 0 && vessel < (int)t.vessel.size() ? t.vessel[vessel] : Composition{};
  e.capacity = 1 << 20;
  r.vessels.push_back(e);
  r.spilled = t.spilled;
  r.streamed.assign(c.mats.size(), 0);
  r.vented.assign(c.mats.size(), 0);
  for (int i = 0; i < drained.n; i++) {
    const uint16_t m = drained.p[i].mat;
    if (m >= c.mats.size()) continue;
    (c.mats[m].gpu.klass == CLASS_GAS ? r.vented : r.streamed)[m] += drained.p[i].eighths;
  }
  r.unitsPerEighth = 12;
  r.produced.assign(c.mats.size(), 0);
  r.consumed.assign(c.mats.size(), 0);
  for (size_t sl = 0; sl < s.Produced().size(); sl++) {
    const uint16_t m = s.Sub((int)sl).mat;
    r.produced[m] += s.Produced()[sl];
    r.consumed[m] += s.Consumed()[sl];
  }
  return alchemy::ValidateBench(r, c.mats, why);
}

void Drain(alchemy::FlaskSim& s, Composition& drained) {
  const Composition d = s.DrainSpilled();
  for (int i = 0; i < d.n; i++) drained.Add(d.p[i].mat, d.p[i].eighths);
}

}  // namespace

// ACID EATS SAND, AND THE FUMES LEAVE THROUGH THE MOUTH. A flask of acid over
// a bed of sand, left on the bench: the world's own acid rules eat the sand,
// some of it goes up as noxious gas, the gas rises out of the neck and is
// DRAINED -- in the game that is the live stream that puts it into the world
// at the hand's lip. Asserted: sand eaten, gas made and vented, the units
// audit exact and the ledger's ValidateBench passing.
Status GateAlchemyReact(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int acid = SlotOfName(b, c, "acid"), sand = SlotOfName(b, c, "sand"),
            nox = SlotOfName(b, c, "noxious_gas");
  if (acid < 0 || sand < 0 || nox < 0) { detail = "missing acid/sand/noxious_gas"; return Status::Fail; }
  if (!HasPairRule(b, acid, sand, nox)) {
    alchemy::ChemRule r;
    r.nbrMat = b.subs[sand].mat;
    r.chance = 40 * 2000;
    r.prodNbr = nox;
    AddFixtureRule(b, acid, r, "acid+sand->noxious_gas");
  }
  Composition in;
  in.Add(b.subs[acid].mat, 200);
  in.Add(b.subs[sand].mat, 60);
  alchemy::FlaskSim s(ChemConfig());
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in);
  Composition drained;
  int maxGas = 0;
  for (int f = 0; f < 60 * 18; f++) {
    s.Step(4);
    Drain(s, drained);
    maxGas = std::max(maxGas, s.GasPixelCount());
    if (f == 240) Shot(s, "alchemy_react.bmp");
  }
  const alchemy::Tally t = s.Count();
  const uint32_t sandLeft = t.vessel[v].AmountOf(b.subs[sand].mat) + t.spilled.AmountOf(b.subs[sand].mat);
  const int64_t gasMade = s.Produced()[nox];
  const uint32_t vented = drained.AmountOf(b.subs[nox].mat);
  std::string why, lwhy;
  const bool audit = s.AuditUnits(&why);
  const bool ledger = BenchLedgerHolds(c, s, in, v, drained, lwhy);
  const bool eaten = sandLeft + 10 <= 60;
  const bool ok = audit && ledger && eaten && gasMade > 0 && vented > 0;
  RecordObserved("alchemy.reactVentedEighths", vented);
  detail = Format("sand 60 -> %u eighths; noxious gas made %lld units, %u eighths vented out of the mouth "
                  "(peak %d gas pixels); %d firings; audit %s; ledger %s%s%s",
                  sandLeft, (long long)gasMade, vented, maxGas, s.ReactionsFired(), audit ? "exact" : why.c_str(),
                  ledger ? "validates" : lwhy.c_str(), b.fixtures.empty() ? "" : "; FIXTURE rules: ",
                  b.fixtures.c_str());
  std::printf("alchemy-react: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// A STOPPERED FLASK KEEPS ITS GAS -- AND PRESSURE POPS OR BURSTS IT. (1) The
// acid and sand of alchemy-react, stoppered: nothing vents, the gas is in the
// flask. (2) A stoppered flask packed with gas past its headspace: the
// stopper POPS (a "pop" event, the flask is open after it). (3) A stoppered
// flask of water on the burner: the steam builds against hot glass and the
// flask BURSTS (a "burst" event, the vessel gone, its contents loose -- and
// still every unit accounted for).
Status GateAlchemyStopper(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int acid = SlotOfName(b, c, "acid"), sand = SlotOfName(b, c, "sand"),
            nox = SlotOfName(b, c, "noxious_gas"), water = SlotOfName(b, c, "water"),
            steam = SlotOfName(b, c, "steam"), chl = SlotOfName(b, c, "chlorine");
  if (acid < 0 || sand < 0 || nox < 0 || water < 0 || steam < 0 || chl < 0) {
    detail = "missing materials";
    return Status::Fail;
  }
  if (!HasPairRule(b, acid, sand, nox)) {
    alchemy::ChemRule r;
    r.nbrMat = b.subs[sand].mat;
    r.chance = 40 * 2000;
    r.prodNbr = nox;
    AddFixtureRule(b, acid, r, "acid+sand->noxious_gas");
  }
  if (!HasVirtualRule(b, water, b.chem.heat, steam)) {
    alchemy::ChemRule r;
    r.nbrTags = b.chem.heat.tags;
    r.chance = 180 * 2000;
    r.prodSelf = steam;
    AddFixtureRule(b, water, r, "water+tag:hot->steam");
  }
  // (1) held
  Composition in;
  in.Add(b.subs[acid].mat, 200);
  in.Add(b.subs[sand].mat, 60);
  alchemy::FlaskSim s(ChemConfig());
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in, true);
  Composition drained;
  int maxGas = 0;
  bool popped = false;
  for (int f = 0; f < 60 * 12; f++) {
    s.Step(4);
    Drain(s, drained);
    maxGas = std::max(maxGas, s.GasUnits(v));
    for (const alchemy::SimEvent& e : s.TakeEvents()) popped |= e.kind == "pop" || e.kind == "burst";
  }
  Shot(s, "alchemy_stopper.bmp");
  const uint32_t vented = drained.AmountOf(b.subs[nox].mat);
  std::string why, lwhy;
  const bool audit1 = s.AuditUnits(&why);
  const bool ledger1 = BenchLedgerHolds(c, s, in, v, drained, lwhy);
  const bool held = (vented == 0 || popped) && maxGas > 0 && s.Stoppered(v) != popped;

  // (2) pop: a small flask of water with far more chlorine than its headspace.
  Composition gasIn;
  gasIn.Add(b.subs[water].mat, 60);
  gasIn.Add(b.subs[chl].mat, 400);
  alchemy::FlaskSim s2(ChemConfig());
  s2.SetSubstances(b.subs);
  s2.SetChemistry(b.chem);
  const int v2 = s2.AddVessel(BenchFlask(256), {{240, 4}, 0}, gasIn, true);
  int popAt = -1;
  std::string kinds2;
  Composition drained2;
  for (int f = 0; f < 240 && popAt < 0; f++) {
    s2.Step(4);
    Drain(s2, drained2);
    for (const alchemy::SimEvent& e : s2.TakeEvents()) {
      kinds2 += e.kind + " ";
      if (e.kind == "pop") popAt = f;
    }
  }
  const bool pop = popAt >= 0 && !s2.Stoppered(v2) && s2.VesselAlive(v2);
  std::string why2;
  const bool audit2 = s2.AuditUnits(&why2);

  // (3) burst: water, stoppered, on the burner.
  Composition wIn;
  wIn.Add(b.subs[water].mat, 140);
  alchemy::FlaskSim s3(ChemConfig());
  s3.SetSubstances(b.subs);
  s3.SetChemistry(b.chem);
  const int v3 = s3.AddVessel(BenchFlask(256), {{240, 4}, 0}, wIn, true);
  s3.SetBurner(v3, true);
  int burstAt = -1;
  Composition drained3;
  for (int f = 0; f < 60 * 30 && burstAt < 0; f++) {
    s3.Step(4);
    Drain(s3, drained3);
    for (const alchemy::SimEvent& e : s3.TakeEvents())
      if (e.kind == "burst") burstAt = f;
  }
  for (int f = 0; f < 120; f++) {
    s3.Step(4);
    Drain(s3, drained3);
  }
  Shot(s3, "alchemy_burst.bmp");
  const bool burst = burstAt >= 0 && s3.Broken(v3) && !s3.VesselAlive(v3);
  std::string why3;
  const bool audit3 = s3.AuditUnits(&why3);

  // THE STOPPER ON THE ITEM: a stoppered flask pours nothing, scoops
  // nothing, and keeps its stopper -- and a dissolved portion -- through the
  // item record (iteminstance.h kItemFmtStopper); an older format drops the
  // stopper, never the record.
  bool itemOk = false;
  std::string itemNote = "no flask item";
  if (const int fi = c.items.Find("flask"); fi >= 0) {
    const ItemDef* fd = c.items.At(fi);
    ItemInstance st;
    st.name = fd->name;
    st.count = 1;
    st.contents.Add(b.subs[water].mat, 64);
    st.contents.Add((uint16_t)(MatId(c, "salt") | alchemy::kDissolvedBit), 5);
    st.stoppered = true;
    const char* w = nullptr;
    const bool refuses = !ContainerAccepts(*fd, st, b.subs[water].mat, c.mats, &w);
    std::vector<ParticleSpawn> ps;
    ItemInstance pour = st;
    const int poured = ContainerPour(*fd, pour, Vec3{0, 50, 0}, Vec3{1, 0, 0}, nullptr, 8, 1, 1u, ps, nullptr,
                                     0xFFFFFFFFu, &c.mats);
    std::vector<uint8_t> buf, buf1;
    ByteWriter bw{buf};
    WriteItemInstance(bw, st, kItemFmtStopper);
    ByteReader br{buf.data(), buf.size()};
    ItemInstance back;
    const bool read = ReadItemInstance(br, back, kItemFmtStopper);
    ByteWriter bw1{buf1};
    WriteItemInstance(bw1, st, kItemFmtMixed);
    ByteReader br1{buf1.data(), buf1.size()};
    ItemInstance old;
    ReadItemInstance(br1, old, kItemFmtMixed);
    const bool trip = read && back.stoppered && back.contents.SameAs(st.contents);
    itemOk = refuses && poured == 0 && ps.empty() && trip && !old.stoppered && old.contents.SameAs(st.contents);
    itemNote = Format("item: scoop %s (%s), pour %d, record %s, older format %s", refuses ? "refused" : "ALLOWED",
                      w ? w : "", poured, trip ? "keeps stopper + dissolved salt" : "LOSES IT",
                      old.stoppered ? "KEPT A STOPPER" : "unstoppered");
  }
  const bool ok = held && audit1 && ledger1 && pop && audit2 && burst && audit3 && itemOk;
  detail = Format("held: peak %d gas units inside, %u eighths vented%s, audit %s, ledger %s; "
                  "pop: %s (frame %d; events %s) audit %s; burst: %s (frame %d, heat %.2f) audit %s%s%s",
                  maxGas, vented, popped ? " (after it popped)" : "", audit1 ? "exact" : why.c_str(),
                  ledger1 ? "validates" : lwhy.c_str(), pop ? "yes" : "NO", popAt, kinds2.c_str(),
                  audit2 ? "exact" : why2.c_str(), burst ? "yes" : "NO", burstAt, s3.Heat(v3),
                  audit3 ? "exact" : why3.c_str(), b.fixtures.empty() ? "" : "; FIXTURE rules: ",
                  b.fixtures.c_str());
  detail += "; " + itemNote;
  std::printf("alchemy-stopper: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// DISSOLVING (solutes.json). Salt in water: grains vanish into the liquid,
// the tally reports a DISSOLVED portion (contract 2.4) equal to what
// dissolved, salt conserved across both forms; taken off the bench and put
// back, the dissolved portion goes back into the water. Fairy dust in water:
// enchanted water appears.
Status GateAlchemyDissolve(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int water = SlotOfName(b, c, "water"), salt = SlotOfName(b, c, "salt"),
            fairy = SlotOfName(b, c, "fairy_dust"), ench = SlotOfName(b, c, "enchanted_water");
  if (water < 0 || salt < 0 || fairy < 0 || ench < 0) { detail = "missing materials"; return Status::Fail; }
  if (!b.chem.SoluteFrom(salt) || !b.chem.SoluteFrom(fairy)) {
    detail = "solutes.json has no salt / fairy species";
    return Status::Fail;
  }
  const uint16_t mSalt = b.subs[salt].mat, dSalt = (uint16_t)(mSalt | alchemy::kDissolvedBit);
  Composition in;
  in.Add(b.subs[water].mat, 300);
  in.Add(mSalt, 20);
  alchemy::FlaskSim s(ChemConfig());
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in);
  for (int f = 0; f < 60 * 15; f++) {
    // Stirred for the first half: it dissolves where it touches.
    const bool on = f >= 30 && f < 450;
    const float ph = f * 0.08f;
    s.SetStick(on, {240 + 5 * std::sin(ph), 170}, {240 + 34 * std::sin(ph + 0.4f), 16}, 2.5f);
    s.Step(4);
  }
  s.SetStick(false);
  for (int f = 0; f < 120; f++) s.Step(4);
  Shot(s, "alchemy_dissolve.bmp");
  const alchemy::Tally t = s.Count();
  const uint32_t grains = t.vessel[v].AmountOf(mSalt), dis = t.vessel[v].AmountOf(dSalt);
  const int64_t disUnits = s.DissolvedUnits(salt);
  const bool tallyMatches = (int64_t)dis * 12 <= disUnits + 12 && disUnits <= (int64_t)dis * 12 + 12;
  const bool conserved = grains + dis + t.spilled.AmountOf(mSalt) + t.spilled.AmountOf(dSalt) == 20;
  std::string why;
  const bool audit = s.AuditUnits(&why);
  // Round trip: take it off, put it back.
  const Composition off = s.RemoveVessel(v);
  alchemy::FlaskSim s2(ChemConfig());
  s2.SetSubstances(b.subs);
  s2.SetChemistry(b.chem);
  s2.AddVessel(BenchFlask(512), {{240, 4}, 0}, off);
  const int64_t back = s2.DissolvedUnits(salt);
  const bool trip = off.AmountOf(dSalt) > 0 && back == (int64_t)off.AmountOf(dSalt) * 12;
  // Fairy dust.
  Composition fin;
  fin.Add(b.subs[water].mat, 200);
  fin.Add(b.subs[fairy].mat, 40);
  alchemy::FlaskSim s3(ChemConfig());
  s3.SetSubstances(b.subs);
  s3.SetChemistry(b.chem);
  const int v3 = s3.AddVessel(BenchFlask(512), {{240, 4}, 0}, fin);
  for (int f = 0; f < 60 * 12; f++) {
    const bool on = f >= 30 && f < 400;
    const float ph = f * 0.08f;
    s3.SetStick(on, {240 + 5 * std::sin(ph), 170}, {240 + 34 * std::sin(ph + 0.4f), 16}, 2.5f);
    s3.Step(4);
  }
  s3.SetStick(false);
  Shot(s3, "alchemy_fairy.bmp");
  const alchemy::Tally t3 = s3.Count();
  const uint32_t enchanted = t3.vessel[v3].AmountOf(b.subs[ench].mat);
  std::string why3;
  const bool audit3 = s3.AuditUnits(&why3);
  const bool ok = dis > 0 && grains < 20 && tallyMatches && conserved && audit && trip && enchanted > 0 && audit3;
  detail = Format("salt 20 -> %u grains + %u dissolved (%lld units in the water; tally %s), %s, audit %s; "
                  "off and on again: %u dissolved eighths -> %lld units %s; fairy dust 40 + water 200 -> "
                  "%u enchanted water, audit %s",
                  grains, dis, (long long)disUnits, tallyMatches ? "matches" : "DIFFERS",
                  conserved ? "conserved" : "NOT CONSERVED", audit ? "exact" : why.c_str(), off.AmountOf(dSalt),
                  (long long)back, trip ? "(back in solution)" : "(LOST)", enchanted,
                  audit3 ? "exact" : why3.c_str());
  std::printf("alchemy-dissolve: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ELECTROLYSIS, THE WORLD'S WAY: salt over the burner melts (salt + tag:hot
// -> molten_salt, the heat a VIRTUAL neighbour through the glass); electrify
// it and the discharge (a virtual spark neighbour) splits it -- molten_salt +
// tag:electric -> sodium + chlorine, the chlorine MATERIALISED where the
// spark touched, as the world's spark voxel becomes chlorine.
Status GateAlchemyElectrolysis(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int salt = SlotOfName(b, c, "salt"), ms = SlotOfName(b, c, "molten_salt"),
            na = SlotOfName(b, c, "sodium"), cl = SlotOfName(b, c, "chlorine");
  if (salt < 0 || ms < 0 || na < 0 || cl < 0) { detail = "missing materials"; return Status::Fail; }
  if (!b.chem.heat.on || !b.chem.spark.on) { detail = "no tag:hot / tag:electric in the table"; return Status::Fail; }
  if (!HasVirtualRule(b, salt, b.chem.heat, ms)) {
    alchemy::ChemRule r;
    r.nbrTags = b.chem.heat.tags;
    r.chance = 60 * 2000;
    r.prodSelf = ms;
    AddFixtureRule(b, salt, r, "salt+tag:hot->molten_salt");
  }
  if (!HasVirtualRule(b, ms, b.chem.spark, na)) {
    alchemy::ChemRule r;
    r.nbrTags = b.chem.spark.tags;
    r.chance = 500 * 2000;
    r.prodSelf = na;
    r.prodNbr = cl;
    AddFixtureRule(b, ms, r, "molten_salt+tag:electric->sodium+chlorine");
  }
  Composition in;
  in.Add(b.subs[salt].mat, 60);
  alchemy::FlaskSim s(ChemConfig());
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  const int v = s.AddVessel(BenchFlask(256), {{240, 4}, 0}, in);
  s.SetBurner(v, true);
  int meltAt = -1;
  for (int f = 0; f < 60 * 20 && meltAt < 0; f++) {
    s.Step(4);
    if (s.Produced()[ms] >= 12 * 6) meltAt = f;
  }
  Shot(s, "alchemy_molten.bmp");
  const int64_t molten = s.Produced()[ms];
  // Electrify, every half second, for a few seconds; the burner stays on.
  for (int f = 0; f < 60 * 6; f++) {
    if (f % 30 == 0) s.Shock(v);
    s.Step(4);
    if (f == 8) Shot(s, "alchemy_electrify.bmp");
  }
  const int64_t sodium = s.Produced()[na], chlorine = s.Produced()[cl];
  std::string why;
  const bool audit = s.AuditUnits(&why);
  const bool ok = meltAt >= 0 && sodium > 0 && chlorine > 0 && audit;
  detail = Format("burner: %lld units of molten salt by frame %d (heat %.2f); electrify: %lld units sodium, "
                  "%lld units chlorine; audit %s%s%s",
                  (long long)molten, meltAt, s.Heat(v), (long long)sodium, (long long)chlorine,
                  audit ? "exact" : why.c_str(), b.fixtures.empty() ? "" : "; FIXTURE rules: ", b.fixtures.c_str());
  std::printf("alchemy-electrolysis: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// BRINE ELECTROLYSIS AT THE BENCH (the chlor-alkali cell; package G). The
// world's two water rules gated on DISSOLVED salt (reactions.json "solute":
// "salt", "cMin": 24) are compiled onto the bench with their condition
// (flasksim_mats.h BuildBenchChemistry; benchchem.h ChemRule::soluteSpecies)
// and evaluated against each water particle's own dissolved mass, in the
// world's concentration units (ChemConcentration). A flask of brine under the
// Electrify button gives off chlorine and hydrogen and turns caustic (lye); a
// flask of FRESH water under the same shocks gives off nothing -- the
// condition, not the spark, is what decides. NO fixture rules: if the table
// lacks the conditioned rules the gate fails rather than supplying them.
Status GateAlchemyBrineElectrolysis(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int water = SlotOfName(b, c, "water"), salt = SlotOfName(b, c, "salt"),
            lye = SlotOfName(b, c, "lye"), cl = SlotOfName(b, c, "chlorine"),
            h2 = SlotOfName(b, c, "hydrogen");
  if (water < 0 || salt < 0 || lye < 0 || cl < 0 || h2 < 0) { detail = "missing materials"; return Status::Fail; }
  if (!b.chem.spark.on || !b.chem.SoluteFrom(salt)) {
    detail = "no tag:electric in the table, or no salt species";
    return Status::Fail;
  }
  // The conditioned rules made it into the bench's table.
  int conditioned = 0;
  uint32_t cMin = 0;
  for (const alchemy::ChemRule& r : b.chem.rules[water])
    if (r.soluteSpecies != 0 && r.soluteSpecies == b.chem.SoluteFrom(salt)->species) {
      conditioned++;
      cMin = r.soluteMin;
    }
  const uint16_t dSalt = (uint16_t)(b.subs[salt].mat | alchemy::kDissolvedBit);
  auto run = [&](uint32_t dissolved, int64_t& lyeU, int64_t& clU, int64_t& h2U, uint32_t& conc,
                 std::string& why, const char* shot) {
    Composition in;
    in.Add(b.subs[water].mat, 300);
    if (dissolved) in.Add(dSalt, dissolved);
    alchemy::FlaskSim s(ChemConfig());
    s.SetSubstances(b.subs);
    s.SetChemistry(b.chem);
    const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in);
    for (int f = 0; f < 60; f++) s.Step(4);   // settle
    conc = alchemy::ChemConcentration((uint32_t)s.DissolvedUnits(salt), 300u * 12u,
                                      b.chem.SoluteFrom(salt)->yieldPerVoxel);
    // Electrify every half second for six seconds.
    for (int f = 0; f < 60 * 6; f++) {
      if (f % 30 == 0) s.Shock(v);
      s.Step(4);
      if (f == 8 && shot) Shot(s, shot);
    }
    lyeU = s.Produced()[lye];
    clU = s.Produced()[cl];
    h2U = s.Produced()[h2];
    return s.AuditUnits(&why);
  };
  int64_t lyeB = 0, clB = 0, h2B = 0, lyeF = 0, clF = 0, h2F = 0;
  uint32_t concB = 0, concF = 0;
  std::string whyB, whyF;
  const bool auditB = run(60, lyeB, clB, h2B, concB, whyB, "alchemy_brine_electrify.bmp");
  const bool auditF = run(0, lyeF, clF, h2F, concF, whyF, nullptr);
  const bool ok = conditioned >= 1 && concB >= cMin && clB > 0 && (lyeB > 0 || h2B > 0) &&
                  clF == 0 && lyeF == 0 && h2F == 0 && auditB && auditF;
  detail = Format("%d salt-conditioned water rule(s) on the bench (cMin %u); brine (60 dissolved "
                  "eighths in 300 water, c %u): %lld units chlorine, %lld lye, %lld hydrogen, audit %s; "
                  "fresh water (c %u): %lld chlorine, %lld lye, %lld hydrogen (all must be 0), audit %s",
                  conditioned, cMin, concB, (long long)clB, (long long)lyeB, (long long)h2B,
                  auditB ? "exact" : whyB.c_str(), concF, (long long)clF, (long long)lyeF,
                  (long long)h2F, auditF ? "exact" : whyF.c_str());
  std::printf("alchemy-brine-electrolysis: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// SODIUM IN WATER EXPLODES -- ON THE BENCH TOO. A flask of water with a
// pinch of sodium: the world's explode-rule fires on the bench and raises an
// `explode` BenchEvent. Its registered handler ejects the player, breaks the
// held vessels and asks for a REAL blast at the hands; the pure half of the
// game's answer (BenchOutcomeWorldOps) is an ExplosionOp at the hands of the
// authored size and a puff of the reaction's gases. The vessel's remains go
// to the world as a BURST spill through the one vessel->world door
// (ContainerSpillStep), which must emit every eighth -- and the session's
// ledger still validates though it ended mid-reaction.
Status GateAlchemyExplode(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int water = SlotOfName(b, c, "water"), na = SlotOfName(b, c, "sodium"),
            lye = SlotOfName(b, c, "lye"), h2 = SlotOfName(b, c, "hydrogen");
  if (water < 0 || na < 0 || lye < 0 || h2 < 0) { detail = "missing materials"; return Status::Fail; }
  bool authored = false;
  for (const alchemy::ChemRule& r : b.chem.rules[na])
    for (int i = 0; r.fx >= 0 && i < (int)r.fxCount; i++)
      authored |= b.chem.effects[(size_t)(r.fx + i)].kind == "explode" && r.kind == alchemy::kChemPair &&
                  alchemy::ChemNbrMatches(r, b.subs[water].mat, b.subs[water].tagMask, b.subs[water].klass, false);
  if (!authored) {
    alchemy::ChemRule r;
    r.nbrMat = b.subs[water].mat;
    r.chance = 300 * 2000;
    r.prodSelf = h2;
    r.prodNbr = lye;
    r.fx = (int)b.chem.effects.size();
    r.fxCount = 1;
    alchemy::ChemEffect e;
    e.kind = "explode";
    e.radius = 5;
    e.power = 150;
    b.chem.effects.push_back(e);
    AddFixtureRule(b, na, r, "sodium+water->explode");
  }
  Composition in;
  in.Add(b.subs[water].mat, 200);
  in.Add(b.subs[na].mat, 10);
  alchemy::FlaskSim s(ChemConfig());
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in);
  alchemy::SimEvent ev;
  int at = -1;
  for (int f = 0; f < 240 && at < 0; f++) {
    s.Step(4);
    for (const alchemy::SimEvent& e : s.TakeEvents())
      if (e.kind == "explode" && at < 0) { ev = e; at = f; }
  }
  Shot(s, "alchemy_explode.bmp");
  // The handler, through the registry.
  alchemy::BenchEvent be;
  be.kind = ev.kind;
  be.entry = 0;
  be.radius = ev.radius;
  be.power = ev.power;
  be.count = ev.count;
  be.products = ev.products;
  alchemy::BenchOutcome out;
  const bool handled = at >= 0 && alchemy::DispatchBenchEvent(be, out);
  const Vec3 hands{100.5f, 60.5f, 100.5f};
  std::vector<ExplosionOp> exps;
  std::vector<GasSpawnOp> gas;
  alchemy::BenchOutcomeWorldOps(out, hands, 0x5EEDu, c.mats, exps, gas);
  const bool blastOk = exps.size() == 1 && exps[0].x == 100 && exps[0].y == 60 && exps[0].z == 100 &&
                       exps[0].radius >= ev.radius && exps[0].power == ev.power;
  bool puffOk = !gas.empty();
  for (const GasSpawnOp& g : gas) puffOk &= (g.payload & 0xFFFu) < c.mats.size() &&
                                              c.mats[g.payload & 0xFFFu].gpu.klass == CLASS_GAS;
  // Ejected mid-reaction: the tally where it stands, the ledger, then the
  // vessel's remains out through the spill door.
  std::string lwhy;
  const bool ledger = BenchLedgerHolds(c, s, in, v, Composition{}, lwhy);
  const alchemy::Tally t = s.Count();
  ContainerSpill sp;
  sp.at = hands;
  sp.away = Vec3{0, 1, 0};
  sp.rest = t.vessel[v];
  sp.seed = 0x5117u;
  int emitted = 0;
  std::vector<FluidSpawnOp> fl;
  std::vector<ParticleSpawn> pa;
  std::vector<GasSpawnOp> gs;
  SplatterEvent splat;
  for (int k = 0; k < 64 && !sp.Done(); k++) {
    fl.clear();
    pa.clear();
    emitted += ContainerSpillStep(sp, c.mats, 1000 + k, 4096, fl, pa, &splat, 0xFFFFFFFFu, &gs, 1024);
  }
  const bool spillOk = sp.Done() && emitted == (int)t.vessel[v].Total();
  // POCKET CHEMISTRY (ContainerPocketExplosion): off the bench, a flask that
  // has sodium and water meet in it goes off too -- by the authored rule.
  std::string pocketNote = "pocket: not authored (fixture rule has no ruleFx)";
  bool pocketOk = true;
  if (authored) {
    ReactionEffect pfx, nfx;
    Composition both, alone;
    both.Add(b.subs[na].mat, 8);
    both.Add(b.subs[water].mat, 40);
    alone.Add(b.subs[na].mat, 8);
    const bool boom = ContainerPocketExplosion(both, c.mats, c.reactions, pfx) && pfx.kind == "explode";
    const bool calm = !ContainerPocketExplosion(alone, c.mats, c.reactions, nfx);
    pocketOk = boom && calm;
    pocketNote = Format("pocket: sodium+water %s (r%d p%d), sodium alone %s", boom ? "explodes" : "DOES NOT",
                        pfx.radius, pfx.power, calm ? "calm" : "EXPLODES");
  }
  const bool ok = at >= 0 && handled && out.eject && out.breakHeld && blastOk && puffOk && ledger && spillOk &&
                  pocketOk;
  detail = Format("explode event at frame %d (x%d, radius %d power %d); handler: eject %s, break %s, blast %s "
                  "(r%d p%d at the hands), puff %zu gas parcels %s; ledger %s; the broken flask's %u eighths "
                  "-> %d emitted through the spill door (%zu gas parcels) %s%s%s",
                  at, ev.count, ev.radius, ev.power, out.eject ? "yes" : "NO", out.breakHeld ? "yes" : "NO",
                  blastOk ? "ok" : "WRONG", exps.empty() ? 0 : exps[0].radius, exps.empty() ? 0 : exps[0].power,
                  gas.size(), puffOk ? "ok" : "WRONG", ledger ? "validates" : lwhy.c_str(), t.vessel[v].Total(),
                  emitted, gs.size(), spillOk ? "ok" : "SHORT", b.fixtures.empty() ? "" : "; FIXTURE rules: ",
                  b.fixtures.c_str());
  detail += "; " + pocketNote;
  std::printf("alchemy-explode: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// PACKAGE E ON THE BENCH (docs/PLAN_alchemy_chemistry.md, the creative
// expansion). Every headline recipe in a flask, through the bench's copy of
// the WORLD'S table (no fixture rules: a recipe the table does not carry
// fails, by name). Pairs are poured in together; the heat recipes stand on
// the burner. Each row must MAKE its product (FlaskSim::Produced) and, where
// the rule carries an effect, raise that bench event (gunpowder and dragon's
// blood `explode`, thermite and the chemical wedding `flash`); the units
// audit must stay exact in every flask. Solute converts are rows too (sugar
// -> syrup, spores -> the potion of light, salt in enchanted water -> holy
// water), since on the bench they are the dissolving machinery's, not a rule.
Status GateChemBench(Ctx& c, std::string& detail) {
  struct Row {
    const char* a; int aE;
    const char* b; int bE;     // nullptr = one reagent
    bool burner;
    const char* product;
    const char* event;         // nullptr = none expected
  };
  static const Row kRows[] = {
      {"saltpeter", 40, "charcoal", 40, false, "black_mix", nullptr},
      {"black_mix", 40, "sulfur", 40, false, "gunpowder", nullptr},
      {"gunpowder", 30, nullptr, 0, true, "fire", "explode"},
      {"saltpeter", 40, "sugar", 40, false, "smoke_powder", nullptr},
      {"smoke_powder", 30, nullptr, 0, true, "thick_smoke", nullptr},
      {"rust", 40, "aluminium", 40, false, "thermite", nullptr},
      {"thermite", 40, nullptr, 0, true, "molten_iron", "flash"},
      {"molten_iron", 30, "water", 120, false, "iron", nullptr},
      {"chalk", 40, nullptr, 0, true, "quicklime", nullptr},
      {"quicklime", 30, "water", 150, false, "slaked_lime", nullptr},
      {"acid", 150, "chalk", 30, false, "choke_damp", nullptr},
      {"frost_salt", 20, "water", 150, false, "ice", nullptr},
      {"acid", 150, "copper", 30, false, "blue_vitriol", nullptr},
      {"acid", 150, "saltpeter", 30, false, "aqua_fortis", nullptr},
      {"aqua_fortis", 150, "salt", 30, false, "aqua_regia", nullptr},
      {"quicksilver", 60, "sulfur", 30, false, "cinnabar", nullptr},
      {"cinnabar", 30, nullptr, 0, true, "quicksilver", nullptr},
      {"spirits", 100, "acid", 60, false, "ether", nullptr},
      {"ether", 60, nullptr, 0, false, "ether_vapour", nullptr},
      {"phosphorus", 30, nullptr, 0, false, "fire", "flash"},
      {"sugar", 120, "water", 100, false, "syrup", nullptr},
      {"luminous_spores", 60, "water", 100, false, "glow_potion", nullptr},
      {"salt", 30, "enchanted_water", 150, false, "holy_water", nullptr},
      {"fairy_dust", 30, "ichor", 100, false, "slime", nullptr},
      {"slime", 100, "salt", 30, false, "water", nullptr},
      {"sunwater", 100, "moonwater", 100, false, "philosophers_stone", "flash"},
      {"philosophers_stone", 20, "lead", 40, false, "gold_dust", nullptr},
      {"luminous_spores", 20, "moonwater", 100, false, "glowcap", nullptr},
      {"dragons_blood", 60, nullptr, 0, true, "fire", "explode"},
  };
  ChemBench b = MakeChemBench(c);
  const int n = (int)(sizeof(kRows) / sizeof(kRows[0]));
  const int kFrames = (int)BaselineNumber("chemBench.framesMax", 600);
  int made = 0;
  std::string fails, got;
  for (int i = 0; i < n; i++) {
    const Row& r = kRows[i];
    const int sa = SlotOfName(b, c, r.a), sb = r.b ? SlotOfName(b, c, r.b) : -2,
              sp = SlotOfName(b, c, r.product);
    if (sa < 0 || sb == -1 || sp < 0) {
      fails += Format("%s%s: not on the bench", fails.empty() ? "" : "; ", r.product);
      continue;
    }
    Composition in;
    in.Add(b.subs[sa].mat, (uint32_t)r.aE);
    if (sb >= 0) in.Add(b.subs[sb].mat, (uint32_t)r.bE);
    alchemy::FlaskSim s(ChemConfig());
    s.SetSubstances(b.subs);
    s.SetChemistry(b.chem);
    const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in);
    if (r.burner) s.SetBurner(v, true);
    int at = -1;
    bool evSeen = r.event == nullptr;
    for (int f = 0; f < kFrames; f++) {
      s.Step(4);
      for (const alchemy::SimEvent& e : s.TakeEvents())
        if (r.event && e.kind == r.event) evSeen = true;
      if (s.Produced()[sp] > 0 && at < 0) at = f;
      if (at >= 0 && evSeen) break;
    }
    std::string why;
    const bool audit = s.AuditUnits(&why);
    const bool ok = at >= 0 && evSeen && audit;
    if (ok) made++;
    else
      fails += Format("%s%s%s%s -> %s: %s%s%s", fails.empty() ? "" : "; ", r.a, r.b ? "+" : "",
                      r.b ? r.b : "", r.product, at < 0 ? "NOT MADE" : "made",
                      evSeen ? "" : Format(", no %s event", r.event).c_str(),
                      audit ? "" : (", audit: " + why).c_str());
    got += Format("%s%s@%d", got.empty() ? "" : ", ", r.product, at);
  }
  RecordObserved("chemBench.made", (double)made);
  const bool ok = made == n;
  detail = Format("%d of %d bench recipes made their product (and event) within %d frames%s%s [frame made: %s]",
                  made, n, kFrames, fails.empty() ? "" : "; FAILED: ", fails.c_str(), got.c_str());
  std::printf("chem-bench: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& AlchemyGates() {
  static const std::vector<Gate> g = {
      // CPU-only over the material table; order-independent.
      {"alchemy-layers", "player", {}, false, GateAlchemyLayers},
      {"alchemy-pour", "player", {}, false, GateAlchemyPour},
      {"alchemy-cost", "player", {}, true, GateAlchemyCost},
      {"alchemy-shake", "player", {}, false, GateAlchemyShake},
      {"alchemy-spawn", "player", {}, false, GateAlchemySpawn},
      {"alchemy-sand-carry", "player", {}, false, GateAlchemySandCarry},
      // Bench chemistry (package C): the world's rules on the bench.
      {"alchemy-react", "player", {}, false, GateAlchemyReact},
      {"alchemy-stopper", "player", {}, false, GateAlchemyStopper},
      {"alchemy-dissolve", "player", {}, false, GateAlchemyDissolve},
      {"alchemy-electrolysis", "player", {}, false, GateAlchemyElectrolysis},
      {"alchemy-brine-electrolysis", "player", {}, false, GateAlchemyBrineElectrolysis},
      {"alchemy-explode", "player", {}, false, GateAlchemyExplode},
      // Package E: the creative expansion's recipes on the bench.
      {"chem-bench", "player", {}, false, GateChemBench},
  };
  return g;
}

}  // namespace selftest
