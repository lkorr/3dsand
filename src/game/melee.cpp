#include "game/melee.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include "game/mob.h"     // MobSystem/Mob: the sweep carves live limbs
#include "phys/debris.h"  // DebrisSystem: ...and melts loose ones
#include "phys/physics.h"
#include "sim/scale.h"    // SkinScaleFor / NeededArtUpsample / kLegacyAuthoring*
#include "sim/tuning.h"   // CurrentTuning().gore: the kerf's shape lives there
#include "sim/voxload.h"  // UpsamplePrefab

// See melee.h for what this is and why. Three halves, in file order: the item
// library, the damage sweep, and the stroke driver.

using nlohmann::json;

// ---- item library ----------------------------------------------------------

namespace {

// One item's own art + sidecar: assets/items/<id>.{vox,json}. Fills the
// geometry, grip and edge halves of the def; the behaviour half comes from
// items.json. Returns false (loudly) if the item cannot be shown, since an
// item that silently fails to load is one that turns up invisible in the hand.
bool LoadItemAsset(const std::string& dir, size_t materialCount,
                   MicroBodySet& micro, ItemDef& d, std::string& errors) {
  const std::string base = dir + "/" + d.name;
  std::ifstream sf(base + ".json");
  if (!sf) {
    errors += "items: \"" + d.name + "\" has no sidecar " + base + ".json\n";
    return false;
  }
  json s;
  try {
    sf >> s;
  } catch (const std::exception& e) {
    errors += "items: " + d.name + ".json parse error: " + e.what() + "\n";
    return false;
  }

  std::string verr, vwarn;
  if (!LoadVoxFile(base + ".vox", materialCount, d.prefab, verr, vwarn)) {
    errors += "items: \"" + d.name + "\" art failed to load: " + verr + "\n";
    return false;
  }
  errors += vwarn;

  // ART COLOURS ARE FILE-LOCAL AND THE PALETTE IS SHARED.
  //
  // A ".col" layer's bytes are slots in THIS .vox's own palette; the renderer
  // indexes ONE merged table that every def and every item resolves against.
  // Fold this file into it and rewrite its voxels NOW, before anything copies
  // them into a shell or a brick — exactly what LoadMobDefs does, and for
  // exactly the same reason.
  //
  // Skipping it does not fail to render: it renders the item in whatever
  // colours the MOBS happened to put at those slot numbers. A gold sash came
  // out the same near-black as the robe over it, which reads as a lighting
  // problem rather than as an un-remapped palette. Items load AFTER mobs into
  // the same set (main.cpp), and LoadMobDefs clears the table, so this must
  // stay on that side of the ordering.
  if (!d.prefab.artColors.empty()) {
    const std::vector<uint8_t> remap =
        MicroBodyMergeArt(micro, d.prefab.artColors, "item/" + d.name, errors);
    for (PrefabModel& pm : d.prefab.models)
      for (PrefabVoxel& v : pm.voxels)
        if (v.color) v.color = remap[v.color];
  }

  // ---- ART SCALE (DESIGN.md §3b) --------------------------------------------
  // Authored as voxels/METRE; the micro-per-world `scale` is derived. Legacy
  // sidecars said `scale` directly, which silently meant "and the world is
  // 10 cm" — see ItemDef::artVoxelsPerMetre.
  if (s.contains("artVoxelsPerMetre")) {
    d.artVoxelsPerMetre = s.value("artVoxelsPerMetre", 0);
  } else {
    const uint32_t legacy = std::max(1u, s.value("scale", 1u));
    d.artVoxelsPerMetre = (int)legacy * kLegacyAuthoringVoxelsPerMetre;
    errors += "items: \"" + d.name + "\" has no \"artVoxelsPerMetre\"; " +
              "reading legacy scale " + std::to_string(legacy) + " as " +
              std::to_string(d.artVoxelsPerMetre) + " art voxels/metre\n";
  }
  if (d.artVoxelsPerMetre <= 0) {
    errors += "items: \"" + d.name + "\" artVoxelsPerMetre must be positive\n";
    d.artVoxelsPerMetre = kVoxelsPerMetre;
  }
  d.artUpsample = NeededArtUpsample(d.artVoxelsPerMetre);
  d.scale = SkinScaleFor(d.artVoxelsPerMetre * (int)d.artUpsample);
  if (d.scale != 1 && d.scale != 2 && d.scale != 4 && d.scale != 8) {
    errors += "items: \"" + d.name + "\" artVoxelsPerMetre " +
              std::to_string(d.artVoxelsPerMetre) + " gives scale " +
              std::to_string(d.scale) + " at " + std::to_string(kVoxelsPerMetre) +
              " world voxels/metre — must be 1, 2, 4 or 8. Using 1; this item " +
              "will be the wrong physical size.\n";
    d.scale = 1;
    d.artUpsample = 1;
  }
  if (d.artUpsample > 1) UpsamplePrefab(d.prefab, d.artUpsample);

  auto findModel = [&](const std::string& want) -> int {
    for (size_t i = 0; i < d.prefab.models.size(); i++)
      if (d.prefab.models[i].name == want) return (int)i;
    return -1;
  };

  // ---- WORN ITEMS ARE MULTI-MODEL ------------------------------------------
  // A robe is a torso panel, two sleeves and a skirt, and each one has to move
  // with a different limb — so an armour .vox is shaped like a MOB's .vox
  // (one named model per part) rather than like the sword's single blade. The
  // cover list is the schema for that; see ItemCover in game/item.h.
  //
  // Parsed BEFORE the single-model block below, because a worn piece has no
  // one model to be: `d.voxels` stays empty and the geometry lives per shell.
  const bool worn = ItemKindIsWorn(d.kind);
  // Authored art units -> world voxels; divides out any upsample.
  const float invScale = d.ArtToWorld();
  if (s.contains("cover") && s["cover"].is_array()) {
    auto vec3Of = [](const json& j, Vec3 dflt) {
      if (!j.is_array() || j.size() != 3) return dflt;
      return Vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    };
    for (const json& c : s["cover"]) {
      if (!c.is_object()) continue;
      ItemCover cv;
      cv.part = c.value("part", "");
      cv.model = c.value("model", cv.part);
      if (cv.part.empty()) {
        errors += "items: \"" + d.name +
                  "\" has a cover entry with no \"part\" — skipped\n";
        continue;
      }
      // Micro -> world voxels, the same conversion every other length in this
      // sidecar takes. `fitBox` is a SIZE and `offset` is a displacement, so
      // both scale and neither is rebased on the model origin: they are
      // measured against the WEARER's limb box, not against the item's art.
      cv.offset = vec3Of(c.contains("offset") ? c["offset"] : json(), Vec3{}) *
                  invScale;
      cv.fitBox = vec3Of(c.contains("fitBox") ? c["fitBox"] : json(), Vec3{}) *
                  invScale;
      cv.hp = c.value("hp", 10.0f);
      const int ci = findModel(cv.model);
      if (ci < 0) {
        errors += "items: \"" + d.name + "\" cover \"" + cv.part +
                  "\" names no model \"" + cv.model + "\" in " + base +
                  ".vox — skipped\n";
        continue;
      }
      const PrefabModel& cm = d.prefab.models[ci];
      cv.size = cm.size;
      cv.modelOffset = cm.offset;
      cv.voxels = cm.voxels;
      // Same shared pool the rigs and the held item use: a shell is drawn by
      // the borrowed slot's own micro path, so it must live where that path
      // looks.
      if (d.scale > 1) {
        cv.microModel = MicroBodyPack(micro, cv.voxels, cv.size, d.scale,
                                      "item/" + d.name + "/" + cv.part, errors);
        if (cv.microModel < 0)
          errors += "items: \"" + d.name + "\" shell \"" + cv.part +
                    "\" has no micro brick and will not render\n";
      }
      d.cover.push_back(std::move(cv));
    }
  }
  if (worn && d.cover.empty()) {
    // An armour item with no shells would equip into its slot and then be
    // invisible and inert, which reads as a broken game rather than as the
    // missing JSON key it is.
    errors += "items: \"" + d.name +
              "\" is a worn kind but declares no \"cover\" entries — it would "
              "be invisible on the body\n";
    return false;
  }

  // The HELD half of an item: one model, one grip, one hilt, one edge. Null
  // for a worn piece, and every block below that needs a single model is
  // written against this pointer rather than against a `worn` test — so a
  // future kind that is both held and worn is one flag, not a re-shuffle.
  const PrefabModel* held = nullptr;
  if (!worn) {
    const std::string modelName = s.value("model", d.name);
    int mi = findModel(modelName);
    // A single-model .vox with an unnamed model is still perfectly usable;
    // only complain when there is genuinely nothing to draw.
    if (mi < 0 && d.prefab.models.size() == 1) mi = 0;
    if (mi < 0) {
      errors += "items: \"" + d.name + "\" has no model named \"" + modelName +
                "\" in " + base + ".vox\n";
      return false;
    }
    held = &d.prefab.models[mi];
    d.size = held->size;
    d.offset = held->offset;
    d.voxels = held->voxels;
    // HEFT IS MEASURED, NOT AUTHORED (item.h ItemDef::heftVolume). The voxel
    // count divided by scale^3 is the item's volume in WORLD voxels, which is
    // the one number that says "this is a greatsword and that is a knife"
    // without anybody writing it down twice. Taken AFTER the upsample above,
    // so `scale` and `voxels` are on the same lattice — reading it before
    // would inflate a low-resolution item by artUpsample^3.
    const float sc = (float)(d.scale ? d.scale : 1u);
    d.heftVolume = (float)d.voxels.size() / (sc * sc * sc);
  }

  d.hp = s.value("hp", 30.0f);
  d.severable = s.value("severable", true);
  d.severImpactSpeed = s.value("severImpactSpeed", 0.0f);
  if (s.contains("spring") && s["spring"].is_object()) {
    d.hasSpring = true;
    d.spring.halflife = s["spring"].value("halflife", 0.15f);
    d.spring.gain = s["spring"].value("gain", 1.0f);
    d.spring.maxAngle = s["spring"].value("maxAngle", 0.7f);
  }

  const float inv = 1.0f / (float)d.scale;

  // THE NEGATED AXIS NEEDS THE MODEL'S DEPTH ADDED BACK.
  //
  // Scene(Z-up) -> engine(Y-up) is (x, z, -y). That third component is the
  // problem: negating y sends the whole model to NEGATIVE engine z, spanning
  // -depth..0 instead of 0..depth. LoadVoxFile then rebases the voxels onto
  // their own min corner (voxload.cpp `em.cells[i] - em.mn`), so the geometry
  // the runtime actually holds starts at zero again.
  //
  // `offset` does NOT record that shift — it is measured AFTER the rebase and
  // reads (0,0,0) here — so a sidecar box converted by the same (x, z, -y) map
  // is left a full model-depth below art that has already been moved up. The
  // hilt centre came out at engine z -0.75 for a blade occupying 0..1.5: not
  // merely off, but on the far side of the origin from its own art.
  //
  // Subtracting `sceneMin` is exactly the rebase the art received (voxload.h),
  // so it lands both in one frame by construction rather than by a correction
  // term someone has to keep true. Deriving the shift from `size` instead
  // would be a second implementation of the same rule, and one that only
  // agrees while the model sits at the scene origin unrotated.
  //
  // Every consumer measures from the min corner: `p.restOffset` is
  // `item->offset` and `anchorLimb` is `gripLocal - restOffset` (avatar.cpp
  // EquipItem), the same `anchor - restOffset` relationship a limb states, and
  // mob.cpp documents its edge as measured from "the part's own ORIGIN (the
  // model's min corner)". A limb never hit this because a rig's generator
  // authors its boxes in the same scene frame it emits its anchors from, so
  // both sides moved together.
  //
  // The symptom this produced: the -90 degree grip rotation about Y maps the
  // item's z error onto WORLD X, so the whole 1.5-voxel mistake surfaced as
  // pure sideways float — the sword hanging a couple of voxels to the right of
  // the fist, correct in height and depth, which reads as a bad grip constant
  // rather than a frame bug.
  //
  // ONE LAST TRAP ON THE SAME AXIS: `sceneMin` is a CELL INDEX, the sidecar's
  // box is a CONTINUOUS extent. On an axis that merely shifts, the low cell and
  // the low face are the same number and the distinction is invisible. On the
  // NEGATED axis they are not: cells 0..n-1 map to cells -(n-1)..0, whose low
  // face is at -n. Subtracting the cell index there leaves the box exactly one
  // cell — a quarter of a world voxel at scale 4 — proud of the art, which is
  // small enough to read as "close enough" and wrong enough to see.
  //
  // So bias the negated component by one cell to reach the face. Written per
  // axis rather than as a blanket `+1` because only z is flipped by the
  // (x, z, -y) map; x and y need the index as it stands.
  //
  // A WORN piece has none of this: no single model to measure a scene origin
  // against, no fist to close on it, no blade. Its placement is the cover
  // list's `offset` against the limb it covers, which is measured in the
  // wearer's frame and so needs no rebase at all.
  const Vec3 modelOrigin =
      held ? Vec3{(float)held->sceneMin.x * inv, (float)held->sceneMin.y * inv,
                  (float)(held->sceneMin.z - 1) * inv}
           : Vec3{};

  // ---- grip contexts ------------------------------------------------------
  // Every context is read whole: no key inherits from another (see item.h).
  if (s.contains("grip") && s["grip"].is_object()) {
    for (auto it = s["grip"].begin(); it != s["grip"].end(); ++it) {
      const json& g = it.value();
      ItemGrip gr;
      if (g.contains("translation") && g["translation"].size() == 3)
        gr.translation = Vec3{g["translation"][0].get<float>(),
                              g["translation"][1].get<float>(),
                              g["translation"][2].get<float>()} * inv;
      if (g.contains("rotation") && g["rotation"].size() == 3)
        gr.rotation = QuatFromEulerDeg({g["rotation"][0].get<float>(),
                                        g["rotation"][1].get<float>(),
                                        g["rotation"][2].get<float>()});
      gr.scale = g.value("scale", 1.0f);
      d.grip[it.key()] = gr;
    }
  }
  // A worn piece is never held, so silence here is correct for it; for
  // anything else, no grip means an item that parks at the wearer's origin
  // sticking out of their navel, which reads as a physics glitch rather than
  // as the missing JSON key it is (item.h).
  if (d.grip.empty() && !worn)
    errors += "items: \"" + d.name +
              "\" declares no grip contexts — it cannot be held\n";

  // ---- the hilt box -------------------------------------------------------
  // Where the fist closes on this item, authored as a box in the item's own
  // micro units (item.h ItemHilt). Same micro -> world conversion as every
  // other length here. Stored as centre + half-extents because the centre is
  // what the socket alignment actually wants and the half-extents are what the
  // debug overlay and the selftest want; deriving either at each use site is
  // how two call sites end up disagreeing about the same box.
  if (s.contains("hilt") && s["hilt"].is_object()) {
    const json& h = s["hilt"];
    if (h.contains("min") && h["min"].size() == 3 &&
        h.contains("size") && h["size"].size() == 3) {
      const Vec3 mn{h["min"][0].get<float>(), h["min"][1].get<float>(),
                    h["min"][2].get<float>()};
      const Vec3 sz{h["size"][0].get<float>(), h["size"][1].get<float>(),
                    h["size"][2].get<float>()};
      if (sz.x > 0 && sz.y > 0 && sz.z > 0) {
        // Scene(Z-up) -> engine(Y-up) is (x, z, -y), the same map the edge
        // block takes. A box is axis-aligned in both frames, so converting the
        // two corners and re-deriving min/extent is enough — but the y/z swap
        // means the naive "min maps to min" is false on the negated axis.
        const Vec3 lo{mn.x, mn.z, -(mn.y + sz.y)};
        const Vec3 ex{sz.x, sz.z, sz.y};
        d.hilt.has = true;
        // Rebased onto the model's min corner (see modelOrigin above), because
        // that is the frame `gripLocal` is consumed in. The half-extents are a
        // SIZE, not a position, so they take the conversion but not the shift.
        d.hilt.center = (lo + ex * 0.5f) * inv - modelOrigin;
        d.hilt.halfExtents = ex * 0.5f * inv;
        // THE HILT MUST LAND ON THE ITEM'S OWN ART.
        //
        // Every frame error above is silent at runtime: the placement code
        // puts the hilt centre on the socket by construction, so the sword is
        // always exactly where the hilt box SAYS the grip is — and a hilt box
        // in the wrong frame simply moves the whole sword, with nothing left
        // to disagree with it. The selftest's grip check inherits that
        // circularity and cannot see the error either.
        //
        // What a wrong frame does break is the relationship to the GEOMETRY:
        // the box stops containing any of the item it claims to be the grip
        // of. That is checkable right here, where both are in hand, so check
        // it rather than trusting the conversion.
        int inside = 0;
        for (const PrefabVoxel& v : d.voxels) {
          const Vec3 c{((float)v.x + 0.5f) * inv, ((float)v.y + 0.5f) * inv,
                       ((float)v.z + 0.5f) * inv};
          const Vec3 dc = c - d.hilt.center;
          if (std::fabs(dc.x) <= d.hilt.halfExtents.x &&
              std::fabs(dc.y) <= d.hilt.halfExtents.y &&
              std::fabs(dc.z) <= d.hilt.halfExtents.z)
            inside++;
        }
        if (inside == 0)
          errors += "items: \"" + d.name +
                    "\" hilt box contains none of the item's own voxels — it "
                    "is in the wrong frame, and the item will be held by empty "
                    "space beside itself\n";
      } else {
        errors += "items: \"" + d.name +
                  "\" hilt has a non-positive size — ignored\n";
      }
    } else {
      errors += "items: \"" + d.name +
                "\" hilt needs both \"min\" and \"size\" (3 each) — ignored\n";
    }
  }

  // ---- cutting edge -------------------------------------------------------
  // Same shape and the same micro -> world conversion as a rig part's edge
  // (mob.cpp), including the scene(Z-up) -> engine(Y-up) axis map, so the
  // sidecar can speak the art's own coordinates.
  if (s.contains("edge") && s["edge"].is_object()) {
    const json& e = s["edge"];
    Vec3 ax{0, 0, 1};
    if (e.contains("axis") && e["axis"].size() == 3)
      ax = {e["axis"][0].get<float>(), e["axis"][1].get<float>(),
            e["axis"][2].get<float>()};
    const Vec3 axEngine{ax.x, ax.z, -ax.y};
    d.hasEdge = true;
    // NOT rebased like the hilt, deliberately. `from`/`to` are DISTANCES along
    // `axis`, not points in the box, so the segment they describe already lies
    // on the model's own origin line — there is no authored cross-section
    // position for a rebase to correct, and subtracting the model origin only
    // slides the segment off that line onto an arbitrary corner of the art.
    //
    // The real gap is that the sidecar cannot say WHERE ACROSS the blade the
    // edge runs, so it rides the origin line rather than the blade's mid-plane
    // (for the sword, y 0 / z 0 against a blade centred at y 0.5 / z 0.75).
    // `halfWidth` is wide enough to cover the difference here, so this is a
    // sharpness question rather than a placement bug — but it wants an
    // authored offset, not a borrowed one, and that is a schema change.
    d.edgeFrom = axEngine * (e.value("from", 0.0f) * inv);
    d.edgeTo = axEngine * (e.value("to", 0.0f) * inv);
    d.edgeHalfWidth = e.value("halfWidth", 1.0f) * inv;
    // THE FLAT, through the same scene(Z-up) -> engine(Y-up) map as `axis`.
    // A DIRECTION, so no `inv` scaling: this says which way the blade's face
    // points, not how big it is. Orthogonalized against the edge, because two
    // separately-authored axes that are nearly-but-not-quite perpendicular
    // would give the roll a slow shear nobody would ever trace back here.
    if (e.contains("flat") && e["flat"].size() == 3) {
      Vec3 fl{e["flat"][0].get<float>(), e["flat"][1].get<float>(),
              e["flat"][2].get<float>()};
      Vec3 flEngine{fl.x, fl.z, -fl.y};
      const Vec3 along = axEngine.normalized();
      flEngine = flEngine - along * along.dot(flEngine);
      if (flEngine.len() > 1e-4f) {
        d.edgeFlat = flEngine.normalized();
        d.hasEdgeFlat = true;
      }
    }
  }

  // Micro brick, packed into the SAME pool the rigs use — a held item is drawn
  // by the borrowed slot's own render path. A worn piece packed one brick PER
  // SHELL up in the cover block; there is no whole-item model to pack here.
  if (d.scale > 1 && held) {
    d.microModel =
        MicroBodyPack(micro, d.voxels, d.size, d.scale, "item/" + d.name,
                      errors);
    if (d.microModel < 0)
      errors += "items: \"" + d.name +
                "\" has no micro brick and will not render\n";
  }
  return true;
}

}  // namespace

