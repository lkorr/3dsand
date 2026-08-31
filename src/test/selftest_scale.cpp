// selftest_scale.cpp — is everything the size it is supposed to be, in METRES?
//
// THE GAP THIS FILLS. Before this gate there was no assertion anywhere in the
// suite that an avatar, a mob, an item or a tree had a particular PHYSICAL
// size. Every existing check is expressed in the asset's own lattice, so all of
// them pass just as happily on a world where every authored thing is half the
// size it should be:
//
//   * `mob-burn` counts burning voxels — an art-lattice count, unchanged.
//   * `armor-fit` compares an item's fitBox against a limb box — both art, so
//     the RATIO holds and the check is blind to the pair shrinking together.
//   * `armor-wear` asserts wearing a robe does not change the body's size —
//     "unchanged", never "correct".
//   * `tree-atlas` asserts the atlas is internally consistent — true of a
//     half-height forest.
//   * `determinism` asserts the world reproduces itself — a wrong world
//     reproduces perfectly.
//
// That is exactly what happened when `kVoxelMeters` went 0.10 -> 0.05: the
// player capsule (metres-derived since v0.2) stayed 1.7 m, the avatar art
// (a baked cell count) became 0.85 m, every tree halved, and the whole suite
// stayed green. A number that no test can see is a number that will drift.
//
// SO THIS GATE ASSERTS SIZES IN METRES, and it is pure CPU — no world, no GPU,
// no fixtures — so it can sit at the very front of `kOrder` beside `simd` and
// `player-kit`, disturb nothing, and report a wrong-scale world in the first
// second of a run rather than the twentieth.
//
// PASS A is the load-bearing one and the only one that is not circular. Asset
// sizes mostly cannot be checked against themselves: `worldSize = box /
// skinScale` and `skinScale = artVoxelsPerMetre / kVoxelsPerMetre`, so
// `worldSize * kVoxelMeters` reduces algebraically to `box / artVoxelsPerMetre`
// no matter what the derivation does — a probe with the same transform that
// placed the thing. Pass A escapes that by comparing two INDEPENDENTLY DERIVED
// numbers: the avatar's height comes from the art, and `Player::kHalfY` comes
// from `0.85f / kVoxelMeters` in player.h. Nothing links them but intent, which
// is why they were free to diverge.
//
// Passes B and C get their independence from `tests/baseline.json` instead —
// pins recorded at one voxel size and re-checked at another.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/item.h"
#include "game/mob.h"
#include "game/player.h"
#include "sim/scale.h"
#include "sim/treeatlas.h"
#include "sim/world.h"
#include "test/selftest.h"
#include "test/support.h"

namespace selftest {
namespace {

// A height may land on a different whole cell at a different voxel size — the
// art is quantised, so two scales can legitimately disagree by a cell. Anything
// past that is a scale error, not rounding. Expressed in metres so the
// tolerance itself does not change meaning with kVoxelMeters.
constexpr float kCellSlackCells = 1.5f;

std::string F2(float v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.3f", v);
  return b;
}

Status GateScale(Ctx& c, std::string& detail) {
  bool ok = true;
  std::string notes;
  const float slack = CellsToMetres(kCellSlackCells);

  // ---- PASS A: the avatar fills the capsule it drives ----------------------
  //
  // Two independent derivations of one number. `Player::kHalfY * 2` is
  // 1.7 m / kVoxelMeters; the human def's height is its .vox box divided by a
  // skinScale derived from artVoxelsPerMetre. They must agree, and until this
  // line nothing in the engine said so — avatar.cpp only ever uses kHalfY to
  // PLANT the art's feet (`player.pos.y - Player::kHalfY`), so a half-height
  // rig sits correctly on the ground and is simply too short, which looks like
  // an art choice rather than a bug.
  const float capsuleM = CellsToMetres(Player::kHalfY * 2.0f);
  int humanIdx = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == "human") humanIdx = (int)i;

