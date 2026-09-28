#include "game/bodyreg.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

// The walk order — debris, then the avatar's parts, then mob limbs — appears
// exactly three times in this file and nowhere else in the codebase.
//
// THE AVATAR BEFORE THE MOBS (2026-09-24, PLAN_corpse_is_a_mob.md). Every walk
// gives up at kMaxBodySlots, so whoever is walked LAST is who a crowded frame
// starves. Corpses are Mobs now and stay rigs for as long as the dead cap lets
// them, so the mob walk is the one that grows; the player's own body must not
// be the one to vanish when it does. Each system's
// Append* takes the base its slots start at; the bases are derived here from
// the same counts every time, so the three arrays cannot disagree.
//
// THE BASE IS WHAT THE WALK EMITS, NOT WHAT THE SYSTEM CONTAINS. Until
// 2026-09-16 the mob base was `debris_.BodyCount()` while all three debris
// walks stop at `kMaxBodies` — and `DebrisSystem::AdoptBody` takes no cap, so
// `bodies_` runs past that ceiling every time a corpse hands over fifteen
// limbs at once and stays there until the next PostStep cull. In that window
// the base overstates the transforms debris actually wrote, so every mob limb
// and every avatar part declared a slot belonging to a DIFFERENT body: your
// arm drawn at a corpse's head, a zombie's torso drawn where your foot is.
// `SlotCount()` is the emitted count and is the only thing a base may be built
// from. See the audit below, which fails loudly if the two ever part again.

void BodyRegistry::BuildXforms(std::vector<BodyXformGpu>& out) const {
  debris_.BuildXforms(out);  // clears + fills [0, SlotCount)
  if (avatar_) avatar_->AppendXforms(out);
  mobs_.AppendXforms(out);
}

void BodyRegistry::BuildHandles(std::vector<uint64_t>& out) const {
  out.clear();
  const uint32_t d = debris_.SlotCount();
  for (uint32_t s = 0; s < d; s++) out.push_back(debris_.BodyHandle(s));
  if (avatar_) avatar_->AppendBodyHandles(out);
  mobs_.AppendBodyHandles(out);
}

void BodyRegistry::BuildInstances(std::vector<BodyVoxInst>& out) {
  debris_.BuildInstances(out);  // clears + fills, slots [0, SlotCount)
  const uint32_t avatarBase = debris_.SlotCount();
  const uint32_t mobBase =
      avatar_ ? avatar_->AppendInstances(out, avatarBase) : avatarBase;
  mobs_.AppendInstances(out, mobBase);
}

