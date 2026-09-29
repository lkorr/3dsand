#include "sim/farfield.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "sim/faredits.h"

namespace {
constexpr int kHyst = 2;  // level-chunk hysteresis, same feel as Stream
// Player fine-chunk coord -> this level's chunk coord (arithmetic shift =
// floor division; one level-k chunk = 16 cells of 2^(k+kFarShiftBase) fine
// voxels = 2^(k+kFarShiftBase) fine chunks; k here is the 0-based index).
IVec3 LevelChunk(IVec3 fineChunk, uint32_t k) {
  int s = (int)(k + 1 + kFarShiftBase);
  return {fineChunk.x >> s, fineChunk.y >> s, fineChunk.z >> s};
}
// centered window origin for the player's level-chunk coord
IVec3 DesiredOrigin(IVec3 fineChunk, uint32_t k) {
  IVec3 lc = LevelChunk(fineChunk, k);
  int h = (int)kFarNChunk / 2;
  return {lc.x - h, lc.y - h, lc.z - h};
}
}  // namespace

void FarField::Enqueue(uint32_t k, uint32_t slot) {
  queue_[k].push_back(slot);
  pending_[k]++;
}

bool FarField::LevelChunkHitsWindow(uint32_t k, IVec3 lc) const {
  if (!world_) return false;
  // A level-k chunk (0-based k) spans 2^(k + 1 + kFarShiftBase) fine chunks
  // per axis, aligned (FarEdits::LevelChunkOf is the same relation).
  const int s = (int)(k + 1 + kFarShiftBase);
  const IVec3 o = world_->WindowOrigin();
  const int n = (int)kNChunk;
  auto hit = [s, n](int l, int w) {
    const int lo = l * (1 << s), hi = lo + (1 << s);
    return lo < w + n && hi > w;
  };
  return hit(lc.x, o.x) && hit(lc.y, o.y) && hit(lc.z, o.z);
}

void FarField::DropPendingPlane(uint32_t k, int axis, int sa) {
  // Newest first: a reversal undoes the MOST RECENT step on this axis. Every
  // record before `r` owns a contiguous run of queue_[k] in record order (the
  // front one partially popped, its `remaining` already net of that), so the
  // dropped run starts at the sum of their remainders.
  std::deque<PlaneRec>& recs = recs_[k];
  for (size_t ri = recs.size(); ri-- > 0;) {
    const PlaneRec& r = recs[ri];
    if (r.axis != axis || r.sa != sa) continue;
    size_t off = 0;
    for (size_t j = 0; j < ri; j++) off += recs[j].remaining;
    queue_[k].erase(queue_[k].begin() + (ptrdiff_t)off,
                    queue_[k].begin() + (ptrdiff_t)(off + r.remaining));
    pending_[k] -= r.remaining;
    reversalDrops_ += r.remaining;
    // Released exactly as PrepareTick releases a plane whose last entry
    // popped: the slab it excluded is the layer the box just stepped back
    // off, and the plane replacing it owns the layer that is stale now.
    if (r.epoch == epoch_[k] && faces_[k][r.axis][r.side] > 0) {
      faces_[k][r.axis][r.side]--;
      uboDirty_ = true;
    }
    recs.erase(recs.begin() + (ptrdiff_t)ri);
    return;
  }
}

