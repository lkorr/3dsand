#include "game/alchemy_bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "game/container.h"
#include "game/flasksim_mats.h"
#include "game/item.h"
#include "sim/rng.h"
#include "sim/materials.h"
#include "sim/solutes.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace alchemy {

namespace {

// Everything a vessel on this table could hold or become is a substance,
// registered once at Open (flasksim_mats.h BenchSubstances): every liquid,
// powder and gas and what their rules make.
bool AllOnBench(const Composition& c, const std::vector<MaterialDef>& mats) {
  for (int i = 0; i < c.n; i++) {
    const uint16_t m = BaseMat(c.p[i].mat);
    if (m == 0 || m >= mats.size()) return false;
    // A solid in a vessel is a reaction's product that came off a bench; it
    // goes back on as grains.
  }
  return true;
}

// ---- THE EVENT REGISTRY (keyed by effect kind) -------------------------------
std::map<std::string, BenchEventHandler>& Registry() {
  static std::map<std::string, BenchEventHandler> r = [] {
    std::map<std::string, BenchEventHandler> m;
    // EXPLODE (sodium in water, and whatever else authors it): the bench is
    // no place to be. The player is thrown out of it, both vessels in their
    // hands break and burst, a REAL explosion goes off at the hands (it can
    // take them off) and fire and smoke billow round it. Size from the
    // effect, a little more for a violent one (many firings in one step).
    m["explode"] = [](const BenchEvent& e, BenchOutcome& o) {
      o.eject = true;
      o.breakHeld = true;
      BenchOutcome::Blast b;
      b.radius = std::clamp(e.radius > 0 ? e.radius + std::min(3, e.count / 6) : 4, 1, (int)kMaxExplosionRadius);
      b.power = e.power > 0 ? e.power : 60;
      // ONE blast at the hands however many firings arrived together (the
      // bench's thread may hand over several steps' worth in one frame): the
      // biggest of them.
      if (o.blasts.empty()) o.blasts.push_back(b);
      else {
        o.blasts[0].radius = std::max(o.blasts[0].radius, b.radius);
        o.blasts[0].power = std::max(o.blasts[0].power, b.power);
      }
      for (uint16_t p : e.products) o.puff.push_back(p);
      o.puffCells = std::max(o.puffCells, b.radius * 10);
      o.message = "it explodes in your hands!";
    };
    // BURST: the glass breaks. A stoppered vessel's pressure does it in the
    // sim itself (its contents are already loose on the table); a RULE that
    // authors "burst" breaks the vessel it fired in. Either way the item is
    // gone at "done".
    m["burst"] = [](const BenchEvent& e, BenchOutcome& o) {
      o.burst.push_back(e.entry);   // already broken (pressure): a no-op
      if (o.message.empty()) o.message = "the glass bursts!";
    };
    // EJECT: thrown back from the bench, nothing broken.
    m["eject"] = [](const BenchEvent&, BenchOutcome& o) {
      o.eject = true;
      if (o.message.empty()) o.message = "you recoil from the bench";
    };
    // FLASH and SHOCK: a word for now; the bench shows its own light and arc.
    m["flash"] = [](const BenchEvent&, BenchOutcome& o) {
      if (o.message.empty()) o.message = "a blinding flash!";
    };
    m["shock"] = [](const BenchEvent&, BenchOutcome& o) {
      if (o.message.empty()) o.message = "it crackles and bites your fingers";
    };
    // POP: the stopper flew out and the gas is free.
    m["pop"] = [](const BenchEvent&, BenchOutcome& o) {
      if (o.message.empty()) o.message = "pop - the stopper flies out";
    };
    return m;
  }();
  return r;
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

Xform HandGoal(const VesselShape& shape, V2 at, V2 grabLocal, float angle, int gridW, int gridH,
               float tableY) {
  const V2 r = Rot(grabLocal, angle);
  Xform g{{std::clamp(at.x - r.x, 0.0f, (float)gridW), at.y - r.y}, angle};
  // The outline's vertical extent at this angle, relative to the pose origin
  // (the base centre): the profile's corners, both sides, rotated. Plus the
  // glass, and a cork's height above the mouth (flaskchem.cpp draws it to
  // local height + 6).
  float lo = 0.0f, hi = 0.0f;
  const float hw = shape.width * 0.5f;
  for (const V2& p : shape.profile)
    for (float side : {-1.0f, 1.0f}) {
      const V2 w = Rot({side * p.x * hw, p.y * shape.height}, angle);
      lo = std::min(lo, w.y);
      hi = std::max(hi, w.y);
    }
  hi = std::max(hi, Rot({0.0f, shape.height + 8.0f}, angle).y);
  lo = std::min(lo, Rot({0.0f, shape.height + 8.0f}, angle).y);
  const float m = shape.wall + 1.0f;
  const float yMin = std::max(tableY, -lo + m);     // base on the table, no glass under the floor
  const float yMax = (float)gridH - hi - m;         // no glass (or cork) over the top
  g.pos.y = std::max(yMin, std::min(yMax, g.pos.y));
  if (yMax < yMin) g.pos.y = yMax;                  // a grid too short: the top wins (never happens at kLiftH)
  return g;
}

bool FindPlaceSpot(const FlaskSim& sim, const VesselShape& shape, int tableW, float tableY, Xform& out) {
  const float half = shape.width * 0.5f + shape.wall + 2;
  const float W = (float)tableW;
  if (2 * half > W) return false;
  for (float frac : {0.66f, 0.33f}) {
    const float want = std::clamp(W * frac, half, W - half);
    for (int k = 0; k < 2 * tableW; k++) {
      const float x = want + (float)((k + 1) / 2) * 3.0f * ((k & 1) ? 1.0f : -1.0f);
      if (x - half < 0 || x + half > W) continue;
      const Xform p{{x, tableY}, 0.0f};
      if (sim.ShapeClear(shape, p)) {
        out = p;
        return true;
      }
    }
  }
  return false;
}

void RegisterBenchEvent(const std::string& kind, BenchEventHandler h) { Registry()[kind] = std::move(h); }

bool DispatchBenchEvent(const BenchEvent& e, BenchOutcome& out) {
  auto& r = Registry();
  auto it = r.find(e.kind);
  if (it == r.end() || !it->second) return false;
  it->second(e, out);
  return true;
}

void BenchOutcomeWorldOps(const BenchOutcome& o, const Vec3& hands, uint32_t seed,
                          const std::vector<MaterialDef>& mats, std::vector<ExplosionOp>& exps,
                          std::vector<GasSpawnOp>& gas) {
  const int32_t cx = (int32_t)std::floor(hands.x), cy = (int32_t)std::floor(hands.y),
                cz = (int32_t)std::floor(hands.z);
  for (const BenchOutcome::Blast& b : o.blasts) {
    ExplosionOp e{};
    e.x = cx;
    e.y = cy;
    e.z = cz;
    e.radius = std::clamp(b.radius, 1, (int32_t)kMaxExplosionRadius);
    e.power = std::max(1, b.power);
    exps.push_back(e);
  }
  // The puff: only gases, spread round the hands in a ball a little wider
  // than the blast, one material after another.
  std::vector<uint16_t> g;
  for (uint16_t m : o.puff)
    if (m < mats.size() && mats[m].gpu.klass == CLASS_GAS && std::find(g.begin(), g.end(), m) == g.end())
      g.push_back(m);
  if (g.empty() || o.puffCells <= 0) return;
  const int n = std::min(o.puffCells, (int)kGasCpuSpawnPerTick / 2);
  const float rad = 1.5f + (o.blasts.empty() ? 2.0f : (float)o.blasts.front().radius * 0.8f);
  for (int k = 0; k < n; k++) {
    const uint32_t h = rng::Hash3(seed, (uint32_t)k, 0xB3C5u);
    auto u = [&](uint32_t salt) { return ((float)(rng::Pcg(h ^ salt) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f; };
    const float x = u(0x11u), y = u(0x22u), z = u(0x33u);
    const float l = std::sqrt(x * x + y * y + z * z);
    const float r = rad * std::cbrt(0.5f * (u(0x44u) + 1.0f)) / std::max(0.3f, l);
    gas.push_back(MakeGasSpawn((int32_t)std::floor(hands.x + x * r), (int32_t)std::floor(hands.y + std::fabs(y) * r),
                               (int32_t)std::floor(hands.z + z * r), g[(size_t)k % g.size()]));
  }
}

// ---- frame thread -------------------------------------------------------------

bool AlchemyBench::Open(int w, int h, const std::vector<MaterialDef>& mats,
                        const std::vector<ReactionGpu>& reactions,
                        const std::vector<SoluteDef>& solutes) {
  Abandon();
  w_ = std::clamp(w, kMinW, kMaxW);
  // Never shorter than the lift room: a flask can always be held upside down
  // over another, whatever the panel's box is (it draws the rest above it).
  h_ = std::clamp(std::max(h, kLiftH), 160, kMaxH);
  mats_ = &mats;
  SimConfig cfg;
  cfg.gridW = w_;
  cfg.gridH = h_;
  cfg.unitsPerEighth = kUnitsPerEighth;
  cfg.unitsPerParticle = kUnitsPerParticle;
  // The burner heats a vessel standing on the table.
  cfg.tableY = kTableY;
  // THE WORLD'S RATES, TWICE AS FAST. A bench pixel is about a hundredth of
  // a voxel, so at the world's per-contact chance a spoonful of sand takes a
  // minute to go in acid; doubled, the reaction reads while you watch.
  cfg.chemRate = 2.0f;
  cfg.gasWind = wind_ = CurrentTuning().tools.alchemyWind;
  sim_ = FlaskSim(cfg);
  const std::vector<Substance> subs = BenchSubstances(mats, reactions);
  sim_.SetSubstances(subs);
  sim_.SetChemistry(BuildBenchChemistry(mats, reactions, solutes, subs));
  entries_.clear();
  slots_.clear();
  live_.clear();
  liveOn_.clear();
  removed_.clear();
  refused_.clear();
  lateRefusal_.clear();
  cmds_.clear();
  held_ = -1;
  simFocus_ = focus_ = -1;
  ticks_ = 0;
  quit_ = false;
  front_.assign((size_t)w_ * h_, 0u);
  back_.clear();
  backReady_ = false;
  streamOut_.assign(1, std::vector<uint32_t>(mats.size(), 0u));
  streamed_.assign(mats.size(), 0u);
  vented_.assign(mats.size(), 0u);
  events_.clear();
  brokenPub_.clear();
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
  refusal_.clear();
  if (!open_ || !mats_ || !def.IsContainer() || inst.count != 1) return false;
  int ei = -1;
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref) ei = (int)i;
  if (ei >= 0 && entries_[ei].onTable) return false;
  // TWO VESSELS AT A TIME: the character works the bench with their own two
  // hands (game/session.h BenchHold), one vessel in each.
  int onTable = 0;
  for (const BenchEntry& e : entries_) onTable += e.onTable;
  if (onTable >= kMaxOnTable) {
    refusal_ = "the bench takes two vessels at a time - take one off first";
    return false;
  }
  if (ei < 0) {
    if (!AllOnBench(inst.contents, *mats_)) {
      refusal_ = "something in it cannot go on the bench";
      return false;
    }
    BenchEntry e;
    e.ref = ref;
    e.item = inst.name;
    e.capacity = def.container.capacity;
    e.before = inst.contents;
    e.after = inst.contents;
    e.layout = inst.layout;
    e.stoppered = inst.stoppered;
    entries_.push_back(e);
    ei = (int)entries_.size() - 1;
  }
  if (entries_[ei].broken) return false;
  entries_[ei].onTable = true;
  std::lock_guard<std::mutex> lk(mu_);
  Cmd c{Cmd::kPlace, ei, ShapeFor(def), entries_[ei].after};
  c.on = entries_[ei].stoppered;
  c.layout = entries_[ei].layout;
  cmds_.push_back(c);
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

void AlchemyBench::SetStopper(KitRef ref, bool on) {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref && entries_[i].onTable) {
      std::lock_guard<std::mutex> lk(mu_);
      Cmd c{Cmd::kStopper, (int)i, {}, {}};
      c.on = on;
      cmds_.push_back(c);
    }
}
void AlchemyBench::SetBurner(KitRef ref, bool on) {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref && entries_[i].onTable) {
      std::lock_guard<std::mutex> lk(mu_);
      Cmd c{Cmd::kBurner, (int)i, {}, {}};
      c.on = on;
      cmds_.push_back(c);
    }
}
void AlchemyBench::BurstEntry(int entry) {
  if (entry < 0 || entry >= (int)entries_.size() || !entries_[entry].onTable) return;
  std::lock_guard<std::mutex> lk(mu_);
  cmds_.push_back({Cmd::kBurst, entry, {}, {}});
}

void AlchemyBench::Shock(KitRef ref) {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].ref == ref && entries_[i].onTable) {
      std::lock_guard<std::mutex> lk(mu_);
      cmds_.push_back({Cmd::kShock, (int)i, {}, {}});
    }
}

