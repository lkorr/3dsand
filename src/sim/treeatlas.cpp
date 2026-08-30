#include "sim/treeatlas.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "sim/tuning.h"   // worldgen.treeTile, for the candidate-set bound

namespace fs = std::filesystem;
using namespace treeatlas;

namespace {

/** One .svtree file, parsed but not yet rebased into the global buffer. */
struct File {
  std::string name;                 // file stem
  std::vector<uint32_t> words;      // the file, verbatim
  std::vector<std::string> names;   // its material name table
  int variantCount = 0;
  int varDirOff = 0;
  int reach = 0, above = 0, crownY = 0, crownR = 0;
  int biome[kBiomeCount] = {0, 0, 0, 0};
  int minY = -1, maxY = -1, maxSlope = 0, sparsity = 1;
  int canopyLocal = 0;   // local palette index of the far-field proxy material
  int shade = 0;
  int autumnChance = 0;
  int leafLocal[3] = {0, 0, 0};
  int autumnLocal[3] = {0, 0, 0};
};

bool ReadFileWords(const fs::path& p, std::vector<uint32_t>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamsize n = f.tellg();
  if (n <= 0 || (n & 3) != 0) return false;
  out.resize(static_cast<size_t>(n) / 4);
  f.seekg(0);
  return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), n));
}

/** Parse and VALIDATE one file. Every offset in a .svtree is a word index into
 *  the file itself, and every one of them is checked here rather than trusted
 *  — an out-of-range column offset would be a GPU read of whatever else is in
 *  the atlas buffer, which is a garbage forest with no error anywhere. */
bool ParseFile(const fs::path& path, File& out, std::string& err) {
  if (!ReadFileWords(path, out.words)) { err = "unreadable"; return false; }
  const auto& w = out.words;
  const size_t n = w.size();
  if (n < static_cast<size_t>(kFileHeaderWords)) { err = "truncated header"; return false; }
  if (w[0] != kFileMagic) { err = "bad magic"; return false; }
  if (w[1] != 1u) { err = "unsupported version " + std::to_string(w[1]); return false; }
  if (w[2] != static_cast<uint32_t>(kFileHeaderWords)) {
    err = "header is " + std::to_string(w[2]) + " words, expected " +
          std::to_string(kFileHeaderWords);
    return false;
  }
  if (w[7] != n) {
    err = "declares " + std::to_string(w[7]) + " words, file holds " +
          std::to_string(n);
    return false;
  }
  out.variantCount = static_cast<int>(w[3]);
  const int nameCount = static_cast<int>(w[4]);
  const uint32_t nameOff = w[5];
  out.varDirOff = static_cast<int>(w[6]);
  out.reach = static_cast<int>(w[8]);
  out.above = static_cast<int>(w[9]);
  out.crownY = static_cast<int>(w[10]);
  out.crownR = static_cast<int>(w[11]);
  for (int i = 0; i < kBiomeCount; i++) out.biome[i] = static_cast<int>(w[12 + i]);
  out.minY = static_cast<int32_t>(w[16]);
  out.maxY = static_cast<int32_t>(w[17]);
  out.maxSlope = static_cast<int>(w[18]);
  out.sparsity = std::max(1, static_cast<int>(w[19]));
  out.canopyLocal = static_cast<int>(w[20]);
  out.shade = static_cast<int>(w[21]);
  out.autumnChance = static_cast<int>(w[22]);
  for (int i = 0; i < 3; i++) {
    out.leafLocal[i] = static_cast<int>(w[23 + i]);
    out.autumnLocal[i] = static_cast<int>(w[26 + i]);
  }

  if (out.variantCount <= 0 || out.variantCount > 64) {
    err = "variant count " + std::to_string(out.variantCount); return false;
  }
  if (nameOff >= n) { err = "name table offset out of range"; return false; }
  const uint32_t nameBytes = w[nameOff];
  if (nameOff + 1 + (nameBytes + 3) / 4 > n) { err = "name table truncated"; return false; }
  {
    std::string blob(reinterpret_cast<const char*>(&w[nameOff + 1]), nameBytes);
    size_t at = 0;
    while (at <= blob.size() && out.names.size() < 4096) {
      size_t nl = blob.find('\n', at);
      if (nl == std::string::npos) { if (at < blob.size()) out.names.push_back(blob.substr(at)); break; }
      out.names.push_back(blob.substr(at, nl - at));
      at = nl + 1;
    }
  }
  if (static_cast<int>(out.names.size()) != nameCount) {
    err = "name table holds " + std::to_string(out.names.size()) + " names, header says " +
          std::to_string(nameCount);
    return false;
  }

  if (out.varDirOff + out.variantCount * kVariantWords > static_cast<int>(n)) {
    err = "variant directory out of range"; return false;
  }
  for (int v = 0; v < out.variantCount; v++) {
    const uint32_t* d = &w[out.varDirOff + v * kVariantWords];
    const uint64_t nx = d[kVNx], ny = d[kVNy], nz = d[kVNz];
    if (nx == 0 || ny == 0 || nz == 0 || nx > 512 || ny > 512 || nz > 512) {
      err = "variant " + std::to_string(v) + " has bad dimensions"; return false;
    }
    if (d[kVAnchorX] >= nx || d[kVAnchorZ] >= nz) {
      err = "variant " + std::to_string(v) + " anchor outside the grid"; return false;
    }
    const uint64_t cols = d[kVColumns];
    if (cols + nx * nz * 2 > n) {
      err = "variant " + std::to_string(v) + " column table out of range"; return false;
    }
    // Spot-check every column's run span. This is O(columns) at load and it is
    // the check that matters: a bad run offset is a silent out-of-bounds read
    // on the GPU, where there is no reporting at all.
    for (uint64_t c = 0; c < nx * nz; c++) {
      const uint64_t off = w[cols + c * 2], cnt = w[cols + c * 2 + 1];
      if (cnt && (off >= n || off + cnt > n)) {
        err = "variant " + std::to_string(v) + " column " + std::to_string(c) +
              " runs out of range"; return false;
      }
    }
  }
  return true;
}

}  // namespace

