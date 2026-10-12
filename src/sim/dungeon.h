// dungeon.h -- THE DUNGEON SHELL: a primitive program carved into the world by
// worldgen (docs/PLAN_dungeons.md §2, §3.4; the D1 AMENDMENT §3.4.1 is the
// format contract; DESIGN.md "Dungeons").
//
// WHAT THIS MODULE OWNS, in the order data flows through it:
//
//   1. THE ASSET. assets/dungeons/<id>/shell.bin (and its human-readable twin
//      shell.json, the same CONTENT) -- the compiled program the JS generator
//      library (assets/editor/dungeon/dungeongen.js) writes. ParseShellJson /
//      ReadShellBin read either into a `Shell`; WriteShellBin writes the
//      binary; SameContent compares two. Material references are STRINGS
//      (slot or materials.json name), resolved at load (ResolveShell).
//   2. THE PLACEMENT. map.json `dungeons[]` (worldmap.h DungeonDef; parsed and
//      anchored by LoadWorldMap). Rigid in the dungeon's local frame: world =
//      (at.x, groundY + yOffset, at.z) + R(yaw) (local - anchor), R linear.
//   3. THE PACKING. PackDungeons appends the GPU block (layout below) to the
//      worldMap buffer behind header word worldmap::kHDungeon: one record per
//      placed dungeon, the prims as fixed records in LOCAL coordinates, the
//      scatter rules, the rotated prop templates and their instances, and a
//      SPARSE 3D INDEX -- a hash table of 64^3 index cells -> the list of prims
//      and instances whose inflated bounds touch the cell. Its size is
//      proportional to the cells a dungeon touches, whatever the dungeon's
//      extent; nothing goes through the 2D site lattice.
//   4. THE EVALUATOR'S CPU TWIN. EvalCell is worldgen_dungeon.wgsli's dgCell,
//      token for token between the MIRROR markers (check_invariants.py
//      "dungeon mirror"), reading the same packed words. The `dungeon-gen`
//      gate holds the GPU to it cell by cell.
//
// DETERMINISM: everything here is integer and a pure function of the asset
// bytes, the map and the material table. The packed words are in prim order;
// the hash table is built in a fixed key order.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct MaterialDef;

