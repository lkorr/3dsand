#include "game/alchemy_bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "game/container.h"
#include "game/flasksim_mats.h"
#include "game/item.h"
#include "sim/materials.h"

namespace alchemy {

namespace {

// Every liquid and powder in the table is a bench substance, registered once
// at Open: a vessel brought in later with something new needs no
// re-register, and the list is a few dozen rows.
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
  // A powder-only vessel (a pouch) is a sack; anything that holds liquid is
  // glass.
  const bool sack = (def.container.holds & (1u << CLASS_LIQUID)) == 0;
  return ShapeWithArea(sack ? PouchShape(100, 110) : FlaskShape(110, 150), area);
}

V2 Rot(V2 v, float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {v.x * c - v.y * s, v.x * s + v.y * c};
}

}  // namespace

// ---- frame thread -------------------------------------------------------------

bool AlchemyBench::Open(int w, int h, const std::vector<MaterialDef>& mats) {
  Abandon();
  w_ = std::clamp(w, 160, kMaxW);
  h_ = std::clamp(h, 160, kMaxH);
  mats_ = &mats;
  SimConfig cfg;
  cfg.gridW = w_;
  cfg.gridH = h_;
  cfg.unitsPerEighth = kUnitsPerEighth;
  cfg.unitsPerParticle = kUnitsPerParticle;
  sim_ = FlaskSim(cfg);
  sim_.SetSubstances(AllSubstances(mats));
  entries_.clear();
  slots_.clear();
  live_.clear();
  removed_.clear();
  cmds_.clear();
  held_ = -1;
  simFocus_ = focus_ = -1;
  ticks_ = 0;
  quit_ = false;
  front_.assign((size_t)w_ * h_, 0u);
  back_.clear();
  backReady_ = false;
  streamOut_.assign(mats.size(), 0u);
  streamed_.assign(mats.size(), 0u);
  streamXSum_ = streamXW_ = 0;
  heldPub_ = -1;
  fresh_ = true;
  open_ = true;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void AlchemyBench::Abandon() {
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
    }
    thread_.join();
  }
  open_ = false;
  entries_.clear();
  slots_.clear();
  front_.clear();
}

bool AlchemyBench::Uses(KitRef ref) const {
  for (const BenchEntry& e : entries_)
    if (e.ref == ref) return true;
  return false;
}

bool AlchemyBench::OnTable(KitRef ref) const {
  for (const BenchEntry& e : entries_)
    if (e.ref == ref) return e.onTable;
  return false;
}

bool AlchemyBench::Place(KitRef ref, const ItemDef& def, const ItemInstance& inst) {
  if (!open_ || !mats_ || !def.IsContainer() || inst.count != 1) return false;
  int ei = -1;
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref) ei = (int)i;
  if (ei >= 0 && entries_[ei].onTable) return false;
  if (ei < 0) {
    if (!AllOnBench(inst.contents, *mats_)) return false;
    BenchEntry e;
    e.ref = ref;
    e.item = inst.name;
    e.capacity = def.container.capacity;
    e.before = inst.contents;
    e.after = inst.contents;
    entries_.push_back(e);
    ei = (int)entries_.size() - 1;
  }
  entries_[ei].onTable = true;
  std::lock_guard<std::mutex> lk(mu_);
  cmds_.push_back({Cmd::kPlace, ei, ShapeFor(def), entries_[ei].after});
  return true;
}

void AlchemyBench::Remove(KitRef ref) {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref && entries_[i].onTable) {
      entries_[i].onTable = false;
      std::lock_guard<std::mutex> lk(mu_);
      cmds_.push_back({Cmd::kRemove, (int)i, {}, {}});
    }
}

bool AlchemyBench::Contents(KitRef ref, Composition& out) const {
  for (size_t i = 0; i < entries_.size(); i++) {
    if (!(entries_[i].ref == ref)) continue;
    std::lock_guard<std::mutex> lk(mu_);
    out = entries_[i].onTable && i < live_.size() && live_[i].n ? live_[i] : entries_[i].after;
    return true;
  }
  return false;
}

bool AlchemyBench::PoseOf(KitRef ref, Xform& pose, float& width, float& height) const {
  for (size_t i = 0; i < entries_.size(); i++) {
    if (!(entries_[i].ref == ref)) continue;
    std::lock_guard<std::mutex> lk(mu_);
    if (i >= poses_.size() || !poses_[i].on) return false;
    pose = poses_[i].x;
    width = poses_[i].w;
    height = poses_[i].h;
    return true;
  }
  return false;
}

