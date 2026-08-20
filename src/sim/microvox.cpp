#include "sim/microvox.h"

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

    MicroSpec spec;
    spec.model = mi.value("model", "");
    spec.subdiv = mi.value("subdiv", 4u);
    if (mi.value("yawVariants", false)) spec.flags |= kMicroYawVariants;
    if (mi.value("jitter", false)) spec.flags |= kMicroJitter;
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