bool AlchemyBench::Devices(KitRef ref, bool& stoppered, bool& burner, float& heat, float& pressure) const {
  for (size_t i = 0; i < entries_.size(); i++) {
    if (!(entries_[i].ref == ref)) continue;
    std::lock_guard<std::mutex> lk(mu_);
    if (i >= poses_.size() || !poses_[i].on) return false;
    stoppered = poses_[i].stoppered;
    burner = poses_[i].burner;
    heat = poses_[i].heat;
    pressure = poses_[i].pressure;
    return true;
  }
  return false;
}

std::vector<BenchEvent> AlchemyBench::TakeEvents() {
  std::vector<BenchEvent> out;
  if (!open_) return out;
  std::lock_guard<std::mutex> lk(mu_);
  out.swap(events_);
  for (size_t i = 0; i < brokenPub_.size() && i < entries_.size(); i++)
    if (brokenPub_[i] && !entries_[i].broken) {
      entries_[i].broken = true;
      entries_[i].onTable = false;
    }
  return out;
}

bool AlchemyBench::Contents(KitRef ref, Composition& out) const {
  for (size_t i = 0; i < entries_.size(); i++) {
    if (!(entries_[i].ref == ref)) continue;
    std::lock_guard<std::mutex> lk(mu_);
    // The tally, once one has counted it on the table -- an EMPTY one too:
    // a flask poured dry reads empty, not what it held when it came on.
    // Before its first tally it still holds what it came with.
    out = entries_[i].onTable && i < liveOn_.size() && liveOn_[i] ? live_[i] : entries_[i].after;
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
    v.stoppered = poses_[i].stoppered;
    v.burner = poses_[i].burner;
    v.heat = poses_[i].heat;
    out.push_back(v);
  }
  return out;
}