std::vector<BenchVesselView> AlchemyBench::Vessels() const {
  std::vector<BenchVesselView> out;
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t i = 0; i < entries_.size() && i < poses_.size(); i++) {
    if (!entries_[i].onTable || !poses_[i].on) continue;
    BenchVesselView v;
    v.ref = entries_[i].ref;
    v.pose = poses_[i].x;
    v.width = poses_[i].w;
    v.height = poses_[i].h;
    v.held = heldPub_ == (int)i;
    out.push_back(v);
  }
  return out;
}

bool AlchemyBench::TakeSpill(Composition& out, float& exitX) {
  out = Composition{};
  exitX = -1.0f;
  if (!open_) return false;
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t m = 0; m < streamOut_.size(); m++) {
    if (!streamOut_[m]) continue;
    if (!out.Add((uint16_t)m, streamOut_[m])) break;
    if (m < streamed_.size()) streamed_[m] += streamOut_[m];
    streamOut_[m] = 0;
  }
  if (out.Empty()) return false;
  exitX = streamXW_ > 0 ? (float)(streamXSum_ / streamXW_) : -1.0f;
  streamXSum_ = streamXW_ = 0;
  return true;
}

KitRef AlchemyBench::Focus() const {
  std::lock_guard<std::mutex> lk(mu_);
  return focus_ >= 0 && focus_ < (int)entries_.size() ? entries_[focus_].ref : KitRef{};
}

void AlchemyBench::Frame(const BenchInput& in, BenchTool tool) {
  if (!open_) return;
  std::lock_guard<std::mutex> lk(mu_);
  input_ = in;
  if (in.pressed) pressedLatch_ = true;
  tiltAcc_ += in.tilt;
  tool_ = tool;
  if (backReady_) {
    front_.swap(back_);
    backReady_ = false;
    fresh_ = true;
  }
  for (auto& r : removed_)
    if (r.first >= 0 && r.first < (int)entries_.size()) entries_[r.first].after = r.second;
  removed_.clear();
}

BenchResult AlchemyBench::Finish() {
  BenchResult r;
  if (!open_) return r;
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
    }
    thread_.join();
  }
  // The thread is gone: its state is ours. Anything still queued is applied.
  std::vector<Cmd> pending;
  pending.swap(cmds_);
  for (Cmd& c : pending) Apply(c);
  for (auto& rm : removed_)
    if (rm.first >= 0 && rm.first < (int)entries_.size()) entries_[rm.first].after = rm.second;
  removed_.clear();
  sim_.SetStick(false);
  // Whatever is carried stops where it is, and what is in flight lands (a
  // stream over a mouth is not a spill). Capped: this runs in one frame,
  // and a calm table exits at once.
  for (Slot& s : slots_)
    if (s.sim >= 0) sim_.SetVesselXform(s.sim, sim_.VesselXform(s.sim));
  sim_.Settle(360);
  // Count() includes what is still in the sim's spill (whole eighths and the
  // fractions): it is the rest of `spilled`, below.
  const Tally t = sim_.Count();
  for (size_t i = 0; i < entries_.size(); i++) {
    if (i < slots_.size() && slots_[i].sim >= 0) entries_[i].after = t.vessel[slots_[i].sim];
    r.vessels.push_back(entries_[i]);
  }
  r.spilled = t.spilled;
  // Anything that fell off the table and the frame never took goes out with
  // the rest at the end.
  for (size_t m = 0; m < streamOut_.size(); m++)
    if (streamOut_[m]) r.spilled.Add((uint16_t)m, streamOut_[m]);
  streamOut_.clear();
  r.streamed = streamed_;
  open_ = false;
  entries_.clear();
  slots_.clear();
  front_.clear();
  return r;
}

// ---- sim thread ---------------------------------------------------------------

bool AlchemyBench::FreeSpot(int entry, float nearX, Xform& out) const {
  // Upright on the table, nearest `nearX` first, clear of every other glass
  // and inside the table.
  const int self = entry < (int)slots_.size() ? slots_[entry].sim : -1;
  if (self < 0) return false;
  const VesselShape& shape = sim_.Shape(self);
  const float half = shape.width * 0.5f + shape.wall + 2;
  for (int k = 0; k < 2 * w_; k++) {
    const float off = (float)((k + 1) / 2) * 3.0f * ((k & 1) ? 1.0f : -1.0f);
    const float x = nearX + off;
    if (x - half < 0 || x + half > (float)w_) continue;
    const Xform p{{x, kTableY}, 0.0f};
    if (sim_.PoseClear(self, p)) {
      out = p;
      return true;
    }
  }
  return false;
}

