#include "game/alchemy_bench.h"

#include <algorithm>
#include <cmath>

#include "game/container.h"
#include "game/flasksim_mats.h"
#include "game/item.h"
#include "sim/materials.h"

namespace alchemy {

namespace {

// Every liquid and powder in the table is a bench substance, registered once
// at Open: a source brought in later with something new needs no re-register,
// and the list is a few dozen rows.
std::vector<Substance> AllSubstances(const std::vector<MaterialDef>& mats) {
  std::vector<Substance> out;
  for (size_t i = 1; i < mats.size() && out.size() < 255; i++)
    if (BenchHolds(mats[i])) out.push_back(SubstanceFromMaterial(mats[i], (uint16_t)i));
  return out;
}

bool AllOnBench(const Composition& c, const std::vector<MaterialDef>& mats) {
  for (int i = 0; i < c.n; i++)
    if (c.p[i].mat >= mats.size() || !BenchHolds(mats[c.p[i].mat])) return false;
  return true;
}

VesselShape ShapeFor(const ItemDef& def) {
  const float area = (float)std::max(1, def.container.capacity) * AlchemyBench::kUnitsPerEighth;
  // Only a powder-only vessel (a pouch) is a sack; anything that holds
  // liquid is glass.
  const bool sack = (def.container.holds & (1u << CLASS_LIQUID)) == 0;
  return ShapeWithArea(sack ? PouchShape(100, 110) : FlaskShape(110, 150), area);
}

V2 Rot(V2 v, float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {v.x * c - v.y * s, v.x * s + v.y * c};
}

const std::vector<MaterialDef>* g_mats = nullptr;   // for SourcePose's shape

}  // namespace

bool AlchemyBench::Open(KitRef ref, const ItemDef& def, const ItemInstance& inst,
                        const std::vector<MaterialDef>& mats) {
  Abandon();
  if (!def.IsContainer() || inst.count != 1 || !AllOnBench(inst.contents, mats)) return false;
  g_mats = &mats;
  SimConfig cfg;
  cfg.gridW = kGridW;
  cfg.gridH = kGridH;
  cfg.unitsPerEighth = kUnitsPerEighth;
  cfg.unitsPerParticle = kUnitsPerEighth * 2;   // a particle is two eighths
  sim_ = FlaskSim(cfg);
  sim_.SetSubstances(AllSubstances(mats));
  BenchEntry e;
  e.ref = ref;
  e.item = inst.name;
  e.capacity = def.container.capacity;
  e.before = inst.contents;
  entries_.push_back(e);
  const VesselShape shape = ShapeFor(def);
  simIndex_.push_back(sim_.AddVessel(shape, {{kGridW * 0.34f, 4.f}, 0.f}, inst.contents));
  open_ = true;
  accum_ = 0;
  pixels_.clear();
  sim_.Render(pixels_);
  return true;
}

void AlchemyBench::Abandon() {
  open_ = false;
  entries_.clear();
  simIndex_.clear();
  source_ = -1;
  tilt_ = 0;
  dragging_ = false;
  pixels_.clear();
}

bool AlchemyBench::Uses(KitRef ref) const {
  for (const BenchEntry& e : entries_)
    if (e.ref == ref) return true;
  return false;
}

bool AlchemyBench::SetSource(KitRef ref, const ItemDef& def, const ItemInstance& inst) {
  if (!open_ || !g_mats || !def.IsContainer() || inst.count != 1 ||
      !AllOnBench(inst.contents, *g_mats))
    return false;
  // One entry per stack for the whole session: a vessel taken off and put
  // back on is the same vessel, holding what it left with.
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref) return false;
  ClearSource();
  BenchEntry e;
  e.ref = ref;
  e.item = inst.name;
  e.capacity = def.container.capacity;
  e.before = inst.contents;
  const VesselShape shape = ShapeFor(def);
  // Upright on the bench to the right of the target, clear of its glass.
  const float tx = sim_.VesselXform(simIndex_[0]).pos.x + sim_.Shape(simIndex_[0]).width * 0.5f;
  const float x = std::min(kGridW - shape.width * 0.5f - 4.f, tx + shape.width * 0.5f + 12.f);
  const int vi = sim_.AddVessel(shape, {{x, 4.f}, 0.f}, inst.contents);
  entries_.push_back(e);
  simIndex_.push_back(vi);
  source_ = (int)entries_.size() - 1;
  tilt_ = 0;
  pivotLeft_ = true;
  const VesselShape& s = sim_.Shape(vi);
  const float lip = s.profile.back().x * s.width * 0.5f;
  pivot_ = {x - lip, 4.f + s.height};
  return true;
}