void BodyRegistry::BuildMicroInsts(std::vector<MicroBodyInstGpu>& out) const {
  out.clear();
  debris_.AppendMicroInsts(out);
  const uint32_t debrisEnd = (uint32_t)out.size();
  const uint32_t avatarBase = debris_.SlotCount();
  // The mobs' base is where the AVATAR WALK ACTUALLY STOPPED, not
  // `avatarBase + LimbBodyCount()`: the walk gives up at kMaxBodySlots and the
  // count does not, so on a crowded frame the two differ and the mob limbs
  // would be laid over somebody else's transforms.
  const uint32_t mobBase =
      avatar_ ? avatar_->AppendMicroInsts(out, avatarBase) : avatarBase;
  const uint32_t avatarEnd = (uint32_t)out.size();
  mobs_.AppendMicroInsts(out, mobBase);

  // ---- ALIASING DIAGNOSTIC --------------------------------------------------
  // Two micro instances sharing the same OWNED model draw from one brick:
  // the first edit overwrites the other's art, and the first free zeroes
  // its dims — an invisible limb, or a limb wearing another body's shape.
  // That is the failure MicroBodyClone was written to prevent (see its
  // header). Catching it HERE, in the compacted draw list, names the TWO
  // bodies involved and which system each one came from.
  //
  // Both checks below run every frame, so they sort (key, inst) pairs in a
  // reused scratch vector rather than building a hash map and a hash set per
  // frame (at most kMaxBodySlots entries: a sort is a few microseconds and
  // allocates nothing once the scratch has grown).
  static thread_local std::vector<std::pair<uint32_t, uint32_t>> pairs;
  if (microSet_) {
    auto region = [&](uint32_t idx) -> const char* {
      if (idx < debrisEnd) return "debris";
      if (idx < avatarEnd) return "avatar";
      return "mob";
    };
    pairs.clear();
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
      const uint32_t m = out[i].model;
      if (m == kMicroBodyNoModel) continue;
      if (m >= microSet_->owned.size() || !microSet_->owned[m]) continue;
      pairs.push_back({m, i});
    }
    std::sort(pairs.begin(), pairs.end());
    for (size_t k = 1; k < pairs.size(); k++) {
      if (pairs[k].first != pairs[k - 1].first) continue;
      // The FIRST drawer of this model is the head of its run.
      size_t h = k - 1;
      while (h > 0 && pairs[h - 1].first == pairs[k].first) h--;
      const uint32_t a = pairs[h].second, i = pairs[k].second;
      Report("MICRO MODEL ALIAS: owned model %u drawn by inst %u "
             "(%s, slot %u) AND inst %u (%s, slot %u)",
             pairs[k].first, a, region(a), out[a].slot, i, region(i),
             out[i].slot);
    }
  }

  // ---- THE SLOT SPACE ITSELF ------------------------------------------------
  // Model aliasing is one of two ways a limb can wear another body's shape;
  // this is the other, and no amount of looking at the model table finds it.
  // An instance is (slot, model) and the shader draws `model` at
  // `bodyXforms[slot]`, so a slot that belongs to somebody else swaps the two
  // bodies on screen just as visibly — and a base computed from a count rather
  // than from a walk is how that happens.
  {
    const uint32_t total = TotalSlots();
    pairs.clear();
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
      const uint32_t s = out[i].slot;
      if (s >= total || s >= kMaxBodySlots)
        Report("MICRO SLOT OUT OF RANGE: inst %u claims slot %u of %u "
               "(ceiling %u) — it will march a transform nobody wrote",
               i, s, total, kMaxBodySlots);
      pairs.push_back({s, i});
    }
    std::sort(pairs.begin(), pairs.end());
    for (size_t k = 1; k < pairs.size(); k++) {
      if (pairs[k].first != pairs[k - 1].first) continue;
      const uint32_t a = pairs[k - 1].second, i = pairs[k].second;
      Report("MICRO SLOT COLLISION: two instances claim slot %u "
             "(inst %u model %u, and inst %u model %u) — one of these two "
             "bodies is drawn at the other's transform",
             pairs[k].first, a, out[a].model, i, out[i].model);
    }
  }
}

// ---- the full holder audit --------------------------------------------------
//
// The draw-list check above can only see holders that DRAW, and the dangerous
// holder usually does not: a limb keeps `microModel` after a sever hands the
// brick to DebrisSystem, a first-person-suppressed avatar part emits no
// instance, a body past kMaxBodies is a holder the walks skip. Any of those,
// freeing later, hands a live creature's record back to the pool — and the
// next carve anywhere in the world gets it. So this walks every system's FULL
// holder list (sim/microbody.h MicroHolder) and reports in words.
uint32_t BodyRegistry::AuditMicroModels() const {
  if (!microSet_) return 0;
  // Phase 1 is allocation-free and runs on every call: collect the indices,
  // look for a repeat or a record that has been zeroed under its holder. Only
  // a hit pays for phase 2, which builds the descriptions. An audit that cost
  // three hundred std::strings a frame would not be left switched on, and one
  // that is not switched on catches nothing.
  scratch_.clear();
  bool suspect = false;
  auto note = [&](uint32_t m) {
    if (m == kMicroBodyNoModel || m >= microSet_->models.size()) return;
    // A record whose dims are zero has been FREED while this holder still
    // points at it (MicroBodyFree zeroes dims); the holder draws nothing and
    // the record is already back on freeModels for the next body to take.
    if ((microSet_->models[m].dims & kMicroBodyDimsMask) == 0) suspect = true;
    scratch_.push_back(m);
  };
  for (uint32_t i = 0; i < debris_.BodyCount(); i++)
    note(debris_.BodyMicroModel(i));
  if (!suspect) {
    // Mob/avatar holders as bare indices (Mob::AppendMicroModelIds): phase 1
    // used to take their full descriptions — an snprintf and a std::string
    // per limb per frame — only to read the model index back out of them.
    ids_.clear();
    mobs_.AppendMicroModelIds(ids_);
    if (avatar_) avatar_->AppendMicroModelIds(ids_);
    for (uint32_t m : ids_) note(m);
  }
  if (!suspect) {
    std::sort(scratch_.begin(), scratch_.end());
    for (size_t i = 1; i < scratch_.size(); i++)
      if (scratch_[i] == scratch_[i - 1] && scratch_[i] < microSet_->owned.size() &&
          microSet_->owned[scratch_[i]]) {
        suspect = true;
        break;
      }
  }
  if (!suspect) return 0;

  // Phase 2: name everybody.
  holders_.clear();
  debris_.AppendMicroHolders(holders_);
  mobs_.AppendMicroHolders(holders_);
  if (avatar_) avatar_->AppendMicroHolders(holders_);

  uint32_t faults = 0;
  std::unordered_map<uint32_t, uint32_t> first;
  first.reserve(holders_.size());
  for (uint32_t i = 0; i < (uint32_t)holders_.size(); i++) {
    const uint32_t m = holders_[i].model;
    if (m == kMicroBodyNoModel) continue;
    if (m >= microSet_->models.size()) {
      Report("MICRO HOLDER OUT OF RANGE: %s holds model %u of %zu",
             holders_[i].what.c_str(), m, microSet_->models.size());
      faults++;
      continue;
    }
    if ((microSet_->models[m].dims & kMicroBodyDimsMask) == 0) {
      // Freed under its holder. Whoever takes this record next inherits a
      // second, invisible owner that will free it a second time.
      Report("MICRO RECORD FREED UNDER ITS HOLDER: model %u (owned=%u) is held "
             "by %s but its dims are zero",
             m, m < microSet_->owned.size() ? microSet_->owned[m] : 0u,
             holders_[i].what.c_str());
      faults++;
    }
    auto it = first.find(m);
    if (it == first.end()) {
      first[m] = i;
      continue;
    }
    if (m < microSet_->owned.size() && microSet_->owned[m]) {
      Report("MICRO MODEL SHARED BY TWO HOLDERS: owned model %u held by %s AND "
             "%s — one of them is about to go invisible and the record is about "
             "to be handed to a third body",
             m, holders_[it->second].what.c_str(), holders_[i].what.c_str());
      faults++;
    }
  }
  return faults;
}