void AlchemyBench::Apply(Cmd& c) {
  if ((int)slots_.size() <= c.entry) slots_.resize(c.entry + 1);
  Slot& s = slots_[c.entry];
  if (c.kind == Cmd::kPlace) {
    if (s.sim >= 0) return;
    // Stood at the first spot clear of the others' glass, from the right.
    const float half = c.shape.width * 0.5f + c.shape.wall + 2;
    Xform pose{{(float)w_ - half - 4, kTableY}, 0.0f};
    for (int k = 0; k < 4 * w_; k++) {
      const float x = (float)w_ - half - 4 - (float)k * 3.0f;
      if (x - half < 0) break;
      const Xform p{{x, kTableY}, 0.0f};
      if (sim_.ShapeClear(c.shape, p)) {
        pose = p;
        break;
      }
    }
    s.sim = sim_.AddVessel(c.shape, pose, c.contents);
    s.cmd = s.goal = pose;
  } else {
    if (s.sim < 0) return;
    Composition out = sim_.RemoveVessel(s.sim);
    s.sim = -1;
    if (held_ == c.entry) held_ = -1;
    std::lock_guard<std::mutex> lk(mu_);
    removed_.push_back({c.entry, out});
  }
}

void AlchemyBench::Tick(const BenchInput& in, bool pressed, float tilt, BenchTool tool, float dt) {
  auto entryOfSim = [&](int sim) {
    for (size_t i = 0; i < slots_.size(); i++)
      if (slots_[i].sim == sim && sim >= 0) return (int)i;
    return -1;
  };
  const int hover = in.over ? entryOfSim(sim_.HitVessel(in.at)) : -1;

  // ---- the hand ----------------------------------------------------------
  if (tool == BenchTool::Hand && pressed && in.over && hover >= 0) {
    held_ = hover;
    const Xform pose = sim_.VesselXform(slots_[held_].sim);
    grabLocal_ = Rot({in.at.x - pose.pos.x, in.at.y - pose.pos.y}, -pose.angle);
    heldAngle_ = pose.angle;
  }
  if (held_ >= 0 && (tool != BenchTool::Hand || !in.down || slots_[held_].sim < 0)) {
    // Let go: it is set down upright on the table, in a free spot nearest
    // where it was let go.
    Slot& s = slots_[held_];
    if (s.sim >= 0) {
      Xform rest;
      const float x = sim_.VesselXform(s.sim).pos.x;
      s.goal = FreeSpot(held_, x, rest) ? rest : Xform{{x, kTableY}, 0.0f};
    }
    held_ = -1;
  }
  if (held_ >= 0) {
    Slot& s = slots_[held_];
    heldAngle_ = std::clamp(heldAngle_ + tilt, -2.9f, 2.9f);
    if (in.over) {
      const V2 r = Rot(grabLocal_, heldAngle_);
      s.goal.pos = {std::clamp(in.at.x - r.x, 0.0f, (float)w_),
                    std::clamp(in.at.y - r.y, kTableY, (float)h_)};
    }
    s.goal.angle = heldAngle_;
  }

  // ---- every vessel follows its goal SMOOTHLY -----------------------------
  // An exponential approach (fast when far, gentle as it arrives), and never
  // more than a little ahead of where the glass actually is: a vessel held
  // against another does not wind up a lead it then releases in one jerk.
  // The jerk is what threw liquid out of a flask that was only picked up.
  const float aPos = 1.0f - std::exp(-dt * 9.0f), aAng = 1.0f - std::exp(-dt * 7.0f);
  for (Slot& s : slots_) {
    if (s.sim < 0) continue;
    const Xform actual = sim_.VesselXform(s.sim);
    s.cmd.pos.x += (s.goal.pos.x - s.cmd.pos.x) * aPos;
    s.cmd.pos.y += (s.goal.pos.y - s.cmd.pos.y) * aPos;
    s.cmd.angle += (s.goal.angle - s.cmd.angle) * aAng;
    const V2 lead{s.cmd.pos.x - actual.pos.x, s.cmd.pos.y - actual.pos.y};
    const float ll = std::sqrt(lead.x * lead.x + lead.y * lead.y);
    if (ll > 10.0f) s.cmd.pos = {actual.pos.x + lead.x * 10.0f / ll, actual.pos.y + lead.y * 10.0f / ll};
    s.cmd.angle = std::clamp(s.cmd.angle, actual.angle - 0.3f, actual.angle + 0.3f);
    sim_.SetVesselXform(s.sim, s.cmd);
  }

  // ---- the stick ------------------------------------------------------------
  // Its tip follows the pointer inside whichever vessel the pointer is in,
  // entering through that vessel's neck and leaning no further than the
  // neck lets it.
  bool stick = false;
  if (tool == BenchTool::Stick && in.down && in.over) {
    const int vs = sim_.HitVessel(in.at, 0.0f);
    if (vs >= 0) {
      const Xform x = sim_.VesselXform(vs);
      const VesselShape& sh = sim_.Shape(vs);
      const V2 nr = Rot({0.0f, sh.height * 0.8f}, x.angle);
      const V2 neck{x.pos.x + nr.x, x.pos.y + nr.y};
      const V2 d{in.at.x - neck.x, in.at.y - neck.y};
      float len = std::sqrt(d.x * d.x + d.y * d.y);
      if (len > 4) {
        // In the vessel's own frame, where "down the neck" is -y.
        V2 u = Rot({d.x / len, d.y / len}, -x.angle);
        const float maxLean = 0.6f;
        const float lean = std::atan2(u.x, -u.y);
        if (std::fabs(lean) > maxLean) {
          const float a = lean > 0 ? maxLean : -maxLean;
          u = {std::sin(a), -std::cos(a)};
        }
        u = Rot(u, x.angle);
        len = std::min(len, sh.height * 0.8f - 4.0f);
        const V2 bottom{neck.x + u.x * len, neck.y + u.y * len};
        const V2 top{neck.x - u.x * sh.height * 0.45f, neck.y - u.y * sh.height * 0.45f};
        sim_.SetStick(true, top, bottom, 2.5f);
        stick = true;
      }
    }
  }
  if (!stick) sim_.SetStick(false);

  simFocus_ = held_ >= 0 ? held_ : hover >= 0 ? hover : simFocus_;
  if (simFocus_ >= 0 && (simFocus_ >= (int)slots_.size() || slots_[simFocus_].sim < 0)) simFocus_ = -1;
  sim_.SetHighlight(held_ >= 0 ? slots_[held_].sim : hover >= 0 ? slots_[hover].sim : -1);
}

