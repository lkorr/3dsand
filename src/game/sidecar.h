#pragma once
//
// THE SIDECAR RESOLVER — how a creature inherits another creature.
//
// A mob sidecar is not a whole creature any more. It says which creature it
// STARTS from (`extends`), which modifiers are applied to it (`effects`), and
// then only what is different about this body: its genome, its name, its
// twelve anchors, the two clip periods its own leg implies. Everything else —
// the limb names and tags, the IK chains, the loco-state ladder, the natural
// weapons, the sockets, the anatomy recipe — belongs to the base and reaches
// this body at LOAD, with nothing baked and nothing to go stale.
//
// That is the whole point. Every generated character used to be a frozen 63 KB
// copy of the human made on its birthday, and four shipped commits that changed
// how a human fights and falls apart reached none of them (2c4f29b). A copy can
// be audited for drift; it cannot be prevented from drifting. An inheritance
// can.
//
// ---- the three rules ------------------------------------------------------
//
// 1. RFC 7396 merge-patch, as nlohmann's `merge_patch` applies it: objects
//    merge key by key, a scalar replaces, an explicit `null` deletes.
//
// 2. NAMED-ARRAY MERGE. An array whose elements ALL carry a string `name`
//    merges BY NAME instead of replacing: a child element patches the base
//    element of the same name, BASE ORDER IS PRESERVED, and an unmatched name
//    appends. This is what lets a character override one limb —
//    `"limbs": [{"name": "legL.L", "hp": 50}]` — instead of restating fifteen.
//
//    Order preservation is not tidiness. TopoSortLimbs' output indexes the
//    save file's positional limb records (mob.cpp, the MOBS chunk), so a merge
//    that reordered limbs would silently misplace every saved mob's damage.
//
//    An array with even one unnamed element replaces wholesale, so `mask`,
//    `groups`, `parts` and every coordinate triple keep the RFC rule.
//
// 3. CLIP RETIME. A clip patch carrying `durationMs` and NO `tracks` scales
//    every inherited key time by new/old. Without it, the two-line override a
//    thin character wants — `{"clips": {"walk": {"durationMs": 638}}}` —
//    changes the clock and leaves the keys where the base put them, which is a
//    walk cycle that ends in the middle of its own stride.
//
// ---- effects --------------------------------------------------------------
//
// An effect is `assets/mobs/effects/<name>.json`, applied AFTER the `extends`
// chain resolves and so to whatever body it lands on:
//
//   { "patch": { ...merged by the three rules above... },
//     "scale": { "speed": 0.746, "gait.cadence": 0.8 } }
//
// `scale` multiplies a numeric leaf by dotted path, and it exists because a
// modifier's numbers are RATIOS, not absolutes. The zombie's old `speed: 23.5`
// and `cadence: 6.4` were the human's numbers three-quarters and four-fifths of
// the way down; poured onto a shorter character they would have set the wrong
// stride (stride = speed / cadence). An effect has to be able to say "three
// quarters of whatever THIS body walks at".
//
// `effects` ACCUMULATES down an `extends` chain rather than replacing, because
// a creature that extends a zombie is a zombie. `"effects": null` clears the
// inherited list, which is the RFC 7396 escape hatch and the only way off.
//
// An effect may not rename or remove a part: the rig vocabulary is the contract
// every other system binds to by name.
//
// ---- and the JS mirror ----------------------------------------------------
//
// assets/editor/sidecar.js is this file, rule for rule. The Models tab, the
// Characters page, gen_mobs.mjs and test_mobgen.mjs all resolve through it, and
// `--gate sidecar-resolve` dumps what C++ resolved so test_mobgen.mjs section N
// can diff the two languages against each other.
//
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sidecar {

using json = nlohmann::json;

// How deep an `extends` chain may go. Depth-bounded rather than
// cycle-detected: a rig eight deep is already a content mistake and a cycle is
// the same mistake with a hang attached, so one check answers both.
constexpr int kMaxExtends = 8;

// Merge `patch` into `target` in place, by the three rules above. `path` is the
// JSON-pointer-ish location of `target` in the document being built ("" at the
// root); it is what tells the merge it is looking at a clip.
void MergePatch(json& target, const json& patch, const std::string& path = "");

// Rewrite `clip`'s key times for a new duration. Public because the retime is
// a content rule worth testing on its own.
void RetimeClip(json& clip, int oldMs, int newMs);

// Apply one effect document (`patch` + `scale`) to a resolved def.
void ApplyEffect(json& def, const json& effect, const std::string& where,
                 std::string& log);

// Read + parse one sidecar. False (with a line in `log`) if it is missing,
// unparseable, or not an object.
bool ReadJsonDoc(const std::string& path, json& out, std::string& log);

// THE ONE ENTRY POINT. Resolve `<dir>/<path>`'s `extends` chain, then apply its
// accumulated `effects` from `<dir>/effects/<name>.json`, into `out`.
//
// `baseOut`, when given, receives the file's OWN `extends` (empty at the root
// of a chain). The resolver erases the key as it goes — a resolved document
// must not invite a second reader to resolve it again — so this is the only
// place that can still answer "whose body did this start from", and
// MobSystem::DefWithEffects needs the answer to recognise a composition it has
// already built.
bool Load(const std::string& dir, const std::string& path, json& out,
          std::string& log, std::string* baseOut = nullptr);

// THE SAME THING, WITH MODIFIERS THE FILE DOES NOT CARRY. `extra` is appended
// to whatever `effects` the chain accumulated (deduplicated, file's own first)
// before any of them are applied, which is what makes "this creature, plus the
// zombie effect" expressible without a file saying so. An effect already on
// the body is not applied twice — biting a zombie does not make it more dead.
bool LoadWithEffects(const std::string& dir, const std::string& path,
                     const std::vector<std::string>& extra, json& out,
                     std::string& log, std::string* baseOut = nullptr);

}  // namespace sidecar