uint32_t BodyRegistry::MicroHolderCount() const {
  holders_.clear();
  debris_.AppendMicroHolders(holders_);
  mobs_.AppendMicroHolders(holders_);
  if (avatar_) avatar_->AppendMicroHolders(holders_);
  uint32_t n = 0;
  for (const MicroHolder& h : holders_)
    if (h.model != kMicroBodyNoModel) n++;
  return n;
}

void BodyRegistry::ReleaseMicroHolders() {
  // DEBRIS LAST. The other two can hand a body to it on the way out (a sever
  // in flight, a dropped shell), and a body adopted after the sweep would be a
  // holder that outlived the table by one line — the same off-by-one-event
  // that made this bug survive the sever-path fix in the first place.
  mobs_.Reset();
  if (avatar_) avatar_->Despawn();
  debris_.Reset();
}

void BodyRegistry::Report(const char* fmt, ...) const {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  // ONE LINE PER DISTINCT FAULT, not one per frame. A body-swap persists for
  // as long as the two holders live, so an unthrottled report is thousands of
  // identical lines a second — which is the same as no report at all, and is
  // why the first version of this diagnostic went unread.
  static std::unordered_set<std::string> said;
  const std::string key(msg);
  if (!said.insert(key).second) return;
  std::fprintf(stderr, "*** %s\n", msg);
  // ...and to a file, because the report that matters is made while somebody
  // is PLAYING, and a game launched from the tuner or from Explorer has no
  // stderr anybody will ever read.
  if (std::FILE* f = std::fopen("build/microbody_audit.log", "a")) {
    std::fprintf(f, "%s\n", msg);
    std::fclose(f);
  }
}

// ---- BodyInstanceArena ------------------------------------------------------

void BodyInstanceArena::Reset() {
  mirror_.clear();
  spans_.clear();
  dirty_.clear();
  ranges_.clear();
  end_ = 0;
  dropped_ = 0;
}

void BodyInstanceArena::MarkDirty(uint32_t lo, uint32_t hi) {
  if (hi > lo) dirty_.push_back({lo, hi});
}

