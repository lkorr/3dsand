#pragma once
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

class ChunkStore;

// ---- far-field EDIT INDEX (cascade edit persistence) ----------------------
//
// THE GAP THIS CLOSES. The cascades have two producers and they disagree about
// history:
//
//   * worldgen.wgsl `fardown` (plan phase 2) downsamples the LIVE voxel grid
//     for every chunk on the dirty list, so an edit made inside the residency
//     window immediately leaves a ghost in every cascade level. That ghost is
//     durable — until something refills its level chunk.
//   * worldgen.wgsl `far` (the sieve) fills a level chunk from PRISTINE
//     procgen. It runs on every incoming plane (the level's window recentering
//     past the edit and back), on every ResetLevel (teleport / load) and on
//     every FullRefill (startup, load, regen).
//
// So the sieve un-does the downsample. Walk far enough that a level's box no
// longer contains your crater and walk back, or reload the world you dug it
// in, and the horizon heals itself. This index is the memory that survives
// that: it records, per cascade level, the material each cascade cell's SAMPLE
// VOXEL held the last time the CPU saw the chunk that owns it, and FarField
// hands those cells to the sieve as a patch list so the fill lands
// edited-then-pristine instead of pristine.
//
// STILL DERIVED, STILL DISPOSABLE (DESIGN.md §9). The index is reconstructible
// from the ChunkStore region files — the same "seed + persisted edits" the
// world itself reconstructs from — and RebuildFromStore does exactly that on
// load. It is never the authority for a voxel, never read by the sim, never
// hashed.
//
// WHAT THE CPU CAN AND CANNOT COMPUTE. The far-field cell rule is
//   byte = farSurfaceMat(material at the cell's center voxel, ...)
// and `farSurfaceMat` needs `genCellIn`, which exists only in WGSL. So this
// index deliberately stores the RAW MATERIAL at the sample voxel and nothing
// else; the surface-skin recolor is applied by the same GPU function the sieve
// and the downsample already call. That is what keeps a patched cell
// byte-identical to what `fardown` would have written for it — the
// sieve/downsample agreement invariant the `far-downsample` gate protects
// extends to the patch for free, instead of becoming a third rule that has to
// be kept in sync.
//
// RESOLUTION (the honest part). The sample rule is unchanged: a level-k cell
// is the single fine voxel at the center of the 2^(k + kFarShiftBase)-wide
// region it covers. An edit is therefore representable at level k only if it
// changes a voxel at a cell center — in practice, only if its linear extent
// reaches a whole cell. See DESIGN.md §9 for the per-level table.
class FarEdits {
 public:
  // GPU patch word, matching the `far` kernel's unpack: cell index inside its
  // level chunk (0..kChunkVol-1, chunk-linear x-fastest) in the low 12 bits,
  // raw material id in the next 12.
  static constexpr uint32_t kCellBits = 12;
  static constexpr uint32_t kCellMask = (1u << kCellBits) - 1u;
  static_assert(kChunkVol <= (1u << kCellBits), "cell index must fit 12 bits");
  static_assert(kMaterialSlots <= (1u << 12), "material id must fit 12 bits");

