#include "game/melee.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sim/scale.h"    // SkinScaleFor / NeededArtUpsample / kLegacyAuthoring*
#include "sim/voxload.h"  // UpsamplePrefab

// See melee.h for what this is and why. This file is only the motion: the
// damage side lives in main.cpp's tick loop, which owns the ray casts and the
// spawn/op lists a carve has to reach.

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

}  // namespace

void MeleeState::AddMouse(float dx, float dy) {
  mouseAccum_.x += dx;
  mouseAccum_.y += dy;
}

void MeleeState::SetArm(const Vec3& handFromShoulder, float reach) {
  armHand_ = handFromShoulder;
  armReach_ = reach;
  armValid_ = reach > 1e-3f;
}

void MeleeState::ClearArm() {
  armValid_ = false;
  armReach_ = 0;
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
      float t = tuning.recoverTime > 1e-4f ? phaseTime_ / tuning.recoverTime : 1.0f;
      return std::clamp(1.0f - t, 0.0f, 1.0f);
    }
    default:
      return 1.0f;
  }
}

void MeleeState::Reset() {
  phase_ = SwingPhase::Idle;
  phaseTime_ = 0;
  mouseAccum_ = Vec3{};
  mouseVel_ = Vec3{};
  mouseSpeed_ = 0;
  cutDir_ = Vec3{};
  handCam_ = Vec3{};
  swing_ = Vec3{};
  recoverHold_ = false;
}