void AlchemyBench::ClearSource() {
  if (source_ < 0) return;
  entries_[source_].after = sim_.RemoveVessel(simIndex_[source_]);
  source_ = -1;
  tilt_ = 0;
  dragging_ = false;
}

Xform AlchemyBench::SourcePose() const {
  const VesselShape& s = sim_.Shape(simIndex_[source_]);
  const float lip = s.profile.back().x * s.width * 0.5f;
  const V2 local{pivotLeft_ ? -lip : lip, s.height};
  const V2 r = Rot(local, tilt_);
  return Xform{{pivot_.x - r.x, pivot_.y - r.y}, tilt_};
}

void AlchemyBench::Frame(float dt, const BenchInput& in, BenchTool tool) {
  if (!open_) return;

  // ---- the source: held by its pouring lip corner, tilted about it --------
  if (source_ >= 0) {
    const int vi = simIndex_[source_];
    if (tool == BenchTool::Pour) {
      if (in.pressed && in.over) {
        // Grab anywhere near the vessel; the offset keeps it from jumping
        // so the pointer can hold the body, not only the lip.
        const std::vector<V2> o = sim_.VesselOutline(vi);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (V2 p : o) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        if (in.at.x > x0 - 10 && in.at.x < x1 + 10 && in.at.y > y0 - 10 && in.at.y < y1 + 10) {
          dragging_ = true;
          grabOffset_ = {pivot_.x - in.at.x, pivot_.y - in.at.y};
        }
      }
      if (!in.down) dragging_ = false;
      if (dragging_ && in.over) {
        pivot_ = {std::clamp(in.at.x + grabOffset_.x, 4.f, kGridW - 4.f),
                  std::clamp(in.at.y + grabOffset_.y, 8.f, kGridH - 4.f)};
      }
    }
    if (in.tilt != 0) {
      // The request may lead the vessel by a little, not by a lot: a flask
      // held against another's glass does not store up a quarter turn to
      // fling itself round the moment it slides free.
      const float actual = sim_.VesselXform(vi).angle;
      const float next = std::clamp(std::clamp(tilt_ + in.tilt, actual - 0.35f, actual + 0.35f),
                                    -2.9f, 2.9f);
      // Crossing upright moves the pivot to the other lip corner, re-anchored
      // where the vessel actually is so it does not jump.
      const bool wantLeft = next > 0 || (next == 0 && pivotLeft_);
      if (wantLeft != pivotLeft_) {
        const Xform cur = sim_.VesselXform(vi);
        const VesselShape& s = sim_.Shape(vi);
        const float lip = s.profile.back().x * s.width * 0.5f;
        const V2 r = Rot({wantLeft ? -lip : lip, s.height}, cur.angle);
        pivot_ = {cur.pos.x + r.x, cur.pos.y + r.y};
        pivotLeft_ = wantLeft;
      }
      tilt_ = next;
    }
    sim_.SetVesselXform(vi, SourcePose());
  }

  // ---- the stirring stick: through the target's neck ----------------------
  bool stick = false;
  if (tool == BenchTool::Stir && in.down && in.over) {
    const int ti = simIndex_[0];
    const Xform tx = sim_.VesselXform(ti);
    const VesselShape& s = sim_.Shape(ti);
    // The pivot is the middle of the neck: the stick can lean as far as the
    // neck lets it, and no further.
    V2 neck = {tx.pos.x, tx.pos.y + s.height * 0.8f};
    V2 d{in.at.x - neck.x, in.at.y - neck.y};
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (d.y < 0 && len > 4) {
      V2 u{d.x / len, d.y / len};
      const float maxLean = 0.6f;   // radians from straight down
      const float lean = std::atan2(u.x, -u.y);
      if (std::fabs(lean) > maxLean) {
        const float a = lean > 0 ? maxLean : -maxLean;
        u = {std::sin(a), -std::cos(a)};
      }
      len = std::min(len, s.height * 0.8f - 4.f);
      const V2 bottom{neck.x + u.x * len, neck.y + u.y * len};
      const V2 top{neck.x - u.x * s.height * 0.45f, neck.y - u.y * s.height * 0.45f};
      sim_.SetStick(true, top, bottom, 2.5f);
      stick = true;
    }
  }
  if (!stick) sim_.SetStick(false);

  // ---- time: 60 steps a second, three substeps each ------------------------
  accum_ = std::min(accum_ + std::max(0.f, dt), 3.f / 60.f);
  while (accum_ >= 1.f / 60.f) {
    sim_.Step(3);
    accum_ -= 1.f / 60.f;
  }
  sim_.Render(pixels_);
}

