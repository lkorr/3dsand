#include "game/anatomy_resolve.h"

#include <algorithm>
#include <unordered_set>

namespace anatomy {
namespace {

struct Layer {
  std::string material;
  int depth = -1;          // -1 = open-ended (the core)
  bool keep = false;
  std::string speckle;
  double speckleFraction = 0.0;
};

// anatomy.js `layersFor`: a limb's override in `recipe.limbs[name]`, else the
// recipe's own list. Normalised, with the last entry always reachable.
std::vector<Layer> LayersFor(const nlohmann::json& recipe,
                             const std::string& limb) {
  const nlohmann::json* src = nullptr;
  if (recipe.contains("limbs") && recipe["limbs"].is_object() &&
      !limb.empty() && recipe["limbs"].contains(limb) &&
      recipe["limbs"][limb].is_object() &&
      recipe["limbs"][limb].contains("layers"))
    src = &recipe["limbs"][limb]["layers"];
  else if (recipe.contains("layers"))
    src = &recipe["layers"];
  std::vector<Layer> out;
  if (!src || !src->is_array()) return out;
  for (const nlohmann::json& l : *src) {
    if (!l.is_object()) continue;
    Layer e;
    e.material = l.value("material", std::string());
    if (l.contains("depth") && l["depth"].is_number())
      e.depth = std::max(0, l["depth"].get<int>());
    e.keep = l.value("keep", false);
    if (l.contains("speckle") && l["speckle"].is_object() &&
        l["speckle"].contains("material")) {
      e.speckle = l["speckle"].value("material", std::string());
      e.speckleFraction =
          std::clamp(l["speckle"].value("fraction", 0.0), 0.0, 1.0);
    }
    out.push_back(std::move(e));
  }
  return out;
}

// anatomy.js `carveFor`: a limb's skeleton rules — eye sockets in the skull,
// ribs in the chest. The last matching rule wins; a kept layer is never carved.
struct Carve {
  std::string material, where;
  bool hasBox[3] = {false, false, false};
  double box[3][2] = {{0, 0}, {0, 0}, {0, 0}};
  int depthMin = 0, depthMax = 254;
  int stripeAxis = -1, stripePeriod = 0, stripeWidth = 0;
};

std::vector<Carve> CarveFor(const nlohmann::json& recipe,
                            const std::string& limb) {
  std::vector<Carve> out;
  if (!recipe.contains("limbs") || !recipe["limbs"].is_object() ||
      limb.empty() || !recipe["limbs"].contains(limb))
    return out;
  const nlohmann::json& L = recipe["limbs"][limb];
  if (!L.is_object() || !L.contains("carve") || !L["carve"].is_array())
    return out;
  static const char* kAxes[3] = {"x", "y", "z"};
  for (const nlohmann::json& c : L["carve"]) {
    if (!c.is_object()) continue;
    Carve e;
    e.material = c.value("material", std::string());
    if (e.material.empty()) continue;
    e.where = c.value("where", std::string());
    if (c.contains("box") && c["box"].is_object())
      for (int a = 0; a < 3; a++) {
        const auto it = c["box"].find(kAxes[a]);
        if (it == c["box"].end() || !it->is_array() || it->size() != 2) continue;
        e.hasBox[a] = true;
        e.box[a][0] = (*it)[0].get<double>();
        e.box[a][1] = (*it)[1].get<double>();
      }
    if (c.contains("depth") && c["depth"].is_array()) {
      const nlohmann::json& d = c["depth"];
      if (d.size() > 0 && d[0].is_number()) e.depthMin = d[0].get<int>();
      if (d.size() > 1 && d[1].is_number()) e.depthMax = d[1].get<int>();
    }
    if (c.contains("stripe") && c["stripe"].is_object()) {
      const nlohmann::json& s = c["stripe"];
      const std::string ax = s.value("axis", std::string());
      for (int a = 0; a < 3; a++)
        if (ax == kAxes[a]) e.stripeAxis = a;
      e.stripePeriod = s.value("period", 0);
      e.stripeWidth = s.value("width", 0);
      if (e.stripePeriod <= 0) e.stripeAxis = -1;
    }
    out.push_back(std::move(e));
  }
  return out;
}

// anatomy.js `carveMatches`. The box test is (l + 0.5) / n in [lo, hi), the
// same double arithmetic JS does, so both sides pick the same cells.
bool CarveMatches(const Carve& c, int whereId, int layerId, const int l[3],
                  const IVec3& size, int depth) {
  if (!c.where.empty() && whereId != layerId) return false;
  if (depth < c.depthMin || depth > c.depthMax) return false;
  const int n[3] = {size.x, size.y, size.z};
  for (int a = 0; a < 3; a++) {
    if (!c.hasBox[a]) continue;
    const double f = (l[a] + 0.5) / n[a];
    if (f < c.box[a][0] || f >= c.box[a][1]) return false;
  }
  if (c.stripeAxis >= 0 && l[c.stripeAxis] % c.stripePeriod >= c.stripeWidth)
    return false;
  return true;
}

// anatomy.js `layerIndexAt`: layers are consumed outermost first, each `depth`
// voxels thick, and the last one (or any with no depth) takes the rest.
int LayerIndexAt(const std::vector<Layer>& layers, int depth) {
  int d = depth;
  for (size_t i = 0; i < layers.size(); i++) {
    if (layers[i].depth < 0 || i + 1 == layers.size()) return (int)i;
    if (d < layers[i].depth) return (int)i;
    d -= layers[i].depth;
  }
  return -1;
}

// `Math.imul(a, b)`: the low 32 bits of the product.
inline uint32_t Imul(uint32_t a, uint32_t b) { return a * b; }

// anatomy.js `cellHash`, coercion for coercion.
//
//   let h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791) ^
//           (salt * 2654435761);
//   h = Math.imul(h ^ (h >>> 16), 0x45d9f3b);
//   h = Math.imul(h ^ (h >>> 16), 0x45d9f3b);
//   return (h ^ (h >>> 16)) >>> 0;
//
// The four multiplies are ORDINARY JS multiplies — doubles, exact at these
// magnitudes — and it is the `^` that truncates each one to 32 bits. So the
// products are computed wide and narrowed at the XOR, which is what the int64
// casts below are for. Getting this wrong does not fail loudly: it produces a
// different set of blood vessels through the muscle, which looks exactly as
// correct as the right one and rewrites every generated body.
uint32_t CellHash(int x, int y, int z, int salt) {
  const auto lo = [](int64_t v) { return (uint32_t)(uint64_t)v; };
  uint32_t h = lo((int64_t)x * 73856093) ^ lo((int64_t)y * 19349663) ^
               lo((int64_t)z * 83492791) ^ lo((int64_t)salt * 2654435761LL);
  h = Imul(h ^ (h >> 16), 0x45d9f3bu);
  h = Imul(h ^ (h >> 16), 0x45d9f3bu);
  return h ^ (h >> 16);
}

}  // namespace

// ---- the depth field --------------------------------------------------------
//
// Depth is measured over the UNION of every model, EXCEPT that a limb's own
// faces are always depth 0. Limbs abut at their joints — the top of a thigh
// sits against the hips, the head sits on the torso — and the union alone calls
// that joint face interior, which put vertebrae on the top of the neck and a
// shoulder socket on the arm, both on show the moment the rig turned a head.
//
// The own-face override is applied AFTER the BFS and not seeded into it: a seed
// would measure the rows under a joint face from that face too, and a neck five
// voxels tall would come out skin / flesh / muscle / flesh / skin with no bone
// in it. Overriding only the face keeps the union's depth underneath, so the
// core runs continuously through the joint.
std::vector<uint8_t> UnionDepth(const Prefab& prefab,
                                const std::vector<bool>* include) {
  const auto counts = [&](size_t mi) {
    return !include || mi >= include->size() || (*include)[mi];
  };
  const IVec3 dim = prefab.size;
  const size_t n = (size_t)std::max(dim.x, 0) * (size_t)std::max(dim.y, 0) *
                   (size_t)std::max(dim.z, 0);
  std::vector<uint8_t> depth(n, kDepthEmpty);
  if (!n) return depth;
  const size_t sx = 1, sy = (size_t)dim.x, sz = (size_t)dim.x * dim.y;

  std::vector<uint8_t> solid(n, 0);
  const auto inBox = [&](int x, int y, int z) {
    return x >= 0 && y >= 0 && z >= 0 && x < dim.x && y < dim.y && z < dim.z;
  };
  for (size_t mi = 0; mi < prefab.models.size(); mi++) {
    if (!counts(mi)) continue;
    const PrefabModel& m = prefab.models[mi];
    for (const PrefabVoxel& v : m.voxels) {
      const int px = v.x + m.offset.x, py = v.y + m.offset.y,
                pz = v.z + m.offset.z;
      if (inBox(px, py, pz)) solid[px + py * sy + pz * sz] = 1;
    }
  }

  std::vector<int32_t> queue;
  queue.reserve(n / 4 + 1);
  for (int z = 0; z < dim.z; z++)
    for (int y = 0; y < dim.y; y++)
      for (int x = 0; x < dim.x; x++) {
        const size_t i = (size_t)x + y * sy + z * sz;
        if (!solid[i]) continue;
        const bool exposed =
            x == 0 || !solid[i - sx] || x == dim.x - 1 || !solid[i + sx] ||
            y == 0 || !solid[i - sy] || y == dim.y - 1 || !solid[i + sy] ||
            z == 0 || !solid[i - sz] || z == dim.z - 1 || !solid[i + sz];
        if (exposed) {
          depth[i] = 0;
          queue.push_back((int32_t)i);
        }
      }

  // BFS inward, saturating at 254 so the empty sentinel stays unambiguous.
  for (size_t head = 0; head < queue.size(); head++) {
    const size_t i = (size_t)queue[head];
    const uint8_t nd = depth[i] < 254 ? (uint8_t)(depth[i] + 1) : (uint8_t)254;
    const int x = (int)(i % (size_t)dim.x);
    const int y = (int)((i / (size_t)dim.x) % (size_t)dim.y);
    const int z = (int)(i / sz);
    const auto step = [&](size_t j) {
      if (solid[j] && depth[j] == kDepthEmpty) {
        depth[j] = nd;
        queue.push_back((int32_t)j);
      }
    };
    if (x > 0) step(i - sx);
    if (x < dim.x - 1) step(i + sx);
    if (y > 0) step(i - sy);
    if (y < dim.y - 1) step(i + sy);
    if (z > 0) step(i - sz);
    if (z < dim.z - 1) step(i + sz);
  }

  // A LIMB'S OWN FACES ARE SURFACE.
  std::vector<uint8_t> own;
  for (size_t mi = 0; mi < prefab.models.size(); mi++) {
    if (!counts(mi)) continue;
    const PrefabModel& m = prefab.models[mi];
    const size_t mn = (size_t)std::max(m.size.x, 0) *
                      (size_t)std::max(m.size.y, 0) *
                      (size_t)std::max(m.size.z, 0);
    if (!mn) continue;
    own.assign(mn, 0);
    const size_t msy = (size_t)m.size.x, msz = (size_t)m.size.x * m.size.y;
    const auto inModel = [&](int x, int y, int z) {
      return x >= 0 && y >= 0 && z >= 0 && x < m.size.x && y < m.size.y &&
             z < m.size.z;
    };
    for (const PrefabVoxel& v : m.voxels)
      if (inModel(v.x, v.y, v.z)) own[v.x + v.y * msy + v.z * msz] = 1;
    const auto at = [&](int x, int y, int z) {
      return inModel(x, y, z) ? own[(size_t)x + y * msy + z * msz] : (uint8_t)0;
    };
    for (const PrefabVoxel& v : m.voxels) {
      if (!inModel(v.x, v.y, v.z)) continue;
      if (at(v.x - 1, v.y, v.z) && at(v.x + 1, v.y, v.z) &&
          at(v.x, v.y - 1, v.z) && at(v.x, v.y + 1, v.z) &&
          at(v.x, v.y, v.z - 1) && at(v.x, v.y, v.z + 1))
        continue;
      const int px = v.x + m.offset.x, py = v.y + m.offset.y,
                pz = v.z + m.offset.z;
      if (inBox(px, py, pz)) depth[px + py * sy + pz * sz] = 0;
    }
  }
  return depth;
}

// ---- the resolve ------------------------------------------------------------

Report Resolve(Prefab& prefab, const nlohmann::json& recipe,
               const std::vector<MaterialDef>& mats, const std::string& where,
               std::string& log) {
  Report rep;
  if (!recipe.is_object() || !(recipe.contains("layers") ||
                               recipe.contains("limbs")))
    return rep;
  rep.ran = true;

  std::unordered_set<std::string> missing;
  const auto matIdOf = [&](const std::string& name) -> int {
    if (name.empty()) return 0;
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == name) return (int)i;
    if (missing.insert(name).second) rep.unresolved.push_back(name);
    return 0;
  };