void FarField::EnqueuePlane(uint32_t k, int axis, int wcoord,
                            const int skipSa[3]) {
  int m = (int)kFarNChunk - 1;
  int sa = wcoord & m;
  // ---- A REVERSAL REFILLS THE SAME SLOTS -----------------------------------
  // The box stepped one level chunk toward -axis after stepping toward +axis
  // (or the reverse) before that step's plane had drained. The torus puts the
  // new incoming layer in the SAME slot layer as the one the undone step
  // queued — (o + N) & m == o & m — and a fill entry fills its slot under the
  // origins current when it is DISPATCHED, so the pending plane would only
  // write bytes this one is about to overwrite. Dropping it halves the work
  // of a player pacing across a chunk boundary.
  DropPendingPlane(k, axis, sa);
  // The incoming plane is the box's high face when the origin stepped toward
  // +axis (wcoord = o + N - 1) and its low face otherwise (wcoord = o).
  const int oa = axis == 0 ? origins_[k].x : axis == 1 ? origins_[k].y
                                                     : origins_[k].z;
  const int side = wcoord == oa + (int)kFarNChunk - 1 ? 1 : 0;
  planes_++;
  uint32_t n = 0;
  for (int b = 0; b < (int)kFarNChunk; b++) {
    for (int a = 0; a < (int)kFarNChunk; a++) {
      int s[3];
      s[axis] = sa;
      s[(axis + 1) % 3] = a;
      s[(axis + 2) % 3] = b;
      // ---- THE SURFACE MAP RIDES THE X/Z PLANES (LOD-seam package A) -----
      // The far surface map is 2D, so a Y step changes nothing in it and an
      // X or Z step turns over whole COLUMNS of it. One entry per level-chunk
      // column refills the map under that column's XZ footprint: the one in
      // slot layer y == 0, which both plane orders below and ResetLevel's
      // slot order put FIRST among its column's entries, so the fill always
      // lands no later than any `farpatch` of the same column (which clears
      // map entries over edited cells and must not be overwritten after).
      const bool mapFill = axis != 1 && s[1] == 0;
      // ---- A DIAGONAL STEP QUEUES THE CROSSING LINE ONCE -----------------
      // Two planes of one Update cross in a line of kFarNChunk slots. The
      // earlier plane already holds it, earlier in this level's FIFO, so its
      // fill lands before this plane's last entry releases this face.
      // EXCEPT a map-fill entry whose earlier twin belongs to a Y plane: the
      // Y plane carries no map fill, so dropping it would leave this column's
      // map stale. It is queued anyway (one extra sieve entry per such step).
      bool dup = false;
      for (int e = 0; e < 3; e++)
        if (e != axis && skipSa[e] >= 0 && s[e] == skipSa[e] &&
            !(mapFill && e == 1))
          dup = true;
      if (dup) { edgeDedupes_++; continue; }
      uint32_t slot = ((uint32_t)s[2] * kFarNChunk + (uint32_t)s[1]) * kFarNChunk +
                      (uint32_t)s[0];
      Enqueue(k, slot | (mapFill ? kFarListMapBit : 0u));
      n++;
    }
  }
  recs_[k].push_back({k, axis, side, sa, n, epoch_[k]});
  faces_[k][axis][side]++;
  uboDirty_ = true;
}

void FarField::PruneLevel(uint32_t k) {
  queue_[k].clear();
  recs_[k].clear();
  pending_[k] = 0;
  bulkPending_[k] = 0;
}

void FarField::ResetLevel(uint32_t k, IVec3 desired) {
  resets_++;
  // Whatever this level still had queued is superseded: every one of those
  // entries names a SLOT the wholesale enqueue below is about to name again.
  // Without this a coalesced reset ADDED kFarNumChunks entries to a backlog
  // instead of replacing it, which is the opposite of the point.
  PruneLevel(k);
  origins_[k] = desired;
  uboDirty_ = true;
  // No farSig clear HERE: the entries that overlap the window raise it as
  // they are dispatched (sigClear_), which is the only moment it is not racing
  // the refill.
  // Slot order is x fastest, then y, then z, so every level-chunk column's
  // y == 0 entry — the one that refills the surface map under it (see
  // EnqueuePlane) — comes before the rest of that column.
  for (uint32_t slot = 0; slot < kFarNumChunks; slot++) {
    const bool mapFill = ((slot / kFarNChunk) % kFarNChunk) == 0;
    Enqueue(k, slot | (mapFill ? kFarListMapBit : 0u));
  }
  bulkPending_[k] += kFarNumChunks;
  // Every plane record queued for this level is now meaningless — the whole
  // level is invalid until the reset lands — so it stops owning a face.
  epoch_[k]++;
  for (int a = 0; a < 3; a++) faces_[k][a][0] = faces_[k][a][1] = 0;
  recs_[k].push_back({k, -1, 0, -1, kFarNumChunks, epoch_[k]});
}