bool AlchemyBench::TakeSpill(std::vector<BenchSpill>& out, float& exitX) {
  out.clear();
  exitX = -1.0f;
  if (!open_) return false;
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t src = 0; src < streamOut_.size(); src++) {
    std::vector<uint32_t>& row = streamOut_[src];
    BenchSpill sp;
    if (src > 0 && src - 1 < entries_.size()) sp.ref = entries_[src - 1].ref;
    for (size_t m = 0; m < row.size(); m++) {
      if (!row[m]) continue;
      if (!sp.what.Add((uint16_t)m, row[m])) break;
      // Gas that left a mouth VENTED; the rest fell off the table.
      const bool gas = mats_ && m < mats_->size() && (*mats_)[m].gpu.klass == CLASS_GAS;
      if (gas && m < vented_.size()) vented_[m] += row[m];
      else if (m < streamed_.size()) streamed_[m] += row[m];
      row[m] = 0;
    }
    if (!sp.what.Empty()) out.push_back(sp);
  }
  if (out.empty()) return false;
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
  // The room's draught (tuning tools.alchemyWind), read here on the frame
  // thread -- F5 reloads the tuning on this thread -- and handed over.
  wind_ = CurrentTuning().tools.alchemyWind;
  if (backReady_) {
    front_.swap(back_);
    backReady_ = false;
    fresh_ = true;
  }
  for (auto& r : removed_)
    if (r.entry >= 0 && r.entry < (int)entries_.size()) {
      entries_[r.entry].after = r.c;
      entries_[r.entry].layout = r.layout;
      entries_[r.entry].stoppered = r.stoppered;
    }
  removed_.clear();
  for (int e : refused_)
    if (e >= 0 && e < (int)entries_.size()) {
      entries_[e].onTable = false;
      lateRefusal_ = "no room on the table for that - take a vessel off first";
    }
  refused_.clear();
}