namespace dungeon {

// ---- the shell, as authored (PLAN §3.4.1) ----------------------------------
enum Op : uint32_t { kOpCarve = 0, kOpFill = 1, kOpSkin = 2, kOpKeep = 3 };
enum Shape : uint32_t {
  kShapeBox = 0, kShapeRBox = 1, kShapeEllipsoid = 2, kShapeCylinder = 3,
  kShapeCapsule = 4, kShapeRamp = 5, kShapeVault = 6, kShapeTube = 7,
};
enum ScatterKind : uint32_t {
  kScRubble = 0, kScStalagmite = 1, kScStalactite = 2, kScCrystal = 3,
  kScMoss = 4, kScPuddle = 5, kScBones = 6, kScOre = 7,
};
enum Surface : uint32_t { kSurfFloor = 0, kSurfCeil = 1, kSurfWall = 2, kSurfAny = 3 };
inline constexpr uint32_t kNoStr = 0xFFFFFFFFu;
inline constexpr int32_t kNoLevel = INT32_MIN;

struct Prim {
  uint32_t op = kOpCarve, shape = kShapeBox;
  uint32_t mat = kNoStr;            // string index
  std::array<int32_t, 3> a{}, b{};  // min / max, or capsule a / b
  int32_t r = 0;                    // rbox r | capsule r | vault rise | ramp stepH
  uint32_t axis = 0;                // cylinder / vault axis (0 x 1 y 2 z) | ramp dir (0 +x 1 -x 2 +z 3 -z)
  int32_t stepD = 0, height = 0, below = 0;   // ramp
  int32_t noiseAmp = 0, noiseScale = 0, noiseOct = 0;
  uint32_t noiseSeed = 0;
  int32_t blend = 0, skin = 0;
  int32_t level = kNoLevel;
  uint32_t firm = kNoStr;           // string index
  uint32_t tubeFirst = 0, tubeCount = 0;
  uint32_t scatterFirst = 0, scatterCount = 0;
  uint32_t piece = kNoStr;          // string index (informational)
  // The optional EXTRAS (PLAN §3.4.1 "PRMX"): a fill that lands only in OPEN
  // space (what the carve, or the sky, left air -- a rubble heap must not turn
  // the buried rock inside a wall to gravel), and a CHECKER of `checker`-cell
  // squares in the local XZ plane alternating `mat` / `mat2` (fill, skin).
  bool openOnly = false;
  uint32_t mat2 = kNoStr;           // string index
  int32_t checker = 0;              // cells; 0 = no pattern
  bool HasExtras() const { return openOnly || mat2 != kNoStr || checker != 0; }
};
struct TubePt { int32_t x = 0, y = 0, z = 0, r = 0; };
struct Scatter {
  uint32_t kind = kScRubble;
  uint32_t densityQ16 = 0;
  int32_t sizeMin = 1, sizeMax = 1;
  uint32_t surface = kSurfFloor;
  uint32_t mat = kNoStr;            // string index; kNoStr = the kind's default
  uint32_t seed = 0;
  int32_t spacing = 0;              // 0 = default
};
struct Template {
  uint32_t id = kNoStr;
  int32_t nx = 0, ny = 0, nz = 0;
  std::vector<uint32_t> palette;    // string indices ("@carve" = the carve code)
  std::vector<std::array<int32_t, 4>> voxels;   // x, y, z, palette index
};
struct Instance {
  uint32_t tmpl = 0;
  int32_t x = 0, y = 0, z = 0;
  uint32_t yawQ = 0;                // quarter turns
  uint32_t id = kNoStr;
};
struct Shell {
  uint32_t version = 1;
  std::string dungeon;              // the "dungeon" field (json only; not in the binary)
  uint32_t sourceHash = 0;
  bool naturalCaves = false;
  std::array<int32_t, 3> anchor{};
  int32_t keepOut = 24;
  std::vector<std::string> strings;
  std::vector<std::pair<uint32_t, uint32_t>> materials;   // (slot, material name) string indices
  std::vector<Prim> prims;
  std::vector<TubePt> tube;
  std::vector<Scatter> scatter;
  std::vector<Template> templates;
  std::vector<Instance> instances;
  const std::string& Str(uint32_t i) const;
  uint32_t Intern(const std::string& s);
};

inline constexpr uint32_t kBinMagic = 0x47445653u;   // 'SVDG'
inline constexpr uint32_t kBinEnd = 0x21444E45u;     // 'END!'
inline constexpr uint32_t kBinPrimExtras = 0x584D5250u;   // 'PRMX': the optional extras block
inline constexpr uint32_t kBinVersion = 1u;
inline constexpr uint32_t kPrimWordsBin = 26u;
inline constexpr uint32_t kScatterWordsBin = 8u;

bool ParseShellJson(const std::string& text, Shell& out, std::string& err);
bool ReadShellBin(const uint8_t* data, size_t n, Shell& out, std::string& err);
std::vector<uint8_t> WriteShellBin(const Shell& s);
// The canonical JSON form (what --dungeon-bake and the tuner see): ParseShellJson
// of it yields SameContent.
std::string ShellToJson(const Shell& s);
// CONTENT equality: every field and every string by VALUE (two writers may
// intern strings in different orders). `why` names the first difference.
bool SameContent(const Shell& a, const Shell& b, std::string* why);
// FNV-1a 32 of a file's bytes with every '\r' dropped (PLAN §3.4.1
// sourceHash: a CRLF checkout must not read as stale). False if unreadable.
bool SourceHashOfFile(const std::string& path, uint32_t& out);
// assets/dungeons/<id>/: shell.bin, else shell.json (with a note in `used`).
bool LoadShellAsset(const std::string& assetDir, const std::string& id, Shell& out,
                    std::string& err, std::string* used = nullptr);
// A dungeon.json / shell.json / shell.bin path (voxserve DUNGEON's <src>).
bool LoadShellPath(const std::string& path, Shell& out, std::string& err, std::string* used = nullptr);

// ---- resolution: strings -> material ids, limits ---------------------------
// Per prim / scatter / template the engine ids. Validates every limit of
// PLAN §3.4.1 and names the offending prim. matClass is by engine id.
inline constexpr uint32_t kCarveMat = 0xFFFu;   // a template cell that clears to air
struct Resolved {
  std::vector<uint32_t> primMat, primFirm;      // 0 = none / air
  std::vector<uint32_t> primMat2;               // the checker's second material, 0 = none
  std::vector<uint32_t> scatterMat;
  std::vector<std::vector<uint32_t>> templatePalette;   // ids, kCarveMat for "@carve"
};
bool ResolveShell(const Shell& s, const std::vector<std::string>& matNames,
                  const std::vector<uint32_t>& matClass, Resolved& out, std::string& err);

// ---- the quarter-turn frame (PLAN §3.4.1 / §3.7 R) ------------------------
// R(q) on cell coordinates, LINEAR (a rotation about the anchor cell): q1
// (x, z) -> (z, -x), q2 (-x, -z), q3 (-z, x). The placement is
// world = base + R(yaw) (local - anchor), D3's world/dungeon_refs.* rule, so
// the shell, the props, the detail layer and the refs land on the same cells.
inline int RotX(int q, int x, int z) { return q == 1 ? z : q == 2 ? -x : q == 3 ? -z : x; }
inline int RotZ(int q, int x, int z) { return q == 1 ? -x : q == 2 ? -z : q == 3 ? x : z; }

// One placed dungeon as the packer and the twin need it.
struct Placed {
  std::string id;
  std::shared_ptr<const Shell> shell;
  std::shared_ptr<const Resolved> res;
  int yawQ = 0;
  std::array<int, 3> origin{};      // world cell of local (0, 0, 0)
  std::array<int, 3> boxMin{}, boxMax{};     // world, inflated, inclusive
  std::array<int, 3> keepMin{}, keepMax{};   // world; keepMax < keepMin = none
};
// The world box + keep box of a shell placed with `yawQ` at `origin`
// (inflated by noise, blend, skin, scatter, the firm shell).
void PlacedBounds(Placed& p);
// Anchor -> origin: the world cell `base` the anchor cell lands on.
std::array<int, 3> OriginFor(const Shell& s, int yawQ, std::array<int, 3> base);

// ---- the GPU block (worldgen_dungeon.wgsli DG_*; check_invariants.py) -----
enum : uint32_t {
  kDgH_Count = 0, kDgH_Recs = 1, kDgH_Hash = 2, kDgH_HashMask = 3, kDgH_ProbeMax = 4,
  kDgH_CellX0 = 5, kDgH_CellY0 = 6, kDgH_CellZ0 = 7, kDgH_CellX1 = 8, kDgH_CellY1 = 9, kDgH_CellZ1 = 10,
  kDgH_GenX0 = 11, kDgH_GenY0 = 12, kDgH_GenZ0 = 13, kDgH_GenX1 = 14, kDgH_GenY1 = 15, kDgH_GenZ1 = 16,
  kDgH_KeepX0 = 17, kDgH_KeepY0 = 18, kDgH_KeepZ0 = 19, kDgH_KeepX1 = 20, kDgH_KeepY1 = 21, kDgH_KeepZ1 = 22,
  kDgH_Flags = 23, kDgFlagEvalOff = 1,
  kDgH_Words = 24,
  kDgD_OriginX = 0, kDgD_OriginY = 1, kDgD_OriginZ = 2, kDgD_Yaw = 3, kDgD_Box = 4, kDgD_Keep = 10,
  kDgD_Words = 16,
  kDgP_Flags = 0, kDgP_Mats = 1, kDgP_AX = 2, kDgP_AY = 3, kDgP_AZ = 4, kDgP_BX = 5, kDgP_BY = 6,
  kDgP_BZ = 7, kDgP_P0 = 8, kDgP_P1 = 9, kDgP_P2 = 10, kDgP_Level = 11, kDgP_NAmp = 12, kDgP_NSeed = 13,
  kDgP_Blend = 14, kDgP_Scat = 15, kDgP_DRec = 16, kDgP_Pat = 17, kDgP_Words = 20,
  kDgPF_OpenOnly = 16777216,   // 1 << 24 in DG_P_FLAGS: the fill lands only in open (air) cells
  kDgS_Kind = 0, kDgS_Dens = 1, kDgS_Size = 2, kDgS_Mat = 3, kDgS_Seed = 4, kDgS_Spacing = 5, kDgS_Words = 8,
  kDgT_Words = 16,
  kDgI_Blk = 0, kDgI_X = 1, kDgI_Y = 2, kDgI_Z = 3, kDgI_Words = 4,
  kDgE_Words = 4,
  kDgCellLog2 = 6,
  kDgListMax = 64,
  kDgInstMax = 32,
};
// What PackDungeons reports (the gates and voxserve's info.json print it).
struct PackStats {
  uint32_t prims = 0, instances = 0, templates = 0, indexCells = 0, listWords = 0;
  uint32_t hashCap = 0, probeMax = 0, words = 0, worstList = 0;
};
// Append the block to `W` and point W[kHDungeon] at it (left 0 for none).
// False + `log` when a cell's list is over kDgListMax / kDgInstMax.
// `evalOff` packs everything but sets kDgFlagEvalOff (worldmap.h
// SetDungeonsEnabled): no list is ever found, so no cell is evaluated.
bool PackDungeons(const std::vector<Placed>& dungeons, std::vector<uint32_t>& W,
                  std::string& log, PackStats* stats = nullptr, bool evalOff = false);

// ---- the CPU twin -----------------------------------------------------------
// dgCell over the packed words `W` (a whole worldMap buffer), for terrain word
// `tw` at world cell (x, y, z): the word the GPU writes. `mats` supplies the
// class / repose words (MaterialGpu) the shader reads from `materials`; `seed`
// is T.seed (the palette jitter).
uint32_t TwinCell(const std::vector<uint32_t>& W, const std::vector<MaterialDef>& mats, uint32_t seed,
                  int x, int y, int z, uint32_t tw);
// The list offset dgListAt would find (0 = none), and the carve field there.
uint32_t TwinListAt(const std::vector<uint32_t>& W, int x, int y, int z);
int TwinCarveAt(const std::vector<uint32_t>& W, const std::vector<MaterialDef>& mats, uint32_t seed,
                int x, int y, int z);

}  // namespace dungeon