  // ---- THE GROUND SLOT, bits 24..31 of a patch word (2026-10-02) -----------
  // The centre sample alone cannot carry a FLUSH re-skin: a gravel track laid
  // at the ground voxel y == h, with the cell's centre one voxel under it,
  // changes no sample, so the patch never heard of it and every village path
  // beyond the window was grass. So the cell that worldgen's farSurfaceMat
  // colours from the ground (the one whose centre is the highest at or below
  // h) also carries the far slot of the voxel AT h on its sample column --
  // h from World::TerrainHeight, the CPU twin of genColumn's (the `sculpt`
  // gate pins the two together). The fine chunk that holds that voxel
  // speaks for it, as the chunk holding a centre speaks for the centre.
  //   kGroundUnknown    nothing known about the ground (a centre-only word)
  //   kGroundNotSolid   the ground voxel holds nothing a cascade cell keeps
  //   anything else     that voxel's far palette slot
  // A cell with ground knowledge but no edited centre carries kMatUntouched
  // in the material field: `farpatch` then keeps the sieve's byte and only
  // swaps in the ground slot. Compaction COMBINES the two fields of one cell
  // (newest known value of each) instead of keeping the last word.
  // Not past shift 5 (3.2 m cells): there the far skin is the flattened
  // canopy (farSurfaceMat), not the voxel.
  static constexpr uint32_t kGroundShift = 24;
  static constexpr uint32_t kGroundUnknown = 0, kGroundNotSolid = 0xFF;
  static constexpr uint32_t kMatUntouched = 0xFFF;
  static_assert(kFarPaletteBaseGpu <= kMatUntouched,
                "kMatUntouched must not be a real material id: real ids sit "
                "below the reserved palette runs");
  static constexpr uint32_t kGroundMaxShift = 5;
  // The ground source: per material id, its far slot when a cascade cell
  // keeps it (worldgen farCellIsSolid: not gas, not micro) and
  // kGroundNotSolid otherwise; and the world whose seed TerrainHeight uses.
  // Until it is set the index records centres only, as it always did.
  void SetGroundSource(std::vector<uint8_t> slotIfSolid, const World* world) {
    groundSlot_ = std::move(slotIfSolid);
    groundWorld_ = world;
    groundH_.clear();
  }

  // ---- THE CONTAINER RULE (2026-10-07; worldgen.wgsl farContainedBy) -------
  // At the fine levels (shift <= kContainMaxShift) a cell whose centre is not
  // already a WALL (far-solid, not liquid) takes the first CONTAINER WALL
  // voxel in it -- x fastest, then y, then z, from its min corner -- where a
  // container wall is a wall voxel with at least one LIQUID and at least one
  // OPEN (neither wall nor liquid) face neighbour: the voxel between a liquid
  // and open space. A 1-voxel basin wall is then never dropped for the air
  // outside it or the water inside it, while a lake bed or the ground under a
  // puddle (liquid and more ground, never open) is left alone. The GPU
  // downsample applies the same rule with the same order, classes and cap;
  // the two must agree or a refilled cell and a downsampled one differ (the
  // `far-contain` gate). What this side cannot see counts as OPEN, the GPU's
  // answer outside the window: a noted chunk's neighbours across its faces,
  // and under the edit layer procgen is guessed from the ground contract
  // (air over h, a wall of unknown material at or under it). Per material
  // id: kContainNone (open), kContainLiquid or kContainWall. Until it is set,
  // centres are recorded raw, as they always were.
  static constexpr int kContainMaxShift = 2;   // == FAR_CONTAIN_MAX_SHIFT
  // A candidate that FLOATS on the liquid is refused (worldgen.wgsl
  // farFloatsOn; ticket 2026-10-11_22-09-52): the first non-wall voxel
  // straight below it, within this reach, is liquid. So is a lone SPECK
  // (Despeck below). A wall holds liquid beside or above it, never under it.
  static constexpr int kFloatReach = 4;        // == FAR_FLOAT_REACH
  static constexpr uint8_t kContainNone = 0, kContainLiquid = 1, kContainWall = 2;
  // ContainedBy's matAt answers besides a material id.
  static constexpr int kMatAtUnseen = -1;   // counts as OPEN, never a candidate
  static constexpr int kMatAtGround = -2;   // a WALL of unknown material, never a candidate
  void SetContainSource(std::vector<uint8_t> kindOf) { containKind_ = std::move(kindOf); }

  // ---- THE COVER RULE (2026-10-10; worldgen.wgsl farCoverPick) -------------
  // At the levels whose cells lie inside one fine chunk (shift <=
  // kCoverMaxShift), a cell whose centre is not far-solid (air, a gas, a MICRO
  // plant: ContainKindOf == kContainNone) takes the dominant COVER material
  // (materials.h MaterialDef::farCover: leaves) of its eight sub-samples --
  // the voxels at c0 + o*half + half/2 per axis, o in {0,1}^3, x fastest --
  // when at least the dominant's per-level minimum of them are cover. Ties go
  // to the sub-sample first in that order. The GPU applies the same rule in
  // two places (the sieve's `farcover` from procgen trees, fardownFeat from
  // the live grid); this is the patch's half, for an evicted edited chunk.
  // Per material id: 0 = not cover, else farCoverWord's (slot | minimums << 8)
  // of the material's slot OWNER. Until it is set, no cover.
  static constexpr int kCoverMaxShift = 4;   // == FAR_COVER_MAX_SHIFT (== CHUNK_SHIFT)
  void SetCoverSource(std::vector<uint32_t> coverOf) { coverOf_ = std::move(coverOf); }

