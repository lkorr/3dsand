#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// Far-field cascade manager (render-only LOD — DESIGN.md §9,
// docs/PLAN_far_field_cascades.md). Keeps kFarLevels nested toroidal 256^3
// volumes centered on the player: recenters each level toward the player at
// most one level-chunk per axis per tick (2-chunk hysteresis, mirroring
// Stream), enqueues incoming planes for GPU fill, and drains the queue at
// kFarListCap level-chunks per tick through worldgen.wgsl `far`.
//
// Derived data only: no readbacks, no sim interaction, not hashed. Fill
// entries name SLOTS; the kernel maps slot -> world level-chunk under the
// origins uploaded the same tick, so a backlogged entry always fills whatever
// is currently resident in that slot — never stale space.
//
// EDIT PERSISTENCE (src/sim/faredits.h). The cascades are still DERIVED and
// DISPOSABLE, but they are no longer derived from the seed alone: they are
// derived from (seed + persisted edits). The sieve regenerates pristine
// terrain, and PrepareTick hands each fill entry the cells the CPU's FarEdits
// index says the player changed, which the same `far` workgroup re-applies.
// Without it every refill — an incoming plane, a teleport, a world load —
// healed the horizon back to procgen and the player's crater vanished.
// FarEdits itself reconstructs from the ChunkStore region files, so nothing
// here is authoritative and nothing here is saved.
class FarField {
 public:
  void Init(World* world) { world_ = world; }

  // Recenter toward the player's FINE-chunk coord. Call between ticks (after
  // Stream::Update), before PrepareTick. A level whose window has gone
  // entirely stale (teleport, load) resets and refills wholesale.
  void Update(IVec3 playerChunk);

  // Pop up to the current cap of queued fills, upload farList + farPatch
  // (+ farUBO when origins changed), and return the dispatch count for this
  // tick's TickParams.farCount / EncodeFarFill. May pop FEWER than the cap
  // when this tick's patch payload budget (kFarPatchCap) is spent — the rest
  // stay queued, exactly as they do when the dispatch cap is the binding one.
  //
  // `drain = false` publishes the UBO and pops NOTHING. It exists for the one
  // caller whose fills would be thrown away: the game runs for seconds with
  // the `far`/`farpatch` pipelines still compiling on a background thread, and
  // EncodeFarFill records no row until they land. Popping into that hole did
  // not merely waste the entries — it emptied pending_, so SafeRadiusMeters
  // reported the FULL 6.5 km horizon and FaceWord reported every face valid
  // while every cascade byte was still zero. The renderer marched those empty
  // levels, rays escaped through the ground, and the sky (clouds included) was
  // drawn UNDER the terrain until the pipelines arrived and the wholesale
  // refill slammed the fog shut. Not draining keeps the bookkeeping true: the
  // fog stays closed on the residency window and the valid box stays empty,
  // which is what "the horizon has not been built yet" is supposed to look
  // like.
  uint32_t PrepareTick(const rhi::Queue& queue, bool drain = true);

  // Entries a RESET may pop per tick (see bulkCap_). Defaults to kFarListCap,
  // which is what the headless drain loops want; the game lowers it.
  void SetBulkCap(uint32_t n) { bulkCap_ = n ? n : 1u; }

  // Entries an INCOMING PLANE may pop per tick (see planeCap_ / kPlayFillCap).
  // A knob rather than a constant because what it trades against — the sieve's
  // per-entry GPU cost — moved by 2.4x in one shader edit, and the next such
  // edit should not need a rebuild to follow (render.farPlaneFillRate).
  void SetPlaneCap(uint32_t n) {
    planeCap_ = n ? (n > kFarListCap ? kFarListCap : n) : 1u;
  }

  // ---- AT MOST ONE BULK SLICE PER FRAME ----------------------------------
  // Stream::BeginFrame's rule (stream.h R4), for the same reason and with the
  // same shape. PrepareTick is a per-TICK call and the frame loop runs up to
  // four ticks on a catch-up frame, so a frame that was already slow paid the
  // refill slice four times and got slower — measured before this gate,
  // `--frames 2500 --autowalk` with a 256-entry slice (~13 ms of GPU) read
  // farFill p99 35.2 ms and max 38.3, i.e. two and three slices landing on
  // one frame. The refill is render-only derived data with no deadline, so
  // deferring the extra slices to the next frame costs nothing but wall
  // clock, which is the axis this whole cap trades on anyway.
  //
  // ONLY the bulk path is gated. Incoming planes (planeCap_) stay per tick:
  // they are the horizon keeping up with a player who is moving, and holding
  // them back is what puts a stale slab on the leading face.
  //
  // Callers without a frame loop never call this, so `frameGated_` stays
  // false and every PrepareTick may take a full slice — which is what the
  // selftest gates and the smokes already did.
  void BeginFrame() { frameGated_ = true; bulkThisFrame_ = false; }