void FarField::FullRefill(const InterestSet& interest) {
  if (interest.Empty()) return;
  const IVec3 playerChunk = interest.Primary();
  refills_++;
  for (uint32_t k = 0; k < kFarLevels; k++) {
    queue_[k].clear();
    recs_[k].clear();
    pending_[k] = 0;
    bulkPending_[k] = 0;
    for (int a = 0; a < 3; a++) faces_[k][a][0] = faces_[k][a][1] = 0;
  }
  // ---- FINEST FIRST, and the order is load-bearing (2026-09-10) ----------
  // This ran COARSEST first, on the argument that "a horizon band appears
  // before the near bands refine". The valid box makes that argument true
  // and undesirable at the same time: a level whose reset is in flight
  // publishes bit 30, raymarch.wgsl's farBox collapses its box to empty, and
  // the ray falls straight through to the next level that HAS data. So with
  // only the coarsest level filled a ray leaving the residency window at
  // 25.6 m immediately marches level-8 cells — 25.6 m across, one cell per
  // window — and the first thing outside the window the player sees is a
  // field of house-sized blocks. It is a horizon, but it is the wrong
  // picture, and it stays wrong for the ~15 s the remaining levels take.
  //
  // Finest first is the same total work with the fog telling the truth about
  // it: level 1 lands and everything out to 51.2 m is correct at 0.2 m cells
  // with fog beyond, then 102 m, 205 m, ... — SafeRadiusMeters doubles as
  // each level completes and kFogLerpPerFrame eases the horizon open. The
  // player never sees a cell bigger than the distance it is at can hide,
  // which is the invariant the whole cascade geometry is built on
  // (world.h's "constant angular resolution" note).
  for (uint32_t k = 0; k < kFarLevels; k++)
    ResetLevel(k, DesiredOrigin(playerChunk, k));
}

uint32_t FarField::RefillBox(IVec3 lo, IVec3 hi) {
  uint32_t total = 0;
  const int n = (int)kFarNChunk;
  const int m = n - 1;
  auto fdiv = [](int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };
  for (uint32_t k = 0; k < kFarLevels; k++) {
    // A level-k chunk spans 2^(k + 1 + kFarShiftBase) fine chunks per axis
    // (LevelChunkHitsWindow), i.e. that many kChunk-voxel chunks.
    const int span = (int)kChunk << (k + 1 + kFarShiftBase);
    const IVec3 o = origins_[k];
    const int x0 = std::max(fdiv(lo.x, span), o.x), x1 = std::min(fdiv(hi.x, span), o.x + m);
    const int y0 = std::max(fdiv(lo.y, span), o.y), y1 = std::min(fdiv(hi.y, span), o.y + m);
    const int z0 = std::max(fdiv(lo.z, span), o.z), z1 = std::min(fdiv(hi.z, span), o.z + m);
    if (x0 > x1 || z0 > z1) continue;
    uint32_t cnt = 0;
    for (int lz = z0; lz <= z1; lz++)
      for (int lx = x0; lx <= x1; lx++) {
        // The column's surface map rides its slot-layer-0 entry (EnqueuePlane's
        // rule), queued FIRST so a farpatch of the same column lands after it.
        const uint32_t sx = (uint32_t)(lx & m), sz = (uint32_t)(lz & m);
        Enqueue(k, ((sz * kFarNChunk + 0u) * kFarNChunk + sx) | kFarListMapBit);
        cnt++;
        for (int ly = y0; ly <= y1; ly++) {
          const uint32_t sy = (uint32_t)(ly & m);
          if (sy == 0u) continue;   // the map entry above already fills it
          Enqueue(k, (sz * kFarNChunk + sy) * kFarNChunk + sx);
          cnt++;
        }
      }
    // A record like a plane's, owning no face (axis -1): PrepareTick's
    // front-record bookkeeping needs every queued entry to belong to one.
    recs_[k].push_back({k, -1, 0, -1, cnt, epoch_[k]});
    total += cnt;
  }
  return total;
}

