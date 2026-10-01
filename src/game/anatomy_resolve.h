#pragma once
//
// WHAT IS UNDER THE SKIN, RESOLVED AT LOAD.
//
// A mob limb is a dense voxel grid and the engine keeps every enclosed cell, so
// "muscle under the skin and bone under that" has always been an authoring
// operation over the .vox interior: give each voxel a material by how deep it
// sits. assets/editor/anatomy.js is that operation and was the only copy of it,
// which made a body's insides ART — and art does not inherit.
//
// That is the one place the inheritance seam leaked. A character extends the
// human's rig, its clips, its states and its natural weapons, and all of that
// reaches it at load with nothing baked; its INTERIOR was a fact frozen into
// 200 KB of voxels on the day it was born, so changing the human's recipe
// reached nobody. It has cost a real bug once already: a head recipe with no
// scalp layer, fixed by regenerating both generated bodies wholesale (2c4f29b).
//
// So the recipe is resolved HERE, from the RESOLVED sidecar — which means
// `extends` and `effects` reach it, and a `skeletal` effect is
// `{"anatomy": {"layers": [{"material": "bone"}]}}` in a patch rather than a
// second copy of somebody's body.
//
// ---- and the bake stays -----------------------------------------------------
//
// This does not replace `node scripts/anatomize_mob.mjs`. The bake is now a
// PREVIEW and a CACHE: the Models tab peels a body by reading its interior off
// disk, scripts/test_anatomy.mjs pins the human to its recipe by reading the
// same, and both keep working. What changes is which of the two is
// AUTHORITATIVE — the recipe is, and the art is a derived copy of it that the
// engine now re-derives every load.
//
// The consequence to hold on to: this is a NO-OP on every asset in the tree
// today, by construction and by gate (`--gate anatomy-parity`). If it rewrites
// a single voxel of a committed body, either the art is stale or the two
// implementations have drifted, and both are worth being told about.
//
// ---- parity is bit-exact, and demanded --------------------------------------
//
// Multi-source 6-connected BFS is order-independent, so the depth field is the
// same however it is walked. The two places that are NOT automatically the same
// are ported with the JavaScript kept in view:
//
//   * `CellHash` is anatomy.js's `cellHash`, respecting JS's int32 coercion at
//     every `^` and `Math.imul`. A different hash is a different speckle and a
//     body full of rewritten voxels that look correct.
//   * the speckle test computes `fraction * 10000` in DOUBLE, because that is
//     what `< L.speckle.fraction * 10000` does in JS.
//
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/materials.h"
#include "sim/voxload.h"

namespace anatomy {

// What a resolve did, for the log and for the parity gate.
struct Report {
  int rewritten = 0;                        // voxels whose material changed
  std::vector<std::string> unresolved;      // recipe names no material has
  bool ran = false;                         // false = no `anatomy` block
};

// Rewrite `prefab`'s interior materials to `recipe`, in place, clearing the art
// colour of every voxel it touches (art overrides the material colour in
// microbody.wgsl, so a painted interior would show the skin's paint on the
// flesh it exposes).
//
// MUST RUN BEFORE `UpsamplePrefab`: depth is counted in art voxels, and a
// block-replicated 2x body would measure every layer twice as thick.
Report Resolve(Prefab& prefab, const nlohmann::json& recipe,
               const std::vector<MaterialDef>& mats, const std::string& where,
               std::string& log);

// The depth-from-surface field the resolve runs over, exposed so a gate can
// ask what a body's interior SHOULD be without rewriting it. `depth[i]` is
// kDepthEmpty for an empty cell, else the 6-connected step count in from the
// nearest exposed face (0 = a face).
constexpr uint8_t kDepthEmpty = 255;
// `include[i]` false leaves model i out of the union (and out of the own-face
// override): Resolve passes it for keep-only limbs such as hair. Null = all.
std::vector<uint8_t> UnionDepth(const Prefab& prefab,
                                const std::vector<bool>* include = nullptr);
// Which models the recipe treats as BODY: false for a keep-only limb (hair),
// which Resolve leaves as painted and keeps out of the depth union. The one
// rule, for Resolve and for any gate that asks what Resolve will touch.
std::vector<bool> BodyModels(const Prefab& prefab, const nlohmann::json& recipe);

}  // namespace anatomy
