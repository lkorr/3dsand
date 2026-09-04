#include "sim/microvox.h"

#include <algorithm>
#include <bit>
#include <fstream>
#include <nlohmann/json.hpp>

#include "sim/voxload.h"
#include "sim/world.h"

using nlohmann::json;

namespace {

// One micro material's parsed JSON, before the .vox is read.
struct MicroSpec {
  std::string model;
  uint32_t subdiv = 4;
  uint32_t flags = 0;
  // (model index within the .vox, ticks) per flipbook frame. Empty => "every
  // model in the file is a frame, at kDefaultFrameTicks each", which is what
  // makes a single-model .vox with no `frames` array Just Work.
  std::vector<std::pair<uint32_t, uint32_t>> frames;
};

// Default flipbook dwell when `frames` is omitted but the .vox holds several
// models. 10 ticks at 30 Hz = 3 Hz, a readable sway rate for grass.
constexpr uint32_t kDefaultFrameTicks = 10;
constexpr uint32_t kMaxFrames = 255;  // frameInfo packs the count in 8 bits

// Named f32 parameter slots per plant kind, in POOL ORDER: slot k of kind K is
// microPool[base + 3 + k] and tracePlant reads it as plantP(d, k). Adding a
// param means appending a name here AND reading the same slot in the shader;
// never reorder. Null-terminated at the first unused slot.
constexpr const char* kPlantParamNames[4][kPlantParamSlots + 1] = {
    // grass: a column of tapered flat blades
    {"halfWidth", "thickness", "taper", "tipFrac", "heightVary", "swayScale",
     "headChance", "rootSpread", "lean", "headLen", nullptr},
    // flower: one stem, a few leaves, a head in one of four styles
    {"stemHalfW", "headR", "headH", "leafCount", "leafLen", "leafW", "swayScale",
     "heightVary", "headDrop", "headCount", "centreR", "petalGap", "petalCount",
     nullptr},
    // mushroom: cone stem under an ellipsoid cap, gills below, spots on top
    {"capRMin", "capRMax", "capH", "stemR", "stemH", "spotChance", "swayScale",
     "gillDepth", "heightVary", "capFlat", nullptr},
    // fern: a rosette of arching, leafleted fronds
    {"frondCount", "frondLen", "rise", "droop", "leafletW", "leafletFreq",
     "swayScale", "rachisW", "spreadJitter", "tiltJitter", nullptr},
};

}  // namespace