float FarField::SafeRadiusMeters() const {
  // THE FARTHEST COMPLETE LEVEL, not the nearest incomplete one (2026-09-10).
  //
  // This used to stop at the innermost level with work outstanding and trust
  // nothing past it, on the reading that a complete coarse level "is only
  // reachable through the gap at level k, so it cannot be trusted to be
  // visible". That reading is wrong about the geometry the renderer actually
  // walks: the levels are NESTED boxes all centred on the player, and
  // traceFar's loop tests each level's box independently — a level whose box
  // farBox has collapsed is `continue`d and the next one picks the ray up at
  // the same t (raymarch.wgsl's valid-box note). A complete level therefore
  // covers everything inside its own half-extent as well as at it, at its own
  // cell size, whatever the levels inside it are doing.
  //
  // Reading it the old way cost the whole point of the adaptive fog during a
  // refill: with all eight levels queued, level 1 completes last, so the fog
  // sat pinned on the residency window (25.6 m) for the entire drain and then
  // snapped to 6.5 km — "the entire area suddenly gets extremely foggy",
  // reported from live play. Taking the max instead lets the horizon open one
  // level at a time as FullRefill's finest-first order lands them.
  //
  // Half-extents come from world.h (kFarHalfExtentMeters), so this tracks
  // kFarN / kFarShiftBase / kFarLevels instead of restating the box relation.
  // The floor is the residency window: the fine march always covers that, and
  // it is the pre-cascade draw distance.
  float best = kWindowHalfExtentMeters;
  for (uint32_t k = 0; k < kFarLevels; k++) {
    if (pending_[k] == 0) {                    // complete: trust its whole box
      best = std::max(best, kFarHalfExtentMeters(k + 1));
      continue;
    }
    if (bulkPending_[k] > 0) continue;         // reset in flight: no data at all
    // Only PLANES outstanding: the level was complete and what is missing is
    // its incoming face, one level chunk thick per queued plane, on the side
    // the player is moving toward. Everything inside those faces is filled, so
    // the trusted radius is the level's own half-extent less the DEEPEST
    // face — under the play cap a plane takes several ticks to land, and
    // fogging the whole level out for that long would pulse the horizon on
    // every chunk boundary crossed.
    //
    // TIMES THE PLANES QUEUED, and it used to be times one (2026-09-12). The
    // subtraction is the width of the slab raymarch.wgsl's farBox is excluding,
    // and that slab is one level chunk PER QUEUED PLANE — so with a backlog
    // this reported the level trustworthy out to a radius whose outer band the
    // renderer was refusing to march. The fog then opened over exactly the
    // region that had no data to show, which is the opposite of what the
    // adaptive fog exists for. Agrees with FaceWord by construction: same
    // counters, same "a face past the box is no data at all" escalation.
    uint32_t layers = 0;
    for (int a = 0; a < 3; a++)
      for (int s = 0; s < 2; s++) layers = std::max(layers, faces_[k][a][s]);
    if (layers >= kFarNChunk) continue;        // box fully excluded: no data
    const float face =
        (float)(kChunk << (k + 1 + kFarShiftBase)) * kVoxelMeters;
    best = std::max(best, kFarHalfExtentMeters(k + 1) - face * (float)layers);
  }
  return best;
}

uint32_t FarField::FaceWord(uint32_t k) const {
  // Layout documented at the declaration (farfield.h) and at the reader
  // (raymarch.wgsl farBox): the 5-bit field (axis * 2 + side) holds the planes
  // queued on that face; bit 30 says the whole level is pending.
  if (bulkPending_[k] > 0) return kFarFaceAllPending;
  uint32_t w = 0;
  for (int a = 0; a < 3; a++) {
    for (int side = 0; side < 2; side++) {
      // A count that does not fit is NOT clamped. Clamping is what made this
      // word lie (farfield.h): the excluded slab would be narrower than the
      // stale one and the renderer would march the difference as terrain.
      // A face that has fallen a whole box behind has no trustworthy data in
      // this level at all, which is exactly what bit 30 says.
      if (faces_[k][a][side] > kFarFaceMax) return kFarFaceAllPending;
      w |= faces_[k][a][side] << (kFarFaceBits * (a * 2 + side));
    }
  }
  return w;
}