  // ---- THE LOOSE TOP (2026-10-10; worldgen.wgsl farRunTop, the pass in
  // fardownFeat; DESIGN.md "A thin layer keeps its footprint") ------------
  // A cell whose centre is far-solid takes the material of T, the top voxel of
  // the far-solid run starting at its centre, when that run ends inside the
  // cell's reach (T in [c + 1, c + step - 1], the voxel over T not far-solid),
  // T is LOOSE (a far-solid liquid or powder) and T is above the ground
  // contract h (World::TerrainHeight). A cell the container rule claimed is
  // left to it. The GPU sees the whole window; this side sees the noted chunk
  // and, when the caller has it, the chunk ABOVE (NoteChunk's `above`: the
  // same eviction batch; a load's rebuild has none) -- a run that leaves both
  // is a downsample-only answer, like a container wall whose liquid is across
  // a chunk face. The substituted word also carries the top's slot in its
  // ground field, so `farpatch`'s farSurfaceMat paints the layer and not the
  // skin over a cell that straddles h; and a column whose ground has a loose
  // layer on it files no ground word (the layer, not the ground, is its top).
  // Per material id: 1 when loose. Until it is set, no substitution.
  void SetLooseSource(std::vector<uint8_t> looseOf) { looseOf_ = std::move(looseOf); }
  // Material of the chunk directly above a noted chunk at chunk-local
  // (x, y, z), or -1 when the caller cannot see it.
  using AboveFn = std::function<int(int, int, int)>;
  uint32_t CoverOf(uint32_t mat) const { return mat < coverOf_.size() ? coverOf_[mat] : 0u; }

  // ---- A LONE VOXEL IS DRAWN AS ITS SURROUNDINGS (worldgen.wgsl farDespeck;
  // ticket 2026-10-11_22-09-52) ------------------------------------------------
  // A SPECK is a far-solid voxel whose far slot is not cover and which no
  // voxel of its 3x3x3 neighbourhood shares that slot with: a cloth scrap on a
  // lake, a blood drop, a flake of ash. Centre-sampled it was a whole 2^k
  // cube; instead the cell is drawn as the slot most of its six face
  // neighbours hold (not-far-solid is a slot of its own; ties to the first in
  // +x,-x,+y,-y,+z,-z order), as that neighbour's material, or air. Anything
  // `matAt` cannot see (kMatAtUnseen) or names only as ground (kMatAtGround)
  // counts as support: never despeck on a guess. Needs SetGroundSource (the
  // far slots) and SetCoverSource; until both are set, `mat` is returned.
  // Public for the far-contain gate's oracle.
  template <class F>
  uint32_t Despeck(uint32_t mat, int x, int y, int z, F&& matAt) const {
    if (groundSlot_.empty() || coverOf_.empty()) return mat;
    const uint32_t s = GroundSlotOf(mat);
    if (s == kGroundNotSolid || s == 0 || CoverOf(mat) != 0) return mat;
    auto slotOf = [&](int m) -> uint32_t {
      const uint32_t g = GroundSlotOf((uint32_t)m);
      return g == kGroundNotSolid ? 0u : g;
    };
    static constexpr int kDir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int fm[6];
    uint32_t fs[6];
    for (int j = 0; j < 6; j++) {
      fm[j] = matAt(x + kDir[j][0], y + kDir[j][1], z + kDir[j][2]);
      if (fm[j] < 0) return mat;
      fs[j] = slotOf(fm[j]);
      if (fs[j] == s) return mat;
    }
    for (int i = 0; i < 27; i++) {
      const int ox = i % 3 - 1, oy = (i / 3) % 3 - 1, oz = i / 9 - 1;
      if (std::abs(ox) + std::abs(oy) + std::abs(oz) < 2) continue;
      const int m = matAt(x + ox, y + oy, z + oz);
      if (m < 0 || slotOf(m) == s) return mat;
    }
    int bestJ = 0, bestN = 0;
    for (int j = 0; j < 6; j++) {
      int n = 0;
      for (int k = 0; k < 6; k++) n += fs[k] == fs[j] ? 1 : 0;
      if (n > bestN) { bestN = n; bestJ = j; }
    }
    return fs[bestJ] == 0 ? (uint32_t)kMatAir : (uint32_t)fm[bestJ];
  }
  // The cover rule for one cell (see SetCoverSource): `centre` its centre
  // material (already known not far-solid), `half` its half-width, `c0` its
  // min corner in whatever coordinates `matAt` takes. Returns the cell's
  // material. Public for the far-cover gate's oracle.
  template <class F>
  uint32_t CoverPick(uint32_t centre, uint32_t level, int half, const int c0[3], F&& matAt) const {
    uint32_t slot[8], mat[8];
    uint32_t n = 0;
    const int q = half >> 1;
    for (int o = 0; o < 8; o++) {
      mat[o] = matAt(c0[0] + (o & 1) * half + q, c0[1] + ((o >> 1) & 1) * half + q,
                     c0[2] + ((o >> 2) & 1) * half + q);
      const uint32_t cw = CoverOf(mat[o]);
      slot[o] = cw & 0xFFu;
      if (cw != 0) n++;
    }
    int best = 0, bestO = -1;
    for (int i = 0; i < 8; i++) {
      if (slot[i] == 0) continue;
      int c = 0;
      for (int j = 0; j < 8; j++) c += slot[j] == slot[i] ? 1 : 0;
      if (c > best) { best = c; bestO = i; }
    }
    if (bestO < 0) return centre;
    const uint32_t minN = (CoverOf(mat[bestO]) >> (8 + (level - 1) * 4)) & 15u;
    return (minN != 0 && n >= minN) ? mat[bestO] : centre;
  }