  // Patch words uploaded by the last PrepareTick (diagnostics / selftest).
  uint32_t LastPatchWords() const { return lastPatchWords_; }

  // Re-derive every origin around the player and refill all levels, FINEST
  // first (startup, load, regen). See the order note in farfield.cpp.
  void FullRefill(IVec3 playerChunk);

  size_t PendingFills() const { return queue_.size(); }

  // ---- WHERE THE QUEUE CAME FROM (2026-09-12) ----------------------------
  // PendingFills alone is a bare count, and a bare count sends you A/B-ing
  // (CLAUDE.md rule 6): the first thing tried against a 437k backlog was
  // raising the plane cap, which moved it by 1.5% because the queue was not
  // planes at all. These say which producer filled it. A RESET is a whole
  // level (kFarNumChunks entries) and comes either from FullRefill or from an
  // origin that fell a whole box behind; a PLANE is kFarNChunk^2 and comes
  // from ordinary travel. They cost the same per entry and want completely
  // different fixes.
  uint64_t ResetsIssued() const { return resets_; }
  uint64_t CoalescedResets() const { return coalesced_; }
  uint64_t RefillsIssued() const { return refills_; }
  uint64_t GapResetsIssued() const { return gapResets_; }
  uint64_t PlanesIssued() const { return planes_; }
  // The largest origin gap Update has had to close, in level chunks, and the
  // level it happened on. kFarNChunk is where a step becomes a reset.
  uint32_t WorstGap() const { return worstGap_; }
  uint32_t WorstGapLevel() const { return worstGapLevel_; }

  // Radius (meters from the player) out to which cascade data is known to be
  // FILLED — the adaptive-fog input (plan phase 3B).
  //
  // Buffers are zero-initialized and a queued fill has not run yet, so a level
  // with outstanding entries may contain air where terrain belongs; rays march
  // straight through it and hit sky. Rather than let that read as holes in the
  // horizon, the renderer fogs everything past the last KNOWN-GOOD band out.
  //
  // The bound is the INNERMOST incomplete level: cascade boxes are nested, so
  // if level k has pending work, level k's half-extent and everything past it
  // is suspect, but levels 1..k-1 are complete and cover out to level k-1's
  // half-extent. (Levels coarser than k may also be complete — FullRefill
  // fills coarsest-first exactly so the horizon exists early — but their data
  // is only reachable through the gap at level k, so it cannot be trusted to
  // be visible.) With no pending work anywhere this is the full outermost
  // half-extent, which is what kFarFogDensity was pinned to.
  float SafeRadiusMeters() const;

  // ---- THE VALID BOX (2026-09-10): what the RENDERER may march ------------
  // A level is toroidal: when its origin steps one level chunk toward the
  // player, the incoming face's SLOTS are the outgoing face's, and they keep
  // the outgoing face's bytes until the sieve refills them. That was invisible
  // while a plane landed in the tick it was queued; under kPlayFillCap a
  // plane takes ~16 ticks and sprint flight backlogs planes deep, so the
  // renderer, which marched the full box from `origins` the tick they moved,
  // drew the hillside BEHIND the player ahead of them and the underground of
  // the bottom face in the sky when the box stepped up.
  //
  // So the box the far readers march is the full box LESS the faces with
  // planes still queued (and empty while a reset is in flight), published in
  // FarParams.origins[k].w — the word every other reader ignores — as six
  // 5-bit counts, one per face (-x,+x,-y,+y,-z,+z, low field first), plus
  // bit 30 = whole level pending. raymarch.wgsl `farBox` unpacks it; a ray in
  // an excluded slab leaves the level at the shrunken face and the next
  // coarser level, which is filled, picks up at the same t (the seam contract
  // traceFar already keeps for TUNE_FAR_STEPS).
  // Nothing on the render path reads a slot before the sieve has written it.
  //
  // ---- THE FIELD MUST HOLD THE WHOLE BOX (2026-09-12) ---------------------
  // It was six FOUR-bit counts saturating at 15, and "always conservative"
  // below was false because of it: a level is kFarNChunk = 32 chunks across,
  // so a face with 20 planes queued had only 15 of its 20 stale chunk layers
  // excluded and the renderer marched the other five AS TERRAIN. Those five
  // hold the OUTGOING face's bytes — the torus has not been refilled yet — so
  // what was drawn ahead of a moving player was the ground from behind them,
  // which snapped to the real ground the moment the sieve caught up. Reached
  // in about a second of flight at the old fill cap (see kPlayFillCap).
  // Five bits holds all 32; past that the count is no longer expressible and
  // the honest answer is bit 30, "this level has nothing you may trust",
  // which costs a fall-through to the next level and never draws a wrong one.
  //
  // Counted per PLANE, not per entry: each EnqueuePlane / ResetLevel pushes a
  // record onto recs_ (FIFO, parallel to queue_) with its entry count, and a
  // face count drops only when that plane's LAST entry is popped — the queue
  // is FIFO so a plane's entries are contiguous and drain in order. A reset
  // bumps the level's epoch and zeroes its faces: plane records queued before
  // it still pop (the queue is not reordered) but no longer own a face.
  // Always conservative — a face that reversed direction is excluded on both
  // sides until both records drain, and a landed face is published one tick
  // late (the UBO is written at the top of the NEXT PrepareTick), never early.
  uint32_t FaceWord(uint32_t k) const;   // the packed w (the selftest reads it)