std::string AlchemyBench::TakeLateRefusal() {
  std::string r;
  r.swap(lateRefusal_);
  return r;
}

BenchResult AlchemyBench::Finish(bool abrupt) {
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
  if (!abrupt) pending.swap(cmds_);
  cmds_.clear();
  for (Cmd& c : pending) Apply(c);
  for (auto& rm : removed_)
    if (rm.entry >= 0 && rm.entry < (int)entries_.size()) {
      entries_[rm.entry].after = rm.c;
      entries_[rm.entry].layout = rm.layout;
      entries_[rm.entry].stoppered = rm.stoppered;
    }
  removed_.clear();
  for (int e : refused_)
    if (e >= 0 && e < (int)entries_.size()) entries_[e].onTable = false;
  refused_.clear();
  sim_.SetStick(false);
  if (!abrupt) {
    // Whatever is carried stops where it is, and what is in flight lands (a
    // stream over a mouth is not a spill). Capped: this runs in one frame,
    // and a calm table exits at once.
    for (Slot& s : slots_)
      if (s.sim >= 0) sim_.TeleportVessel(s.sim, sim_.VesselXform(s.sim));
    sim_.PauseChemistry(true);
    sim_.Settle(360);
  }
  r.ejected = abrupt;
  // Count() includes what is still in the sim's spill (whole eighths and the
  // fractions): it is the rest of `spilled`, below.
  const Tally t = sim_.Count();
  for (size_t i = 0; i < entries_.size(); i++) {
    if (i < slots_.size() && slots_[i].sim >= 0) {
      entries_[i].after = t.vessel[slots_[i].sim];
      entries_[i].stoppered = sim_.Stoppered(slots_[i].sim);
      if (sim_.Broken(slots_[i].sim)) {
        entries_[i].broken = true;
        entries_[i].after = Composition{};
        entries_[i].layout.reset();
      } else {
        // Left standing on the table: its picture as it stands (the gas an
        // open one loses below is not part of the key).
        auto lay = std::make_shared<VesselLayout>(sim_.SnapshotVessel(slots_[i].sim));
        sim_.FinishLayout(*lay, entries_[i].after);
        entries_[i].layout = std::move(lay);
      }
    }
    r.vessels.push_back(entries_[i]);
  }
  // The ledger, by material id.
  r.unitsPerEighth = kUnitsPerEighth;
  r.produced.assign(mats_ ? mats_->size() : 0, 0);
  r.consumed.assign(mats_ ? mats_->size() : 0, 0);
  const std::vector<int64_t>& pr = sim_.Produced();
  const std::vector<int64_t>& co = sim_.Consumed();
  for (size_t s = 0; s < pr.size(); s++) {
    const uint16_t m = sim_.Sub((int)s).mat;
    if (m < r.produced.size()) {
      r.produced[m] += pr[s];
      r.consumed[m] += co[s];
    }
  }
  r.spilled = t.spilled;
  // AN OPEN VESSEL KEEPS NO GAS: what is in its headspace at "done" goes out
  // with the rest of the spill (a stoppered one keeps it -- ItemInstance::
  // stoppered -- and it is the only kind of vessel in the world that holds
  // a gas, which is why ContainerTopMat never has to pour one).
  if (mats_)
    for (BenchEntry& e : r.vessels) {
      if (e.stoppered || e.broken) continue;
      for (int i = e.after.n - 1; i >= 0; i--) {
        const uint16_t m = e.after.p[i].mat;
        if (IsDissolved(m) || m >= mats_->size() || (*mats_)[m].gpu.klass != CLASS_GAS) continue;
        const uint32_t got = e.after.Take(m, e.after.p[i].eighths);
        r.spilled.Add(m, got);
      }
    }
  // Anything that fell off the table and the frame never took goes out with
  // the rest at the end.
  for (const std::vector<uint32_t>& row : streamOut_)
    for (size_t m = 0; m < row.size(); m++)
      if (row[m]) r.spilled.Add((uint16_t)m, row[m]);
  streamOut_.clear();
  r.streamed = streamed_;
  r.vented = vented_;
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
    // Stood upright at the free spot nearest one of the table's two places
    // (a third and two thirds of the way across), the one not taken first.
    // NO SPOT, NO PLACING: it used to fall back to the table's right end
    // whether or not a vessel stood there, and on a small window the new
    // vial spawned inside the other one (owner, 2026-09-27). It goes back to
    // the kit untouched, with the reason (Frame -> TakeLateRefusal).
    Xform pose;
    if (!FindPlaceSpot(sim_, c.shape, w_, kTableY, pose)) {
      std::lock_guard<std::mutex> lk(mu_);
      refused_.push_back(c.entry);
      return;
    }
    // Settled before it is shown (FlaskSim::AddVessel): it arrives at rest,
    // stoppered if it was put away stoppered.
    s.sim = sim_.AddVessel(c.shape, pose, c.contents, c.on, c.layout.get());
    s.goal = pose;
  } else if (c.kind == Cmd::kRemove) {
    if (s.sim < 0) return;
    const bool stop = sim_.Stoppered(s.sim);
    // The picture first: RemoveVessel empties the glass.
    auto lay = std::make_shared<VesselLayout>(sim_.SnapshotVessel(s.sim));
    Composition out = sim_.RemoveVessel(s.sim);
    sim_.FinishLayout(*lay, out);
    s.sim = -1;
    if (held_ == c.entry) held_ = -1;
    std::lock_guard<std::mutex> lk(mu_);
    removed_.push_back({c.entry, out, stop, std::move(lay)});
  } else if (s.sim >= 0) {
    if (c.kind == Cmd::kStopper) sim_.SetStopper(s.sim, c.on);
    else if (c.kind == Cmd::kBurner) sim_.SetBurner(s.sim, c.on);
    else if (c.kind == Cmd::kShock) sim_.Shock(s.sim);
    else if (c.kind == Cmd::kBurst) sim_.Burst(s.sim);
  }
}