  // ---- THE THIN RULE (2026-10-14; worldgen.wgsl farThinPick, DESIGN.md "A
  // thin feature keeps its cell") -------------------------------------------
  // At the levels whose cells are at most kThinMaxShift (levels 1-2), a cell
  // that holds no matter after the centre and the cover rule takes the
  // dominant far slot among its eight sub-samples (CoverPick's positions)
  // that are THIN -- far-solid, not loose, not cover, and with BOTH face
  // neighbours not far-solid along at least one axis -- when at least
  // kThinMin of them are. Ties go to the first sub-sample in scan order; the
  // material returned is that sub-sample's. `matAt` answers a material id,
  // kMatAtUnseen or kMatAtGround; both of those count as solid and are never
  // a candidate (no claim on a guess). Needs SetGroundSource (far slots),
  // SetCoverSource and SetLooseSource; until they are set, `centre` is
  // returned. Public for the gates' oracles.
  static constexpr int kThinMaxShift = 2;   // == FAR_THIN_MAX_SHIFT
  static constexpr int kThinMin = 2;        // == FAR_THIN_MIN
  bool ThinReady() const { return !groundSlot_.empty() && !coverOf_.empty() && !looseOf_.empty(); }
  template <class F>
  uint32_t ThinPick(uint32_t centre, int half, const int c0[3], F&& matAt) const {
    if (!ThinReady()) return centre;
    auto solid = [&](int m) {
      if (m < 0) return true;   // unseen / procgen ground: solid, no claim
      return m != (int)kMatAir && GroundSlotOf((uint32_t)m) != kGroundNotSolid;
    };
    uint32_t slot[8], mat[8];
    int pos[8][3];
    const int q = half >> 1;
    for (int o = 0; o < 8; o++) {
      pos[o][0] = c0[0] + (o & 1) * half + q;
      pos[o][1] = c0[1] + ((o >> 1) & 1) * half + q;
      pos[o][2] = c0[2] + ((o >> 2) & 1) * half + q;
      const int m = matAt(pos[o][0], pos[o][1], pos[o][2]);
      mat[o] = m < 0 ? 0u : (uint32_t)m;
      slot[o] = 0;
      if (m <= 0 || !solid(m) || LooseOf((uint32_t)m) || CoverOf((uint32_t)m) != 0) continue;
      slot[o] = GroundSlotOf((uint32_t)m);
    }
    // Only a slot two candidates share can win: ask the others' neighbours
    // nothing (worldgen.wgsl farThinNeedMask), then keep the thin ones.
    uint32_t thinSlot[8];
    for (int o = 0; o < 8; o++) {
      thinSlot[o] = 0;
      if (slot[o] == 0) continue;
      int c = 0;
      for (int j = 0; j < 8; j++) c += slot[j] == slot[o] ? 1 : 0;
      if (c < kThinMin) continue;
      bool thin = false;
      for (int a = 0; a < 3 && !thin; a++) {
        int p[3] = {pos[o][0], pos[o][1], pos[o][2]};
        p[a] += 1;
        const bool up = !solid(matAt(p[0], p[1], p[2]));
        p[a] -= 2;
        const bool dn = !solid(matAt(p[0], p[1], p[2]));
        thin = up && dn;
      }
      if (thin) thinSlot[o] = slot[o];
    }
    // A candidate the neighbour test dropped no longer counts for its slot:
    // the count is among the thin ones (farThinPick).
    int best = 0, bestO = -1;
    for (int i = 0; i < 8; i++) {
      if (thinSlot[i] == 0) continue;
      int c = 0;
      for (int j = 0; j < 8; j++) c += thinSlot[j] == thinSlot[i] ? 1 : 0;
      if (c > best) { best = c; bestO = i; }
    }
    return (bestO >= 0 && best >= kThinMin) ? mat[bestO] : centre;
  }