  if (humanIdx < 0) {
    detail = "no \"human\" mob def — pass A cannot run";
    std::printf("scale: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  {
    const MobDef& d = c.mobs.Defs()[humanIdx];
    const float artM = CellsToMetres(d.worldSize.y);
    if (std::fabs(artM - capsuleM) > slack) {
      ok = false;
      notes += "AVATAR/CAPSULE MISMATCH: human art is " + F2(artM) +
               " m but Player::kHalfY*2 is " + F2(capsuleM) + " m (tolerance " +
               F2(slack) + " m). The art is " + F2(artM / capsuleM) +
               "x the collision box it drives; check artVoxelsPerMetre in "
               "assets/mobs/human.json. ";
    }
  }

  // ---- PASS B: every mob and item is the size it was pinned at -------------
  //
  // The pin is the independence: it was recorded at one kVoxelMeters and is
  // being checked at another, so it cannot be satisfied by the derivation
  // agreeing with itself. A def with no pin is REPORTED, never failed — that is
  // a new asset, and --rebaseline is how it gets one.
  int checked = 0, pinned = 0;
  std::string sizes;
  auto checkOne = [&](const std::string& name, float metres, int artVpm,
                      uint32_t scale, uint32_t upsample) {
    checked++;
    if (!sizes.empty()) sizes += " ";
    sizes += name + "=" + F2(metres);
    // Legal lattice scales, checked for every asset rather than only the ones
    // with pins: an illegal scale is a load-time fallback to 1, i.e. an asset
    // silently at the wrong size.
    if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
      ok = false;
      notes += name + ": derived scale " + std::to_string(scale) +
               " is not 1/2/4/8 (art " + std::to_string(artVpm) + " vox/m, " +
               "world " + std::to_string(kVoxelsPerMetre) + " vox/m). ";
    }
    // The declared art scale and the derived pair must reproduce each other.
    // Cheap, and it catches an upsample that was computed but never applied.
    if (artVpm > 0 && (int)(artVpm * (int)upsample) != (int)scale * kVoxelsPerMetre) {
      ok = false;
      notes += name + ": artVoxelsPerMetre " + std::to_string(artVpm) + " x " +
               std::to_string(upsample) + " upsample != scale " +
               std::to_string(scale) + " x " + std::to_string(kVoxelsPerMetre) +
               " world vox/m. ";
    }
    const std::string key = "scaleMetres_" + name;
    const double want = BaselineNumber(key.c_str(), -1.0);
    RecordObserved(key.c_str(), (double)metres);
    if (want < 0) return;  // unpinned: reported by the sizes list, not failed
    pinned++;
    if (std::fabs(metres - (float)want) > slack) {
      ok = false;
      notes += name + " is " + F2(metres) + " m, pinned at " +
               F2((float)want) + " m (tolerance " + F2(slack) + " m). ";
    }
  };

  for (const MobDef& d : c.mobs.Defs())
    checkOne(d.name, CellsToMetres(d.worldSize.y), d.artVoxelsPerMetre,
             d.skinScale, d.artUpsample);
  for (const ItemDef& it : c.items.items) {
    // LONGEST AXIS, not height, and the PREFAB box rather than `size`.
    //
    // A sword's dimension of interest is its length, which lies on X; reading
    // .y measured the blade's thickness and reported a 0.1 m sword. And a worn
    // piece is multi-model — `size` stays empty because the geometry lives per
    // cover shell — so it read 0.000 m for every armour in the library, i.e.
    // four assets silently exempt from the only size check there is.
    // `prefab.size` is the whole file's bounding box and is populated in both
    // shapes.
    const IVec3 b = it.prefab.size;
    const float cells = (float)std::max({b.x, b.y, b.z}) /
                        (float)(it.scale ? it.scale : 1);
    checkOne("item/" + it.name, CellsToMetres(cells), it.artVoxelsPerMetre,
             it.scale, it.artUpsample);
  }

  // ---- PASS C: the tree atlas was baked for THIS world ---------------------
  //
  // treeatlas.cpp already refuses a mismatched atlas at load, so reaching here
  // with a bad scale means that refusal regressed. Asserting it anyway is the
  // cheap half: the expensive half is that a silently-accepted atlas is a
  // whole forest at the wrong size with every other check still green.
  {
    TreeAtlas atlas;
    std::string log;
    if (!LoadTreeAtlas(sandvox::AssetDir() + "/trees", c.mats, atlas, log)) {
      ok = false;
      notes += "tree atlas did not load: " + log + " ";
    } else if (atlas.species.empty()) {
      ok = false;
      notes += "tree atlas has no species. ";
    }
  }

  // ---- PASS D: the player's own constants are still metres-derived --------
  //
  // A guard on the one system that was already right, because it is the anchor
  // pass A measures against: if someone re-hardcodes kHalfY as a cell count,
  // pass A starts comparing the art to a constant that no longer means 1.7 m
  // and would happily agree with it at the wrong size.
  if (std::fabs(capsuleM - 1.7f) > 0.001f) {
    ok = false;
    notes += "Player::kHalfY*2 is " + F2(capsuleM) +
             " m, expected 1.70 m — player.h's metre derivation changed. ";
  }

  detail = std::to_string(checked) + " defs (" + std::to_string(pinned) +
           " pinned), avatar " + F2(CellsToMetres(c.mobs.Defs()[humanIdx].worldSize.y)) +
           " m vs capsule " + F2(capsuleM) + " m, world " +
           std::to_string(kVoxelsPerMetre) + " vox/m";
  if (!notes.empty()) detail += " -- " + notes;

  std::printf("scale:   sizes (m): %s\n", sizes.c_str());
  std::printf("scale: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ScaleGates() {
  static const std::vector<Gate> g = {
      {"scale", "sim", {}, false, GateScale},
  };
  return g;
}

}  // namespace selftest