void MeleeState::Update(float dt, bool held, bool armed, const Vec3& right,
                        const Vec3& up, const Vec3& fwd) {
  if (dt <= 0) return;

  // ---- mouse velocity ------------------------------------------------------
  // The accumulator holds this frame's raw pixels; convert to px/sec and
  // smooth. Draining it here (rather than in AddMouse) is what makes the
  // per-frame/per-tick split in melee.h work: several ticks may run per frame,
  // and only the first sees new motion.
  const Vec3 pixels = mouseAccum_;   // THIS tick's raw travel, in pixels
  Vec3 instant{mouseAccum_.x / dt, mouseAccum_.y / dt, 0};
  mouseAccum_ = Vec3{};
  mouseVel_ = Lerp(mouseVel_, instant, SmoothAlpha(tuning.dirSmoothing, dt));
  mouseSpeed_ = std::sqrt(mouseVel_.x * mouseVel_.x + mouseVel_.y * mouseVel_.y);

  // Screen motion -> a direction in the camera plane. Screen +y is DOWN, so it
  // maps to -up: a downward flick must cut downward, and getting this sign
  // wrong produces a weapon that mirrors the player's hand, which reads as
  // broken long before anyone works out why.
  //
  // THE SAME MAPPING THE HAND MOVES BY, gains included, so the cut direction is
  // by construction the direction the hand is already travelling. (It used to
  // be a separate pair of gains with a mirrored X, from when the mouse aimed
  // the blade instead of carrying the hand — see melee.h.) Built from the
  // smoothed VELOCITY rather than this tick's pixels because one tick of raw
  // delta is far too noisy to steer a cut with.
  Vec3 gained = right * (mouseVel_.x * tuning.moveGainX) +
                up * (-mouseVel_.y * tuning.moveGainY);
  Vec3 screenDir = gained.len() > 1e-6f ? gained.normalized() : Vec3{};

  // ---- the hand's own travel this tick --------------------------------------
  // A DISPLACEMENT, not a target. `pixels` is what the mouse physically moved
  // since the last tick (AddMouse accumulates per FRAME and this drains it), so
  // integrating it is exactly "the hand went as far as the mouse went" — and it
  // is immune to the frame/tick ratio: a frame with no tick leaves its pixels
  // in the accumulator for the next one rather than losing or doubling them.
  const Vec3 travel = right * (pixels.x * tuning.moveGainX) +
                      up * (-pixels.y * tuning.moveGainY);

  if (!armed) {
    // Unarmed: collapse to idle and forget any half-built swing, so picking a
    // weapon back up never resumes a cut the player started with empty hands.
    if (phase_ != SwingPhase::Idle) Reset();
  }

  phaseTime_ += dt;

  // The seed of last resort: only reached when the rig cannot say where its own
  // hand is (SetArm never called, or the arm is gone). Never a pose the live
  // hand is pulled toward.
  const Vec3 guard = fwd * tuning.guardForward + up * tuning.guardUp +
                     right * tuning.guardSide;

  switch (phase_) {
    case SwingPhase::Idle:
      if (armed && held) {
        // TAKE OVER FROM WHERE THE SWORD IS. The arm keeps the pose the walk
        // cycle left it in, expressed in the camera frame so the mouse can
        // push it from there. Because the seed round-trips through the same
        // shoulder-relative convention the IK target uses (see
        // Mob::WeaponArmPose), the first driven tick asks the solver for the
        // pose it is ALREADY in — weight can go straight to 1 with nothing
        // visible happening, which is the whole point.
        const Vec3 seed = armValid_ ? armHand_ : guard;
        handCam_ = Vec3{seed.dot(right), seed.dot(up), seed.dot(fwd)};
        swing_ = Vec3{};
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
      // COMMIT. The direction is frozen here and not touched again until the
      // slash ends: a cut that keeps steering with the mouse mid-swing feels
      // like dragging the blade through treacle, and it also makes the sweep
      // curve, which the damage code would then have to chord.
      // screenDir is unit-or-zero, so this is the "there is a direction at
      // all" guard; commitSpeed is measured on TRUE mouse speed in pixels, so
      // it means the same thing at every value of the move gains.
      if (mouseSpeed_ > tuning.commitSpeed && screenDir.len() > 0.5f) {
        cutDir_ = screenDir;
        phase_ = SwingPhase::Slash;
        phaseTime_ = 0;
      }
      break;
    }

    case SwingPhase::Slash:
      if (phaseTime_ >= tuning.slashTime) {
        recoverHold_ = armed && held;
        phase_ = SwingPhase::Recover;
        phaseTime_ = 0;
      }
      break;

    case SwingPhase::Recover:
      if (phaseTime_ >= tuning.recoverTime) {
        phase_ = (armed && held) ? SwingPhase::Guard : SwingPhase::Idle;
        phaseTime_ = 0;
        swing_ = Vec3{};
      }
      break;
  }

  // ---- the hand ------------------------------------------------------------
  // ONE control law for every live phase: the mouse moves the hand, and the
  // hand stays where it was moved to. Nothing here reads a pose to return to,
  // which is what makes the blade aimable — you can put it high on the right
  // and leave it there, and the next push starts from there.
  //
  // Integration continues THROUGH the slash on purpose: the mouse is still
  // moving during those 170 ms and the arc below is added on top, so a cut is
  // the player's own travel plus a follow-through rather than a canned stroke
  // that ignores the second half of the flick.
  if (phase_ != SwingPhase::Idle) {
    handCam_ += Vec3{travel.dot(right), travel.dot(up), travel.dot(fwd)};
    // Clamp to what the arm can actually reach. The stored value is clamped —
    // not just the target handed to the IK — so pushing into the limit does not
    // bank travel that has to be wound back before the arm moves again.
    const float reach =
        (armValid_ ? armReach_ : tuning.fallbackReach) * tuning.reachFraction;
    const float d = handCam_.len();
    if (reach > 1e-3f && d > reach) handCam_ = handCam_ * (reach / d);
  }

  switch (phase_) {
    case SwingPhase::Idle:
      // Let the arm hang; the walk cycle owns the pose. Decaying rather than
      // snapping means a stale offset never re-enters as a jump.
      hand_ = Lerp(hand_, Vec3{}, SmoothAlpha(0.12f, dt));
      break;

    case SwingPhase::Slash: {
      // The cut, as an offset ADDED to the hand the player is steering: the
      // blade travels from the near side of the cut direction, through where
      // the hand is, out to the far side, so the weapon passes ACROSS the
      // player's front rather than poking outward. `t` is eased so the middle
      // of the stroke is the fast part, which is both how a real cut works and
      // what makes the speed-scaled damage land at the middle of the arc where
      // the player aimed.
      //
      // The arc BOWS OUTWARD (the fwd term) instead of being a straight slide:
      // an arm swings about a shoulder, so the hand is furthest from the body
      // at mid-stroke. That bulge is also what carries the blade through a
      // target rather than past it.
      float t = std::clamp(phaseTime_ / std::max(tuning.slashTime, 1e-4f), 0.0f,
                           1.0f);
      float e = t * t * (3.0f - 2.0f * t);           // smoothstep
      float bow = 4.0f * e * (1.0f - e);             // 0 at the ends, 1 mid
      swing_ = cutDir_ * (tuning.swingReach * (e - 0.5f)) +
               fwd * (bow * tuning.swingReach * 0.28f);
      break;
    }

    case SwingPhase::Recover:
      // Unwind only the follow-through. The steered position is untouched, so a
      // cut ENDS where the mouse ended — the arm does not spring back to a
      // guard the player never asked for.
      swing_ = Lerp(swing_, Vec3{}, SmoothAlpha(0.07f, dt));
      break;

    default:
      break;
  }

  if (phase_ != SwingPhase::Idle) {
    hand_ = right * handCam_.x + up * handCam_.y + fwd * handCam_.z + swing_;
  }
  // The blade is NOT re-aimed: it keeps the angle the fist holds it at, and the
  // arm is what moves (see Mob::SetWeaponPose). These two are reported for the
  // HUD only.
  bladeDir_ = up;
  bladeUp_ = fwd;

  if (bladeDir_.len() < 1e-4f) bladeDir_ = up;
  if (bladeUp_.len() < 1e-4f) bladeUp_ = fwd;
}