  // Forgets every NOTED edit. The BASE (SetBase) survives: it is the authored
  // edit layer, which belongs to the map, not to the store being replaced.
  void Clear() {
    byChunk_.clear();
    merged_.clear();
    cells_ = 0;
  }

  // ---- THE BASE: the authored edit layer (sim/worldedit.h, map-overhaul P7) --
  // The sieve fills a level chunk from procgen and procgen has never heard of
  // the edit layer, so without this the horizon shows the layer only where a
  // layer chunk has been evicted MODIFIED — and P7 stops layer-only chunks
  // counting as modified. The base is the layer's cells, swept into cascade
  // samples exactly like a noted chunk (only the cells that ARE a sample
  // center speak, the same resolution rule), and laid UNDER the noted edits:
  // a crater dug into a layer structure and evicted wins over the structure.
  // Replaces any earlier base; an empty list clears it.
  //
  // THE BASE ALSO CARRIES THE STAMPS' CONTAINER CELLS (2026-10-07). A stamped
  // structure is pristine procgen: its chunks are never dirty, so its cascade
  // cells come from the sieve alone, and the sieve centre-samples -- a
  // 1-voxel basin wall on the unsampled parity is lost and the liquid behind
  // it shows (Harrowby's quench trough). The rule (kContainMaxShift above) is
  // a pure function of the template and the pad, so it is evaluated HERE,
  // once per whole-window generation, and its few changed cells ride the
  // patch the sieve already applies -- laid under the layer's own words. The
  // first cut evaluated it inside the `far` kernel instead and cost that
  // entry ~45 s of driver compile per worldgen.wgsl edit (16.8 s -> 62 s).
  // Called with {} when the map has no layer (worldedit.cpp QueueWindow), so
  // the stamps' half is always current.
  struct BaseCell {
    int32_t x, y, z;   // world voxel
    uint32_t mat;      // raw material id
  };
  void SetBase(const std::vector<BaseCell>& cells);
  size_t BaseLevelChunks() const { return base_.size(); }
  // The stamps' thin-rule words of the last SetBase (StampThin).
  size_t StampThinWords() const { return stampThinCount_; }
  // ---- THE INDEX IS CAPPED (2026-09-24) ------------------------------------
  // It was an unbounded unordered_map, and what feeds it is every chunk the
  // stream's sticky `modified_` calls written — which includes a chunk that
  // was only ever dirty for SMOKE drifting through or water settling, so a
  // session that burned a forest grew ~585 patch words per touched chunk,
  // forever. Past this many recorded words a NEW level chunk is refused and
  // counted (RefusedChunks); level chunks already in the index keep updating,
  // so a re-noted edit is never lost to the cap. 2^24 words is 64 MiB — about
  // 28,000 edited fine chunks' worth at 585 words each.
  static constexpr size_t kMaxCells = size_t(1) << 24;
  uint64_t RefusedChunks() const { return refused_; }
  bool Empty() const { return byChunk_.empty() && base_.empty(); }
  size_t LevelChunks() const { return byChunk_.size(); }
  // Samples recorded (before compaction — a cell re-noted by a second
  // eviction of the same chunk counts twice until its level chunk is
  // compacted). Diagnostics, and the cap's measure.
  size_t Cells() const { return cells_; }