void AlchemyBench::Run() {
  using clock = std::chrono::steady_clock;
  const auto period = std::chrono::microseconds(16667);
  auto next = clock::now();
  std::vector<uint32_t> pic;
  std::vector<Cmd> cmds;
  int lastFocus = -2;
  while (true) {
    BenchInput in;
    bool pressed = false;
    float tilt = 0;
    BenchTool tool = BenchTool::Hand;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (quit_) break;
      cmds.swap(cmds_);
      in = input_;
      pressed = pressedLatch_;
      pressedLatch_ = false;
      tilt = tiltAcc_;
      tiltAcc_ = 0;
      tool = tool_;
    }
    const bool hadCmds = !cmds.empty();
    for (Cmd& c : cmds) Apply(c);
    cmds.clear();
    Tick(in, pressed, tilt, tool, 1.0f / 60.0f);
    sim_.Step(4);
    ticks_++;
    float exitX = -1.0f;
    const Composition fell = sim_.DrainSpilled(&exitX);

    const bool draw = hadCmds || sim_.Active() || simFocus_ != lastFocus || (ticks_ % 30) == 0;
    lastFocus = simFocus_;
    if (draw) sim_.Render(pic);
    std::vector<Composition> live;
    const bool tally = hadCmds || (ticks_ % 6) == 0;
    if (tally) {
      const Tally t = sim_.Count();
      live.resize(slots_.size());
      for (size_t i = 0; i < slots_.size(); i++)
        if (slots_[i].sim >= 0) live[i] = t.vessel[slots_[i].sim];
    }
    std::vector<PoseView> poses(slots_.size());
    for (size_t i = 0; i < slots_.size(); i++)
      if (slots_[i].sim >= 0) {
        const VesselShape& sh = sim_.Shape(slots_[i].sim);
        poses[i] = {sim_.VesselXform(slots_[i].sim), sh.width, sh.height, true};
      }
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (draw) {
        back_.swap(pic);
        backReady_ = true;
      }
      if (tally) live_.swap(live);
      poses_.swap(poses);
      focus_ = simFocus_;
      heldPub_ = held_;
      if (!fell.Empty()) {
        uint32_t units = 0;
        for (int i = 0; i < fell.n; i++) {
          if (fell.p[i].mat >= streamOut_.size()) streamOut_.resize(fell.p[i].mat + 1, 0u);
          streamOut_[fell.p[i].mat] += fell.p[i].eighths;
          units += fell.p[i].eighths;
        }
        if (exitX >= 0) {
          streamXSum_ += (double)exitX * units;
          streamXW_ += units;
        }
      }
    }
    next += period;
    const auto now = clock::now();
    if (now > next + std::chrono::milliseconds(50)) next = now;   // never spiral
    std::this_thread::sleep_until(next);
  }
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
  for (size_t m = 0; m < r.streamed.size() && m < diff.size(); m++)
    diff[m] -= (int64_t)r.streamed[m];
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