 private:
  void ResetLevel(uint32_t k, IVec3 desired);        // origin jump + full refill
  // Drop everything level k still has queued. Only ResetLevel calls it, and
  // only when that level is about to be re-queued wholesale: the entries it
  // removes would fill slots the reset is about to fill anyway, and leaving
  // them in would make the reset COST more than the backlog it replaces
  // instead of less. queue_ and recs_ stay parallel because a record owns a
  // contiguous run of its own level's entries and both sides lose level k
  // entirely (see PrepareTick's front-record bookkeeping).
  void PruneLevel(uint32_t k);
  void EnqueuePlane(uint32_t k, int axis, int wcoord);  // one incoming plane
  void Enqueue(uint32_t k, uint32_t slot);           // queue + per-level counter

  World* world_ = nullptr;
  IVec3 origins_[kFarLevels] = {};  // per level, level-chunk units
  std::deque<uint32_t> queue_;  // packed (level-1) << kFarSlotShift | slot
  // One record per EnqueuePlane / ResetLevel, in queue order (see FaceWord).
  // axis < 0 is a reset: it drains like any other record but owns no face.
  struct PlaneRec {
    uint32_t level;      // 0-based
    int axis;            // 0..2, or -1 for a reset
    int side;            // 0 = low face (-), 1 = high face (+)
    uint32_t remaining;  // entries of this plane still in queue_
    uint32_t epoch;      // epoch_[level] when queued; stale after a reset
  };
  std::deque<PlaneRec> recs_;
  uint32_t faces_[kFarLevels][3][2] = {};  // planes queued per level face
  uint32_t epoch_[kFarLevels] = {};
  // Outstanding entries per level, mirroring queue_ (the queue is FIFO across
  // levels, so scanning it per frame would be O(24576); these counters make
  // SafeRadiusMeters O(kFarLevels)). Incremented on every enqueue, decremented
  // as PrepareTick pops.
  uint32_t pending_[kFarLevels] = {};
  // Of those, how many came from a RESET (ResetLevel / FullRefill: a whole
  // level, 32,768 entries) rather than from an incoming plane. Popped first,
  // because the queue is FIFO and a reset's entries precede any plane queued
  // after it. Two consumers: PrepareTick's cap (a reset takes kFarListCap per
  // tick — the horizon has to exist — while planes take kPlayFillCap), and
  // SafeRadiusMeters (a plane-incomplete level is trusted out to its edge
  // minus the plane; a reset-incomplete one only to the level inside it).
  uint32_t bulkPending_[kFarLevels] = {};
  // THE PLAY CAP (2026-09-09). A far entry is a 16^3-cell sieve — 4,096
  // genCell evaluations plus the surface skin — and measured ~0.2 ms of GPU
  // each on an RTX 3060 Ti: a level's incoming plane is 1,024 of them, so
  // walking across a level-1 chunk boundary (every 32 voxels) put 180-240 ms
  // of GPU into ONE tick. `--frames 900 --autowalk`: farField p50 0.002 ms,
  // p99 184 ms, max 206 ms — the largest GPU spike in the walking frame, and
  // what the CPU saw as a present wait or, before the tick throttle, a
  // snapshot stall. 64 per tick is ~12 ms of GPU, spreads a plane over 16
  // ticks, and covers walking demand (~50 entries/tick across all levels)
  // with headroom. Sprint flight demands ~700/tick and backlogs at ANY cap the
  // frame can afford; that is the render-only horizon lagging the player,
  // which the adaptive fog already covers, and it drains the moment the
  // player slows. Resets are exempt (see bulkPending_).
  //
  // The MEAN is inherent, only the shape moves: walking costs ~8.5 ms/tick of
  // sieve whatever the cap (an L1 plane per 32 voxels ~ 5 ms per voxel of
  // travel, plus the coarser levels). Measured `--autowalk --duel-dummy`,
  // whole-frame p50 / p95 / p99 / max, >33 ms: uncapped 19.8 / 28 / 52 / 153,
  // 2.5%; cap 64: 20.6 / 43 / 54 / 73, 19%; cap 32: 29.1 / 48 / 64 / 81, 42%.
  // 32 put ~5 ms on nearly every frame and the median frame lost a vblank;
  // 64 keeps the median and only cuts the tail.
  //
  // ---- 64 WAS A COST, AND THE COST MOVED (2026-09-12) ---------------------
  // "The next lever is the sieve's per-entry cost, not this number" is what
  // this comment used to end on, and that lever got pulled: worldgen.wgsl's
  // `far` gained the SKY CEILING early-out its sibling genChunk always had, so
  // a level chunk above the terrain no longer runs 4,096 genCellCol calls to
  // conclude "air". Measured `--frames 900 --autofly-surface`, one exe, two
  // asset trees, exclusive lock, the same 265k entries over the same 306 ticks:
  //   farField GPU  mean 15.90 -> 6.65 ms/frame, p99 65.5 -> 37.3, max 150 -> 47
  // i.e. 44.5 us -> 18.6 us per entry. 64 entries is 1.2 ms of GPU now, not the
  // ~12 ms this number was sized for, and the cap had stopped paying for
  // itself: it was the reason the queue backlogged at all.
  //
  // WHY THE BACKLOG WAS NOT MERELY COSMETIC. Measured on the same harness
  // before any of this: 437,248 entries — 427 planes, 1.7 whole refills — still
  // queued after 22 s of flight, which at 64/tick is 3.8 MINUTES of drain. Two
  // things break at that depth and neither is "the horizon is a bit behind":
  // FaceWord's per-face counter saturated and stopped excluding the stale
  // slabs (see FaceWord), and the levels it did exclude collapsed far enough
  // that traceFar fell through to a COARSER one at short range — 3.2 m and
  // 6.4 m cells at 40 m, which is what "the LOD is showing me house-sized
  // blocks" actually was.
  //
  // 256 is 4.8 ms/tick at the measured per-entry cost, still under what 64 was
  // sized to spend, and it covers sprinting diagonally (~130 entries/tick)
  // with headroom instead of falling behind at a walk on two axes. It is a
  // DEFAULT now, not a constant: render.farPlaneFillRate owns it, so the next
  // time the sieve's cost moves this does not need a rebuild to follow.
  static constexpr uint32_t kPlayFillCap = 256;
  // THE RESET CAP, and why it is no longer kFarListCap in the game
  // (2026-09-10). A full refill is kFarLevels x kFarNumChunks = 262,144 sieve
  // entries. kFarListCap (4,096) slices that into 64 ticks of 267 ms —
  // measured `--frames 1500 --autowalk`: farField p99 210 ms, max 267 ms, 63
  // frames over 100 ms — which is the 2 fps stall the horizon's arrival was
  // reported as from live play. The game therefore spends a bounded slice per
  // tick (render.farRefillRate, whose comment carries the measured table of
  // what each slice size costs) and lets the refill take longer in wall clock
  // while staying interactive. The headless drain loops
  // (selftest_render.cpp's DrainFullRefill, the far gates) have no frame to
  // protect and keep the default.
  uint32_t bulkCap_ = kFarListCap;
  uint64_t resets_ = 0, gapResets_ = 0, planes_ = 0, refills_ = 0;
  uint64_t coalesced_ = 0;
  uint32_t worstGap_ = 0, worstGapLevel_ = 0;
  uint32_t planeCap_ = kPlayFillCap;
  bool frameGated_ = false;   // BeginFrame has been called at least once
  bool bulkThisFrame_ = false;
  bool uboDirty_ = true;
  // Reused across ticks so a fill-heavy frame does not reallocate: the header
  // is 2 u32 per dispatched entry, the payload is the concatenated patch runs.
  std::vector<uint32_t> patchHeader_;
  std::vector<uint32_t> patchPayload_;
  uint32_t lastPatchWords_ = 0;
};