  const IVec3 dim = prefab.size;
  const size_t n = (size_t)std::max(dim.x, 0) * (size_t)std::max(dim.y, 0) *
                   (size_t)std::max(dim.z, 0);
  if (!n) return rep;
  const size_t sy = (size_t)dim.x, sz = (size_t)dim.x * dim.y;
  // A KEEP-ONLY LIMB IS NOT BODY (anatomy.js planAnatomy says why): hair
  // lying on the scalp would otherwise bury it and push the skull inward.
  std::vector<bool> include(prefab.models.size(), true);
  for (size_t mi = 0; mi < prefab.models.size(); mi++) {
    const std::vector<Layer> ls = LayersFor(recipe, prefab.models[mi].name);
    include[mi] = ls.empty() || !std::all_of(ls.begin(), ls.end(),
                                             [](const Layer& l) { return l.keep; });
  }
  const std::vector<uint8_t> depth = UnionDepth(prefab, &include);

  // GARMENTS: surface voxels that are CLOTHING, not body. The voxel directly
  // under one takes the FIRST layer's material — skin under the shorts, not
  // flesh under the shorts — while everything deeper follows the schedule
  // unchanged, so the bone core is where it would be on a bare limb. Kept in
  // prefab space because "under a garment" crosses model boundaries: the
  // shorts are the hips model and the thigh is another.
  std::unordered_set<int> garmentIds;
  if (recipe.contains("garments") && recipe["garments"].is_array())
    for (const nlohmann::json& g : recipe["garments"])
      if (g.is_string()) {
        const int id = matIdOf(g.get<std::string>());
        if (id) garmentIds.insert(id);
      }
  std::vector<uint8_t> garment;
  if (!garmentIds.empty()) {
    garment.assign(n, 0);
    for (const PrefabModel& m : prefab.models)
      for (const PrefabVoxel& v : m.voxels) {
        const int px = v.x + m.offset.x, py = v.y + m.offset.y,
                  pz = v.z + m.offset.z;
        if (px < 0 || py < 0 || pz < 0 || px >= dim.x || py >= dim.y ||
            pz >= dim.z)
          continue;
        const size_t i = (size_t)px + py * sy + pz * sz;
        if (garmentIds.count((int)v.material) && depth[i] == 0) garment[i] = 1;
      }
  }
  const auto underGarment = [&](int px, int py, int pz) {
    if (garment.empty()) return false;
    const size_t i = (size_t)px + py * sy + pz * sz;
    return (px > 0 && garment[i - 1]) || (px < dim.x - 1 && garment[i + 1]) ||
           (py > 0 && garment[i - sy]) || (py < dim.y - 1 && garment[i + sy]) ||
           (pz > 0 && garment[i - sz]) || (pz < dim.z - 1 && garment[i + sz]);
  };