bool LoadMicroVox(const std::string& materialsPath, const std::string& assetDir,
                  std::vector<MaterialDef>& mats, MicroSet& out, std::string& log) {
  out.table.assign(kMaterialSlots, MicroBrickGpu{kMicroNoBrick, 0, 0, 0});
  out.pool.clear();
  out.materialCount = 0;
  out.frameCount = 0;

  // MATF_MICRO is derived state: clear it first so a reload that REMOVES a
  // micro block puts the material back on the cube path instead of leaving it
  // pointing at a stale flag with no brick behind it.
  for (MaterialDef& d : mats) d.gpu.flags &= ~kMatFlagMicro;

  std::ifstream f(materialsPath);
  if (!f) {
    log += "cannot open " + materialsPath + "\n";
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    log += materialsPath + ": JSON parse error: " + e.what() + "\n";
    return false;
  }
  if (!j.contains("materials") || !j["materials"].is_array()) return true;

  for (const auto& m : j["materials"]) {
    if (!m.contains("micro")) continue;
    std::string id = m.value("id", "");
    const json& mi = m["micro"];

    // Resolve the material id the same way the rest of the pipeline does:
    // index in `mats` IS the 12-bit material id (slot 0 is air).
    int matId = -1;
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == id) { matId = (int)i; break; }
    if (matId < 0) {
      log += materialsPath + ": micro on unknown material \"" + id + "\"\n";
      continue;
    }
    if (!mi.is_object()) {
      log += materialsPath + ": material \"" + id + "\": \"micro\" must be an object\n";
      continue;
    }

    // ---- analytic plants: a `plant` block instead of a model ---------------
    // The cells of this material render a parametric plant (tracePlant in
    // raymarch.wgsl) — no .vox is involved; the pool holds a PlantDef:
    //   w0   kind (0..7) | bodyMat (8..15) | tipMat (16..23) | accentMat (24..31)
    //   w1   accent2Mat (0..7) | stemMat (8..15) | tile (16..23) | foot (24..31)
    //   w2   minH (0..7) | maxH (8..15) | count (16..23) | style (24..31)
    //   w3.. kPlantParamSlots f32 words, named per kind in kPlantParamNames
    //
    // `tile` 0 is a COLUMN plant (grass, a flower: one column, every cell of
    // it rebuilds the same plant from the column hash). A nonzero tile is a
    // TILE plant (fern, big toadstool): worldgen paints a foot x foot footprint
    // and the renderer reconstructs one plant per tile from plantTileAt in
    // common.wgsl — the tile/foot here must match that species' PLANT_*
    // constants, which check_invariants.py verifies.
    //
    // AUTHORING RULES, both load-bearing:
    //   * A stacked pair (tall_grass / tall_grass_head) must declare IDENTICAL
    //     plant blocks apart from body/tip — every cell of a column derives
    //     the same plant independently, so differing params would tear it at
    //     the material boundary.
    //   * Column plants must stay inside their column: the shader clamps the
    //     wind/trample bend rather than let a blade cross into a neighbour cell
    //     the world DDA never traces. Tile plants must stay inside their
    //     footprint for the same reason.
    if (mi.contains("plant")) {
      const json& pl = mi["plant"];
      if (!pl.is_object()) {
        log += materialsPath + ": material \"" + id + "\": micro.plant must be an object\n";
        continue;
      }
      const std::string kindName = pl.value("kind", "");
      uint32_t kind = 0;
      const char* const* names = nullptr;
      if (kindName == "grass") { kind = kPlantGrass; names = kPlantParamNames[0]; }
      else if (kindName == "flower") { kind = kPlantFlower; names = kPlantParamNames[1]; }
      else if (kindName == "mushroom") { kind = kPlantMushroom; names = kPlantParamNames[2]; }
      else if (kindName == "fern") { kind = kPlantFern; names = kPlantParamNames[3]; }
      else {
        log += materialsPath + ": material \"" + id +
               "\": micro.plant.kind must be grass|flower|mushroom|fern (got \"" +
               kindName + "\")\n";
        continue;
      }
      // Palette materials by NAME (the glyphs.json precedent): body defaults
      // to the material itself, everything else to the body.
      auto resolve = [&](const std::string& name, int fallback) -> int {
        if (name.empty()) return fallback;
        for (size_t i = 0; i < mats.size(); i++)
          if (mats[i].name == name) return (int)i;
        log += materialsPath + ": material \"" + id + "\": plant names unknown material \"" +
               name + "\"\n";
        return fallback;
      };
      const int body = resolve(pl.value("body", ""), matId);
      const int tip = resolve(pl.value("tip", ""), body);
      const int accent = resolve(pl.value("accent", ""), tip);
      const int accent2 = resolve(pl.value("accent2", ""), accent);
      const int stem = resolve(pl.value("stem", ""), body);
      if (body > 255 || tip > 255 || accent > 255 || accent2 > 255 || stem > 255) {
        log += materialsPath + ": material \"" + id +
               "\": plant materials must have ids 1..255 (8-bit pack)\n";
        continue;
      }
      const uint32_t tile = std::min(pl.value("tile", 0u), 255u);
      const uint32_t foot = std::min(pl.value("foot", 1u), 255u);
      const uint32_t minH = std::min(pl.value("minH", 1u), 255u);
      const uint32_t maxH = std::min(std::max(pl.value("maxH", minH), minH), 255u);
      const uint32_t count = std::min(pl.value("count", 1u), 255u);
      const uint32_t style = std::min(pl.value("style", 0u), 255u);
      float params[kPlantParamSlots] = {};
      if (pl.contains("params")) {
        const json& pp = pl["params"];
        if (!pp.is_object()) {
          log += materialsPath + ": material \"" + id + "\": micro.plant.params must be an object\n";
          continue;
        }
        for (auto it = pp.begin(); it != pp.end(); ++it) {
          int slot = -1;
          for (uint32_t k = 0; k < kPlantParamSlots && names[k]; k++)
            if (it.key() == names[k]) { slot = (int)k; break; }
          if (slot < 0) {
            log += materialsPath + ": material \"" + id + "\": plant kind " + kindName +
                   " has no param \"" + it.key() + "\"\n";
            continue;
          }
          params[slot] = it.value().is_number() ? it.value().get<float>() : 0.0f;
        }
      }
      if (out.pool.size() + kPlantPoolWords > kMicroPoolWords) {
        log += materialsPath + ": micro brick pool full (" +
               std::to_string(kMicroPoolWords) + " words)\n";
        continue;
      }
      const uint32_t base = (uint32_t)out.pool.size();
      out.pool.push_back(kind | ((uint32_t)body << 8) | ((uint32_t)tip << 16) |
                         ((uint32_t)accent << 24));
      out.pool.push_back((uint32_t)accent2 | ((uint32_t)stem << 8) | (tile << 16) |
                         (foot << 24));
      out.pool.push_back(minH | (maxH << 8) | (count << 16) | (style << 24));
      for (uint32_t k = 0; k < kPlantParamSlots; k++)
        out.pool.push_back(std::bit_cast<uint32_t>(params[k]));
      // frameCount 1 / period 0: the flipbook machinery is idle for plants.
      out.table[matId] =
          MicroBrickGpu{base, 0, 1u, kMicroSway | kMicroPlant};
      mats[matId].gpu.flags |= kMatFlagMicro;
      out.materialCount++;
      continue;
    }

    MicroSpec spec;
    spec.model = mi.value("model", "");
    spec.subdiv = mi.value("subdiv", 4u);
    if (mi.value("yawVariants", false)) spec.flags |= kMicroYawVariants;
    if (mi.value("jitter", false)) spec.flags |= kMicroJitter;
    if (mi.value("sway", false)) spec.flags |= kMicroSway;
    if (spec.model.empty()) {
      log += materialsPath + ": material \"" + id + "\": micro.model is required\n";
      continue;
    }
    // subdiv must be 2, 4 or 8: the nested DDA's step cap and the brick's word
    // stride are both derived from the log, and a non-power-of-two would make
    // the cell-local coordinate math a divide instead of a shift.
    uint32_t subdivLog2 = 0;
    if (spec.subdiv == 2) subdivLog2 = 1;
    else if (spec.subdiv == 4) subdivLog2 = 2;
    else if (spec.subdiv == 8) subdivLog2 = 3;
    else {
      log += materialsPath + ": material \"" + id + "\": micro.subdiv must be 2, 4 or 8 (got " +
             std::to_string(spec.subdiv) + ")\n";
      continue;
    }

    if (mi.contains("frames")) {
      if (!mi["frames"].is_array()) {
        log += materialsPath + ": material \"" + id + "\": micro.frames must be an array\n";
        continue;
      }
      for (const auto& fr : mi["frames"]) {
        uint32_t model = fr.value("model", 0u);
        uint32_t ticks = fr.value("ticks", kDefaultFrameTicks);
        // A zero-tick frame would give a zero-length loop and a division by
        // zero in the shader's frame select; clamp loudly rather than silently.
        if (ticks == 0) {
          log += materialsPath + ": material \"" + id +
                 "\": micro frame ticks must be >= 1\n";
          ticks = 1;
        }
        spec.frames.push_back({model, ticks});
      }
      if (spec.frames.empty()) {
        log += materialsPath + ": material \"" + id + "\": micro.frames is empty\n";
        continue;
      }
    }

    // ---- load the .vox ----
    Prefab pf;
    std::string err, warn;
    std::string path = assetDir + "/" + spec.model;
    // materialCount = kMaterialSlots so the loader does not warn about palette
    // indices: for a micro brick the index IS the material id and the caller
    // has already validated the material table, and the 8-bit pack range is
    // checked below with a message that names the actual problem.
    if (!LoadVoxFile(path, kMaterialSlots, pf, err, warn)) {
      log += path + ": " + err;
      continue;
    }

    if (spec.frames.empty())
      for (size_t i = 0; i < pf.models.size() && i < kMaxFrames; i++)
        spec.frames.push_back({(uint32_t)i, kDefaultFrameTicks});
    if (spec.frames.size() > kMaxFrames) {
      log += path + ": more than " + std::to_string(kMaxFrames) + " micro frames\n";
      continue;
    }

    // The model must FIT one cell. The nested DDA marches a subdiv^3 box in
    // cell-local space, so anything larger would spill outside the cell the
    // world DDA is standing in and the overflow would simply never be drawn.
    //
    // SMALLER IS ALLOWED, and that matters for authoring. The .vox loader
    // rebases the whole prefab to its own min corner, so a tuft of grass that
    // (correctly) does not reach the edges of its cell comes back as a box
    // smaller than subdiv^3 — demanding an exact match would force every author
    // to plant dummy corner voxels, which are visible. Instead a small model is
    // placed CENTRED in X and Z and FLOOR-ALIGNED in Y: plants grow up from the
    // ground, so bottom-aligning is the one choice that never leaves a tuft
    // floating, and centring horizontally is what makes the sub-cell jitter
    // read as jitter rather than as a bias.
    const uint32_t S = spec.subdiv;
    if (pf.size.x > (int)S || pf.size.y > (int)S || pf.size.z > (int)S) {
      log += path + ": micro model box is " + std::to_string(pf.size.x) + "x" +
             std::to_string(pf.size.y) + "x" + std::to_string(pf.size.z) +
             ", must fit within " + std::to_string(S) + "^3 (subdiv " +
             std::to_string(S) + ")\n";
      continue;
    }
    const int padX = ((int)S - pf.size.x) / 2;
    const int padY = 0;  // floor-aligned: plants grow from the ground up
    const int padZ = ((int)S - pf.size.z) / 2;

    const uint32_t wordsPerBrick = (S * S * S) / 4;
    const uint32_t need = (uint32_t)spec.frames.size() * (1 + wordsPerBrick);
    if (out.pool.size() + need > kMicroPoolWords) {
      log += path + ": micro brick pool full (" + std::to_string(kMicroPoolWords) +
             " words)\n";
      continue;
    }

    const uint32_t base = (uint32_t)out.pool.size();
    // ---- header: cumulative tick offsets, one per frame ----
    uint32_t acc = 0;
    for (const auto& fr : spec.frames) {
      acc += fr.second;
      out.pool.push_back(acc);
    }
    const uint32_t period = acc;

    // ---- payload: one packed brick per frame ----
    bool bad = false;
    for (const auto& fr : spec.frames) {
      uint32_t mi2 = fr.first;
      if (mi2 >= pf.models.size()) {
        log += path + ": micro frame references model " + std::to_string(mi2) +
               " but the file has " + std::to_string(pf.models.size()) + "\n";
        bad = true;
        break;
      }
      const PrefabModel& pm = pf.models[mi2];
      std::vector<uint8_t> cells(S * S * S, 0);
      for (const PrefabVoxel& v : pm.voxels) {
        // Model-local -> prefab-local (the loader rebased the prefab box) ->
        // brick-local (centre in X/Z, floor in Y — see the pad comment above).
        int x = v.x + pm.offset.x + padX;
        int y = v.y + pm.offset.y + padY;
        int z = v.z + pm.offset.z + padZ;
        if (x < 0 || y < 0 || z < 0 || x >= (int)S || y >= (int)S || z >= (int)S) {
          bad = true;
          break;
        }
        if (v.material == 0 || v.material > 255) {
          // 8 bits per micro voxel is what makes the pool affordable; a micro
          // model may therefore only use materials 1..255. Naming the limit
          // beats a silently truncated id painting the wrong colour.
          log += path + ": micro voxel material id " + std::to_string(v.material) +
                 " out of range 1..255\n";
          bad = true;
          break;
        }
        cells[(size_t)(z * S + y) * S + x] = (uint8_t)v.material;
      }
      if (bad) break;
      for (uint32_t w = 0; w < wordsPerBrick; w++) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4; b++) word |= (uint32_t)cells[w * 4 + b] << (b * 8);
        out.pool.push_back(word);
      }
    }
    if (bad) {
      out.pool.resize(base);  // roll the partial entry back out of the pool
      continue;
    }

    out.table[matId] = MicroBrickGpu{
        base, subdivLog2,
        (uint32_t)spec.frames.size() | (period << 8), spec.flags};
    mats[matId].gpu.flags |= kMatFlagMicro;
    out.materialCount++;
    out.frameCount += (uint32_t)spec.frames.size();
  }

  // The buffer is fixed-size on the GPU, so a pool that never got a single
  // brick still needs one word of content for the upload to be well-formed.
  if (out.pool.empty()) out.pool.push_back(0);
  return true;
}