bool LoadTreeAtlas(const std::string& dir, const std::vector<MaterialDef>& mats,
                   TreeAtlas& out, std::string& log) {
  out = TreeAtlas{};
  char buf[512];

  // AN ENGINE MATERIAL ID IS THE INDEX INTO `mats`, NOT THE INDEX PLUS ONE.
  // `mats` already carries AIR at index 0 — LoadAssets synthesises it and it is
  // not in materials.json — so the +1 that a 1-based table would need has
  // already been paid for by that entry. Every other resolver in the engine
  // agrees (FindMaterial in materials.cpp, FindMaterialId in debris.cpp and
  // mob.cpp all return `(int)i`), and so does VoxPaletteJson, which emits
  // `"id": i` for the browser.
  //
  // This line read `i + 1` and it is worth saying exactly what that cost,
  // because the failure was SILENT: every name still resolved, so nothing was
  // reported, and every tree simply wore the material one slot along from the
  // one it named. `wood` became `sand` (a POWDER: trunks rained out of the
  // canopy, and the branches they had been holding up came loose and were
  // converted to rigidbodies by the support scan), `birch_wood` became `petal`
  // (trees made of flowers), `bark_light` became id 111 — one PAST the end of
  // the table, so the shader read a garbage MaterialGpu and the branch
  // skeleton rendered as transparent, non-colliding ghosts — and all three
  // leaf tiers shifted, which is why the Trees tab's colours never matched the
  // forest. One table, one wrong index, five apparently unrelated bugs.
  std::unordered_map<std::string, uint32_t> byName;
  for (size_t i = 0; i < mats.size(); i++) byName[mats[i].name] = static_cast<uint32_t>(i);

  std::vector<fs::path> paths;
  std::error_code ec;
  if (fs::is_directory(dir, ec)) {
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec) && e.path().extension() == ".svtree")
        paths.push_back(e.path());
    }
  }
  // Sorted by name: the species INDEX reaches the world through a hash roll, so
  // directory order would make the forest depend on the filesystem.
  std::sort(paths.begin(), paths.end());

  std::vector<File> files;
  for (const auto& p : paths) {
    File f;
    f.name = p.stem().string();
    std::string err;
    if (!ParseFile(p, f, err)) {
      snprintf(buf, sizeof buf, "tree atlas: %s is malformed (%s)\n",
               f.name.c_str(), err.c_str());
      log += buf;
      return false;
    }
    files.push_back(std::move(f));
  }

  const int ns = static_cast<int>(files.size());
  const int speciesDir = kHeaderWords;
  const int biomeTable = speciesDir + ns * kSpeciesWords;
  const int biomeStride = 1 + ns;
  int cursor = biomeTable + kBiomeCount * biomeStride;

  std::vector<int> payloadBase(ns);
  for (int i = 0; i < ns; i++) {
    payloadBase[i] = cursor;
    cursor += static_cast<int>(files[i].words.size());
  }

  out.words.assign(static_cast<size_t>(cursor), 0u);
  uint32_t* W = out.words.data();
  W[kHMagic] = kMagic;
  W[kHVersion] = kVersion;
  W[kHSpeciesCount] = static_cast<uint32_t>(ns);
  W[kHTotalWords] = static_cast<uint32_t>(cursor);
  W[kHBiomeTable] = static_cast<uint32_t>(biomeTable);
  W[kHSpeciesDir] = static_cast<uint32_t>(speciesDir);
  W[kHBiomeCount] = static_cast<uint32_t>(kBiomeCount);

  int unresolved = 0;
  for (int i = 0; i < ns; i++) {
    File& f = files[i];
    const int base = payloadBase[i];

    // ---- copy the payload and rebase every offset ---------------------------
    std::copy(f.words.begin(), f.words.end(), out.words.begin() + base);

    // Local palette index (1..N) -> engine material id, resolved BY NAME.
    std::vector<uint32_t> remap(f.names.size() + 1, 0u);
    for (size_t k = 0; k < f.names.size(); k++) {
      auto it = byName.find(f.names[k]);
      if (it != byName.end()) {
        remap[k + 1] = it->second;
      } else {
        snprintf(buf, sizeof buf,
                 "tree atlas: %s names material '%s', which materials.json does "
                 "not define -- those voxels become air\n",
                 f.name.c_str(), f.names[k].c_str());
        log += buf;
        unresolved++;
      }
    }

    for (int v = 0; v < f.variantCount; v++) {
      uint32_t* d = W + base + f.varDirOff + v * kVariantWords;
      const uint32_t nx = d[kVNx], ny = d[kVNy], nz = d[kVNz];
      const uint32_t colsLocal = d[kVColumns];
      d[kVColumns] = colsLocal + base;
      d[kVRuns] += base;
      for (uint32_t c = 0; c < nx * nz; c++) {
        uint32_t* cell = W + base + colsLocal + c * 2;
        const uint32_t off = cell[0], cnt = cell[1];
        cell[0] = off + base;
        for (uint32_t r = 0; r < cnt; r++) {
          uint32_t* run = W + base + off + r;
          const uint32_t local = *run & 0xFFFu;
          // Rewrite the material half in place; the y0/len half is untouched.
          *run = (*run & 0xFFFFF000u) |
                 (local < remap.size() ? remap[local] : 0u);
        }
      }
      (void)ny;
    }

    // ---- species directory --------------------------------------------------
    uint32_t* s = W + speciesDir + i * kSpeciesWords;
    s[kSVariantDir] = static_cast<uint32_t>(base + f.varDirOff);
    s[kSVariantCount] = static_cast<uint32_t>(f.variantCount);
    s[kSReach] = static_cast<uint32_t>(f.reach);
    s[kSAbove] = static_cast<uint32_t>(f.above);
    s[kSCrownY] = static_cast<uint32_t>(f.crownY);
    s[kSCrownR] = static_cast<uint32_t>(f.crownR);
    s[kSMinY] = static_cast<uint32_t>(f.minY);
    s[kSMaxY] = static_cast<uint32_t>(f.maxY);
    s[kSMaxSlope] = static_cast<uint32_t>(f.maxSlope);
    s[kSSparsity] = static_cast<uint32_t>(f.sparsity);
    auto resolveLocal = [&](int local) -> uint32_t {
      return (local > 0 && local < static_cast<int>(remap.size())) ? remap[local] : 0u;
    };
    s[kSCanopyMat] = resolveLocal(f.canopyLocal);
    s[kSShade] = static_cast<uint32_t>(f.shade);
    // Autumn is only armed when BOTH ramps resolved; a half-resolved
    // substitution would turn some leaves and not others on the same tree.
    bool autumnOk = f.autumnChance > 0;
    for (int k = 0; k < 3; k++) {
      s[kSLeaf0 + k] = resolveLocal(f.leafLocal[k]);
      s[kSAutumn0 + k] = resolveLocal(f.autumnLocal[k]);
      if (!s[kSLeaf0 + k] || !s[kSAutumn0 + k]) autumnOk = false;
    }
    s[kSAutumnChance] = autumnOk ? static_cast<uint32_t>(f.autumnChance) : 0u;

    out.maxReachXZ = std::max(out.maxReachXZ, f.reach);
    out.maxAbove = std::max(out.maxAbove, f.above);

    TreeSpeciesInfo info;
    info.name = f.name;
    info.variants = f.variantCount;
    info.reachXZ = f.reach; info.above = f.above;
    info.crownY = f.crownY; info.crownR = f.crownR;
    for (int b = 0; b < kBiomeCount; b++) info.biome[b] = f.biome[b];
    info.minY = f.minY; info.maxY = f.maxY;
    info.maxSlope = f.maxSlope; info.sparsity = f.sparsity;
    info.canopyMat = s[kSCanopyMat]; info.shade = f.shade;
    info.autumnChance = static_cast<int>(s[kSAutumnChance]);
    info.words = f.words.size();
    out.species.push_back(std::move(info));
  }

  // ---- biome table ----------------------------------------------------------
  // Per biome: [total, cum_0, cum_1, ...]. Cumulative rather than raw so the
  // shader's pick is one modulo and a linear scan over at most `ns` entries,
  // with no division and no float. `sparsity` divides the weight here rather
  // than gating after the pick: a post-pick rejection would make a sparse
  // species STEAL tiles from the others and leave holes in the forest.
  for (int b = 0; b < kBiomeCount; b++) {
    uint32_t* row = W + biomeTable + b * biomeStride;
    uint32_t acc = 0;
    for (int i = 0; i < ns; i++) {
      const TreeSpeciesInfo& s = out.species[i];
      acc += static_cast<uint32_t>(std::max(0, s.biome[b] / std::max(1, s.sparsity)));
      row[1 + i] = acc;
    }
    row[0] = acc;
  }

  W[kHMaxReach] = static_cast<uint32_t>(out.maxReachXZ);
  W[kHMaxAbove] = static_cast<uint32_t>(out.maxAbove);
  out.speciesCount = ns;

  // THE CANDIDATE-SET BOUND (see treeatlas.h). worldgen keeps nine trees per
  // column because the tile arithmetic says nine is enough, and that derivation
  // reads the widest species' reach — which is now asset data. Refusing here is
  // the only place the two can be held together: past the bound the shader
  // silently DROPS a candidate, and the symptom is a canopy missing from some
  // columns and present on others, which nothing else in the repo would notice.
  {
    const int tile = std::max(16, CurrentTuning().worldgen.treeTile);
    const int bound = treeatlas::MaxReachForNineCandidates(tile);
    if (out.maxReachXZ > bound) {
      snprintf(buf, sizeof buf,
               "tree atlas: widest species reaches %d voxels, but worldgen's "
               "nine-candidate set only holds up to %d at treeTile %d -- narrow "
               "the crown or raise worldgen.treeTile\n",
               out.maxReachXZ, bound, tile);
      log += buf;
      return false;
    }
  }

  if (ns == 0) {
    log += "tree atlas: no .svtree in " + dir +
           " -- the world will have no trees (run `node scripts/bake_trees.mjs`)\n";
  } else {
    snprintf(buf, sizeof buf,
             "tree atlas: %d species, %d variants, %.2f MiB, reach %d, above %d%s\n",
             ns,
             [&] { int t = 0; for (auto& s : out.species) t += s.variants; return t; }(),
             out.Bytes() / 1048576.0, out.maxReachXZ, out.maxAbove,
             unresolved ? " (WITH UNRESOLVED MATERIALS)" : "");
    log += buf;
  }
  return true;
}