  // Record one fine chunk's contribution to every cascade level. `words` is
  // kChunkVol voxel words in chunk-linear order (the same layout genChunk and
  // RleDecodeChunk use). Re-noting a chunk REPLACES its previous entries in
  // effect: a fine chunk always contributes the SAME cells, the newer word for
  // a cell wins at compaction, and a level chunk whose appended runs outgrow
  // twice its cell count is compacted on the spot (Append), so storage stays
  // bounded however often a chunk is re-noted.
  void NoteChunk(IVec3 wc, const uint32_t* words, const AboveFn& above = {});
  // Same, for a page-table sentinel slot whose whole content is one material
  // (EMPTY / UNIFORM / JITTER — JITTER varies only the palette nibble, which
  // the far field does not store).
  void NoteUniformChunk(IVec3 wc, uint32_t mat, const AboveFn& above = {});

  // Patch words for one level chunk (1-based level, coord in level-CHUNK
  // units), or nullptr when that chunk holds no edits. One word per cell,
  // sorted COLUMN-MAJOR — by (z, x, y) of the cell, see ColumnKey — so the
  // `farpatch` kernel, which gives each thread a contiguous run, finds the
  // cells of one (x, z) column adjacent and builds that column's genColumn
  // once instead of once per cell.
  //
  // NOT const, because this is where a level chunk's appended runs are
  // COMPACTED. Merging on every NoteChunk would be quadratic on a bulk build:
  // a level-1 chunk is fed by 64 fine chunks in arbitrary order, so 64 sorted
  // merges into a vector growing to 4,096 is ~130k element copies per level
  // chunk, and RebuildFromStore over a fully-flushed save does that for every
  // level chunk of the window. Appending and sorting once, on first read, is
  // O(n log n) total and moves the cost onto the chunks the fill queue
  // actually asks about.
  const std::vector<uint32_t>* Lookup(uint32_t level, IVec3 levelChunk);

  // Re-derive the whole index from the persisted world. Returns the number of
  // stored chunks visited. O(store) — a load-time cost, never a frame cost.
  size_t RebuildFromStore(ChunkStore& store);

  // The level chunk that owns fine chunk `wc` at 1-based `level`. One fine
  // chunk always lies inside exactly one level chunk: a level-k chunk is
  // 2^(k + kFarShiftBase + 4) fine voxels wide and aligned, which is a
  // multiple of the 16-voxel fine chunk for every k >= 1.
  static IVec3 LevelChunkOf(IVec3 wc, uint32_t level) {
    const int s = (int)(level + kFarShiftBase);
    return {wc.x >> s, wc.y >> s, wc.z >> s};
  }