bool LoadItems(const std::string& dir, size_t materialCount,
               MicroBodySet& micro, ItemLibrary& out, std::string& errors) {
  out = ItemLibrary{};
  const std::string path = dir + "/items.json";
  std::ifstream f(path);
  if (!f) {
    errors += "items: cannot open " + path + "\n";
    return false;
  }
  json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    errors += std::string("items: parse error: ") + e.what() + "\n";
    return false;
  }
  for (const auto& it : j.value("items", json::array())) {
    ItemDef d;
    d.name = it.value("id", "");
    if (d.name.empty()) {
      errors += "items: an entry has no \"id\" — skipped\n";
      continue;
    }
    // Kind by NAME, through the one table in game/item.h. An unknown kind is
    // skipped loudly rather than defaulting to None, which would be an item
    // that loads, appears in the pack, and then goes into no slot at all.
    const std::string kind = it.value("kind", "");
    d.kind = ItemKindFromName(kind);
    if (d.kind == ItemKind::None) {
      errors += "items: \"" + d.name + "\" has unknown kind \"" + kind +
                "\" — skipped\n";
      continue;
    }
    d.damage = it.value("damage", 12.0f);
    d.carveBonus = it.value("carveBonus", 0.0f);
    // The authored heft OVERRIDE. Absent (or 0) is the normal case and means
    // "derive it from the art" — LoadItemAsset below fills heftVolume and
    // ItemDef::HeftFactor does the division. Present is for the item whose
    // geometry lies about its mass.
    d.heft = it.value("heft", 0.0f);
    // items.json authors reach in world voxels at the legacy 10 vox/m
    // baseline; rescale so the same number means the same distance.
    d.reach = it.value("reach", 9.0f) *
              ((float)kVoxelsPerMetre / (float)kLegacyAuthoringVoxelsPerMetre);
    // A broken item is skipped, never fatal: one bad asset must not cost the
    // player their whole hotbar (DESIGN.md §6, the same rule mob defs follow).
    if (!LoadItemAsset(dir, materialCount, micro, d, errors)) continue;
    out.items.push_back(std::move(d));
  }
  if (out.items.empty()) errors += "items: no usable items in " + path + "\n";
  return !out.items.empty();
}