bool BodyInstanceArena::CommitPass(const std::vector<BodyVoxInst>& inst,
                                   const std::vector<uint64_t>& handles,
                                   bool repacking) {
  ranges_.clear();
  dropped_ = 0;
  const uint32_t n = (uint32_t)inst.size();
  uint32_t i = 0;
  while (i < n) {
    // Bits 16..27 are the slot (debris.wgsl BodyVoxInst); 28..31 the art.
    const uint32_t slot = (inst[i].packed >> 16) & 0xFFFu;
    uint32_t j = i + 1;
    while (j < n && ((inst[j].packed >> 16) & 0xFFFu) == slot) j++;
    const uint32_t cnt = j - i;
    uint64_t key = slot < handles.size() && handles[slot]
                       ? handles[slot]
                       : ((1ull << 63) | slot);
    auto it = spans_.find(key);
    // A slot whose run is split in two (no walk does that today) must not
    // share one span with itself: the second run takes a salted key.
    if (it != spans_.end() && it->second.lastSeen == commit_) {
      key ^= 0x4000000000000000ull + (uint64_t)i;
      it = spans_.find(key);
    }
    if (it != spans_.end() && it->second.cap < cnt) {
      spans_.erase(it);  // outgrew its slack: the old span becomes a hole
      it = spans_.end();
    }
    bool fresh = false;
    if (it == spans_.end()) {
      uint32_t cap = cnt + cnt / 8 + 8;
      if (end_ + cap > kMaxBodyVoxInstances) {
        if (!repacking) return false;  // Commit repacks and comes back
        cap = cnt;
        if (end_ + cap > kMaxBodyVoxInstances) {
          dropped_ += cnt;
          i = j;
          continue;
        }
      }
      Span s;
      s.off = end_;
      s.cap = cap;
      end_ += cap;
      if (mirror_.size() < end_) mirror_.resize(end_);
      it = spans_.emplace(key, s).first;
      fresh = true;
    }
    Span& sp = it->second;
    BodyVoxInst* dst = mirror_.data() + sp.off;
    const BodyVoxInst* src = inst.data() + i;
    bool changed = false;
    if (fresh || sp.count != cnt) {
      std::memcpy(dst, src, (size_t)cnt * sizeof(BodyVoxInst));
      MarkDirty(sp.off, sp.off + cnt);
      changed = true;
    } else if (std::memcmp(dst, src, (size_t)cnt * sizeof(BodyVoxInst)) != 0) {
      // (The bulk compare above is the common case — an untouched body — at
      // memcmp speed; the per-instance walk only runs on a body that moved.)
      // Only the stretch that differs: a burning body's changed payloads.
      uint32_t a = 0;
      while (a < cnt && std::memcmp(dst + a, src + a, sizeof(BodyVoxInst)) == 0) a++;
      uint32_t b = cnt;
      while (b > a && std::memcmp(dst + b - 1, src + b - 1, sizeof(BodyVoxInst)) == 0) b--;
      std::memcpy(dst + a, src + a, (size_t)(b - a) * sizeof(BodyVoxInst));
      MarkDirty(sp.off + a, sp.off + b);
      changed = true;
    }
    sp.count = cnt;
    sp.lastSeen = commit_;
    if (changed) {
      // The local AABB, recomputed only when the run's content changed.
      float lo[3] = {src[0].lx, src[0].ly, src[0].lz};
      float hi[3] = {lo[0], lo[1], lo[2]};
      for (uint32_t k = 1; k < cnt; k++) {
        const float p[3] = {src[k].lx, src[k].ly, src[k].lz};
        for (int ax = 0; ax < 3; ax++) {
          lo[ax] = std::min(lo[ax], p[ax]);
          hi[ax] = std::max(hi[ax], p[ax]);
        }
      }
      for (int ax = 0; ax < 3; ax++) {
        sp.lo[ax] = lo[ax];
        sp.hi[ax] = hi[ax] + 1.0f;  // an instance is its min corner; the cube is 1 wide
      }
    }
    BodyDrawRange r;
    r.first = sp.off;
    r.count = cnt;
    r.slot = slot;
    for (int ax = 0; ax < 3; ax++) {
      r.lo[ax] = sp.lo[ax];
      r.hi[ax] = sp.hi[ax];
    }
    ranges_.push_back(r);
    i = j;
  }
  return true;
}