void FarField::Update(const InterestSet& interest) {
  if (interest.Empty()) return;  // no centre => leave the cascades put
  const IVec3 playerChunk = interest.Primary();
  for (uint32_t k = 0; k < kFarLevels; k++) {
    IVec3 desired = DesiredOrigin(playerChunk, k);
    int d[3] = {desired.x - origins_[k].x, desired.y - origins_[k].y,
                desired.z - origins_[k].z};
    const uint32_t gap = (uint32_t)std::max({std::abs(d[0]), std::abs(d[1]),
                                             std::abs(d[2])});
    if (gap > worstGap_) { worstGap_ = gap; worstGapLevel_ = k + 1; }
    if (gap >= kFarNChunk) {
      gapResets_++;
      ResetLevel(k, desired);  // whole window stale (teleport / load)
      continue;
    }
    bool stepped = false;
    // The slot layer each axis's plane took THIS Update, so a later axis's
    // plane can leave out the line the two share (EnqueuePlane).
    int stepSa[3] = {-1, -1, -1};
    for (int axis = 0; axis < 3; axis++) {
      if (std::abs(d[axis]) < kHyst) continue;
      stepped = true;
      int dir = d[axis] > 0 ? 1 : -1;
      int* o = axis == 0 ? &origins_[k].x : axis == 1 ? &origins_[k].y
                                                      : &origins_[k].z;
      *o += dir;
      uboDirty_ = true;
      // incoming plane: the window's leading face after the shift
      int wcoord = dir > 0 ? *o + (int)kFarNChunk - 1 : *o;
      EnqueuePlane(k, axis, wcoord, stepSa);
      stepSa[axis] = wcoord & ((int)kFarNChunk - 1);
    }

    // ---- COALESCE A FACE THAT HAS FALLEN A WHOLE BOX BEHIND (2026-09-12) ---
    // The origin steps at most one level chunk per axis per tick and each step
    // queues a 1,024-entry plane, but the drain is a per-tick cap — so sustained
    // travel queues planes faster than they land and the backlog grows without
    // any bound at all. Measured `--frames 900 --autofly-surface`: 342 planes
    // queued in 329 ticks against a plane demand of ~1,065 entries/tick, and at
    // the old 64/tick cap a flight left 437,248 entries — 3.8 MINUTES of drain
    // — still queued when the run ended.
    //
    // At kFarNChunk planes deep the face has turned over the ENTIRE box: every
    // slot on this axis has been recycled, so there is nothing left in the
    // level worth keeping and FaceWord is already publishing kFarFaceAllPending
    // for it. A reset costs kFarNumChunks entries; the planes it replaces cost
    // kFarNChunk * kFarNChunk * kFarNChunk, which is the SAME number — except
    // the reset also drops the redundant re-queues of slots the origin has
    // wrapped past more than once, drains on the bulk cap rather than the play
    // cap, and leaves the level CORRECT instead of partially stale.
    //
    // So the backlog per level is bounded by one level's worth of entries, and
    // "the horizon is minutes behind" becomes "the horizon is one refill
    // behind" — which the finest-first order and the adaptive fog already
    // handle, because it is the same state a load leaves.
    if (stepped) {
      uint32_t worst = 0;
      for (int a = 0; a < 3; a++)
        for (int s = 0; s < 2; s++) worst = std::max(worst, faces_[k][a][s]);
      if (worst >= kFarNChunk) {
        coalesced_++;
        ResetLevel(k, desired);
      }
    }
  }
}