namespace {

// Exponential smoothing that is framerate-independent: the fraction of the
// error removed per second is what is specified, not the fraction per frame.
// The naive `a += (b - a) * k` form makes every constant in this file mean a
// different thing at 30 fps than at 144.
float SmoothAlpha(float halflife, float dt) {
  if (halflife <= 1e-5f) return 1.0f;
  return 1.0f - std::exp2(-dt / halflife);
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// Basis coords -> world, and back. The pair below is the only place the two
// frames meet: everything the stroke driver remembers is in the BASIS frame
// (x = right, y = up, z = fwd), because the basis turns with the view every
// tick and a world-space memory would read that turn as blade motion.
Vec3 ToWorld(const Vec3& l, const Vec3& right, const Vec3& up, const Vec3& fwd) {
  return right * l.x + up * l.y + fwd * l.z;
}
Vec3 ToBasis(const Vec3& w, const Vec3& right, const Vec3& up, const Vec3& fwd) {
  return Vec3{w.dot(right), w.dot(up), w.dot(fwd)};
}

// A unit vector perpendicular to `d`, chosen consistently. Only reached when
// the rig cannot say which way the flat of the blade faces.
Vec3 AnyPerp(const Vec3& d) {
  const Vec3 alt = std::fabs(d.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
  const Vec3 p = alt - d * d.dot(alt);
  return p.len() > 1e-5f ? p.normalized() : Vec3{1, 0, 0};
}

}  // namespace

// =============================================================================
// THE STROKE DRIVER
// =============================================================================
//
// Read melee.h first; this half of the file is only the arithmetic. The one
// thing worth restating next to the code is the SHAPE of the state, because
// everything else falls out of it:
//
//   az_, el_, radius_        the tip, on a sphere about the shoulder. PURE
//                            INTEGRAL of the control input. Nothing decays it,
//                            nothing pulls it toward a pose, and the clamps act
//                            on the STORED value so pushing into one banks
//                            nothing that has to be wound back.
//   swingAz_/El_/Out_        the committed cut's follow-through, ADDED to the
//                            above. This is the only thing that decays.
//   bladeDirL_/FlatL_/poleL_ derived, smoothed, in the basis frame.
//
// Azimuth is measured from +z (`fwd`) toward +x (`right`), so 0 is straight
// ahead and positive is the basis's right; elevation is the angle above the
// x/z plane. World space is entered only at the bottom of Update.

// =============================================================================
// THE DAMAGE SWEEP
// =============================================================================
//
// Lifted verbatim out of main.cpp's tick loop when the NPC and the gate needed
// it too; the only behavioural addition is the edge-alignment scale below.
EdgeSweepResult MeleeSweepDamage(const EdgeSweep& s, const MeleeTuning& t,
                                 const Mob& wielder, Physics& phys,
                                 MobSystem& mobs, DebrisSystem& debris,
                                 World& world,
                                 std::vector<ParticleSpawn>& spawns) {
  EdgeSweepResult out;
  if (!s.valid || s.dt <= 1e-6f) return out;
  // Tip speed is what scales the damage: the base of a blade barely moves in a
  // swing that whips the point through, and a cut should reflect that. Measured
  // over the tick, in world voxels/sec.
  const Vec3 travel = s.bNow - s.bPrev;
  out.tipSpeed = travel.len() / s.dt;
  if (out.tipSpeed <= t.minSpeed) return out;
  out.power = std::clamp(
      (out.tipSpeed - t.minSpeed) / std::max(t.fullSpeed - t.minSpeed, 1e-3f),
      0.0f, 1.0f);
  // THE EDGE HAS TO LEAD. A blade travelling in its own flat is a club: the
  // same speed through the same point does a fraction of the damage, and the
  // difference is the entire reason the stroke driver bothers to roll the
  // weapon. `edgeFloor` is what keeps a flat from being free — a flat still
  // bruises, and still breaks what it lands on.
  out.edgeAlign = MeleeEdgeAlign(s.flatNow, travel, t.edgeFloor);
  // ONE `power`, AND IT IS ALREADY EDGE-SCALED. Everything downstream — the
  // damage, the blade-cut scope, and the KERF'S DEPTH AND LENGTH below — reads
  // this value, so a flat-on slap does a shallower wound as well as a weaker
  // one without the wound model needing to know that edge alignment exists.
  // Deliberate: it is the one place the two halves of the melee overhaul meet,
  // and multiplying `edgeAlign` in a second time further down would square it.
  const float power = out.power * out.edgeAlign;
  const float radius = s.halfWidth + s.carveBonus;
  // ---- WHAT THE WOUND MODEL NEEDS, MEASURED ONCE ---------------------------
  // WHICH WAY THE EDGE IS GOING: how far the blade's MIDPOINT travelled over
  // the tick, which is the axis a kerf penetrates along and the one thing a
  // radius has no way to express. The mid-blade rather than the tip because a
  // cut near the hilt is going where the hilt is going. A per-sweep constant,
  // so it is lifted clear of the sample loops below.
  const auto& goreT = CurrentTuning().gore;
  const Vec3 sweepDir =
      ((s.aNow + s.bNow) - (s.aPrev + s.bPrev)).normalized();

  // ---- BLADE ON BLADE, ASKED FIRST AND ASKED GEOMETRICALLY ----------------
  //
  // Before any probe, because a parry ENDS the sweep: letting the ray probes
  // run first would let a blow that was stopped by a sword still open the arm
  // behind it on the same tick.
  //
  // And geometrically, because the probes cannot answer it. They are rays down
  // the swinging blade's axis — right for a body, useless against a weapon,
  // whose collider is the item's own art at the item's own scale and about a
  // quarter of a voxel thick. See MobSystem::FindParry for the measurement.
  {
    Mob* blocker = nullptr;
    uint64_t blockBody = 0;
    Vec3 blockAt{};
    if (mobs.FindParry(wielder, s.aPrev, s.bPrev, s.aNow, s.bNow,
                       t.blockGap + s.halfWidth, blocker, blockBody, blockAt)) {
      out.arrested = true;
      out.block.attackerId = wielder.Id();
      out.block.blockerId = blocker->Id();
      out.block.blockerBody = blockBody;
      out.block.at = blockAt;
      out.block.power = power;
      // THE BLOCKING BLADE PAYS. Ordinary limb damage on the item's own slot:
      // items carry hp and severImpactSpeed (item.h), so a weapon wears down
      // under repeated parries and one that catches something far too fast is
      // knocked out of the hand — both by mechanisms that were already there.
      // Deliberately NOT inside a BladeCutScope: a clang is not a
      // dismemberment and must not arm the wet cue.
      mobs.Damage(blockBody, s.damage * power * t.blockItemDamage, blockAt,
                  out.tipSpeed);
      // ...and the defender's guard is beaten open a little, deterministically.
      mobs.PushBlockEvent(out.block, t);
      return out;
    }
  }

  // Sample along the blade AND across the sweep, so a fast cut does not tunnel
  // between ticks. Both counts are bounded and scale with how far the blade
  // actually moved (CLAUDE.md rule 2): a stationary blade costs one probe, and
  // no swing can cost more than kMaxSteps * kMaxAlong however fast it is
  // flicked.
  const float sweepLen = travel.len();
  const int kMaxSteps = 6, kMaxAlong = 5;
  const int steps = std::clamp(
      (int)std::ceil(sweepLen / std::max(radius, 0.5f)), 1, kMaxSteps);
  const int along = std::clamp(
      (int)std::ceil((s.bNow - s.aNow).len() / std::max(radius, 0.5f)), 1,
      kMaxAlong);
  // One hit per body per swing tick: without this the same limb is carved once
  // per probe and a single cut removes a whole arm.
  std::vector<uint64_t> hitBodies;
  for (int step = 1; step <= steps && (int)hitBodies.size() < 8; step++) {
    const float u = (float)step / (float)steps;
    const Vec3 a = s.aPrev + (s.aNow - s.aPrev) * u;
    const Vec3 b = s.bPrev + (s.bNow - s.bPrev) * u;
    const Vec3 seg = b - a;
    const float segLen = seg.len();
    if (segLen < 1e-4f) continue;
    // ---- THE PROBES TILE THE BLADE; THEY DO NOT SAMPLE IT -------------------
    //
    // Each ray runs along the blade's own axis for exactly the distance to the
    // NEXT sample point, plus the blade's own half-thickness. That covers the
    // whole edge with no gaps, which is what "the pose is the hitbox" (melee.h
    // note 1) actually requires and what the first version did not do: it cast
    // `along + 1` rays of a FIXED ~1 voxel from points spaced 1.6 voxels apart,
    // so 40% of the blade probed nothing at all.
    //
    // On a human-sized target that is invisible — a torso is wide enough that
    // some ray finds it — and on a SWORD it is fatal. Measured in `npc-block`:
    // two blades whose edges passed within 0.28 voxels of each other, five
    // sweeps run, ZERO bodies hit. Emergent blocking cannot work through a
    // hitbox with holes in it, and neither can a thrust at a limb.
    //
    // The loop bound moves with it: `k < along` rather than `k <= along`,
    // because the last START point plus its own tile is what reaches the tip.
    // Casting from the tip as well would put a whole extra tile PAST the point,
    // which is a hitbox longer than the weapon.
    const float probe = std::max(segLen / (float)along + radius, 0.6f);
    for (int k = 0; k < along; k++) {
      const float v = (float)k / (float)along;
      const Vec3 p = a + (b - a) * v;
      float frac = 1.0f;
      const uint64_t hb = phys.CastRayBody(p, seg.normalized(), probe, frac);
      if (!hb) continue;
      bool seen = false;
      for (uint64_t h : hitBodies) seen |= (h == hb);
      if (seen) continue;
      // A weapon must not cut its wielder. The wielder's own parts are
      // permanently inside the swing arc — the blade starts in its own hand —
      // so without this every guard would saw through the arm holding it.
      if (wielder.OwnsBody(hb)) continue;
      const Vec3 at = p + seg.normalized() * (frac * probe);

      // A PROBE THAT DID FIND A WEAPON. The geometric test above is what
      // actually detects parries; this is the safety net for the rare ray that
      // happens to catch a blade edge-on, and its only job is to make sure a
      // weapon is never CARVED as if it were flesh. `HeldSlot()` IS the
      // borrowed slot the item filled (Mob::EquipItem), so this cannot drift
      // from what is really in the fist, and it is the same distinction
      // Mob::StainWound draws when it refuses to bleed a sword.
      {
        int li = -1;
        Mob* owner = mobs.FindOwner(hb, &li);
        if (owner != nullptr && li >= 0 && li == owner->HeldSlot()) continue;
      }
      hitBodies.push_back(hb);
      // LIVE FLESH CARVES; DEBRIS MELTS. The same two populations the laser
      // splits on, through the same two calls — a mob limb loses voxels exactly
      // where the edge crossed it, which is what makes dismemberment geometric
      // rather than a threshold (DESIGN.md §7 "Carving living bodies").
      const float dmg = s.damage * power;
      // Everything severed inside this scope is a BLADE cut, and gets the wet
      // dismember sound on top of the creature's own cry. Both calls below can
      // sever several frames deep — Damage() at zero hp or over the impact
      // threshold, CutLimb() when the lattice is cut through — so the cause is
      // marked around them rather than passed down through a chain the laser
      // and explosions also use.
      MobSystem::BladeCutScope blade(mobs, power);
      if (mobs.Damage(hb, dmg, at, out.tipSpeed)) {
        // A KERF, NOT A BITE. The radial carve this replaced took a sphere out
        // of the limb, which at any radius that felt like a sword was most of
        // an arm — and Damage() severed on contact anyway, so the shape never
        // got to matter. Now it is the only thing that decides dismemberment:
        // the slot follows the blade's own edge and the direction the swing is
        // going, and a limb comes off when the lattice has been cut through
        // (game/mob.h BladeCut).
        //
        // THIS IS THE ONLY PLACE THE KERF IS BUILT. It lives inside the sweep
        // rather than at the player's call site precisely because there are
        // three callers — the player's tick, an attacking NPC's tick, and the
        // gate — and a second copy of these six lines is how the player's cut
        // and the NPC's quietly stop being the same cut.
        BladeCut cut;
        cut.at = at;
        cut.edgeAxis = seg.normalized();
        // A stationary blade has no travel direction to speak of; fall back to
        // boring along its own length, which is what a press with no swing
        // behind it does.
        cut.cutDir = sweepDir.len() > 1e-4f ? sweepDir : seg.normalized();
        // The blade's OWN thickness decides the kerf's width; the tuning knob
        // only scales it, because the geometry is supposed to be what decides
        // the wound (items.json says so about carveBonus for the same reason).
        cut.halfWidth = std::max(radius * goreT.cutWidth, 0.08f);
        // `power` here is speed x edge-alignment (see the note where it is
        // formed) and `s.heft` is the weapon's own volume, so how deep the
        // wound goes is: how fast, how well-angled, and how much sword.
        cut.depth = (goreT.cutDepth + goreT.cutDepthPower * power) * s.heft;
        cut.length = goreT.cutLength * (0.4f + 0.6f * power) * s.heft;
        cut.power = power;
        // Counter-based, off the tick and the probe index: the ragged rim and
        // the blood soak must replay identically, and nothing here may key on
        // a Jolt float.
        cut.seed = s.tick * 2654435761u + (uint32_t)hitBodies.size() * 40503u;
        mobs.CutLimb(hb, cut, world, spawns);
      } else {
        debris.MeltBodyAt(hb, at, radius, world, spawns);
      }
    }
  }
  out.bodiesHit = (int)hitBodies.size();
  return out;
}

float MeleeEdgeAlign(const Vec3& flat, const Vec3& travel, float floorFrac) {
  const float fl = flat.len(), tl = travel.len();
  // No roll reported, or a blade that is not moving: neither is evidence of a
  // BAD angle, so neither may be punished. A stationary blade is already doing
  // no damage — `minSpeed` sees to that — and scaling it a second time here
  // would double-count the same fact.
  if (fl < 1e-6f || tl < 1e-6f) return 1.0f;
  // |cos| between the flat's normal and the travel: 1 is a pure slap with the
  // side, 0 is a perfectly edge-on cut. Absolute because a blade cuts equally
  // well on either face — this is a double-edged sword and, more to the point,
  // the sign only says which way the flat happens to be pointing.
  const float c = std::fabs(flat.dot(travel) / (fl * tl));
  const float align = 1.0f - std::clamp(c, 0.0f, 1.0f);
  const float floorF = std::clamp(floorFrac, 0.0f, 1.0f);
  return floorF + (1.0f - floorF) * align;
}

void MeleeState::Feed(float dx, float dy) {
  inputAccum_.x += dx;
  inputAccum_.y += dy;
}

void MeleeState::FeedReach(float dr) { inputAccum_.z += dr; }

void MeleeState::Step(const StrokeSample& s, float dt, bool armed,
                      const Vec3& right, const Vec3& up, const Vec3& fwd) {
  Feed(s.dx, s.dy);
  FeedReach(s.dReach);
  Update(dt, s.held, armed, right, up, fwd);
}

void MeleeState::SetStroke(const Vec3& handFromShoulder,
                           const Vec3& tipFromShoulder, const Vec3& flat,
                           float reach) {
  armHand_ = handFromShoulder;
  armTip_ = tipFromShoulder;
  armFlat_ = flat;
  // THE BLADE LENGTH IS MEASURED, NEVER AUTHORED HERE. It is the rigid
  // hand-to-point distance of whatever is in the fist this tick, so a dagger
  // and a greatsword steer the same way and neither needs a tuning row.
  bladeLen_ = (tipFromShoulder - handFromShoulder).len();
  armReach_ = reach;
  armValid_ = reach > 1e-3f;
}

void MeleeState::ClearArm() {
  armValid_ = false;
  armReach_ = 0;
  bladeLen_ = 0;
  armFlat_ = Vec3{};
}

float MeleeState::PoseWeight() const {
  switch (phase_) {
    case SwingPhase::Idle:
      return 0.0f;
    case SwingPhase::Recover: {
      // Only the RELEASING recover fades: a recover between two cuts (the
      // button is still down) is still the player's arm and handing it back
      // mid-combination would drop the blade to the walk pose for a fifth of a
      // second. `recoverHold_` is what the phase was entered for.
      if (recoverHold_) return 1.0f;
      float t =
          tuning.recoverTime > 1e-4f ? phaseTime_ / tuning.recoverTime : 1.0f;
      return std::clamp(1.0f - t, 0.0f, 1.0f);
    }
    default:
      return 1.0f;
  }
}

WeaponPose MeleeState::Pose() const {
  WeaponPose p;
  p.hand = hand_;
  p.bladeDir = bladeDir_;
  p.bladeFlat = bladeFlat_;
  p.bendPole = bendPole_;
  p.weight = PoseWeight();
  p.wristMaxAngle = tuning.wristMaxAngle;
  // The driver ALWAYS steers the blade. The flag exists for the other caller:
  // Mob::SetWeaponPose's legacy four-argument form, which the pose-limit gates
  // use to drive the arm at a bare point with no opinion about the weapon.
  p.steerBlade = true;
  return p;
}

void MeleeState::Arrest() {
  // Only a LIVE cut can be arrested. Guard and Idle have nothing to stop, and
  // a stroke already recovering must not have its recover restarted — a caller
  // that sees a parry on three consecutive ticks would otherwise hold the arm
  // in Recover forever.
  if (phase_ != SwingPhase::Wind && phase_ != SwingPhase::Slash) return;
  phase_ = SwingPhase::Recover;
  phaseTime_ = 0;
  // The follow-through the cut had left does NOT happen: the arc decays from
  // wherever the blade was stopped, which is what makes a parry read as the
  // blow being checked rather than as the animation finishing anyway.
  swingAz_ = 0;
  swingEl_ = 0;
  swingOut_ = 0;
  // The button is still down (that is what a parry mid-swing means), so the
  // arm is kept rather than handed back — see PoseWeight.
  recoverHold_ = true;
}

void MeleeState::Nudge(float dAz, float dEl) {
  // The STORED stroke, so the shove is subject to the same stops as any other
  // input and banks nothing past them (melee.h, "the clamp banks nothing").
  const float azLo = handSign_ >= 0 ? -tuning.azAcross : -tuning.azOut;
  const float azHi = handSign_ >= 0 ? tuning.azOut : tuning.azAcross;
  az_ = std::clamp(az_ + dAz, azLo, azHi);
  el_ = std::clamp(el_ + dEl, tuning.elMin, tuning.elMax);
}

void MeleeState::Reset() {
  phase_ = SwingPhase::Idle;
  phaseTime_ = 0;
  inputAccum_ = Vec3{};
  mouseVel_ = Vec3{};
  mouseSpeed_ = 0;
  cutDir_ = Vec3{};
  cutAz_ = cutEl_ = 0;
  az_ = el_ = radius_ = 0;
  swingAz_ = swingEl_ = swingOut_ = 0;
  tipPrev_ = Vec3{};
  tipVel_ = Vec3{};
  tangent_ = Vec3{};
  extendLive_ = 0;
  framePrimed_ = false;
  recoverHold_ = false;
}

// ---- the derived half -------------------------------------------------------
//
// Given the integrated stroke, produce the tip, the blade frame, the bend pole
// and finally the hand. Everything is in BASIS coordinates until the last four
// lines; nothing here reads the input.
void MeleeState::RadiusBand(float& lo, float& hi, float& handRadius) const {
  const float handReach =
      (armValid_ ? armReach_ : tuning.fallbackReach) * tuning.reachFraction;
  // THE LIVE extension, not the tuning target: a take-over starts the arm
  // wherever the animation had it and eases from there, and a band computed
  // from the target would refuse the pose the arm is actually in.
  handRadius = std::clamp(extendLive_ > 1e-4f ? extendLive_
                                              : handReach * tuning.handExtend,
                          0.05f, handReach);
  const float L = bladeLen_;
  if (L < 1e-4f) {
    // No blade: the point IS the hand, so the band is simply the arm.
    lo = handReach * 0.15f;
    hi = handReach;
    return;
  }
  // The reach annulus of a one-link chain of length L pinned at radius
  // handRadius. Margins keep the ends off the degenerate cases: at `hi` the
  // blade lies exactly along the arm's own line (nothing left to lean) and at
  // `lo` it doubles back on it.
  const float margin = std::min(0.25f, std::max(L, handRadius) * 0.05f);
  lo = std::fabs(L - handRadius) + margin;
  hi = L + handRadius - margin;
  if (hi <= lo) {           // degenerate: the two lengths coincide
    lo = std::max(handRadius * 0.4f, 0.05f);
    hi = handRadius + L;
  }
}

void MeleeState::RebuildFrame(float dt, const Vec3& right, const Vec3& up,
                              const Vec3& fwd) {
  float rLo = 0, rHi = 0, rHand = 0;
  RadiusBand(rLo, rHi, rHand);

  // THE TOTAL, clamped: the steered stroke plus the cut's follow-through. az_
  // and el_ were already clamped as they were integrated (that is what banks
  // nothing); this second clamp is on the SUM, because a follow-through must
  // not carry the point past vertical or through the far shoulder either.
  const float azHi = handSign_ > 0 ? tuning.azOut : tuning.azAcross;
  const float azLo = handSign_ > 0 ? -tuning.azAcross : -tuning.azOut;
  const float az = std::clamp(az_ + swingAz_, azLo, azHi);
  const float el = std::clamp(el_ + swingEl_, tuning.elMin, tuning.elMax);
  const float r = std::clamp(radius_ + swingOut_, rLo, rHi);

  const float ce = std::cos(el), se = std::sin(el);
  const Vec3 tipL{r * ce * std::sin(az), r * se, r * ce * std::cos(az)};

  // ---- how fast the point is moving, and which way ------------------------
  // Measured in BASIS coordinates on purpose. In world space, turning the view
  // moves the tip without the player having moved the blade at all, and the
  // edge would roll to lead a "travel" that is really the camera panning.
  const Vec3 inst =
      (framePrimed_ && dt > 1e-6f) ? (tipL - tipPrev_) * (1.0f / dt) : Vec3{};
  tipPrev_ = tipL;
  tipVel_ = framePrimed_
                ? Lerp(tipVel_, inst, SmoothAlpha(tuning.bladeSmoothing, dt))
                : Vec3{};

  const Vec3 radial = tipL.len() > 1e-5f ? tipL.normalized() : Vec3{0, 0, 1};
  // The TANGENTIAL part of the travel is the stroke: the radial part is a
  // thrust, and a thrust has no sweep plane to speak of. Held from the last
  // tick when there is nothing to read, so a blade brought to a stop keeps the
  // roll it was cutting with instead of snapping to an arbitrary one.
  {
    const Vec3 t = tipVel_ - radial * radial.dot(tipVel_);
    if (t.len() > 1e-3f) tangent_ = t.normalized();
  }

  // ---- HOW FAR THE BLADE LEANS OFF THE RADIUS -----------------------------
  //
  // NOT A TASTE CONSTANT — a law of cosines. The point is at radius `r`, the
  // blade is a rigid `bladeLen_` long, and the hand is to be held at `rHand`:
  // that triangle has exactly one interior angle at the point, so the blade's
  // angle to the shoulder-to-point line is DETERMINED. Solving it here instead
  // of picking a lean is what guarantees the derived hand is somewhere the arm
  // can actually be. The first version leaned a fixed amount off the radius
  // and, with a 1.1 m sword on a 1.0 m arm, put the hand AT the shoulder and
  // folded the whole chain into the chest — measured through the rig, the sword
  // then missed its commanded point by up to 19.8 voxels on an 11.2 voxel
  // stroke, which is the arm not following the mouse at all.
  //
  // WHICH SIDE it leans to is the only free choice, and `handLead` makes it:
  // +1 puts the hand AHEAD of the point along the travel, which is a sabre cut.
  //
  // THE ARM SETTLES, THE PLANE TURNS, AND THE BLADE IS REBUILT FROM BOTH. The
  // two smoothed quantities are `extendLive_` (how far the hand is held from
  // the shoulder) and `perpL_` (which way the lean goes); the direction itself
  // is recomputed exactly every tick, so |tip - hand| is bladeLen_ and |hand|
  // is extendLive_ at EVERY instant, transients included. Smoothing the
  // direction instead put the hand at the shoulder on every reversal — see the
  // note on perpL_ in melee.h.
  const float handReachNow =
      (armValid_ ? armReach_ : tuning.fallbackReach) * tuning.reachFraction;
  extendLive_ = extendLive_ > 1e-4f ? extendLive_ : rHand;
  extendLive_ +=
      (std::clamp(handReachNow * tuning.handExtend, 0.05f, handReachNow) -
       extendLive_) *
      SmoothAlpha(tuning.extendSmoothing, dt);

  // WHICH WAY THE LEAN GOES: with the travel (so the hand leads the point), or
  // against it. Rotated toward the target ABOUT THE RADIUS at a bounded rate,
  // which is the only way to cross a reversal without passing through the
  // degenerate middle. With nothing travelling the plane simply holds.
  {
    Vec3 want = tangent_;
    if (want.len() < 1e-3f) want = perpL_;
    want = want - radial * radial.dot(want);
    if (want.len() < 1e-4f) want = AnyPerp(radial);
    want = want.normalized();
    // Re-project the stored plane onto the CURRENT radius first: the radius
    // moved this tick, and a perpendicular to last tick's is not one to this.
    Vec3 cur = perpL_ - radial * radial.dot(perpL_);
    if (cur.len() < 1e-4f) cur = AnyPerp(radial);
    cur = cur.normalized();
    const float c = std::clamp(cur.dot(want), -1.0f, 1.0f);
    const float sgn = cur.cross(want).dot(radial) < 0.0f ? -1.0f : 1.0f;
    float turn = std::acos(c) * sgn;
    const float maxTurn = std::max(tuning.leanTurnRate, 0.0f) * dt;
    turn = std::clamp(turn, -maxTurn, maxTurn);
    // Rodrigues about the radius; `cur` is already perpendicular to it, so the
    // axial term drops out.
    perpL_ = cur * std::cos(turn) + radial.cross(cur) * std::sin(turn);
    perpL_ = perpL_.len() > 1e-5f ? perpL_.normalized() : cur;
  }

  Vec3 wantDir = radial;
  if (bladeLen_ > 1e-4f && r > 1e-4f) {
    const float cosT = std::clamp((r * r + bladeLen_ * bladeLen_ -
                                   extendLive_ * extendLive_) /
                                      (2.0f * bladeLen_ * r),
                                  -1.0f, 1.0f);
    const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    const float s = tuning.handLead >= 0.0f ? 1.0f : -1.0f;
    wantDir = radial * cosT - perpL_ * (sinT * s);
  }
  if (wantDir.len() < 1e-5f) wantDir = radial;
  wantDir = wantDir.normalized();
  // THE FLAT FACES OUT OF THE STROKE PLANE, which is the same statement as
  // "the edge leads the travel": the cutting plane is spanned by the blade and
  // by where it is going, so its normal is their cross product.
  Vec3 wantFlat = wantDir.cross(tangent_);
  if (wantFlat.len() < 1e-3f) wantFlat = bladeFlatL_;   // no travel: hold the roll
  if (wantFlat.len() < 1e-3f) wantFlat = AnyPerp(wantDir);
  wantFlat = wantFlat.normalized();
  const float a = SmoothAlpha(tuning.bladeSmoothing, dt);
  // NOT SMOOTHED — see above. The smoothing that makes this continuous lives in
  // `extendLive_` and `perpL_`, both of which the direction is exactly derived
  // from, so the hand-to-point constraint holds on every tick instead of only
  // in the steady state.
  bladeDirL_ = wantDir;
  bladeFlatL_ = Lerp(bladeFlatL_, wantFlat, a);
  // Re-orthogonalize every tick: two independently smoothed unit vectors drift
  // out of square, and a "flat normal" that is not perpendicular to the blade
  // is not a frame — the hand orientation built from it would shear.
  bladeFlatL_ = bladeFlatL_ - bladeDirL_ * bladeDirL_.dot(bladeFlatL_);
  bladeFlatL_ = bladeFlatL_.len() > 1e-5f ? bladeFlatL_.normalized()
                                          : AnyPerp(bladeDirL_);

  // THE HAND IS THE TIP MINUS A BLADE. Not the other way round — that
  // inversion IS this rewrite. The residual clamp below is a safety net; the
  // thing that actually keeps the arm in reach is `extendLive_`, which the
  // blade's own angle is solved against, so nothing banks.
  Vec3 handL = tipL - bladeDirL_ * bladeLen_;
  const float handReach =
      (armValid_ ? armReach_ : tuning.fallbackReach) * tuning.reachFraction;
  const float hd = handL.len();
  if (handReach > 1e-3f && hd > handReach) handL = handL * (handReach / hd);

  // THE ELBOW TRAILS THE HAND — and it is the HAND'S OWN travel that says so,
  // not the point's. The pole names the plane the two-bone solver bends in, and
  // a plane is only defined relative to the chain it bends: the solver
  // immediately orthogonalizes whatever it is given against the shoulder-to-
  // HAND direction. The first version handed it the tip's tangent, which for a
  // long blade is a completely different direction — measured mid-sweep, the
  // tip tangent came out at 153 degrees to the arm, so 89% of the pole was the
  // useless component the solver throws away and the bend plane was decided by
  // the 11% that survived. Building it from the hand's travel keeps it
  // perpendicular to the arm by construction.
  //
  // With no travel it falls back to straight behind, which is these rigs' own
  // authored arm pole ([0,0,-1]) and the pose a resting elbow is in.
  {
    const Vec3 handVel =
        (framePrimed_ && dt > 1e-6f) ? (handL - handPrev_) * (1.0f / dt)
                                     : Vec3{};
    handPrev_ = handL;
    handVel_ = framePrimed_ ? Lerp(handVel_, handVel, a) : Vec3{};
    const Vec3 handDir = handL.len() > 1e-4f ? handL.normalized() : Vec3{0, 0, 1};
    Vec3 along = handVel_ - handDir * handDir.dot(handVel_);
    Vec3 wantPole = along.len() > 1e-2f ? along.normalized() * -1.0f
                                        : Vec3{0, 0, -1};
    // ...orthogonalized here too, so the value handed across is already a plane
    // rather than a hint the solver has to rescue.
    wantPole = wantPole - handDir * handDir.dot(wantPole);
    if (wantPole.len() < 1e-3f) {
      wantPole = Vec3{0, 0, -1};
      wantPole = wantPole - handDir * handDir.dot(wantPole);
    }
    if (wantPole.len() < 1e-3f) wantPole = AnyPerp(handDir);
    poleL_ = Lerp(poleL_, wantPole.normalized(), a);
    poleL_ = poleL_ - handDir * handDir.dot(poleL_);
    poleL_ = poleL_.len() > 1e-5f ? poleL_.normalized() : AnyPerp(handDir);
  }

  tip_ = ToWorld(tipL, right, up, fwd);
  hand_ = ToWorld(handL, right, up, fwd);
  bladeDir_ = ToWorld(bladeDirL_, right, up, fwd);
  bladeFlat_ = ToWorld(bladeFlatL_, right, up, fwd);
  bendPole_ = ToWorld(poleL_, right, up, fwd);
  framePrimed_ = true;
}

void MeleeState::Update(float dt, bool held, bool armed, const Vec3& right,
                        const Vec3& up, const Vec3& fwd) {
  if (dt <= 0) return;

  // ---- input velocity ------------------------------------------------------
  // The accumulator holds this frame's raw deltas; convert to units/sec and
  // smooth. Draining it here (rather than in Feed) is what makes the
  // per-frame/per-tick split in melee.h work: several ticks may run per frame,
  // and only the first sees new motion.
  const Vec3 delta = inputAccum_;    // THIS tick's raw travel
  const Vec3 instant{inputAccum_.x / dt, inputAccum_.y / dt, 0};
  inputAccum_ = Vec3{};
  mouseVel_ = Lerp(mouseVel_, instant, SmoothAlpha(tuning.dirSmoothing, dt));
  mouseSpeed_ = std::sqrt(mouseVel_.x * mouseVel_.x + mouseVel_.y * mouseVel_.y);

  // Screen motion -> a direction in CONTROL space (azimuth, elevation). Screen
  // +y is DOWN, so it maps to -elevation: a downward flick must cut downward,
  // and getting this sign wrong produces a weapon that mirrors the player's
  // hand, which reads as broken long before anyone works out why.
  //
  // THE SAME MAPPING THE STROKE MOVES BY, gains included, so the cut direction
  // is by construction the direction the point is already travelling. Built
  // from the smoothed VELOCITY rather than this tick's raw delta because one
  // tick is far too noisy to steer a cut with.
  const float gAz = mouseVel_.x * tuning.aimGainX;
  const float gEl = -mouseVel_.y * tuning.aimGainY;
  const float gLen = std::sqrt(gAz * gAz + gEl * gEl);
  const bool haveDir = gLen > 1e-9f;
  const float dirAz = haveDir ? gAz / gLen : 0.0f;
  const float dirEl = haveDir ? gEl / gLen : 0.0f;

  if (!armed) {
    // Unarmed: collapse to idle and forget any half-built swing, so picking a
    // weapon back up never resumes a cut the player started with empty hands.
    if (phase_ != SwingPhase::Idle) Reset();
  }

  phaseTime_ += dt;

  float rLo = 0, rHi = 0, rHand = 0;
  RadiusBand(rLo, rHi, rHand);
  const float tipReach = rHi;
  const float azHi = handSign_ > 0 ? tuning.azOut : tuning.azAcross;
  const float azLo = handSign_ > 0 ? -tuning.azAcross : -tuning.azOut;

  // The seed of last resort: only reached when the rig cannot say where its own
  // blade is (SetStroke never called, or the arm is gone). Never a pose the
  // live hand is pulled toward.
  const Vec3 guard = fwd * tuning.guardForward + up * tuning.guardUp +
                     right * tuning.guardSide;

  bool seededThisTick = false;

  switch (phase_) {
    case SwingPhase::Idle:
      if (armed && held) {
        // TAKE OVER FROM WHERE THE SWORD IS. The stroke starts at the blade's
        // ACTUAL point, expressed in the basis frame, so the arm keeps the pose
        // the walk cycle left it in and the player pushes it from there.
        // Because the seed round-trips through the same shoulder-relative
        // convention the IK target uses (Mob::WeaponStrokePose), the first
        // driven tick asks the solver for the pose it is ALREADY in — weight
        // can go straight to 1 with nothing visible happening, which is the
        // whole point.
        const Vec3 seedTip = armValid_ ? armTip_ : guard;
        const Vec3 seedHand = armValid_ ? armHand_ : guard;
        Vec3 tipL = ToBasis(seedTip, right, up, fwd);
        // THE ARM STARTS AS EXTENDED AS IT ACTUALLY IS. `extendLive_` is what
        // the whole blade geometry is solved against, so seeding it from the
        // live hand is what makes the take-over exact: the law of cosines then
        // reproduces the blade's real angle rather than the tuning's, and the
        // arm eases out to `handExtend` over the following fifth of a second.
        extendLive_ = std::max(ToBasis(seedHand, right, up, fwd).len(), 0.05f);
        radius_ = tipL.len();
        // The band has to be recomputed against that seeded extension, or the
        // clamp below would judge the pose the arm is IN against the pose the
        // tuning wants it in and move the blade on the take-over tick.
        RadiusBand(rLo, rHi, rHand);
        if (radius_ < 1e-4f) {
          tipL = Vec3{0, 0, tipReach};
          radius_ = tipReach;
        }
        el_ = std::asin(std::clamp(tipL.y / radius_, -1.0f, 1.0f));
        az_ = std::atan2(tipL.x, tipL.z);
        // Clamped like any other stroke state. A rig whose rest pose sat
        // outside the window would be moved by this — the window is authored
        // wide enough that a hanging arm is inside it, and elMin's comment in
        // melee.h says so.
        az_ = std::clamp(az_, azLo, azHi);
        el_ = std::clamp(el_, tuning.elMin, tuning.elMax);
        radius_ = std::clamp(radius_, rLo, rHi);
        swingAz_ = swingEl_ = swingOut_ = 0;

        // SEED THE DERIVED FRAME FROM THE BLADE ITSELF, and skip this tick's
        // rebuild entirely. One tick of smoothing toward the commanded frame
        // would already move the hand by ~19% of the difference, which is a
        // pop — small, but exactly the pop take-over exists to avoid.
        const Vec3 handL = ToBasis(seedHand, right, up, fwd);
        const Vec3 bd = tipL - handL;
        bladeDirL_ = bd.len() > 1e-4f
                         ? bd.normalized()
                         : (tipL.len() > 1e-5f ? tipL.normalized()
                                               : Vec3{0, 0, 1});
        Vec3 fl = ToBasis(armFlat_, right, up, fwd);
        fl = fl - bladeDirL_ * bladeDirL_.dot(fl);
        bladeFlatL_ = fl.len() > 1e-3f ? fl.normalized() : AnyPerp(bladeDirL_);
        // AND THE LEAN PLANE FROM THE BLADE'S OWN ANGLE. Inverting the law of
        // cosines' own form: the blade is `radial * cos - perp * sin * s`, so
        // the perpendicular is what is left of it once the radial part is
        // taken out, negated by the lead's sign. Seeded this way, the first
        // rebuilt tick reproduces the seed exactly instead of rotating the
        // sword to whichever side the tuning happens to prefer.
        {
          const Vec3 rad = tipL.len() > 1e-5f ? tipL.normalized() : Vec3{0, 0, 1};
          Vec3 pc = bladeDirL_ - rad * rad.dot(bladeDirL_);
          const float s = tuning.handLead >= 0.0f ? 1.0f : -1.0f;
          if (pc.len() > 1e-4f) {
            perpL_ = pc.normalized() * -s;
          } else {
            // A blade that starts along its own radius says nothing about which
            // way it will lean, and the fallback matters: `AnyPerp` picks the
            // VERTICAL perpendicular, which starts every stroke leaning
            // downward and takes a fifth of a second to rotate out. A sword is
            // swung sideways far more often than it is dropped, so the
            // horizontal perpendicular is the better prior — and it is the one
            // a horizontal cut is already asking for.
            const Vec3 h = rad.cross(Vec3{0, 1, 0});
            perpL_ = h.len() > 1e-3f ? h.normalized() : AnyPerp(rad);
          }
        }
        poleL_ = Vec3{0, 0, -1};
        tangent_ = Vec3{};
        tipVel_ = Vec3{};
        tipPrev_ = tipL;
        handPrev_ = handL;
        handVel_ = Vec3{};
        framePrimed_ = true;
        tip_ = ToWorld(tipL, right, up, fwd);
        hand_ = ToWorld(handL, right, up, fwd);
        bladeDir_ = ToWorld(bladeDirL_, right, up, fwd);
        bladeFlat_ = ToWorld(bladeFlatL_, right, up, fwd);
        bendPole_ = ToWorld(poleL_, right, up, fwd);
        seededThisTick = true;

        phase_ = SwingPhase::Guard;
        phaseTime_ = 0;
      }
      break;

    case SwingPhase::Guard:
    case SwingPhase::Wind: {
      if (!held) {
        // Released: unwind whatever follow-through is left and hand the arm
        // back (PoseWeight fades over this phase).
        recoverHold_ = false;
        phase_ = SwingPhase::Recover;
        phaseTime_ = 0;
        break;
      }
      phase_ = mouseSpeed_ > tuning.commitSpeed * 0.35f ? SwingPhase::Wind
                                                        : SwingPhase::Guard;
      // COMMIT. The ARC's direction is frozen here and not touched again until
      // the slash ends: a cut whose own arc keeps re-steering mid-swing feels
      // like dragging the blade through treacle. (The player's steering is NOT
      // frozen — az_/el_ keep integrating underneath, which is the
      // follow-through melee.h promises.) commitSpeed is measured on TRUE input
      // speed, so it means the same thing at every value of the aim gains.
      if (mouseSpeed_ > tuning.commitSpeed && haveDir) {
        cutAz_ = dirAz;
        cutEl_ = dirEl;
        cutDir_ = (right * dirAz + up * dirEl).normalized();
        phase_ = SwingPhase::Slash;
        phaseTime_ = 0;
      }
      break;
    }

    case SwingPhase::Slash:
      if (phaseTime_ >= tuning.slashTime) {
        // FOLD THE ARC INTO THE STROKE. A cut ENDS WHERE IT WENT: after a full
        // sweep the sword really is across your body, and the player steers it
        // back from there. Unwinding the arc over the recover instead — which
        // the previous law did, because its arc was centred on the hand and
        // only half of it was follow-through — would drag the blade two
        // radians backwards through the target it had just passed through.
        //
        // Continuous by construction: at e = 1 the arc is exactly `swingAz_`,
        // so moving it from one accumulator to the other changes no geometry.
        // `swingOut_` is deliberately NOT folded — the mid-stroke bulge is a
        // shape, not a destination, and it belongs back at the steered radius.
        az_ = std::clamp(az_ + swingAz_, azLo, azHi);
        el_ = std::clamp(el_ + swingEl_, tuning.elMin, tuning.elMax);
        swingAz_ = swingEl_ = 0;
        recoverHold_ = armed && held;
        phase_ = SwingPhase::Recover;
        phaseTime_ = 0;
      }
      break;

    case SwingPhase::Recover:
      if (phaseTime_ >= tuning.recoverTime) {
        phase_ = (armed && held) ? SwingPhase::Guard : SwingPhase::Idle;
        phaseTime_ = 0;
        swingAz_ = swingEl_ = swingOut_ = 0;
      }
      break;
  }

  // ---- integrate the stroke ------------------------------------------------
  // ONE control law for every live phase: the input moves the point, and the
  // point stays where it was moved to. Nothing here reads a pose to return to,
  // which is what makes the blade aimable — you can put it high on the right
  // and leave it there, and the next push starts from there.
  //
  // Integration continues THROUGH the slash on purpose: the mouse is still
  // moving during those 170 ms and the arc below is added on top, so a cut is
  // the player's own travel plus a follow-through rather than a canned stroke
  // that ignores the second half of the flick.
  //
  // THE CLAMPS ARE ON THE STORED VALUE, not on a target derived from it. That
  // is what stops a sustained push into a stop from banking travel that has to
  // be wound back before the blade moves again.
  if (phase_ != SwingPhase::Idle && !seededThisTick) {
    az_ = std::clamp(az_ + delta.x * tuning.aimGainX, azLo, azHi);
    el_ =
        std::clamp(el_ - delta.y * tuning.aimGainY, tuning.elMin, tuning.elMax);
    radius_ = std::clamp(radius_ + delta.z * tuning.reachGain, rLo, rHi);
  }

  switch (phase_) {
    case SwingPhase::Slash: {
      // THE CUT, as an arc ADDED to the stroke the player is steering: the
      // point travels from the near side of the cut direction, through where
      // the player has it aimed, out to the far side — so the blade passes
      // ACROSS the wielder's front rather than poking outward. `t` is eased so
      // the middle of the stroke is the fast part, which is both how a real cut
      // works and what makes the speed-scaled damage land at the middle of the
      // arc where the player aimed.
      //
      // The arc BOWS OUTWARD (swingOut_) instead of holding one radius: an arm
      // swings about a shoulder and is furthest from the body at mid-stroke.
      // That bulge is also what carries the blade through a target rather than
      // past it, and it is the radial channel earning its keep.
      const float t =
          std::clamp(phaseTime_ / std::max(tuning.slashTime, 1e-4f), 0.0f, 1.0f);
      const float e = t * t * (3.0f - 2.0f * t);   // smoothstep
      const float bow = 4.0f * e * (1.0f - e);     // 0 at the ends, 1 mid
      // The anticipation rides on the same bow, so it is zero at both ends:
      // no pop on the tick that commits, and the arc still finishes at exactly
      // one `swingArc` for the fold above to absorb.
      const float drive = e - tuning.swingAnticipate * bow;
      swingAz_ = cutAz_ * tuning.swingArc * drive;
      swingEl_ = cutEl_ * tuning.swingArc * drive;
      swingOut_ = bow * tuning.swingExtend * tipReach;
      break;
    }

    case SwingPhase::Recover: {
      // Unwind only the follow-through. The steered stroke is untouched, so a
      // cut ENDS where the mouse ended — the blade does not spring back to a
      // guard the player never asked for.
      const float k = SmoothAlpha(0.07f, dt);
      swingAz_ += (0.0f - swingAz_) * k;
      swingEl_ += (0.0f - swingEl_) * k;
      swingOut_ += (0.0f - swingOut_) * k;
      break;
    }

    default:
      break;
  }

  if (phase_ == SwingPhase::Idle) {
    // Let the arm hang; the walk cycle owns the pose. Decaying rather than
    // snapping means a stale offset never re-enters as a jump.
    const float k = SmoothAlpha(0.12f, dt);
    hand_ = Lerp(hand_, Vec3{}, k);
    tip_ = Lerp(tip_, Vec3{}, k);
    framePrimed_ = false;
  } else if (!seededThisTick) {
    RebuildFrame(dt, right, up, fwd);
  }

  if (bladeDir_.len() < 1e-4f) bladeDir_ = up;
  if (bladeFlat_.len() < 1e-4f) bladeFlat_ = fwd;
  if (bendPole_.len() < 1e-4f) bendPole_ = fwd * -1.0f;
}