void BodyInstanceArena::Commit(const std::vector<BodyVoxInst>& inst,
                               const std::vector<uint64_t>& handles) {
  commit_++;
  bool ok = CommitPass(inst, handles, /*repacking=*/false);
  if (ok) {
    // Retire spans nobody has drawn for a while; their room becomes a hole.
    uint64_t live = 0;
    for (auto it = spans_.begin(); it != spans_.end();) {
      if (commit_ - it->second.lastSeen > kKeepCommits) {
        it = spans_.erase(it);
      } else {
        live += it->second.cap;
        ++it;
      }
    }
    // Holes past the live data (plus 1 MiB of headroom): repack.
    ok = (uint64_t)end_ <= 2 * live + 65536;
  }
  if (!ok) {
    spans_.clear();
    dirty_.clear();
    end_ = 0;
    CommitPass(inst, handles, /*repacking=*/true);
    dirty_.clear();
    MarkDirty(0, end_);
    repacks_++;
  }
  if (dropped_ != lastDropped_) {
    // THE OVERFLOW USED TO BE SILENT: the list stopped at kMaxBodyVoxInstances
    // and whichever bodies came last in the walk were simply not drawn.
    if (dropped_)
      std::fprintf(stderr,
                   "*** body instances: %u cube instances NOT DRAWN — the "
                   "buffer holds %u (kMaxBodyVoxInstances) and the bodies "
                   "want more\n",
                   dropped_, kMaxBodyVoxInstances);
    lastDropped_ = dropped_;
  }
}

void BodyInstanceArena::TakeUploads(std::vector<std::pair<uint32_t, uint32_t>>& out) {
  out.clear();
  if (dirty_.empty()) return;
  std::sort(dirty_.begin(), dirty_.end());
  // 64 instances (1 KiB) of unchanged data are cheaper to re-send than a
  // second WriteBuffer.
  constexpr uint32_t kGap = 64;
  std::pair<uint32_t, uint32_t> cur = dirty_[0];
  for (size_t k = 1; k < dirty_.size(); k++) {
    if (dirty_[k].first <= cur.second + kGap) {
      cur.second = std::max(cur.second, dirty_[k].second);
    } else {
      out.push_back(cur);
      cur = dirty_[k];
    }
  }
  out.push_back(cur);
  dirty_.clear();
}

void CullBodyRanges(const std::vector<BodyDrawRange>& ranges,
                    const std::vector<BodyXformGpu>& xforms, const Vec3& eye,
                    const Vec3& fwd, const Vec3& right, const Vec3& up,
                    float tanHalfFovY, float aspect,
                    std::vector<std::pair<uint32_t, uint32_t>>& draws,
                    uint32_t* instancesDrawn) {
  draws.clear();
  uint32_t drawn = 0;
  const float ty = tanHalfFovY, tx = tanHalfFovY * aspect;
  // A sphere clears a side plane |x| <= z*t when x - z*t <= r*sqrt(1+t^2).
  const float kx = std::sqrt(1.0f + tx * tx), ky = std::sqrt(1.0f + ty * ty);
  for (const BodyDrawRange& r : ranges) {
    bool visible = true;
    if (r.slot < xforms.size()) {
      const BodyXformGpu& x = xforms[r.slot];
      const Vec3 lc{0.5f * (r.lo[0] + r.hi[0]), 0.5f * (r.lo[1] + r.hi[1]),
                    0.5f * (r.lo[2] + r.hi[2])};
      const Vec3 he{0.5f * (r.hi[0] - r.lo[0]), 0.5f * (r.hi[1] - r.lo[1]),
                    0.5f * (r.hi[2] - r.lo[2])};
      // Same rotation debris.wgsl's quatRotate applies (q * v * q^-1).
      const Vec3 u{x.quat[0], x.quat[1], x.quat[2]};
      const Vec3 t = u.cross(lc) * 2.0f;
      const Vec3 wc = Vec3{x.pos[0], x.pos[1], x.pos[2]} + lc + t * x.quat[3] +
                      u.cross(t);
      // +1 voxel of margin: TAA jitter, the render offset's sub-voxel slide,
      // and the float error of a quaternion that is not quite unit.
      const float rad = he.len() + 1.0f;
      const Vec3 d = wc - eye;
      const float z = d.dot(fwd), xr = d.dot(right), yu = d.dot(up);
      if (z < -rad) visible = false;
      else if (std::fabs(xr) - z * tx > rad * kx) visible = false;
      else if (std::fabs(yu) - z * ty > rad * ky) visible = false;
    }
    if (!visible) continue;
    drawn += r.count;
    if (!draws.empty() && draws.back().first + draws.back().second == r.first)
      draws.back().second += r.count;
    else
      draws.push_back({r.first, r.count});
  }
  if (instancesDrawn) *instancesDrawn = drawn;
}

uint32_t BodyRegistry::TotalSlots() const {
  return debris_.SlotCount() + mobs_.LimbBodyCount() +
         (avatar_ ? avatar_->LimbBodyCount() : 0u);
}

bool BodyRegistry::AnyInstancesDirty() const {
  return debris_.InstancesDirty() || mobs_.InstancesDirty() ||
         (avatar_ && avatar_->InstancesDirty());
}