uint32_t FarField::PrepareTick(const rhi::Queue& queue, bool drain) {
  if (!world_) return 0;
  if (uboDirty_) {
    FarParams fp{};
    for (uint32_t k = 0; k < kFarLevels; k++) {
      fp.origins[k][0] = origins_[k].x;
      fp.origins[k][1] = origins_[k].y;
      fp.origins[k][2] = origins_[k].z;
      fp.origins[k][3] = (int32_t)FaceWord(k);   // the render's valid box
    }
    queue.WriteBuffer(world_->farUBO, 0, &fp, sizeof(fp));
    uboDirty_ = false;
  }
  if (sigClear_) {
    static const std::vector<uint32_t> zeros(kNumSlots, 0u);
    queue.WriteBuffer(world_->farSig, 0, zeros.data(), zeros.size() * 4);
    sigClear_ = false;
  }
  // The UBO above is published either way — a caller that is not draining
  // still has to tell the renderer which levels it may march.
  if (!drain || PendingFills() == 0) return 0;
  // A reset (teleport, load, the startup horizon) drains at the bulk cap; in
  // play, incoming planes drain at kPlayFillCap (see farfield.h).
  bool bulk = false;
  for (uint32_t k = 0; k < kFarLevels; k++) bulk = bulk || bulkPending_[k] > 0;
  // One bulk slice per FRAME under the frame gate (farfield.h BeginFrame).
  // A second tick in the same frame falls back to the play cap rather than to
  // nothing: whatever planes the player's travel queued behind the reset
  // still have to keep up, and they are one plane cap's worth.
  if (bulk && frameGated_) {
    if (bulkThisFrame_) bulk = false;
    else bulkThisFrame_ = true;
  }
  const uint32_t cap = (uint32_t)std::min(
      PendingFills(), (size_t)(bulk ? bulkCap_ : planeCap_));
  list_.clear();
  list_.reserve(cap);
  patchHeader_.clear();
  patchPayload_.clear();

  FarEdits* edits = world_->farEdits;
  const int m = (int)kFarNChunk - 1;
  bool budgetHit = false;
  // FINEST LEVEL FIRST (see queue_): level 1's pending planes are the ones in
  // view at the shortest range, so they go before any coarser level's.
  for (uint32_t k = 0; k < kFarLevels && !budgetHit; k++) {
    std::deque<uint32_t>& q = queue_[k];
    while (!q.empty() && list_.size() < cap) {
      // The queue word is the slot plus, for one entry per level-chunk
      // column, the surface-map fill flag (EnqueuePlane / ResetLevel).
      const uint32_t mapBit = q.front() & kFarListMapBit;
      const uint32_t slot = q.front() & kFarSlotMask;

      // The queue names SLOTS, so the world level chunk resident in that slot
      // is resolved here under the SAME origins the kernel will read this
      // tick — the mirror of common.wgsl's farSlotToChunk. Resolving it any
      // earlier (at enqueue time) would name the chunk that used to live
      // there.
      const IVec3 sc{(int)(slot % kFarNChunk),
                     (int)((slot / kFarNChunk) % kFarNChunk),
                     (int)(slot / (kFarNChunk * kFarNChunk))};
      const IVec3 o = origins_[k];
      const IVec3 lc{o.x + ((sc.x - o.x) & m), o.y + ((sc.y - o.y) & m),
                     o.z + ((sc.z - o.z) & m)};

      // ---- this entry's edit patches (far-field edit persistence) --------
      const std::vector<uint32_t>* patch = nullptr;
      if (edits && !edits->Empty()) patch = edits->Lookup(k + 1, lc);
      const uint32_t n = patch ? (uint32_t)patch->size() : 0u;
      // BUDGET BEFORE EMISSION (CLAUDE.md's rule for every other queue here):
      // an entry that will not fit stays queued rather than being filled with
      // a truncated patch, which would leave the horizon showing half an edit
      // and no record that it did. One entry can hold at most kChunkVol = 4096
      // words, far under the cap, so this can never deadlock.
      if (patchPayload_.size() + n > kFarPatchCap) { budgetHit = true; break; }

      patchHeader_.push_back((uint32_t)patchPayload_.size());
      patchHeader_.push_back(n);
      if (n) patchPayload_.insert(patchPayload_.end(), patch->begin(), patch->end());

      list_.push_back((k << kFarSlotShift) | slot | mapBit);
      q.pop_front();
      // A refill of cells a resident chunk owns undoes that chunk's last
      // downsample: clear the signatures on the NEXT PrepareTick (sigClear_).
      if (!sigClear_ && LevelChunkHitsWindow(k, lc)) sigClear_ = true;
      // The dispatch is encoded in THIS tick's submit, so the entry counts as
      // filled from here on — SafeRadiusMeters is read on the render path of
      // the same frame, one submit behind at worst.
      pending_[k]--;
      // resets pop first in a level. THE LAST ONE REPUBLISHES THE UBO: while
      // bulkPending_ is nonzero FaceWord says kFarFaceAllPending ("march
      // nothing in this level"), so the word must be re-sent the moment it
      // stops being true. Without this the ALL-PENDING word published by the
      // first PrepareTick of a reset stayed on the GPU after the drain, and a
      // level only came back when some unrelated face change happened to set
      // uboDirty_: every headless FullRefill drain (--shot, the far gates'
      // DrainFullRefill, the seam frames) rendered ZERO cascade pixels
      // (rmPxFar 0.000, traceFar skipping every level), and in play a player
      // standing still after a load had no horizon until level 1 stepped.
      if (bulkPending_[k] > 0 && --bulkPending_[k] == 0) uboDirty_ = true;
      // The plane this entry belongs to is the level's front record (same
      // FIFO). Its face is released when its last entry is dispatched —
      // published next tick, after this tick's sieve is in the queue.
      PlaneRec& r = recs_[k].front();
      if (--r.remaining == 0) {
        if (r.axis >= 0 && r.epoch == epoch_[r.level] &&
            faces_[r.level][r.axis][r.side] > 0) {
          faces_[r.level][r.axis][r.side]--;
          uboDirty_ = true;
        }
        recs_[k].pop_front();
      }
    }
  }
  const uint32_t count = (uint32_t)list_.size();
  if (count == 0) return 0;   // first entry alone blew the budget: cannot happen
  queue.WriteBuffer(world_->farList, 0, list_.data(), count * 4);
  // The header is written for EVERY dispatched entry, always — the kernel
  // indexes it by dispatch index and a stale pair points the patch loop at
  // another entry's payload. 8 bytes per entry, so <= 32 KiB in the fullest
  // tick a full refill can produce, and zero bytes in a tick with no fills.
  queue.WriteBuffer(world_->farPatch, 0, patchHeader_.data(), count * 8);
  if (!patchPayload_.empty())
    queue.WriteBuffer(world_->farPatch, (uint64_t)kFarPatchBase * 4,
                      patchPayload_.data(), patchPayload_.size() * 4);
  lastPatchWords_ = (uint32_t)patchPayload_.size();
  return count;
}