  for (PrefabModel& m : prefab.models) {
    const std::vector<Layer> layers = LayersFor(recipe, m.name);
    if (layers.empty()) continue;
    std::vector<int> ids(layers.size()), speck(layers.size());
    for (size_t li = 0; li < layers.size(); li++) {
      ids[li] = matIdOf(layers[li].material);
      speck[li] = matIdOf(layers[li].speckle);
    }
    const int surfaceId = ids[0];
    const std::vector<Carve> carve = CarveFor(recipe, m.name);
    std::vector<int> carveId(carve.size()), carveWhere(carve.size());
    for (size_t k = 0; k < carve.size(); k++) {
      carveId[k] = matIdOf(carve[k].material);
      carveWhere[k] = matIdOf(carve[k].where);
    }
    // What this recipe puts UNDER the surface. A `keep` voxel made of one of
    // these is not the painted character — it is a face this recipe once baked
    // as interior, back before own-face depth existed — so it gets the kept
    // layer's material back and its art cleared.
    std::unordered_set<int> interior;
    for (size_t li = 0; li < layers.size(); li++) {
      if (layers[li].keep) continue;
      if (ids[li]) interior.insert(ids[li]);
      if (speck[li]) interior.insert(speck[li]);
    }
    for (int id : carveId)
      if (id) interior.insert(id);
    for (PrefabVoxel& v : m.voxels) {
      const int px = v.x + m.offset.x, py = v.y + m.offset.y,
                pz = v.z + m.offset.z;
      if (px < 0 || py < 0 || pz < 0 || px >= dim.x || py >= dim.y ||
          pz >= dim.z)
        continue;
      const uint8_t d = depth[(size_t)px + py * sy + pz * sz];
      if (d == kDepthEmpty) continue;
      const int li = LayerIndexAt(layers, (int)d);
      if (li < 0) continue;
      const Layer& L = layers[(size_t)li];
      if (L.keep && !interior.count((int)v.material)) continue;
      int mat = ids[(size_t)li];
      if (d == 1 && surfaceId && !garmentIds.empty() &&
          underGarment(px, py, pz)) {
        mat = surfaceId;
      } else if (speck[(size_t)li] && L.speckleFraction > 0.0 &&
                 (double)(CellHash(px, py, pz, li + 1) % 10000u) <
                     L.speckleFraction * 10000.0) {
        mat = speck[(size_t)li];
      }
      if (!L.keep) {
        const int l[3] = {v.x, v.y, v.z};
        for (size_t k = 0; k < carve.size(); k++)
          if (carveId[k] && CarveMatches(carve[k], carveWhere[k],
                                         ids[(size_t)li], l, m.size, (int)d))
            mat = carveId[k];
      }
      if (!mat) continue;                                 // unresolved: leave
      if ((int)v.material == mat && v.color == 0) continue;  // idempotent
      v.material = (uint16_t)mat;
      v.color = 0;
      rep.rewritten++;
    }
  }

  for (const std::string& u : rep.unresolved)
    log += where + ": anatomy recipe names material \"" + u +
           "\", which materials.json does not have — those voxels are left as "
           "the art drew them\n";
  // A NON-ZERO COUNT IS NEWS. The bake is a cache of exactly this computation,
  // so a committed body should need no rewriting at all; anything else is
  // either stale art or a drift between this and assets/editor/anatomy.js.
  if (rep.rewritten)
    log += where + ": anatomy resolved " + std::to_string(rep.rewritten) +
           " voxel(s) the baked art did not already have — re-bake it "
           "(`node scripts/anatomize_mob.mjs " + prefab.name + "`) so the "
           "Models tab's peel shows what the engine loads\n";
  return rep;
}

}  // namespace anatomy
