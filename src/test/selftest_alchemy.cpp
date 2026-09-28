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
  // ONE POUR IS ONE CHAOTIC SAMPLE: a glob leaving the lip a frame early or
  // late lands on the rim of A or in it, so a single run's spill moved 0 ->
  // 60 eighths between RNG seeds of the same code (lab, 2026-09-27: the
  // pre-coherence sim spilled 7.4 on average over 16 seeds with two over the
  // limit, the current one 2.0 with none). The same pour runs under
  // alchemy.pourSeeds seeds; what it claims -- most of the oil arrives, little
  // splashes -- is asserted on the MEAN; conservation and the spill's
  // bookkeeping on EVERY run.
  struct Run { bool cons, cons2, streamed, partitioned, fromA; uint32_t oilIn, sandIn, oilSpilled, spilled, drainedPour, dumped; std::string why, why2, order, source; };
  auto run = [&](uint32_t seed, bool shot) {
  Run R{};
  alchemy::SimConfig cfg;
  cfg.seed = seed;
  FlaskSim s(cfg);
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
  // WHO SPILLED IT (DrainSpilled's SpillBy): each drained eighth is also
  // filed under the vessel it LEFT, which is what the bench pours from in
  // the world. It must partition the drain exactly.
  Composition drained;
  uint32_t byVessel[2] = {0, 0}, byUnknown = 0;
  bool partitioned = true;
  auto drain = [&]() {
    FlaskSim::SpillBy by;
    const Composition d = s.DrainSpilled(nullptr, &by);
    for (int i = 0; i < d.n; i++) drained.Add(d.p[i].mat, d.p[i].eighths);
    uint32_t sum = by.unknown.Total();
    byUnknown += by.unknown.Total();
    for (size_t v = 0; v < by.vessel.size(); v++) {
      sum += by.vessel[v].Total();
      if (v < 2) byVessel[v] += by.vessel[v].Total();
    }
    if (sum != d.Total()) partitioned = false;
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
  if (shot) Shot(s, "alchemy_pour.bmp");
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
  // After settling, the target has sand under water under oil (reported).
  std::string order;
  Ordered(s, subs, order);
  (void)poured;
  R.cons = cons; R.why = why; R.oilIn = oilIn; R.sandIn = sandIn; R.oilSpilled = oilSpilled;
  R.spilled = spilled; R.drainedPour = drained.Total(); R.order = order;
  // ...then A is turned over and emptied off the table, drained as it
  // falls: the live stream must carry real matter and still add up.
  const uint32_t pourBy[2] = {byVessel[0], byVessel[1]}, pourUnknown = byUnknown;
  byVessel[0] = byVessel[1] = byUnknown = 0;
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
  R.cons2 = cons2; R.why2 = why2; R.streamed = streamed; R.dumped = drained.Total();
  // The dump is A's (the LEFT vessel, x 96; B stands upright to its right):
  // what falls must be filed under A, not under B nor "nobody" -- the bench
  // pours it from A's lip in the world. (Loose sand B dropped on the table
  // earlier may fall with it, hence a share, not all.)
  const uint32_t dumped = byVessel[0] + byVessel[1] + byUnknown;
  const bool fromA = A == 0 && dumped > 0 && byVessel[0] >= 0.9 * dumped;
  R.source = Format("pour A %u B %u ? %u, dump A %u B %u ? %u%s", pourBy[0], pourBy[1], pourUnknown, byVessel[0],
                    byVessel[1], byUnknown, partitioned ? "" : " (NOT A PARTITION)");
  R.partitioned = partitioned;
  R.fromA = fromA;
  return R;
  };
  const int seeds = std::max(1, (int)BaselineNumber("alchemy.pourSeeds", 5));
  const uint32_t oilB = b.AmountOf((uint16_t)oil);
  double oilInSum = 0, oilSpillSum = 0;
  bool every = true;
  std::string per, fails;
  for (int k = 0; k < seeds; k++) {
    const uint32_t seed = k == 0 ? alchemy::SimConfig{}.seed : 0x5eed + 7919u * (uint32_t)k;
    const Run R = run(seed, k == 0);
    oilInSum += R.oilIn;
    oilSpillSum += R.oilSpilled;
    per += Format("%s%u/%u", k ? ", " : "", R.oilIn, R.oilSpilled);
    const bool okRun = R.cons && R.cons2 && R.streamed && R.partitioned && R.fromA;
    if (!okRun)
      fails += Format(" [seed %d: %s; dump %s; %s]", k, R.cons ? "conserved" : R.why.c_str(),
                      R.cons2 ? "conserved" : R.why2.c_str(), R.source.c_str());
    every = every && okRun;
    if (k == 0)
      detail = Format("seed 0: into A oil %u/%u sand %u/%u; spilled oil %u, all %u (%u drained live); order %s; "
                      "dumped %u drained live; spill source %s",
                      R.oilIn, oilB, R.sandIn, b.AmountOf((uint16_t)sand), R.oilSpilled, R.spilled,
                      R.drainedPour, R.order.c_str(), R.dumped, R.source.c_str());
  }
  const double oilInMean = oilInSum / seeds, oilSpillMean = oilSpillSum / seeds;
  const double minPoured = BaselineNumber("alchemy.pourMinFrac", 0.8);
  const double maxSpill = BaselineNumber("alchemy.pourMaxSpillFrac", 0.05);
  const bool moved = oilInMean >= minPoured * oilB;
  const bool tidy = oilSpillMean <= maxSpill * oilB;
  RecordObserved("alchemy.pourOilInMean", oilInMean);
  RecordObserved("alchemy.pourOilSpilledMean", oilSpillMean);
  detail += Format(" | %d seeds (oil in / oil spilled): %s; mean in %.1f (min %.0f), mean spilled %.1f (max %.1f); "
                   "every run conserved with its spill filed: %s%s",
                   seeds, per.c_str(), oilInMean, minPoured * oilB, oilSpillMean, maxSpill * oilB,
                   every ? "yes" : "NO", fails.c_str());
  const bool ok = every && moved && tidy;
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
// more than alchemy.sandWorstMovePxMax (baseline.json), and the whole of it
// sleeps once set down.
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
  const int maxWorst = (int)BaselineNumber("alchemy.sandWorstMovePxMax", 12);
  const bool ok = cons && t.spilled.Total() == 0 && bigFrac <= maxBig && worst <= maxWorst && sleptAt >= 0;
  std::printf("alchemy-sand-carry: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// A FLASK HELD UPSIDE DOWN OVER ANOTHER POURS INTO IT (owner, 2026-09-27: "it's
// impossible to lift it above the other flask and rotate upside down"). The
// bench's grid is never shorter than AlchemyBench::kLiftH, whatever its box;
// this runs at EXACTLY that height, the tightest a player ever gets. Two
// full-size flasks (1024 eighths); the right one holds water. The hand
// (alchemy::HandGoal, the bench's own clamp) grabs it by the belly, lifts it,
// swings its lip over the other's mouth tipped to 75 degrees (before the
// water reaches the lip) and turns it on to upside down (pi), the pivot
// sliding from the pouring corner to the mouth's centre, 30 px over the
// other's. Asserted: it gets there (angle ~pi, its glass under the grid's top
// all the way), no particle ever goes above the grid (nothing leaves through
// the top), most of the water arrives (alchemy.liftMinFrac), the splash off
// the rim is bounded (alchemy.liftMaxSpillFrac -- a pour splashes
// chaotically, as alchemy-pour's does: a loose cap, not a tuning target) and
// the tally plus the drained spill is exact. Then the
// pointer is dragged far above the grid (off the panel): the flask stops at
// the top, its glass still inside. SANDVOX_LIFT_SHOTS=1 writes a picture every
// 10 frames (alchemy_lift_NNN.bmp) to see where a splash comes from.
Status GateAlchemyLift(Ctx& c, std::string& detail) {
  using alchemy::AlchemyBench;
  const int water = MatId(c, "water");
  if (water < 0) { detail = "no water"; return Status::Fail; }
  Composition in;
  in.Add((uint16_t)water, 400);
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  alchemy::SimConfig cfg = BenchConfig();
  cfg.gridH = AlchemyBench::kLiftH;
  cfg.tableY = AlchemyBench::kTableY;
  FlaskSim s(cfg);
  s.SetSubstances(subs);
  const alchemy::VesselShape shape = BenchFlask(1024);
  const float ty = AlchemyBench::kTableY;
  const int T = s.AddVessel(shape, {{150, ty}, 0}, Composition{});
  const int S = s.AddVessel(shape, {{330, ty}, 0}, in);
  const float h = shape.height;
  const V2 grabL{0.0f, h * 0.35f};
  auto rot = [](V2 v, float a) { return V2{v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)}; };
  auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
  // The pour pivots on the LEFT LIP CORNER (the lowest one while it turns
  // CCW), 30 px over the middle of the other's mouth. Pivoted on the mouth's
  // centre, the stream left the lower corner and splashed on the rim.
  const float lipHalf = shape.profile.back().x * shape.width * 0.5f;
  const V2 L{-lipHalf, h};                   // the corner, vessel frame
  const V2 M{150, ty + h + 30.0f};           // ...goes here
  const float liftY = M.y + 20.0f;           // S's base, lifted upright
  Composition drained;
  auto drain = [&]() {
    const Composition d = s.DrainSpilled();
    for (int i = 0; i < d.n; i++) drained.Add(d.p[i].mat, d.p[i].eighths);
  };
  float topGlass = 0, topLiquid = 0, maxAngle = 0;
  auto watch = [&]() {
    for (const V2& p : s.VesselOutline(S)) topGlass = std::max(topGlass, p.y);
    for (const V2& p : s.Positions()) topLiquid = std::max(topLiquid, p.y);
    maxAngle = std::max(maxAngle, s.VesselXform(S).angle);
  };
  // The hand: the pointer is the grab point of the pose wanted; HandGoal is
  // what the bench makes of it.
  auto hand = [&](const Xform& want) {
    const V2 g = rot(grabL, want.angle);
    const V2 at{want.pos.x + g.x, want.pos.y + g.y};
    s.SetVesselXform(S, alchemy::HandGoal(shape, at, grabL, want.angle, cfg.gridW, cfg.gridH, ty));
  };
  // Lift (0.67 s); swing the mouth over the other's while tipping it to 75
  // degrees, before the water reaches the lip (1.2 s); then turn on to
  // upside down about the mouth (1.5 s). The glass stays clear of the other
  // flask all the way (at 75 degrees the bulb is beside the other's neck).
  const float kSwing = 1.309f;
  // What has LEFT THE TABLE so far (drained); in-flight water is not a spill.
  auto spillNow = [&]() { return drained.Total(); };
  uint32_t spillSwing = 0, spillTurn = 0;
  int f = 0;
  for (; f < 200; f++) {
    Xform p{{330, liftY}, 0.0f};
    if (f < 40) {
      p.pos.y = ty + (liftY - ty) * ease(f / 40.0f);
    } else {
      float ang;
      V2 corner;
      if (f < 110) {
        const float e = ease((f - 40) / 70.0f);
        ang = kSwing * e;
        const V2 c0{330 + L.x, liftY + L.y};
        corner = {c0.x + (M.x - c0.x) * e, c0.y + (M.y - c0.y) * e};
      } else {
        ang = kSwing + (3.14159f - kSwing) * ease((f - 110) / 90.0f);
        corner = M;
      }
      // The pivot slides from the corner to the mouth's centre as it comes
      // upside down (both corners are level then, and a corner over the
      // middle would leave half the mouth off the other's).
      const float k = std::clamp((3.14159f - ang) / (3.14159f - kSwing), 0.0f, 1.0f);
      const V2 r = rot({L.x * k, L.y}, ang);
      p = {{corner.x - r.x, corner.y - r.y}, ang};
    }
    hand(p);
    s.Step(4);
    drain();
    watch();
    if (f == 109) spillSwing = spillNow();
    if (f == 150) Shot(s, "alchemy_lift_turn.bmp");
    if (std::getenv("SANDVOX_LIFT_SHOTS") && (f % 10) == 0) Shot(s, Format("alchemy_lift_%03d.bmp", f).c_str());
  }
  spillTurn = spillNow() - spillSwing;
  // Held upside down until it has run out (or 10 s).
  const V2 rpi = rot({0.0f, h}, 3.14159f);
  const Xform inverted{{M.x - rpi.x, M.y - rpi.y}, 3.14159f};
  for (int hold = 0; hold < 600; hold++, f++) {
    hand(inverted);
    if (std::getenv("SANDVOX_LIFT_SHOTS") && (f % 10) == 0 && hold < 120) Shot(s, Format("alchemy_lift_%03d.bmp", f).c_str());
    s.Step(4);
    drain();
    watch();
    if (hold > 150 && (hold % 30) == 0 && s.MovingCount(0.3f) == 0) break;
  }
  Shot(s, "alchemy_lift.bmp");
  const alchemy::Tally t = s.Count();
  const uint32_t arrived = t.vessel[T].AmountOf((uint16_t)water);
  const uint32_t left = t.vessel[S].AmountOf((uint16_t)water);
  const uint32_t spilled = t.spilled.AmountOf((uint16_t)water) + drained.AmountOf((uint16_t)water);
  alchemy::Tally all = t;
  for (int i = 0; i < drained.n; i++) all.spilled.Add(drained.p[i].mat, drained.p[i].eighths);
  std::string why;
  const bool cons = Conserved(in, all, why);
  // The drag off the panel: the pointer far above the grid, still inverted.
  float dragTop = 0;
  for (int k = 0; k < 90; k++) {
    s.SetVesselXform(S, alchemy::HandGoal(shape, {M.x, (float)cfg.gridH + 2000.0f}, grabL, 3.14159f, cfg.gridW,
                                          cfg.gridH, ty));
    s.Step(4);
    drain();
    watch();
  }
  for (const V2& p : s.VesselOutline(S)) dragTop = std::max(dragTop, p.y);
  const float gridTop = (float)cfg.gridH;
  const double minFrac = BaselineNumber("alchemy.liftMinFrac", 0.8);
  const double maxSpill = BaselineNumber("alchemy.liftMaxSpillFrac", 0.15);
  const bool turned = maxAngle >= 3.1f;
  const bool inside = topGlass + shape.wall <= gridTop && dragTop + shape.wall <= gridTop && topLiquid <= gridTop;
  const bool poured = arrived >= minFrac * 400;
  const bool tidy = spilled <= maxSpill * 400;
  RecordObserved("alchemy.liftArrived", (double)arrived);
  detail = Format("grid %d px tall: turned to %.2f rad; glass top %.1f (dragged off the panel: %.1f), liquid top %.1f; "
                  "water 400 -> %u in the lower flask, %u left in the inverted one, %u spilled; %s",
                  cfg.gridH, maxAngle, topGlass, dragTop, topLiquid, arrived, left, spilled,
                  cons ? "conserved" : why.c_str());
  detail += Format(" (off the table while swinging %u, while turning %u)", spillSwing, spillTurn);
  const bool ok = turned && inside && poured && tidy && cons;
  std::printf("alchemy-lift: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// A VESSEL COMING ONTO THE TABLE NEVER LANDS ON ANOTHER (owner, 2026-09-27: on
// a small window a new vial spawned inside the other one, the glasses tangled
// and it blew up). alchemy::FindPlaceSpot, the bench's own placing, over a
// range of table widths from far too narrow to roomy, for three pairs (two
// full flasks, a flask and a vial, a pouch and a flask), and then the first
// taken off and a vial put on in its place. Asserted, every time: a vessel is
// either REFUSED or stands wholly on the table with its glass clear of every
// other (PoseClear), and every vessel holds exactly what it came with and
// nothing is outside them; and at the bench's narrowest table
// (AlchemyBench::kMinW) and up, both of every pair fit.
Status GateAlchemyPlace(Ctx& c, std::string& detail) {
  using alchemy::AlchemyBench;
  const int water = MatId(c, "water"), sand = MatId(c, "sand");
  if (water < 0 || sand < 0) { detail = "missing water/sand"; return Status::Fail; }
  Composition in;
  in.Add((uint16_t)water, 150);
  in.Add((uint16_t)sand, 30);
  auto subs = alchemy::SubstancesFor(c.mats, {&in});
  const alchemy::VesselShape flask = BenchFlask(1024), vial = BenchFlask(256),
                             pouch = alchemy::ShapeWithArea(alchemy::PouchShape(100, 110), 1024 * 12);
  struct Pair { const char* name; const alchemy::VesselShape* a; const alchemy::VesselShape* b; };
  const Pair pairs[] = {{"flask+flask", &flask, &flask}, {"flask+vial", &flask, &vial}, {"pouch+flask", &pouch, &flask}};
  const float ty = AlchemyBench::kTableY;
  int placed = 0, refused = 0, bad = 0;
  std::string notes;
  for (int w : {160, 200, 240, 264, 280, AlchemyBench::kMinW, 320, 400, 560}) {
    for (const Pair& pr : pairs) {
      alchemy::SimConfig cfg = BenchConfig();
      cfg.gridW = w;
      cfg.gridH = AlchemyBench::kLiftH;
      FlaskSim s(cfg);
      s.SetSubstances(subs);
      std::vector<int> vs;
      auto put = [&](const alchemy::VesselShape& sh) {
        Xform p;
        if (!alchemy::FindPlaceSpot(s, sh, w, ty, p)) {
          refused++;
          return false;
        }
        vs.push_back(s.AddVessel(sh, p, in));
        placed++;
        return true;
      };
      auto check = [&](const char* when) {
        const alchemy::Tally t = s.Count();
        for (int v : vs) {
          if (!s.VesselAlive(v)) continue;
          const Xform x = s.VesselXform(v);
          const float half = s.Shape(v).width * 0.5f + s.Shape(v).wall;
          const bool clear = s.PoseClear(v, x);
          const bool onTable = x.pos.x - half >= 0 && x.pos.x + half <= (float)w;
          const bool holds = t.vessel[v].SameAs(in);
          if (!clear || !onTable || !holds) {
            bad++;
            notes += Format(" [w %d %s %s: vessel %d at %.0f%s%s%s]", w, pr.name, when, v, x.pos.x,
                            clear ? "" : " OVERLAPS", onTable ? "" : " OFF THE TABLE", holds ? "" : " contents changed");
          }
        }
        if (t.spilled.Total()) {
          bad++;
          notes += Format(" [w %d %s %s: %u eighths outside]", w, pr.name, when, t.spilled.Total());
        }
      };
      const bool a = put(*pr.a), b = put(*pr.b);
      check("placed");
      if (w >= AlchemyBench::kMinW && !(a && b)) {
        bad++;
        notes += Format(" [w %d %s: refused on a table the bench uses]", w, pr.name);
      }
      // The first taken off, a vial put on where there is room.
      if (a && !vs.empty()) {
        s.RemoveVessel(vs.front());
        vs.erase(vs.begin());
        put(vial);
        check("swapped");
      }
      for (int k = 0; k < 20; k++) s.Step(4);
      check("after a moment");
    }
  }
  detail = Format("%d vessels placed, %d refused (no clear spot), %d violations%s", placed, refused, bad, notes.c_str());
  const bool ok = bad == 0;
  std::printf("alchemy-place: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
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

// WHAT TURNS TO AIR INSIDE GLASS (owner, 2026-09-27: "matter + air = deletes
// should still take place except for things that are purposefully to 'dry
// them up' like blood ... [and] should NOT occur if the stopper is on").
// (1) An open flask of blood for 20 s: all of it is still there -- blood's
// `blood -> air` is a DRYING rule (reactions.json "drying"), which never fires
// inside a vessel. (2) An open flask of noxious gas: it vents and/or fades --
// a decay to air still fires in an open vessel. (3) The same gas stoppered:
// none of it fades. Every run: the units audit exact, the ledger validating.
Status GateAlchemyKeeps(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int blood = SlotOfName(b, c, "blood"), nox = SlotOfName(b, c, "noxious_gas");
  if (blood < 0 || nox < 0) { detail = "missing blood/noxious_gas"; return Status::Fail; }
  bool drying = false, noxFades = false;
  for (const alchemy::ChemRule& r : b.chem.rules[blood]) drying |= r.drying && r.prodSelf == alchemy::kChemAir;
  for (const alchemy::ChemRule& r : b.chem.rules[nox])
    noxFades |= r.kind == alchemy::kChemDecay && r.prodSelf == alchemy::kChemAir;
  struct Run { uint32_t inVessel = 0, out = 0; int64_t faded = 0; bool ok = false; std::string why; };
  auto run = [&](int slot, uint32_t amount, bool stoppered, int secs) {
    Run o;
    Composition in;
    in.Add(b.subs[slot].mat, amount);
    alchemy::FlaskSim s(ChemConfig());
    s.SetSubstances(b.subs);
    s.SetChemistry(b.chem);
    const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in, stoppered);
    Composition drained;
    for (int f = 0; f < 60 * secs; f++) {
      s.Step(4);
      Drain(s, drained);
    }
    const alchemy::Tally t = s.Count();
    const uint16_t m = b.subs[slot].mat;
    o.inVessel = t.vessel[v].AmountOf(m);
    o.out = t.spilled.AmountOf(m) + drained.AmountOf(m);
    o.faded = s.Consumed()[slot];
    std::string a, l;
    const bool audit = s.AuditUnits(&a), ledger = BenchLedgerHolds(c, s, in, v, drained, l);
    o.why = std::string(audit ? "" : " audit: " + a) + (ledger ? "" : " ledger: " + l);
    o.ok = audit && ledger;
    return o;
  };
  const Run rb = run(blood, 200, false, 20), rn = run(nox, 40, false, 20), rs = run(nox, 40, true, 10);
  // One eighth of slack for the tally's largest-remainder rounding.
  const bool bloodKept = rb.inVessel + rb.out + 1 >= 200 && rb.faded == 0;
  const bool noxGone = rn.inVessel * 4 <= 40 && (rn.faded > 0 || rn.out > 0);
  const bool sealedKept = rs.inVessel + 1 >= 40 && rs.faded == 0;
  const bool ok = rb.ok && rn.ok && rs.ok && drying && bloodKept && noxGone && sealedKept && noxFades;
  detail = Format("blood rule %s; open flask 20 s: blood 200 -> %u eighths in it (+%u out, %lld units dried)%s; "
                  "noxious gas 40 -> %u in it, %u vented, %lld units faded%s; stoppered 10 s: 40 -> %u in it, "
                  "%lld units faded%s",
                  drying ? "is a drying rule" : "NOT MARKED drying", rb.inVessel, rb.out, (long long)rb.faded,
                  rb.why.c_str(), rn.inVessel, rn.out, (long long)rn.faded, rn.why.c_str(), rs.inVessel,
                  (long long)rs.faded, rs.why.c_str());
  std::printf("alchemy-keeps: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ETHER EVAPORATES AT AIR, AND ITS VAPOUR FILLS THE BOTTLE (owner,
// 2026-09-27: "ether SHOULD turn into ether vapor, but it should produce far
// more vapor, and it also should only do that when touching air; the bottle
// should be full of both ether and ether vapor, which should fill up and
// replace all of the air"). Gas is VOLUMINOUS on the bench (SimConfig::
// gasExpand gas units to a unit of matter, one a pixel at rest).
// (1) STOPPERED: the vapour fills the headspace, its air goes to ~0, the
//     evaporation then STOPS (no evaporation over the last 6 s), no pop at
//     room temperature, the audit exact and the ledger validating. Then the
//     burner: the heated bottle goes (pop, burst or the vapour's explosion).
// (2) OPEN: it evaporates -- only at its surface (liquid right above an
//     evaporating particle at most `alchemy.evaporateMaxBuriedFraction` of
//     the time) -- and the vapour leaves through the mouth or fades.
// (3) VOLUME: one eighth of ether, evaporated wholly in a small stoppered
//     flask, is vapour on at least gasExpand x the pixels it had as liquid.
Status GateAlchemyEvaporate(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int ether = SlotOfName(b, c, "ether"), vap = SlotOfName(b, c, "ether_vapour");
  if (ether < 0 || vap < 0) { detail = "missing ether/ether_vapour"; return Status::Fail; }
  const uint16_t mE = b.subs[ether].mat, mV = b.subs[vap].mat;
  const alchemy::SimConfig cfg = ChemConfig();
  const int E = std::max(1, cfg.gasExpand);

  // (1) stoppered
  Composition in;
  in.Add(mE, 200);
  alchemy::FlaskSim s(cfg);
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in, true);
  Composition drained;
  std::string events;
  const int secs = (int)BaselineNumber("alchemy.evaporateSealedSec", 36.0);
  int evapAt = 0;
  float peakP = 0;
  for (int f = 0; f < 60 * secs; f++) {
    s.Step(4);
    Drain(s, drained);
    peakP = std::max(peakP, s.Pressure(v));
    for (const alchemy::SimEvent& e : s.TakeEvents()) events += e.kind + " ";
    if (f == 60 * (secs - 6)) evapAt = s.Evaporations();
    if (f == 60 * 8) Shot(s, "alchemy_evaporate_8s.bmp");
  }
  Shot(s, "alchemy_evaporate.bmp");
  const alchemy::Tally t1 = s.Count();
  const int air = s.AirPixelsIn(v), gasPx = s.GasPixelsIn(v), lateEvap = s.Evaporations() - evapAt;
  const float p1 = s.Pressure(v);
  std::string why1, lwhy1;
  const bool audit1 = s.AuditUnits(&why1), ledger1 = BenchLedgerHolds(c, s, in, v, drained, lwhy1);
  const int maxAir = (int)BaselineNumber("alchemy.evaporateMaxAirPixels", 20);
  const bool filled = air <= maxAir && gasPx > 0 && t1.vessel[v].AmountOf(mV) > 0;
  const bool stopped = lateEvap == 0;
  const bool held = events.empty() && s.Stoppered(v) && p1 < cfg.popAt && drained.Total() == 0;
  // ...then heated, it goes.
  std::string hotEvents;
  for (int f = 0; f < 60 * 10 && hotEvents.empty(); f++) {
    if (f == 0) s.SetBurner(v, true);
    s.Step(4);
    Drain(s, drained);
    for (const alchemy::SimEvent& e : s.TakeEvents()) hotEvents += e.kind + " ";
  }
  const bool goes = hotEvents.find("pop") != std::string::npos || hotEvents.find("burst") != std::string::npos ||
                    hotEvents.find("explode") != std::string::npos;

  // (2) open
  alchemy::FlaskSim s2(cfg);
  s2.SetSubstances(b.subs);
  s2.SetChemistry(b.chem);
  const int v2 = s2.AddVessel(BenchFlask(512), {{240, 4}, 0}, in);
  Composition drained2;
  for (int f = 0; f < 60 * 20; f++) {
    s2.Step(4);
    Drain(s2, drained2);
    if (f == 60 * 18) Shot(s2, "alchemy_evaporate_open.bmp");
  }
  // SANDVOX_EVAP_OPEN_SHOTS=1: the open flask run on past the gate's window
  // (after the tally below is taken from a copy of it), pictures of the
  // steady spill over the lip. Look-iteration only.
  if (std::getenv("SANDVOX_EVAP_OPEN_SHOTS")) {
    alchemy::FlaskSim s4 = s2;
    Composition d4;
    for (int f = 0; f < 60 * 30; f++) {
      s4.Step(4);
      Drain(s4, d4);
      if (f % 300 == 299) Shot(s4, Format("alchemy_evaporate_open_%02d.bmp", 20 + (f + 1) / 60).c_str());
    }
    std::printf("alchemy-evaporate: open +30 s: %u eighths out, pressure %.2f\n", d4.AmountOf(mV), s4.Pressure(v2));
  }
  const alchemy::Tally t2 = s2.Count();
  const int evap2 = s2.Evaporations(), buried2 = s2.BuriedEvaporations();
  const int64_t faded2 = s2.Consumed()[vap];
  const uint32_t vented2 = drained2.AmountOf(mV) + t2.spilled.AmountOf(mV);
  std::string why2, lwhy2;
  const bool audit2 = s2.AuditUnits(&why2), ledger2 = BenchLedgerHolds(c, s2, in, v2, drained2, lwhy2);
  const double maxBuried = BaselineNumber("alchemy.evaporateMaxBuriedFraction", 0.15);
  const bool surface = evap2 > 0 && buried2 <= maxBuried * evap2;
  const bool leaves = t2.vessel[v2].AmountOf(mE) < 200 && (faded2 > 0 || vented2 > 0);

  // (3) volume
  Composition one;
  one.Add(mE, 1);
  alchemy::FlaskSim s3(cfg);
  s3.SetSubstances(b.subs);
  s3.SetChemistry(b.chem);
  const int v3 = s3.AddVessel(BenchFlask(16), {{240, 4}, 0}, one, true);
  for (int f = 0; f < 60 * 12 && s3.ParticleCount() > 0; f++) s3.Step(4);
  for (int f = 0; f < 60 * 3; f++) s3.Step(4);
  const int liquidPx = cfg.unitsPerEighth;   // one eighth of liquid covers this many pixels
  const int vapPx = s3.GasPixelsIn(v3);
  std::string why3;
  const bool audit3 = s3.AuditUnits(&why3);
  // Volume ratio: gasExpand gas units a matter unit, gasRest a pixel at rest.
  const bool voluminous = s3.ParticleCount() == 0 && vapPx >= E / s3.GasRest() * liquidPx;

  RecordObserved("alchemy.evaporateAirPixels", air);
  RecordObserved("alchemy.evaporatePressure", p1);
  const bool ok = audit1 && ledger1 && filled && stopped && held && goes && audit2 && ledger2 && surface && leaves &&
                  audit3 && voluminous;
  detail = Format("stoppered %d s: ether 200 -> %u + vapour %u eighths, headspace %d vapour px / %d air px, "
                  "%d evaporations (%d in the last 6 s), pressure %.2f (peak %.2f, pops at %.1f)%s, audit %s, ledger %s; "
                  "heated: %s; open 20 s: ether -> %u, %d evaporations (%d with liquid above), vapour %lld units "
                  "faded, %u eighths out of the mouth, audit %s, ledger %s; volume: 1 eighth (%d px of liquid) -> "
                  "%d px of vapour (E %d)%s",
                  secs, t1.vessel[v].AmountOf(mE), t1.vessel[v].AmountOf(mV), gasPx, air, s.Evaporations(), lateEvap,
                  p1, peakP, cfg.popAt, events.empty() ? "" : (" EVENTS: " + events).c_str(),
                  audit1 ? "exact" : why1.c_str(), ledger1 ? "validates" : lwhy1.c_str(),
                  hotEvents.empty() ? "NOTHING" : hotEvents.c_str(), t2.vessel[v2].AmountOf(mE), evap2, buried2,
                  (long long)faded2, vented2, audit2 ? "exact" : why2.c_str(), ledger2 ? "validates" : lwhy2.c_str(),
                  liquidPx, vapPx, E, audit3 ? "" : (", audit " + why3).c_str());
  std::printf("alchemy-evaporate: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// A HEAVY VAPOUR STAYS IN A FLASK THAT IS ONLY MOVED (owner, 2026-09-27:
// "gases that are heavier than air are also falling out of the glass when it
// gets moved at all, and need to stay in the glass, in the case of ether").
// An open flask of ether with ether vapour over it, chemistry paused (pure
// transport: no evaporation, no fading), carried like alchemy-shake -- lifted,
// swung side to side at 1.5 Hz, set down: it loses ~0 vapour
// (`alchemy.gasCarryMaxLostUnits` gas units) and the count is exact. Then
// lifted and tipped past level: the vapour POURS out (at least
// `alchemy.gasCarryMinPouredFraction` of it within 6 s).
Status GateAlchemyGasCarry(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int ether = SlotOfName(b, c, "ether"), vap = SlotOfName(b, c, "ether_vapour");
  if (ether < 0 || vap < 0) { detail = "missing ether/ether_vapour"; return Status::Fail; }
  if (!b.subs[vap].heavy) { detail = "ether_vapour is not a heavy gas"; return Status::Fail; }
  const uint16_t mE = b.subs[ether].mat, mV = b.subs[vap].mat;
  Composition in;
  in.Add(mE, 60);
  in.Add(mV, 20);
  alchemy::FlaskSim s(ChemConfig());
  s.SetSubstances(b.subs);
  s.SetChemistry(b.chem);
  s.PauseChemistry(true);
  const int v = s.AddVessel(BenchFlask(256), {{240, 4}, 0}, in);
  const int gas0 = s.GasUnits(v);
  Composition drained;
  const int stopAt = 150;
  for (int f = 0; f < 60 * 4; f++) {
    const float t = f / 60.0f;
    const float dx = f < stopAt ? 70.0f * std::sin(6.2832f * 1.5f * t) : 0.0f;
    const float y = f < 20 ? 4.0f + 1.5f * f : f < stopAt ? 34.0f : 4.0f;
    s.SetVesselXform(v, {{240 + dx, y}, 0});
    s.Step(4);
    Drain(s, drained);
    if (f == 75) Shot(s, "alchemy_gas_carry.bmp");
  }
  const int gas1 = s.GasUnits(v);
  const alchemy::Tally t1 = s.Count();
  const uint32_t lostEighths = drained.AmountOf(mV) + t1.spilled.AmountOf(mV);
  std::string why1;
  const bool audit1 = s.AuditUnits(&why1);
  Composition both;
  both.Add(mE, 60);
  both.Add(mV, 20);
  std::string cwhy;
  alchemy::Tally tc = t1;
  for (int i = 0; i < drained.n; i++) tc.spilled.Add(drained.p[i].mat, drained.p[i].eighths);
  const bool cons = Conserved(both, tc, cwhy);
  const int maxLost = (int)BaselineNumber("alchemy.gasCarryMaxLostUnits", 16);
  const bool kept = gas0 > 0 && gas0 - gas1 <= maxLost && lostEighths == 0;
  // Tipped past level.
  for (int f = 0; f < 60 * 9; f++) {
    const float a = f < 30 ? 0.0f : std::min(2.8f, (f - 30) / 60.0f);
    s.SetVesselXform(v, {{240, 200}, a});
    s.Step(4);
    Drain(s, drained);
    if (f == 60 * 5) Shot(s, "alchemy_gas_pour.bmp");
  }
  const int gas2 = s.GasUnits(v);
  std::string why2;
  const bool audit2 = s.AuditUnits(&why2);
  const double minPoured = BaselineNumber("alchemy.gasCarryMinPouredFraction", 0.5);
  const bool pours = gas1 > 0 && (double)(gas1 - gas2) >= minPoured * gas1;
  RecordObserved("alchemy.gasCarryLostUnits", gas0 - gas1);
  const bool ok = audit1 && cons && kept && audit2 && pours;
  detail = Format("carried: vapour %d -> %d gas units inside, %u eighths out; %s, audit %s; tipped past level: "
                  "%d -> %d gas units inside (%.0f%% poured), audit %s",
                  gas0, gas1, lostEighths, cons ? "conserved" : cwhy.c_str(), audit1 ? "exact" : why1.c_str(), gas1,
                  gas2, gas1 > 0 ? 100.0 * (gas1 - gas2) / gas1 : 0.0, audit2 ? "exact" : why2.c_str());
  std::printf("alchemy-gas-carry: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// A LIGHT GAS LEAVES AN OPEN FLASK; A STOPPERED ONE KEEPS IT (owner,
// 2026-09-27: "smoke isn't leaving the flask ... as if it's been stoppered
// when it hasn't. This has been recurring for a few attempts now"). A flask
// seeded with 40 eighths of noxious gas (light), chemistry paused so only
// transport moves it (no fading by rule), on the bench's own draught
// (SimConfig::gasWind default, the tuning's tools.alchemyWind):
//   (1) OPEN, 20 s: at least `alchemy.gasVentMinFraction` of its gas leaves
//       through the mouth, the units audit exact and the ledger validating;
//   (2) OPEN IN A STILL ROOM (gasWind 0), 20 s: some still leaves on its own
//       buoyancy (`alchemy.gasVentStillMinFraction`) -- the mouth is open to
//       the solve, not a lid;
//   (3) STOPPERED, 10 s: every gas unit is still inside, nothing drained.
// Records the cost of a bench frame (Step(4)) with the gas moving.
Status GateAlchemyGasVent(Ctx& c, std::string& detail) {
  ChemBench b = MakeChemBench(c);
  const int nox = SlotOfName(b, c, "noxious_gas");
  if (nox < 0) { detail = "missing noxious_gas"; return Status::Fail; }
  if (b.subs[nox].heavy) { detail = "noxious_gas is a heavy gas (this gate wants a light one)"; return Status::Fail; }
  const uint16_t mN = b.subs[nox].mat;
  struct Run { int gas0 = 0, gas1 = 0; uint32_t out = 0; double ms = 0; bool ok = false; std::string why; };
  auto run = [&](bool stoppered, float wind, int secs) {
    Run o;
    Composition in;
    in.Add(mN, 40);
    alchemy::SimConfig cfg = ChemConfig();
    if (wind >= 0) cfg.gasWind = wind;
    alchemy::FlaskSim s(cfg);
    s.SetSubstances(b.subs);
    s.SetChemistry(b.chem);
    s.PauseChemistry(true);
    const int v = s.AddVessel(BenchFlask(512), {{240, 4}, 0}, in, stoppered);
    o.gas0 = s.GasUnits(v);
    Composition drained;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 60 * secs; f++) {
      s.Step(4);
      Drain(s, drained);
      if (!stoppered && wind < 0 && f == 60 * 5) Shot(s, "alchemy_gas_vent.bmp");
    }
    o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / (60.0 * secs);
    o.gas1 = s.GasUnits(v);
    o.out = drained.AmountOf(mN) + s.Count().spilled.AmountOf(mN);
    std::string a, l;
    const bool audit = s.AuditUnits(&a), ledger = BenchLedgerHolds(c, s, in, v, drained, l);
    o.why = std::string(audit ? "" : " audit: " + a) + (ledger ? "" : " ledger: " + l);
    o.ok = audit && ledger;
    return o;
  };
  const Run open = run(false, -1.0f, 20), still = run(false, 0.0f, 20), shut = run(true, -1.0f, 10);
  auto gone = [](const Run& r) { return r.gas0 > 0 ? (double)(r.gas0 - r.gas1) / r.gas0 : 0.0; };
  const double minOpen = BaselineNumber("alchemy.gasVentMinFraction", 0.5);
  const double minStill = BaselineNumber("alchemy.gasVentStillMinFraction", 0.1);
  const bool vents = open.gas0 > 0 && gone(open) >= minOpen && open.out > 0;
  const bool stillVents = gone(still) >= minStill && still.out > 0;
  const bool kept = shut.gas0 > 0 && shut.gas1 == shut.gas0 && shut.out == 0;
  RecordObserved("alchemy.gasVentFraction", gone(open));
  RecordObserved("alchemy.gasVentStillFraction", gone(still));
  RecordObserved("alchemy.gasVentMsPerFrame", open.ms);
  const bool ok = open.ok && still.ok && shut.ok && vents && stillVents && kept;
  detail = Format("open, draught %.2f: %d -> %d gas units in 20 s (%.0f%% gone, need %.0f%%), %u eighths out%s; "
                  "still room: %d -> %d (%.0f%% gone, need %.0f%%), %u out%s; stoppered 10 s: %d -> %d, %u out%s; "
                  "%.2f ms a bench frame",
                  ChemConfig().gasWind, open.gas0, open.gas1, 100 * gone(open), 100 * minOpen, open.out,
                  open.why.c_str(), still.gas0, still.gas1, 100 * gone(still), 100 * minStill, still.out,
                  still.why.c_str(), shut.gas0, shut.gas1, shut.out, shut.why.c_str(), open.ms);
  std::printf("alchemy-gas-vent: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
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

// SHAKEN LIQUIDS GO BACK TO THEIR LAYERS, AND POWDER ON WATER SPREADS.
// (1) A stoppered flask of water and oil is shaken and turned over and
// back, then set down. Owner report 2026-09-27: a shaken mix left drops of
// water floating in the oil and on top of it, and the flask went to sleep
// like that. Asserted: the shake really mixed it (FlaskSim::InvertedPairs,
// a heavier liquid over a lighter one, high when the hand stops); by the
// end 95% of those are gone and the mean heights are in density order; and
// it is asleep (it stays awake only while the count is still falling). The
// same shake with SimConfig::sortDrive = 0 and crossLone = 0 is REPORTED as
// the control.
// (2) Charcoal (lighter than water) poured in a thin stream onto water.
// Asserted: it spreads -- at least twice as wide as with floatSpread off
// (reported as the control, a 45-degree heap) -- and it settles asleep.
Status GateAlchemyResort(Ctx& c, std::string& detail) {
  const int water = MatId(c, "water"), oil = MatId(c, "oil"), charcoal = MatId(c, "charcoal");
  if (water < 0 || oil < 0 || charcoal < 0) { detail = "missing water/oil/charcoal"; return Status::Fail; }
  struct Shake { int invAtStop = 0, invEnd = 0, sleptF = -1; bool ordered = false, asleepEnd = false; std::string order; };
  const int stopAt = 240, frames = 60 * 30;
  auto shake = [&](bool fixed) {
    Shake r;
    Composition in;
    in.Add((uint16_t)water, 320);
    in.Add((uint16_t)oil, 320);
    auto subs = alchemy::SubstancesFor(c.mats, {&in});
    alchemy::SimConfig cfg = BenchConfig();
    if (!fixed) { cfg.sortDrive = 0; cfg.crossLone = 0; }
    FlaskSim s(cfg);
    s.SetSubstances(subs);
    const int v = s.AddVessel(BenchFlask(1024), {{240, 30}, 0}, in, true);
    for (int f = 0; f < frames; f++) {
      const float t = f / 60.0f;
      if (f < stopAt) {
        const float dx = 60.0f * std::sin(6.2832f * 2.0f * t), dy = 20.0f * std::sin(6.2832f * 3.1f * t);
        s.SetVesselXform(v, {{240 + dx, 60 + dy}, 2.6f * std::sin(6.2832f * 0.6f * t)});
      } else {
        s.SetVesselXform(v, {{240, 30}, 0});
      }
      s.Step(4);
      if (f == stopAt) r.invAtStop = s.InvertedPairs(v);
      if (f > stopAt + 60 && r.sleptF < 0 && s.VesselAsleep(v)) r.sleptF = f;
    }
    r.invEnd = s.InvertedPairs(v);
    r.asleepEnd = s.VesselAsleep(v);
    r.ordered = Ordered(s, subs, r.order);
    if (fixed) Shot(s, "alchemy_resort.bmp");
    return r;
  };
  const Shake on = shake(true), off = shake(false);
  const int minMixed = (int)BaselineNumber("alchemy.resortMinMixed", 150);
  const bool mixed = on.invAtStop >= minMixed;
  // Re-sorted: what is left out of order is the few pairs pinned at the
  // glass that UpdateSleep lets sleep (sortStall), at most
  // alchemy.resortMaxLeft -- or a twentieth of the mix, if the shake mixed
  // more. It was only the twentieth: the pinned residue is the same ~15 pairs
  // however hard the shake mixed (lab, 2026-09-27: 15 left of 539 before the
  // contents rode the vessel's frame, 15-18 of ~207 after), and a shake that
  // mixed less (vesselFeel) failed on the same residue.
  const int maxLeft = std::max((int)BaselineNumber("alchemy.resortMaxLeft", 20), on.invAtStop / 20);
  const bool sorted = on.invEnd <= maxLeft && on.ordered;
  const bool slept = on.asleepEnd;

  struct Spread { int width = 0, grains = 0; bool asleep = false; };
  auto spread = [&](bool enable) {
    Spread r;
    Composition in, extra;
    in.Add((uint16_t)water, 500);
    extra.Add((uint16_t)charcoal, 1);
    auto subs = alchemy::SubstancesFor(c.mats, {&in, &extra});
    alchemy::SimConfig cfg = BenchConfig();
    cfg.floatSpread = enable;
    FlaskSim s(cfg);
    s.SetSubstances(subs);
    const int v = s.AddVessel(BenchFlask(1024), {{240, 4}, 0}, in);
    const int slot = s.SlotOf((uint16_t)charcoal);
    const float top = 4 + s.Shape(v).height + 12;
    for (int f = 0; f < 60 * 30; f++) {
      if (f < 200) s.EmitGrains(slot, {240, top}, 2, {0, -0.5f});
      s.Step(4);
    }
    r.grains = s.GrainCount();
    r.asleep = s.VesselAsleep(v);
    if (enable) Shot(s, "alchemy_float.bmp");
    const auto b = s.GrainBox(slot);
    r.width = b[0] < 0 ? 0 : b[1] - b[0] + 1;
    return r;
  };
  const Spread fOn = spread(true), fOff = spread(false);
  const bool skin = fOn.grains > 100 && fOn.width >= 2 * fOff.width && fOn.asleep;
  RecordObserved("alchemy.resortInvertedEnd", (double)on.invEnd);
  RecordObserved("alchemy.floatSkinWidth", (double)fOn.width);
  detail = Format("shaken: %d inverted when the hand stopped (min %d), %d at the end (max %d), order %s, asleep %s | "
                  "control (no sort drive, lone drops held apart): %d, %d at the end, order %s || charcoal on "
                  "water: %d grains %d px wide, %s | control floatSpread off: %d px wide",
                  on.invAtStop, minMixed, on.invEnd, maxLeft, on.order.c_str(),
                  on.sleptF < 0 ? "never" : Format("%.1f s after", (on.sleptF - stopAt) / 60.0).c_str(),
                  off.invAtStop, off.invEnd, off.order.c_str(), fOn.grains, fOn.width,
                  fOn.asleep ? "asleep" : "AWAKE", fOff.width);
  const bool ok = mixed && sorted && slept && skin;
  std::printf("alchemy-resort: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// CONTENTS STAY COHERENT WHILE THEIR VESSEL MOVES, AND A MIXED BLOB FALLS AS
// ONE (owner, 2026-09-27: "the liquid gets stuck in the powder when moving
// upwards ... everything should take up 99% the same volume when it's moved";
// "dirt in slime poured out fell really slowly and the slime hovered on it").
//
// (1) CARRY. A flask of water over a sand bed, and one of slime over dirt,
// lifted, shaken, tipped and set down. Every 6 frames the OCCUPIED AREA is
// measured -- grain pixels together with the pixels within 1.6 px of a
// particle (a settled lattice covers its whole area at that radius, and
// liquid squeezed, torn or soaked into the bed covers less) -- against the
// same flask at rest. Asserted (tests/baseline.json alchemy.coherence*): its
// mean and worst deviation while moving, and the deviation once set down
// again. Before the contents rode the vessel's frame the lift alone
// drove the liquid into the sand (mean 11.5%, worst 27%, 175 particles in
// grain pixels); after, 1.4-1.7% and ~4%.
// (2) FREE FALL. The flask is taken out from under its contents in mid-air
// (TeleportVessel), once with slime alone and once with slime over dirt. The
// mixed blob's liquid and its grains must have fallen at least
// alchemy.fallRatioMin of what the pure slime fell in 30 frames (before: the
// slime 0.48 of it, standing on its dirt).
// (3) POURED OUT. Each flask turned over high up: the mixed one must be half
// empty within alchemy.pourOutRatioMax of the time the pure slime takes
// (before: 2.6 x, the dirt clogged the neck and held the slime on it).
Status GateAlchemyCoherence(Ctx& c, std::string& detail) {
  const int water = MatId(c, "water"), sand = MatId(c, "sand"), slime = MatId(c, "slime"), dirt = MatId(c, "dirt");
  if (water < 0 || sand < 0 || slime < 0 || dirt < 0) { detail = "missing water/sand/slime/dirt"; return Status::Fail; }
  struct Occ { int area = 0, trapped = 0; };
  auto occupied = [](const FlaskSim& s) {
    Occ o;
    const int W = s.GridW(), H = s.GridH();
    std::vector<uint8_t> m((size_t)W * H, 0);
    std::vector<V2> gp;
    s.GrainPositions(gp);
    for (V2 g : gp) {
      const int x = (int)g.x, y = (int)g.y;
      if (x >= 0 && y >= 0 && x < W && y < H) m[(size_t)y * W + x] = 2;
    }
    const float R = 1.6f;
    for (V2 p : s.Positions()) {
      const int cx = (int)std::floor(p.x), cy = (int)std::floor(p.y);
      if (cx >= 0 && cy >= 0 && cx < W && cy < H && m[(size_t)cy * W + cx] == 2) o.trapped++;
      for (int y = (int)std::floor(p.y - R); y <= (int)std::floor(p.y + R); y++)
        for (int x = (int)std::floor(p.x - R); x <= (int)std::floor(p.x + R); x++) {
          if (x < 0 || y < 0 || x >= W || y >= H) continue;
          const float dx = x + 0.5f - p.x, dy = y + 0.5f - p.y;
          if (dx * dx + dy * dy <= R * R && !m[(size_t)y * W + x]) m[(size_t)y * W + x] = 1;
        }
    }
    for (uint8_t v : m) o.area += v != 0;
    return o;
  };

  // ---- (1) carry ----
  struct Carry { double mean = 0, worst = 0, after = 0; int trapped = 0; uint32_t spilled = 0; };
  auto carry = [&](int liq, int pow, uint32_t liqE, uint32_t powE, const char* shot) {
    Carry r;
    Composition in;
    in.Add((uint16_t)liq, liqE);
    in.Add((uint16_t)pow, powE);
    auto subs = alchemy::SubstancesFor(c.mats, {&in});
    FlaskSim s(BenchConfig());
    s.SetSubstances(subs);
    const int v = s.AddVessel(BenchFlask(1024), {{240, 4}, 0}, in);
    for (int f = 0; f < 30; f++) s.Step(4);
    const Occ rest = occupied(s);
    int n = 0;
    auto smooth = [](float u) { u = std::clamp(u, 0.f, 1.f); return u * u * (3 - 2 * u); };
    for (int f = 0; f < 270; f++) {
      const float t = f / 60.0f;
      Xform x{{240, 4}, 0};
      if (t < 1.0f) x.pos.y = 4 + 150 * smooth(t);                                          // lift
      else if (t < 2.5f) x.pos = {240 + 60 * std::sin(6.2832f * 1.5f * (t - 1)), 154};     // shake
      else if (t < 3.5f) { x.pos = {240, 154}; x.angle = 0.6f * std::sin(6.2832f * (t - 2.5f)); }  // tip
      else x.pos.y = 154 - 150 * smooth(t - 3.5f);                                          // set down
      s.SetVesselXform(v, x);
      s.Step(4);
      if (f % 6) continue;
      const Occ o = occupied(s);
      const double d = std::fabs((double)o.area / rest.area - 1.0);
      r.mean += d;
      r.worst = std::max(r.worst, d);
      r.trapped = std::max(r.trapped, o.trapped);
      n++;
    }
    r.mean /= std::max(1, n);
    for (int f = 0; f < 300; f++) s.Step(4);
    r.after = std::fabs((double)occupied(s).area / rest.area - 1.0);
    r.spilled = s.Count().spilled.Total();
    Shot(s, shot);
    return r;
  };
  const Carry ws = carry(water, sand, 400, 150, "alchemy_coherence_sand.bmp");
  const Carry sd = carry(slime, dirt, 400, 100, "alchemy_coherence_dirt.bmp");
  const double meanMax = BaselineNumber("alchemy.coherenceMeanMax", 0.03);
  const double worstMax = BaselineNumber("alchemy.coherenceWorstMax", 0.08);
  const double afterMax = BaselineNumber("alchemy.coherenceAfterMax", 0.03);
  auto carryOk = [&](const Carry& r) {
    // (What a hard shake throws out of the mouth is alchemy-shake's business;
    // a real loss shows here as area missing once set down.)
    return r.mean <= meanMax && r.worst <= worstMax && r.after <= afterMax;
  };
  RecordObserved("alchemy.coherenceMeanSand", ws.mean);
  RecordObserved("alchemy.coherenceMeanDirt", sd.mean);

  // ---- (2) free fall ----
  auto fall = [&](bool withDirt, double* liqDrop, double* grainDrop) {
    Composition in;
    in.Add((uint16_t)slime, 300);
    if (withDirt) in.Add((uint16_t)dirt, 60);
    auto subs = alchemy::SubstancesFor(c.mats, {&in});
    FlaskSim s(BenchConfig());
    s.SetSubstances(subs);
    const int v = s.AddVessel(BenchFlask(512), {{120, 220}, 0}, in);
    auto means = [&](double* ly, double* gy) {
      *ly = 0;
      for (V2 p : s.Positions()) *ly += p.y;
      *ly /= std::max<size_t>(1, s.Positions().size());
      std::vector<V2> gp;
      s.GrainPositions(gp);
      *gy = 0;
      for (V2 g : gp) *gy += g.y;
      *gy /= std::max<size_t>(1, gp.size());
    };
    double l0, g0, l1, g1;
    means(&l0, &g0);
    s.TeleportVessel(v, {{400, 4}, 0});
    for (int f = 0; f < 30; f++) s.Step(4);
    means(&l1, &g1);
    *liqDrop = l0 - l1;
    *grainDrop = g0 - g1;
    if (withDirt) Shot(s, "alchemy_coherence_fall.bmp");
  };
  double pureDrop, unused, mixDrop, mixGrainDrop;
  fall(false, &pureDrop, &unused);
  fall(true, &mixDrop, &mixGrainDrop);
  const double fallMin = BaselineNumber("alchemy.fallRatioMin", 0.85);
  const double liqRatio = mixDrop / std::max(1e-6, pureDrop), grainRatio = mixGrainDrop / std::max(1e-6, pureDrop);
  const bool fell = liqRatio >= fallMin && grainRatio >= fallMin;
  RecordObserved("alchemy.fallLiquidRatio", liqRatio);

  // ---- (3) poured out ----
  auto pourOut = [&](bool withDirt) {
    Composition in;
    in.Add((uint16_t)slime, 300);
    if (withDirt) in.Add((uint16_t)dirt, 60);
    auto subs = alchemy::SubstancesFor(c.mats, {&in});
    FlaskSim s(BenchConfig());
    s.SetSubstances(subs);
    const int v = s.AddVessel(BenchFlask(512), {{200, 200}, 0}, in);
    const uint32_t total = s.Count().vessel[v].Total();
    for (int f = 0; f < 900; f++) {
      s.SetVesselXform(v, {{200, 200}, std::min(3.0f, 3.0f * f / 60.0f)});
      s.Step(4);
      if (s.Count().vessel[v].Total() * 2 <= total) return f / 60.0;
    }
    return 99.0;
  };
  const double pureHalf = pourOut(false), mixHalf = pourOut(true);
  const double pourRatio = mixHalf / std::max(1e-6, pureHalf);
  const bool poured = pourRatio <= BaselineNumber("alchemy.pourOutRatioMax", 1.5);
  RecordObserved("alchemy.pourOutRatio", pourRatio);

  auto carryText = [&](const char* what, const Carry& r) {
    return Format("%s: occupied area off by %.2f%% mean, %.2f%% worst (max %.1f%%, %.1f%%), %d particles in grain "
                  "pixels at worst, %.2f%% once set down, spilled %u",
                  what, 100 * r.mean, 100 * r.worst, 100 * meanMax, 100 * worstMax, r.trapped, 100 * r.after,
                  r.spilled);
  };
  detail = carryText("water+sand", ws) + " | " + carryText("slime+dirt", sd) +
           Format(" | free fall in 30 frames: slime alone %.1f px, with dirt its slime %.1f (%.2f) and dirt %.1f "
                  "(%.2f), min %.2f | poured out: half out in %.2f s alone, %.2f s with dirt (%.2f x, max %.2f)",
                  pureDrop, mixDrop, liqRatio, mixGrainDrop, grainRatio, fallMin, pureHalf, mixHalf, pourRatio,
                  BaselineNumber("alchemy.pourOutRatioMax", 1.5));
  const bool ok = carryOk(ws) && carryOk(sd) && fell && poured;
  std::printf("alchemy-coherence: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
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
      {"alchemy-resort", "player", {}, false, GateAlchemyResort},
      {"alchemy-spawn", "player", {}, false, GateAlchemySpawn},
      {"alchemy-sand-carry", "player", {}, false, GateAlchemySandCarry},
      {"alchemy-lift", "player", {}, false, GateAlchemyLift},
      {"alchemy-place", "player", {}, false, GateAlchemyPlace},
      {"alchemy-coherence", "player", {}, false, GateAlchemyCoherence},
      // Bench chemistry (package C): the world's rules on the bench.
      {"alchemy-react", "player", {}, false, GateAlchemyReact},
      {"alchemy-keeps", "player", {}, false, GateAlchemyKeeps},
      {"alchemy-evaporate", "player", {}, false, GateAlchemyEvaporate},
      {"alchemy-gas-carry", "player", {}, false, GateAlchemyGasCarry},
      {"alchemy-gas-vent", "player", {}, false, GateAlchemyGasVent},
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