void AlchemyBench::Tick(const BenchInput& in, bool pressed, float tilt, BenchTool tool, float dt) {
  auto entryOfSim = [&](int sim) {
    for (size_t i = 0; i < slots_.size(); i++)
      if (slots_[i].sim == sim && sim >= 0) return (int)i;
    return -1;
  };
  const int hover = in.over ? entryOfSim(sim_.HitVessel(in.at)) : -1;

  // ---- the mouth: the hand puts the stopper in and takes it out ------------
  // Pointing at a vessel's mouth with the hand shows a ghost cork there (a
  // ring round the real one when it is stoppered); a click toggles it, and
  // does NOT pick the vessel up (owner, 2026-09-27: no trip to the stopper
  // tool for every cork). Not while a vessel is in the hand.
  const int mouth = tool == BenchTool::Hand && in.over && held_ < 0 ? MouthAt(in.at) : -1;
  if (mouth != mouthHover_) redraw_ = true;
  mouthHover_ = mouth;
  const bool corked = pressed && mouth >= 0;
  if (corked) {
    sim_.SetStopper(mouth, !sim_.Stoppered(mouth));
    redraw_ = true;
  }

  // ---- the stopper and the burner: a click on a vessel toggles it ----------
  if (pressed && hover >= 0 && slots_[hover].sim >= 0) {
    const int sv = slots_[hover].sim;
    if (tool == BenchTool::Stopper) sim_.SetStopper(sv, !sim_.Stoppered(sv));
    else if (tool == BenchTool::Burner) sim_.SetBurner(sv, !sim_.Burner(sv));
  }
  // ---- Electrify: the vessel in hand, else the one pointed at or last touched
  if (in.shock) {
    const int e = held_ >= 0 ? held_ : hover >= 0 ? hover : simFocus_;
    if (e >= 0 && e < (int)slots_.size() && slots_[e].sim >= 0) sim_.Shock(slots_[e].sim);
  }

  // ---- the hand ----------------------------------------------------------
  if (tool == BenchTool::Hand && pressed && !corked && in.over && hover >= 0) {
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
    // Turns without limit, round and round either way. The angle is kept in
    // [-pi, pi] by moving it AND the glass by the same whole turn
    // (FlaskSim::UnwindAngle), so nothing sees a jump, and a vessel let go
    // after three turns goes back upright the short way, not three turns back.
    heldAngle_ += tilt;
    constexpr float kTurn = 6.28318531f;
    if (std::fabs(heldAngle_) > kTurn * 0.5f && s.sim >= 0) {
      const float k = std::round(heldAngle_ / kTurn) * kTurn;
      heldAngle_ -= k;
      sim_.UnwindAngle(s.sim, k);
    }
    // Clamped to the GRID, which runs above the visible table (kLiftH): the
    // pointer may leave the box while the button is held (`over` stays up
    // for a drag) and the vessel follows until its glass meets the top.
    if (in.over) s.goal = HandGoal(sim_.Shape(s.sim), in.at, grabLocal_, heldAngle_, w_, h_, kTableY);
    s.goal.angle = heldAngle_;
  }

  // ---- every vessel goes to its goal ---------------------------------------
  // How it gets there is the sim's (FlaskSim::MoveVessels): an acceleration
  // limit and a braking approach, so a flick of the pointer is a firm move
  // of the glass, never a jerk that throws the liquid out. The bench used to
  // smooth the goal itself and then the sim capped the step, which was lag
  // on top of jerk.
  for (Slot& s : slots_)
    if (s.sim >= 0) sim_.SetVesselXform(s.sim, s.goal);

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
      if (sim_.Stoppered(vs)) {
        // A STOPPERED vessel: the stick cannot go in. It rests on the cork,
        // upright along the vessel's axis -- the closing segment is glass to
        // the contents, and the stick (which pushes particles, never glass)
        // must not stir through it.
        const V2 up = Rot({0.0f, 1.0f}, x.angle);
        // The cork's cap is drawn to local height + 6 (flaskchem.cpp, the
        // stopper); the stick's round end (radius 2.5) sits on it.
        const float rest = sh.height + 9.0f;
        const V2 cork{x.pos.x + up.x * rest, x.pos.y + up.y * rest};
        const V2 top{cork.x + up.x * sh.height * 0.45f, cork.y + up.y * sh.height * 0.45f};
        sim_.SetStick(true, top, cork, 2.5f);
        stick = true;
      } else if (len > 4) {
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

// The cork's footprint in a vessel's own frame, as flaskchem.cpp's Render
// draws it (step 6, "THE STOPPER"): from 4 px down the neck to 6 above the
// lip, the neck's width tapering in, a cap proud of the lip. `grow` widens it
// all round (negative shrinks), for the hover's ring and the hit margin.
static bool InCork(const VesselShape& sh, V2 l, float grow) {
  const float H0 = sh.height;
  const float mh = sh.profile.back().x * sh.width * 0.5f;
  const float top = H0 + 6.0f + grow, bot = H0 - 4.0f - grow;
  if (l.y < bot || l.y > top) return false;
  const float half = l.y < H0 + 1.0f ? mh + 0.5f - std::max(0.0f, H0 + 1.0f - l.y) * 0.25f
                                     : mh + 2.0f;
  return std::fabs(l.x) <= half + grow;
}

int AlchemyBench::MouthAt(V2 at) const {
  for (const Slot& s : slots_) {
    if (s.sim < 0 || !sim_.VesselAlive(s.sim) || sim_.Broken(s.sim)) continue;
    const Xform x = sim_.VesselXform(s.sim);
    const V2 l = Rot({at.x - x.pos.x, at.y - x.pos.y}, -x.angle);
    // A generous margin: the mouth is a few pixels wide, the pointer is not
    // that precise, and above the lip there is nothing else to point at.
    if (InCork(sim_.Shape(s.sim), l, 5.0f)) return s.sim;
  }
  return -1;
}

void AlchemyBench::PaintMouthHover(std::vector<uint32_t>& pic) const {
  const int v = mouthHover_;
  if (v < 0 || !sim_.VesselAlive(v) || pic.size() != (size_t)w_ * h_) return;
  const VesselShape& sh = sim_.Shape(v);
  const Xform x = sim_.VesselXform(v);
  const bool stoppered = sim_.Stoppered(v);
  // The bounding box of the grown cork, in picture pixels.
  const float mh = sh.profile.back().x * sh.width * 0.5f + 5.0f;
  const V2 corners[4] = {{-mh, sh.height - 8.0f}, {mh, sh.height - 8.0f},
                         {-mh, sh.height + 10.0f}, {mh, sh.height + 10.0f}};
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (const V2& c : corners) {
    const V2 w = Rot(c, x.angle);
    x0 = std::min(x0, x.pos.x + w.x); x1 = std::max(x1, x.pos.x + w.x);
    y0 = std::min(y0, x.pos.y + w.y); y1 = std::max(y1, x.pos.y + w.y);
  }
  // 0xAABBGGRR, rows top-down (flaskchem.cpp's put): `a` of (r,g,b) over it.
  auto over = [&](int px, int py, int r, int g, int b, int a) {
    uint32_t& o = pic[(size_t)(h_ - 1 - py) * w_ + px];
    const int dr = o & 255, dg = (o >> 8) & 255, db = (o >> 16) & 255, da = (o >> 24) & 255;
    const int oa = std::max(da, a);
    o = (uint32_t)(dr + (r - dr) * a / 255) | (uint32_t)(dg + (g - dg) * a / 255) << 8 |
        (uint32_t)(db + (b - db) * a / 255) << 16 | (uint32_t)oa << 24;
  };
  for (int py = std::max(0, (int)y0); py <= std::min(h_ - 1, (int)y1); py++)
    for (int px = std::max(0, (int)x0); px <= std::min(w_ - 1, (int)x1); px++) {
      const V2 l = Rot({px + 0.5f - x.pos.x, py + 0.5f - x.pos.y}, -x.angle);
      if (stoppered) {
        // Stoppered: a bright ring just outside the real cork -- "this comes out".
        if (InCork(sh, l, 1.6f) && !InCork(sh, l, 0.0f)) over(px, py, 255, 236, 190, 190);
      } else if (InCork(sh, l, 0.0f)) {
        // Open: a translucent cork where one would go, its edge brighter.
        const bool rim = !InCork(sh, l, -1.0f);
        over(px, py, 250, 232, 196, rim ? 170 : 70);
      }
    }
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
    float wind = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (quit_) break;
      cmds.swap(cmds_);
      in = input_;
      input_.shock = false;   // a press, consumed once
      pressed = pressedLatch_;
      pressedLatch_ = false;
      tilt = tiltAcc_;
      tiltAcc_ = 0;
      tool = tool_;
      wind = wind_;
    }
    sim_.SetGasWind(wind);
    const bool hadCmds = !cmds.empty();
    for (Cmd& c : cmds) Apply(c);
    cmds.clear();
    Tick(in, pressed, tilt, tool, 1.0f / 60.0f);
    sim_.Step(4);
    ticks_++;
    float exitX = -1.0f;
    FlaskSim::SpillBy fellBy;
    const Composition fell = sim_.DrainSpilled(&exitX, &fellBy);
    // The chemistry's events, in entry terms.
    std::vector<BenchEvent> evs;
    for (const SimEvent& se : sim_.TakeEvents()) {
      BenchEvent e;
      e.kind = se.kind;
      e.entry = -1;
      for (size_t i = 0; i < slots_.size(); i++)
        if (slots_[i].sim == se.vessel && se.vessel >= 0) e.entry = (int)i;
      e.at = se.at;
      e.radius = se.radius;
      e.power = se.power;
      e.amount = se.amount;
      e.what = se.what;
      e.selfMat = se.selfMat;
      e.nbrMat = se.nbrMat;
      e.products = se.products;
      e.count = se.count;
      evs.push_back(e);
    }

    // Redrawn every step while anything moves, and at 30 a second while the
    // table only LOOKS alive (glow, fizz, the light on water: FlaskSim::
    // Animated); a table with nothing on it every half second.
    const bool draw = hadCmds || redraw_ || sim_.Active() || simFocus_ != lastFocus ||
                      (sim_.Animated() ? (ticks_ & 1) == 0 : (ticks_ % 30) == 0);
    lastFocus = simFocus_;
    redraw_ = false;
    if (draw) {
      sim_.Render(pic);
      PaintMouthHover(pic);
    }
    // SANDVOX_BENCH_PROF: where a bench frame's time went, each second
    // (FlaskSim::Profile; the gate alchemy-ether-fire prints the same).
    static const bool prof = std::getenv("SANDVOX_BENCH_PROF") != nullptr;
    if (prof && (ticks_ % 60) == 0) {
      const FlaskSim::Profile& p = sim_.Prof();
      const double n = 60.0;
      std::printf("bench prof: %.2f ms a bench frame (move %.2f liquid %.2f grains %.2f gas flow %.2f move %.2f "
                  "chem %.2f render %.2f) | gas px %d particles %d grains %d fires %d\n",
                  (p.Total() + p.render) / n, p.move / n, p.liquid / n, p.grains / n, p.gasFlow / n, p.gasMove / n,
                  (p.chemPart + p.chemGrain + p.chemGas + p.chemTail) / n, p.render / n, p.peakGasPx,
                  p.peakParticles, p.peakGrains, p.peakFires);
      std::fflush(stdout);
      sim_.ResetProfile();
    }
    std::vector<Composition> live;
    std::vector<uint8_t> liveOn;
    const bool tally = hadCmds || (ticks_ % 6) == 0;
    if (tally) {
      const Tally t = sim_.Count();
      live.resize(slots_.size());
      liveOn.assign(slots_.size(), 0);
      for (size_t i = 0; i < slots_.size(); i++)
        if (slots_[i].sim >= 0) {
          live[i] = t.vessel[slots_[i].sim];
          liveOn[i] = 1;
        }
    }
    std::vector<PoseView> poses(slots_.size());
    std::vector<uint8_t> broken(slots_.size(), 0);
    for (size_t i = 0; i < slots_.size(); i++) {
      if (slots_[i].sim >= 0 && sim_.Broken(slots_[i].sim)) broken[i] = 1;
      if (slots_[i].sim >= 0 && sim_.VesselAlive(slots_[i].sim)) {
        const int sv = slots_[i].sim;
        const VesselShape& sh = sim_.Shape(sv);
        poses[i] = {sim_.VesselXform(sv), sh.width, sh.height, true};
        poses[i].stoppered = sim_.Stoppered(sv);
        poses[i].burner = sim_.Burner(sv);
        poses[i].heat = sim_.Heat(sv);
        poses[i].pressure = sim_.Stoppered(sv) && (ticks_ % 6) == 0 ? sim_.PressureFraction(sv) : 0.0f;
      }
    }
    if (held_ >= 0 && held_ < (int)broken.size() && broken[held_]) held_ = -1;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (draw) {
        back_.swap(pic);
        backReady_ = true;
      }
      if (tally) {
        live_.swap(live);
        liveOn_.swap(liveOn);
      }
      // The pressure is sampled every sixth step: keep the last between.
      for (size_t i = 0; i < poses.size() && i < poses_.size(); i++)
        if (poses[i].stoppered && (ticks_ % 6) != 0) poses[i].pressure = poses_[i].pressure;
      poses_.swap(poses);
      brokenPub_.swap(broken);
      for (BenchEvent& e : evs) events_.push_back(std::move(e));
      focus_ = simFocus_;
      heldPub_ = held_;
      if (!fell.Empty()) {
        // Filed under the ENTRY each part left (FlaskSim's vessel index ->
        // the slot holding it), [0] for no known vessel. fellBy partitions
        // `fell`, so the totals -- and ValidateBench's sum -- are unchanged.
        auto file = [&](size_t src, const Composition& c) {
          if (streamOut_.size() <= src) streamOut_.resize(src + 1);
          std::vector<uint32_t>& row = streamOut_[src];
          for (int i = 0; i < c.n; i++) {
            if (c.p[i].mat >= row.size()) row.resize(c.p[i].mat + 1, 0u);
            row[c.p[i].mat] += c.p[i].eighths;
          }
        };
        file(0, fellBy.unknown);
        for (size_t v = 0; v < fellBy.vessel.size(); v++) {
          if (fellBy.vessel[v].Empty()) continue;
          size_t src = 0;
          for (size_t i = 0; i < slots_.size(); i++)
            if (slots_[i].sim == (int)v) src = i + 1;
          file(src, fellBy.vessel[v]);
        }
        uint32_t units = 0;
        for (int i = 0; i < fell.n; i++) units += fell.p[i].eighths;
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
  // CONSERVATION, per material, with the reactions' LEDGER:
  //   before + produced - consumed == after + spilled + streamed + vented
  // A dissolved portion is its powder (contract 2.4). The ledger is in units
  // and rounds to eighths exactly the way the sim's tally rounds a
  // material's total (LedgerEighths), so a reaction that made seven units of
  // gas is the same number of eighths on both sides -- and a unit the sim
  // lost or made without recording it is not.
  std::vector<int64_t> diff(mats.size() + 1, 0);
  auto acc = [&](const Composition& c, int sign) {
    for (int i = 0; i < c.n; i++) {
      const uint16_t m = BaseMat(c.p[i].mat);
      if (m < diff.size()) diff[m] += sign * (int64_t)c.p[i].eighths;
    }
  };
  for (const BenchEntry& e : r.vessels) {
    acc(e.before, +1);
    acc(e.after, -1);
  }
  acc(r.spilled, -1);
  for (size_t m = 0; m < r.streamed.size() && m < diff.size(); m++) diff[m] -= (int64_t)r.streamed[m];
  for (size_t m = 0; m < r.vented.size() && m < diff.size(); m++) diff[m] -= (int64_t)r.vented[m];
  for (size_t m = 0; m < diff.size(); m++) {
    const int64_t net = (m < r.produced.size() ? r.produced[m] : 0) - (m < r.consumed.size() ? r.consumed[m] : 0);
    if (net) diff[m] += LedgerEighths(net, std::max(1, r.unitsPerEighth));
  }
  for (size_t m = 0; m < diff.size(); m++)
    if (diff[m] != 0) {
      why = "the bench did not conserve " + (m < mats.size() ? mats[m].name : std::string("?")) +
            " (" + std::to_string(diff[m]) + " eighths)";
      return false;
    }
  // Capacity: what does not fit runs over the lip -- the top layer first.
  // Measured by VOLUME: dissolved matter and a stoppered flask's gas take no
  // room of their own (container.h ContainerVolume).
  for (BenchEntry& e : r.vessels) {
    while ((int)ContainerVolume(e.after, mats) > e.capacity) {
      int top = -1;
      int32_t topD = 0;
      for (int i = 0; i < e.after.n; i++) {
        const uint16_t m = e.after.p[i].mat;
        if (IsDissolved(m) || m >= mats.size() || mats[m].gpu.klass == CLASS_GAS) continue;
        if (top < 0 || mats[m].gpu.density < topD) { top = i; topD = mats[m].gpu.density; }
      }
      if (top < 0) break;
      const uint16_t tm = e.after.p[top].mat;
      const uint32_t over = ContainerVolume(e.after, mats) - (uint32_t)e.capacity;
      const uint32_t got = e.after.Take(tm, over);
      r.spilled.Add(tm, got);
      if (!got) break;
    }
  }
  return true;
}

}  // namespace alchemy