 private:
  struct LKey {
    int32_t x, y, z;
    uint32_t level;
    bool operator==(const LKey& o) const {
      return x == o.x && y == o.y && z == o.z && level == o.level;
    }
  };
  struct LKeyHash {
    size_t operator()(const LKey& k) const noexcept {
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](uint32_t v) { h ^= v; h *= 1099511628211ull; };
      mix((uint32_t)k.x);
      mix((uint32_t)k.y);
      mix((uint32_t)k.z);
      mix(k.level);
      return (size_t)(h ^ (h >> 32));
    }
  };

  // One level chunk's patch words. `sorted` is false while unread appends are
  // outstanding; Lookup compacts (stable sort by cell index, last write per
  // cell wins) and sets it.
  struct Chunk {
    std::vector<uint32_t> words;
    bool sorted = true;
  };
  // The cell index is x-fastest (x + y*16 + z*256); this reorders it to
  // (z, x, y) so the cells of one column are adjacent. A bijection of the cell
  // index, so "equal key" is still "same cell" for the dedupe.
  static uint32_t ColumnKey(uint32_t w) {
    const uint32_t ci = w & kCellMask;
    return (ci & ~0xFFu) | ((ci & 15u) << 4) | ((ci >> 4) & 15u);
  }
  // Sort by ColumnKey (stable: the newest of equal cells is the last) and keep
  // the last word of each cell. Lookup's lazy compaction and Append's bound.
  void Compact(Chunk& c);
  // Append `add` (itself sorted, one word per cell) to the level chunk's list.
  // Later appends win on a repeated cell — re-noting the same fine chunk must
  // not grow the compacted list, and the newer read is the truth.
  void Append(const LKey& key, const std::vector<uint32_t>& add);

  // The per-level sample sweep shared by NoteChunk / NoteUniformChunk: calls
  // `sample(fineOffsetInChunk)` for every cascade cell center that lands in
  // this chunk and returns the raw material for it.
  void Sweep(IVec3 wc, const std::function<uint32_t(int, int, int)>& sample,
             const AboveFn& above);
  std::vector<uint8_t> looseOf_;   // SetLooseSource
  bool LooseOf(uint32_t mat) const { return mat < looseOf_.size() && looseOf_[mat] != 0; }

  std::unordered_map<LKey, Chunk, LKeyHash> byChunk_;
  // The base (compacted, ColumnKey order) and, per level chunk that has BOTH a
  // base and noted words, their merge (noted wins per cell). A merge is
  // dropped whenever its noted list is appended to or the base is replaced.
  std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash> base_;
  std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash> merged_;
  size_t cells_ = 0;                // appended samples, before compaction
  uint64_t refused_ = 0;            // new level chunks refused at kMaxCells
  std::vector<uint32_t> scratch_;   // per-level `add` list

  // The ground source (SetGroundSource) and a TerrainHeight cache, keyed on
  // (seed, x, z): the same sample columns are asked for by every chunk of a
  // column and every re-note of it. Bounded by a wholesale clear.
  std::vector<uint8_t> groundSlot_;
  std::vector<uint8_t> containKind_;   // SetContainSource
  uint8_t ContainKindOf(uint32_t mat) const {
    return mat < containKind_.size() ? containKind_[mat] : kContainNone;
  }
  // The rule for one cell: `centre` is its centre material, `c0` its min
  // corner in whatever coordinates `matAt` takes; matAt returns a material id,
  // kMatAtUnseen or kMatAtGround. Returns the cell's material.
  uint32_t ContainedBy(uint32_t centre, int step, const int c0[3],
                       const std::function<int(int, int, int)>& matAt) const;
  std::vector<uint32_t> coverOf_;   // SetCoverSource
  // THE CONTAINER RULE FOR STAMPS (SetBase's first contribution): the cells
  // of every map stamp holding liquid where the rule changes the centre
  // sample, filed into `out` as patch words. See SetBase.
  void StampContainers(std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash>& out);
  // THE THIN RULE FOR STAMPS (SetBase's other stamp contribution; ThinPick
  // over every map stamp's block, its pad and the ground contract), filed
  // into `out`. Cached across SetBase calls on a signature of the stamps'
  // blocks and placement: a map's stamps are evaluated once per content.
  void StampThin(std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash>& out);
  std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash> stampThinWords_;
  uint64_t stampThinSig_ = 0;
  bool stampThinValid_ = false;
  size_t stampThinCount_ = 0;
  const World* groundWorld_ = nullptr;
  std::unordered_map<uint64_t, int> groundH_;
  uint32_t groundMapHash_ = 0;   // the map groundH_ was filled under (GroundHeight)
  int GroundHeight(int x, int z);
  uint32_t GroundSlotOf(uint32_t mat) const {
    return mat < groundSlot_.size() ? groundSlot_[mat] : kGroundNotSolid;
  }
  // The ground word for sample column (x, z) at `level` whose ground voxel
  // holds `mat`, filed into `out` under the level chunk of the cell it colours.
  void GroundWord(uint32_t level, int x, int h, int z, uint32_t mat,
                  std::unordered_map<LKey, std::vector<uint32_t>, LKeyHash>& out) const;
  // Combine one cell's words (append order) into one: the newest known
  // material, the newest known ground slot.
  static uint32_t CombineCell(const uint32_t* w, size_t n);
};