uint32_t TreeAtlasCellAt(const TreeAtlas& atlas, int species, int variant,
                         int lx, int ly, int lz) {
  if (species < 0 || species >= atlas.speciesCount) return 0;
  const uint32_t* W = atlas.words.data();
  const uint32_t* s = W + W[kHSpeciesDir] + species * kSpeciesWords;
  if (variant < 0 || variant >= static_cast<int>(s[kSVariantCount])) return 0;
  const uint32_t* d = W + s[kSVariantDir] + variant * kVariantWords;
  const int nx = static_cast<int>(d[kVNx]), ny = static_cast<int>(d[kVNy]),
            nz = static_cast<int>(d[kVNz]);
  if (lx < 0 || ly < 0 || lz < 0 || lx >= nx || ly >= ny || lz >= nz) return 0;
  const uint32_t* col = W + d[kVColumns] + (static_cast<size_t>(lz) * nx + lx) * 2;
  const uint32_t off = col[0], cnt = col[1];
  for (uint32_t k = 0; k < cnt; k++) {
    const uint32_t r = W[off + k];
    const int y0 = static_cast<int>((r >> 16) & 0x1FFu);
    const int len = static_cast<int>((r >> 25) & 0x7Fu);
    if (ly < y0) return 0;              // runs are sorted; nothing below can hit
    if (ly < y0 + len) return r & 0xFFFu;
  }
  return 0;
}