float AlchemyBench::SourceTilt() const {
  if (!open_ || source_ < 0) return 0.0f;
  return sim_.VesselXform(simIndex_[source_]).angle;
}

V2 AlchemyBench::TargetMouth() const {
  if (!open_) return {};
  const Xform x = sim_.VesselXform(simIndex_[0]);
  return {x.pos.x, x.pos.y + sim_.Shape(simIndex_[0]).height};
}

Composition AlchemyBench::LiveTarget() const {
  if (!open_) return {};
  Tally t = sim_.Count();
  return t.vessel[simIndex_[0]];
}

Composition AlchemyBench::LiveSource() const {
  if (!open_ || source_ < 0) return {};
  Tally t = sim_.Count();
  return t.vessel[simIndex_[source_]];
}

BenchResult AlchemyBench::Finish() {
  BenchResult r;
  if (!open_) return r;
  sim_.SetStick(false);
  sim_.Settle(900);
  const Tally t = sim_.Count();
  for (size_t i = 0; i < entries_.size(); i++) {
    // A source taken off earlier already has its `after`.
    if (sim_.VesselAlive(simIndex_[i])) entries_[i].after = t.vessel[simIndex_[i]];
    r.vessels.push_back(entries_[i]);
  }
  r.spilled = t.spilled;
  Abandon();
  return r;
}

bool ValidateBench(BenchResult& r, const std::vector<MaterialDef>& mats, std::string& why) {
  // Conservation, per material.
  std::vector<int64_t> diff(mats.size() + 1, 0);
  auto acc = [&](const Composition& c, int sign) {
    for (int i = 0; i < c.n; i++)
      if (c.p[i].mat < diff.size()) diff[c.p[i].mat] += sign * (int64_t)c.p[i].eighths;
  };
  for (const BenchEntry& e : r.vessels) {
    acc(e.before, +1);
    acc(e.after, -1);
  }
  acc(r.spilled, -1);
  for (size_t m = 0; m < diff.size(); m++)
    if (diff[m] != 0) {
      why = "the bench did not conserve " + (m < mats.size() ? mats[m].name : std::string("?")) +
            " (" + std::to_string(diff[m]) + " eighths)";
      return false;
    }
  // Capacity: what does not fit runs over the lip -- the top layer first.
  for (BenchEntry& e : r.vessels) {
    while ((int)e.after.Total() > e.capacity && !e.after.Empty()) {
      ItemInstance probe;
      probe.contents = e.after;
      const uint16_t top = ContainerTopMat(probe, &mats);
      const uint32_t over = e.after.Total() - (uint32_t)e.capacity;
      const uint32_t got = e.after.Take(top, over);
      r.spilled.Add(top, got);
    }
  }
  return true;
}

}  // namespace alchemy
