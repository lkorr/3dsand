// selftest.cpp — the gate harness: ordering, baseline diffing, reporting.
//
// The registry itself is assembled here from the per-domain translation units
// so that adding a gate means touching one file plus one line in kGroups.

#include "test/selftest.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "gpu/resources.h"
#include "sim/oprecord.h"   // op-stream clamp counters + SANDVOX_RECORD_OPS
#include "sim/pagetable.h"  // PagesHighWater for the pool-margin report
#include "sim/tuningstamp.h"  // the tuning/materials/reactions desync stamp
#include "test/support.h"

using namespace sandvox;

namespace selftest {

// Each domain file exposes its gates through one of these.
const std::vector<Gate>& TerrainGates();
const std::vector<Gate>& TreeGates();
const std::vector<Gate>& BiomeGates();
const std::vector<Gate>& EnvTruthGates();
// The Node generator data gates (test_mobgen / test_anatomy / test_environment.mjs),
// shelled out to through scripts/generator_parity.mjs. No World, no GPU.
const std::vector<Gate>& GeneratorGates();
const std::vector<Gate>& ScaleGates();
const std::vector<Gate>& SimGates();
const std::vector<Gate>& CaGates();
// The two gas-particle gates (docs/PLAN_gas_particles.md §4). Their own TU
// because they are their own domain — the window edge as a SINK — and because
// both build a 50k-op mutation fixture that has nothing to do with the CA
// gates' chambers.
const std::vector<Gate>& GasGates();
const std::vector<Gate>& WindGates();
const std::vector<Gate>& WaterGates();
const std::vector<Gate>& RenderGates();
const std::vector<Gate>& PlayerGates();
const std::vector<Gate>& MobGates();
const std::vector<Gate>& BodyGates();
const std::vector<Gate>& FloaterGates();
const std::vector<Gate>& AudioGates();
const std::vector<Gate>& WorldIoGates();
const std::vector<Gate>& VoxRegionGates();
const std::vector<Gate>& SpellGates();
const std::vector<Gate>& PlayerKitGates();
const std::vector<Gate>& VesselGates();
const std::vector<Gate>& GrimoireGates();
// The spell GRAPH (PLAN_spell_graph phase 2): layout, the linearizer, the tree
// edit ops. CPU-only over glyphs.json and the generated oracle.
const std::vector<Gate>& SpellGraphGates();
const std::vector<Gate>& SwingGates();
const std::vector<Gate>& EquipmentGates();
const std::vector<Gate>& DyeGates();
const std::vector<Gate>& WoundGates();
const std::vector<Gate>& ImpactGates();
// ONE list for all six combat gates (two feel gates and four NPC ones), even
// though kOrder splits them to opposite ends of the run: the registry is a
// pool of every gate a TU offers and the ORDER is kOrder's business alone.
const std::vector<Gate>& CombatGates();
// The network layer (src/net/*). CPU-only: no world, no GPU, no assets.
const std::vector<Gate>& NetGates();

// THE EXECUTION ORDER, and it is load-bearing.
//
// Gates share one World/Simulation and several depend on state a previous gate
// left behind, so this reproduces the order the single RunSelftest ran them in
// — NOT the order the per-domain files happen to be linked in. Grouping by file
// silently reordered debris/prefab the first time this was written, which is
// exactly the class of bug the ordering note in selftest.h warns about.
//
// Add a gate by putting its name here as well as in its domain file. A gate
// missing from this list is a link-time-visible mistake (it never runs), which
// is the failure mode we want rather than one that runs in an arbitrary slot.
const char* const kOrder[] = {
    // FIRST, and deliberately. `terrain` measures pristine worldgen at the
    // origin and asserts the CPU height mirror against the GPU's voxels — the
    // property every later gate's fixture placement silently assumes. It also
    // has to run before anything moves the window, and it leaves the origin
    // exactly where `determinism` (which does not set it) needs it.
    // FIRST OF ALL, and it costs nothing to put it there: `player-kit` is
    // pure CPU with its own fixtures — no world, no GPU, no assets — so it can
    // neither disturb the pristine worldgen `terrain` needs nor be disturbed
    // by anything. Running it before the expensive gates also means a broken
    // equipment model is reported in the first second of a full run.
    // FIRST OF ALL, for the same reason player-kit is early and then some:
    // `simd` is pure arithmetic — no world, no GPU, no assets, no fixtures —
    // so it can neither disturb pristine worldgen nor be disturbed. It asserts
    // that sim/scan.h and sim/rng_simd.h compute what their scalar definitions
    // compute, which is a precondition for trusting ANY later gate's page
    // table: PageTable::Classify decides sentinel promotion with them.
    "simd",
    // With it, for the same reason: `weak-flame` is pure CPU over the compiled
    // reaction table -- no world, no GPU, no fixtures -- and asserts that the
    // loader's neighborChance expansion produced the rules the author wrote.
    "weak-flame",
    // THIRD, and for the same reasons: `scale` is pure CPU over the loaded
    // defs and the atlas files -- no world, no GPU, no fixtures. It asserts
    // that everything authored is the PHYSICAL size it claims, which every
    // other gate in the suite is blind to (they measure art-lattice counts,
    // which are identical on a world where everything is half-size). A
    // wrong-scale world makes every later gate meaningless, so it belongs
    // with the other cheap front-loaded checks rather than after them.
    "scale",
    "player-kit",
    "vessel",
    // With it: `spells-oracle` is pure CPU over glyphs.json and the generated
    // grammar oracle -- no world, no GPU, nothing left behind -- and a parser
    // that disagrees with the reference script should be the first thing a
    // full run says.
    "spells-oracle",
    // And `grimoire`: CPU-only over its own fixtures, beside `player-kit`
    // for the same reasons (plan §12c).
    "grimoire",
    // And beside it `spell-graph` (PLAN_spell_graph phase 2), for the third
    // time and the same reason: CPU-only over glyphs.json, the generated
    // oracle and its own fixtures, milliseconds, nothing left behind. It
    // belongs next to `spells-oracle` because it asserts the other half of the
    // same contract -- that parser agrees with the reference script, this that
    // every tree the parser builds can be SAID again as words.
    "spell-graph",
    // ...and `spell-magnitude` beside it (PLAN_spell_magnitude 2.6): the
    // same CPU-only footing, over the same glyph table.
    "spell-magnitude",
    // And with them, for the same reason: `swing` is MeleeState alone — no
    // world, no GPU, no assets, its own fixtures — so it costs milliseconds
    // and disturbs nothing. It asserts the swing's INPUT MAPPING, which is the
    // one part of melee no other gate can see (`mob`'s melee subtests drive
    // SetWeaponPose directly and never touch the mouse).
    "swing",
    // ...and its derivative, immediately after it. `swing-smooth` replays
    // every authored HELD style through the same driver and asserts that
    // nothing — hand, point, blade direction, blade roll, arm claim — steps
    // in one tick. Same cost, same independence: it reads
    // attack_styles.json and builds its own MeleeState, and touches no world.
    "swing-smooth",
    // ...and with them `cut-path`: a cut may be a LIST OF LEGS run inside the
    // one Cut phase (strokes.h "A CUT IS A PATH"), and nothing in the shipped
    // library uses one yet, so this is the only gate that would notice a path
    // that parsed and then ran as a straight line. Its styles are built in
    // memory and its loader probe is a temp file — milliseconds, no world.
    "cut-path",
    // AND WITH THEM: `tick-input` is Player alone over a synthetic ground
    // lambda — no world, no GPU, no assets — and it asserts that the
    // controller's trajectory is a function of the COMMAND STREAM and not of
    // the frame schedule (docs/PLAN_multiplayer_now.md N2). Front-loaded for
    // the same reason `swing` is: a controller that has drifted back onto the
    // frame clock makes every later movement gate measure something else.
    "tick-input",
    // ...and immediately after it `view-smooth`, which is the same Player,
    // the same synthetic ground lambda and the same milliseconds, asserting
    // the RENDER half of what tick-input asserts about the sim half: the
    // drawn eye and the drawn body are one continuous motion across a step
    // the controller takes in a single tick.
    "view-smooth",
    // AND WITH THEM, for the fourth time and the same reason: `net-loopback`
    // is src/net alone — two in-memory Links, one loopback TCP pair, a Hello
    // table and a pure-value pacer. No world, no GPU, no assets, nothing left
    // behind, and it runs in milliseconds. It belongs beside `tick-input`
    // because it asserts the other half of the same contract: N2 pinned that
    // the controller is a function of the command stream, this pins that the
    // stream can cross a wire and be refused when the two builds disagree.
    "net-loopback",
    // AND BESIDE IT, `authority` (M9.4-A): pure arithmetic over two peers'
    // positions and window origins, no Ctx, no world, no GPU, milliseconds.
    // It belongs immediately after `net-loopback` because it answers the
    // question the transport raises next — the wire can carry a stream, and
    // this says WHOSE stream it is. Every later mob/debris ownership gate
    // (M9.4-B/C) is measuring behaviour that only makes sense if this passed.
    "authority",
    // AND WITH THEM, for the third time and the same reason: `combat-tuning`
    // and `combat-cues` are pure CPU over tuning.json and the sound library —
    // no world, no GPU, no fixtures, nothing left behind (the one temp file
    // each writes is next to the asset it probes and is removed before the
    // gate returns). Front-loaded rather than appended with the other combat
    // work because a tuning group that stopped reaching its struct makes every
    // later melee gate measure the wrong numbers silently, and it is better
    // reported in the first second than in the fiftieth.
    "combat-tuning", "combat-cues",
    // Same shape (pure CPU, one temp file removed before it returns), for
    // every tuning_params.def row: a dead slider is reported in the first
    // second rather than never.
    "tuning-reach",
    // SECOND, and for the same reason: `tree-atlas` reads assets/trees/*.svtree
    // off disk and asserts on the bytes. No world, no GPU, no state left
    // behind -- and when the atlas is wrong every gate after it is measuring a
    // forest nobody authored, so it belongs before them rather than after.
    "tree-atlas",
    // AND RIGHT AFTER IT, for the same reason: `biomes` reads assets/biomes,
    // assets/water and the placement block of assets/trees off disk and
    // asserts they agree with each other and with the atlas that was just
    // checked. No world, no GPU, nothing left behind.
    "biomes",
    // With them: `generator-parity` runs the Node generator data gates
    // (scripts/generator_parity.mjs). A child process over files on disk -- no
    // World, no GPU, nothing left behind -- and cached on its inputs, so it
    // costs one node start unless a generator or its data changed. SKIPS
    // without node on PATH.
    "generator-parity",
    "terrain",
    // Right after terrain, on the same pristine world: the painted map's
    // biome reaches the kernel (CPU twin at cell centres, GPU skin in-window).
    "worldmap",
    // With it: the map's spawn site is where a player can actually start --
    // outside the harness box, on land, not in a tarn, where trees may grow.
    // CPU only (the height mirror and the map), nothing left behind.
    "spawn-site",
    // Then: an edited biome table reaches the NEXT worldgen without a restart
    // (the F7 / Apply path). Regenerates twice and leaves the pristine world
    // it found, so `waterbody` below sees what `terrain` left.
    "env-reload",
    // Then: the number the Environment tab shows is the number the world
    // has (PLAN_environment_truth P-H). One synthetic one-biome world per
    // biome file, measured against the page's prediction; regenerates per
    // biome and leaves the pristine world through the same reload +
    // regen `env-reload` uses, so `waterbody` still sees what `terrain` left.
    "env-truth",
    // SECOND, and it wants the same thing `terrain` does: pristine worldgen at
    // an unmoved origin. Its whole subject is the ANALYTIC basin registry, and
    // the authored lake at (420,420) has to be resident for that to mean
    // anything — so it runs before `streaming` shifts the window rather than
    // after, and it regenerates on the way out so `determinism` (which
    // regenerates anyway) finds exactly what `terrain` left.
    "waterbody",
    // The current field (M4). Right after `waterbody` because it is the same
    // chain, and because pass S rebuilds the world for each of its three
    // arms - which makes it a poor neighbour for anything that wanted the
    // world left alone. It restores pristine worldgen on the way out.
    "current",
    // IMMEDIATELY BEFORE `determinism`, and the slot is chosen rather than
    // convenient. `ops-replay` runs its own worldgen and 200 ticks, so it
    // leaves the world in a state the gates after it would otherwise inherit —
    // except that `determinism` regenerates and its OWN two runs prove the
    // hash it pins is independent of whatever it inherited (run 2 starts from
    // run 1's leavings and agrees with run 1 bit for bit). So inserting here
    // is invisible downstream, and a break in the op stream is reported in the
    // first seconds of a full run instead of the last.
    "ops-replay",
    // Right after it, and for the same reason it sits here: `chunk-hash` runs
    // its own worldgen twice and ~110 ticks, and it REGENERATES on the way
    // out — so `determinism` below, which regenerates anyway and proves its
    // own hash independent of what it inherited, is the correct neighbour for
    // a gate that churns the world. It never touches the world hash: the
    // digest is a SECOND accumulator with a chunk-local key (M9.3-A).
    "chunk-hash",
    // ...then `chunk-resync`, which is what the digest is FOR: it runs the
    // same 80-tick scene twice (a control and a drift-and-repair arm), replays
    // the second, and regenerates on the way out. Same argument as its
    // neighbours for sitting here; the extra reason is that it is the one gate
    // that calls Stream::ReplaceChunk, and putting it next to the gate that
    // pins the digest keeps the whole M9.3 convergence story in one place in
    // the log (M9.3-C).
    "chunk-resync",
    // ...and `ops-exchange` beside them, for the third time the same argument:
    // it worldgens three times and runs 200 ticks per arm (two seats plus a
    // replay), and `determinism` below regenerates and proves its hash
    // independent of whatever it inherited. It is the op stream's OTHER half:
    // `ops-replay` says the stream describes the tick, this says two machines
    // build the same stream (M9.3-B).
    "ops-exchange",
    // ...and `store-sync` immediately after it (M9.5-B). Mostly CPU — two
    // StoreSync ends over a loopback pair and two ChunkStores, no Stream —
    // but its last two rows SAVE and LOAD a world to prove meta.svm's SVM5
    // tick/seed pair round-trips and that an SVM4 file still loads. So it
    // belongs with the gates that regenerate on the way out rather than with
    // the streaming block: it leaves the ordinary worldgen at the origin
    // behind it, and `determinism` below regenerates anyway and proves its
    // hash independent of whatever it inherited.
    "store-sync",
    "determinism", "sleep",       "ca-skip",
    // Per-material angle of repose. It runs its own worldgen per arm, builds a
    // sealed stone room and pours into it, and it PATCHES ONE MATERIAL'S GPU
    // TABLE ENTRY for each arm — restoring the authored table before it
    // returns, which is why it sits with the other self-contained CA gates and
    // not next to anything that reads a material by hand.
    "repose",      "ca-slope",
    "ca-slope-hybrid", "ca-level-one", "ca-level", "ca-level-pond",
    // Right after the other liquid-shape gates: same fixture neighbourhood,
    // same dim-dawn pinning, and it is the negative of `ca-slope` — the
    // 2-wide geometry the thin-film riser step CANNOT resolve, asserted to go
    // to sleep rather than to drain.
    "ca-gutter",
    // ...and the same rule where it is NOT isolated: a real desert tarn's sand
    // shore, shipped tuning, seam on. Right after ca-gutter because the two are
    // one claim in two halves. It stands the window somewhere else entirely and
    // regenerates at the origin before returning, so it neither needs nor
    // disturbs a neighbour.
    "pond-shore",
    "evaporation", "wind",      "wind-gas",   "wind-prim",
    "blood-stain", "flung-liquid", "fluid-det",     "fluid-identity", "fluid-settle",
    "fluid-excite", "fluid-onwater", "debris-float", "fluid-stain", "fluid-react", "fluid-self-react", "far-fog",  "far-downsample",
    "far-persist",
    // `shadow-cache` recompiles raymarch.wgsl three times (its three arms are
    // const-folded, so they do not exist without a reload) and restores the
    // baseline tuning before returning. It leaves no world state behind, so
    // it sits with the other rendering gates rather than at either end.
    "screenshots", "fire-depth", "shadow-cache", "openness", "gi-bounce", "gi-nightfall", "cave-time", "glow", "plants",
    // `taa` runs its own worldgen and leaves no world state behind — it only
    // draws the same view four ways and compares the images. It sits AFTER
    // `shadow-cache` because that gate reloads the shaders three times and
    // restores the baseline tuning; running before it would put a shader
    // rebuild in the middle of a 16-frame accumulation.
    "taa",
    // `denoise` is the same shape as `taa` (own worldgen, draws one view two
    // ways, leaves nothing behind) and sits beside it for the same reason.
    "denoise",
    // `clouds` is the same shape again (own worldgen, draws, leaves nothing
    // behind — it restores the tuning and the weather pin on the way out).
    "clouds",
    // With the other render gates: `body-shade` runs its own worldgen and is
    // the one gate that draws a RIGIDBODY. It writes the body instance buffer
    // directly (like `fire-depth`) rather than going through the DebrisSystem,
    // so it leaves no bodies behind for `debris` or `settle-back` — which
    // matters, because neither of those resets the debris system and both
    // assert over BodyCount().
    "body-shade",
    // Beside it: the other gate that draws a rigidbody, standing in water. It
    // leaves a filled basin 45 cells from body-shade's site, clear of that
    // gate's roof; nothing after it reads that region before regenerating.
    "underwater-body",
    "player-walk", "player-waterjump", "player-ledgegrab", "player-crouch",
    "player-fastfall",
    "player-plants",
    // TWO PLAYERS IN ONE WORLD (M9.1 P3). Here, with the other player gates,
    // and NOT beside `tick-input` at the front of the list even though they are
    // the same group in PlayerGates(): `tick-input` is pure CPU over a
    // synthetic ground lambda, while this one runs its own worldgen twice, ticks
    // the GPU 600 times, spawns two avatars and a creature, and reads the world
    // hash. It therefore has to be AFTER `determinism` (whose pinned hash must
    // not be measured downstream of a gate that regenerated the world) and it
    // belongs with the gates that cost seconds rather than milliseconds.
    //
    // Self-contained in both directions: it resets debris and mobs and rewinds
    // the mob id counter on the way into EVERY arm, and it regenerates pristine
    // worldgen at kDefaultSeed on the way out — the same discipline `mob-burn`
    // and `undead` use, and what lets it sit between two gates that know
    // nothing about it.
    "two-players",
    // A PEER'S BODY IN THE SAME WORLD (M9.2 package B). Immediately after
    // `two-players` and for the same reasons, plus one of its own: it reuses
    // that gate's window origin, its flat-spot search and its "crowder"
    // targeting fixture, so the two share a shape a reader can check side by
    // side. Self-contained in both directions in the same way — it resets
    // debris and mobs into every arm and regenerates pristine worldgen at
    // kDefaultSeed on the way out.
    "remote-ghost",
    // W2-N: roles -> layers, owner-scoped with two capsules and two avatars.
    // CPU + Jolt only, touches no World state, removes every body it made.
    "layer-roles",
    // W1-F: one player's grenade carves and launches the other (and carves,
    // but never launches, a peer's ghost). CPU + Jolt, resets debris and mobs
    // on both sides; beside the other two-body gates.
    "blast-players",
    "debris",
    // `audio-spatial` touches no World at all (it is the mixer and a Camera),
    // so its slot is free; it sits with the other audio gates.
    "audio-impact", "audio-mob-voice", "audio-ambience", "audio-spatial",
    // "mob" restored to its original slot (it sat between prefab and
    // settle-back until ec764e8 dropped it from both here and MobGates()).
    // The position matters: gates share one World and several depend on what
    // an earlier one left behind, so re-adding it anywhere else would be a
    // different test.
    // `sidecar-resolve` reads assets/mobs/ off disk and merges JSON — no
    // World, no GPU, no fixtures — so it is order-independent and costs
    // milliseconds. Placed immediately before `mob` because a sidecar that no
    // longer resolves takes every mob gate after it down, and the run should
    // say which one it was.
    "prefab",      "sidecar-resolve", "anatomy-parity", "damage-cause", "mob",
    // `debris-coat` beside `settle-back`: the same loose-body burn path, run
    // from debris.Reset() on its own pads, and it leaves no bodies behind.
    "settle-back", "debris-coat", "player-body",
    // Wearing things. After `mob` because it spawns the avatar def on real
    // terrain and carves a shell, which wants the same standing world the
    // body gates run in; before `ragdoll-joints` because it leaves the rig
    // undressed and MobSystem reset, which is what that gate expects to find.
    "armor-wear", "item-ground", "loot", "armor-fit",
    // Pure anim over its own five-part fixture — it touches no shared World and
    // so is order-independent; it sits here to keep the armour gates together.
    "armor-track", "armor-stock",
    // THE COMMONER WARDROBE AND ITS DYE. Here because it dresses the same
    // stock rig `armor-stock` just undressed, and for the same reason: the
    // shipped pieces want a real def on real terrain. Everything else it
    // asserts is pure CPU (the shader constant, the packing, the stacking
    // rule, the save payload) and costs nothing.
    "dye",
    "ragdoll-joints",
    // Beside `ragdoll-joints` and for the same reason: both are pure Jolt over
    // their own fixture, 640 voxels from anything, and both remove every body
    // and patch they make. Neither reads the shared World, so the slot is free
    // — but it has to be AFTER the gates that assert over BodyCount().
    "body-fastfall",
    // Same shape as `body-fastfall`: pure Jolt, its own bodies 900 voxels out,
    // all removed before it returns.
    "big-body-collider",
    // AFTER the debris gates and BEFORE anything that owns bodies of its own.
    //
    // It installs an ownership function on the SHARED DebrisSystem, which
    // turns every body west of its seam into a ghost that emits nothing — so
    // a gate running while that function was installed would measure silence
    // and the failure would be attributed to it. The gate clears the function
    // and Resets() on its way out, and the slot here is the belt to that
    // braces: it comes after the gates that assert over BodyCount() and
    // SettledBack() for the same reason `body-fastfall` above does, and it
    // regenerates the world on the way in so it inherits nothing either.
    "debris-ghost",
    "save-load",   "save-entities", "region-store",
    // SVR3 codec (PLAN_save_system.md S3). Runs its own worldgen + 150 ticks
    // and leaves that world behind; chunk-exchange next regenerates on entry.
    "region-codec",
    // S4 entity split (PLAN_save_system.md). Regenerates on the way in,
    // spawns and drops its own fixtures, and puts back the two process
    // globals it touches (the mob id counter and the celestial clock) on the
    // way out; the world it leaves behind is one chunk-exchange regenerates.
    "save-split",
    // W1-D material names in saves (sim/mattable.h). Regenerates on the way
    // in, saves and loads its own dir under a permuted material table, and
    // resets mobs + debris and regenerates at the origin on the way out --
    // so chunk-exchange, which regenerates on entry anyway, inherits nothing.
    "save-material-remap",
    // BETWEEN region-store and streaming, and the slot is chosen rather than
    // convenient. It regenerates the world several times (four arms, each
    // with its own worldgen and its own ReloadWindow) and it SHIFTS the
    // window in arm A — so it has to sit before a gate that regenerates on
    // the way in and inherits nothing, which `streaming` does in its first
    // three lines. `region-store` ahead of it is pure CPU ChunkStore and
    // leaves nothing at all. It also regenerates at the origin on the way
    // out, so `streaming` starts where it always did (M9.5-A).
    "chunk-exchange",
    // `gen-settle` (PLAN_save_system S2): how many chunks come out MODIFIED
    // with no player input, attributed. It regenerates on the way in (and
    // three more times inside), shifts the window 48 chunks in +X, and
    // regenerates at the origin on the way out -- so it inherits nothing and
    // `streaming`, which regenerates on the way in anyway, is its neighbour.
    "gen-settle",
    "streaming",     "spells",        "spell-timing",
    "page-roundtrip", "daylight-boundary",
    // Support-loss flagging from the MUTATION path. Cheap and
    // self-contained (its own worldgen, an all-stone fixture the CA
    // cannot touch), and it regenerates the world on the way in, so it
    // neither inherits nor leaves anything the gates around it care
    // about.
    "support-flag",
    // Per-voxel body reactivity. Late, and it must be: it lights real fires and
    // pours real acid at absolute coordinates, and it regenerates the world on
    // the way out so the gates after it still find pristine terrain (rule 7).
    "mob-burn",
    // The undead variant. Straight after `mob-burn` because it regenerates
    // worldgen on the way in, which is exactly what a gate following that one
    // wants, and it leaves nothing behind: no ticks, no fire, and it resets
    // mobs and debris on both exits.
    "undead",
    // ...and becoming one at runtime, straight after it: same fixture shape
    // (regenerates worldgen on the way in, resets mobs and debris on both
    // exits) and it wants `undead` to have already proved the authored
    // composition, since its first claim is that the authored one still wins.
    //
    // IT APPENDS DEFS THAT DO NOT GO AWAY. `newcomer+zombie` stays in the def
    // list for the rest of the run — composed defs are cached for the session
    // on purpose — so a later gate that walks Defs() by index sees one extra
    // creature at the end. Nothing before it moves (the append is an append),
    // which is the property that makes this safe to run in-suite at all.
    "zombify",
    // The pack. Beside `zombify` because half its claim IS a rising, and on
    // the same terms: it regenerates worldgen on the way in and resets mobs
    // and debris on every exit. It EDITS `human`'s loot table and restores the
    // pristine def list before it returns — the rule `crowd` states, and it
    // matters more here because a def left carrying a fixture table would put
    // two daggers on every villager every other NPC gate spawns.
    "mob-loot",
    // Mob-vs-mob spacing. Next to `undead` and for the same reasons: it
    // regenerates worldgen on the way in, ticks no fire and pours no acid,
    // and resets mobs and debris on every exit. It also RESTORES the mob defs
    // it edits for its control arm, which matters more than usual here --
    // gates share one MobSystem, so a def left modified would retune every
    // NPC gate after it.
    "crowd",
    // Right after it, for the same reasons and one more: `mob-handoff`
    // regenerates worldgen on the way in, resets mobs and debris on the way
    // out, and — because it SPAWNS — saves and restores the mob id counter,
    // which is the perturbation mob.h's NextIdCounter note is about. It also
    // installs an ownership function and clears it again; a gate after it
    // that found one still installed would be measuring ghosts.
    "mob-handoff",
    // P2c (PLAN_corpse_is_a_mob.md): the same exit contract as `mob-handoff`
    // (mobs and debris reset, id counter restored, tuning restored) and no
    // ownership function installed at all -- machine B is a second MobSystem
    // on its own Physics, destroyed before the gate returns.
    "net-corpse",
    // The same exit contract as `net-corpse` (mobs, debris and id counter
    // reset) plus: the ghost avatar list and BOTH ownership closures it
    // installs through ScanHandoffs are cleared before it returns.
    "net-player-corpse",
    // MOBS v4 (PLAN_save_system S5a). Beside `mob-handoff` because it shares
    // that gate's exit contract: it resets mobs and debris and puts the id
    // counter back. It ticks nothing and needs no terrain.
    "mob-save-delta",
    // S5b (PLAN_save_system). After `mob-save-delta` for the same exit
    // contract (mobs and debris reset, id counter restored) plus its own: it
    // teleports the window (ReloadWindow) away and back, saves and loads a
    // world dir, and so REGENERATES worldgen at the home window on the way
    // out, removes its park function and clears the store.
    "mob-park",
    // MOBS v6 (PLAN_corpse_is_a_mob P2a). After `mob-park` for the same exit
    // contract: mobs, debris and risings reset, id counter restored, park
    // function removed, store cleared, worldgen regenerated at home.
    "corpse-save",
    // Armour reactivity, right after `mob-burn` and for the same reasons: it
    // lights real fires and pours real acid at absolute coordinates, and it
    // regenerates the world on the way out so the gates after it still find
    // pristine terrain (rule 7).
    "armor-react",
    // Fire's DOWNWARD reach, and it sits here for the same reason the two
    // above do: it lights a real fire at absolute coordinates and burns for
    // 900 ticks, and it regenerates the world on the way out so the gates
    // after it still find pristine terrain (rule 7).
    "fire-down",
    // Rain against fire (weather::SimRainWord): lights a leaf sheet at absolute
    // coordinates under three pinned skies, restores the pin and regenerates
    // on the way out — fire-down's reasons, fire-down's slot.
    "rain-fire",
    // Grid coats in reactions (DESIGN.md §6, "A coat is a co-located virtual
    // neighbour"): lights oiled and wet ground at absolute coordinates, pins
    // the weather clear and restores it, regenerates on the way out --
    // rain-fire's reasons, rain-fire's slot.
    "stain-react",
    // The substep stamp alias vs sleep (rule-unification W2-R): acid shafts
    // on anchored steel columns at absolute coordinates, weather pinned clear
    // and restored, regenerates on the way out -- stain-react's reasons.
    "stamp-sleep",
    // ---- THE WINDOW EDGE AS A SINK (docs/PLAN_gas_particles.md §4) --------
    // Straight after `fire-down`, and for exactly the reasons the three gates
    // above it give. Both of these light no fire, but they do the same KIND of
    // damage to the shared world: a 55k-cell air shaft and a 4,096-voxel smoke
    // puff at absolute coordinates, 800 ticks of CA, and a wind field turned up
    // to 20 m/s and put back. Each regenerates worldgen on the way out, so the
    // gates after them still find pristine terrain (CLAUDE.md rule 7), and
    // neither declares a dependency — both build their own world, so neither
    // can be silently SKIPPED behind a known-failing gate.
    //
    // They sit here rather than beside `sleep` because the property they assert
    // is about smoke leaving the CEILING, which is the same subject as the fire
    // gates and the same class of perturbation; `sleep` wants a world nobody
    // has thrown 4,096 gas voxels into.
    "gas-leave", "gas-reenter",
    // ...and the far fire plumes, APPENDED to that group rather than spliced
    // into it (CLAUDE.md rule 7: a new gate in a shared-World suite goes last
    // in its group, so it inherits state instead of changing what everything
    // after it inherits). It paints one chunk of ember, harvests the words,
    // regenerates immediately, and regenerates again on the way out, so the
    // gates after it still find pristine terrain.
    "gas-farplume",
    // ...and its long-range half, appended after it for the group's own
    // stated reason. It builds the same kind of fixture 200 m out and
    // regenerates on the way out, so it leaves the world as it found it.
    "gas-farplume2",
    // The swing's OTHER half. `swing` up top is MeleeState alone and costs
    // milliseconds; this one stands an avatar on real terrain with the blade
    // drawn, spawns a dummy to cut, and measures the sword's world trajectory
    // through the whole IK/clamp/physics pipeline. It belongs down here with
    // the world-touching gates for exactly the reasons they give: it carves
    // real bodies at absolute coordinates, and it regenerates worldgen on the
    // way out so the gates after it still find pristine terrain (rule 7).
    "swing-plane",
    // The discrete strikes on the same fixture: every authored style through
    // the player's runner, plus the head keep-out. Regenerates on the way out
    // like its neighbour, so it slots directly after it.
    "player-styles",
    // NPC AI. After the world-restoring gates above because all of them want
    // pristine terrain to place a fixture on and each regenerates on its way
    // out; before voxregion because ai-approach writes a real stone wall and
    // regenerates too, and stacking two world-restoring gates next to each
    // other keeps the "who left the world like this" question answerable
    // (CLAUDE.md rule 7). APPENDED at the end of this group rather than spliced
    // into the middle of it: a new gate in a shared-World suite goes last in
    // its group, so it inherits state instead of changing what everything
    // after it inherits.
    "ai-dummy", "ai-face", "ai-approach",
    // ...and `ai-reach` after them, by the same append rule: it spawns and
    // resets four times over but writes no terrain, so it inherits the world
    // `ai-approach` restored and hands it on untouched.
    "ai-reach",
    // ...and `ai-pursue` after THAT, by the same append rule again: it also
    // only spawns and resets (a duelist and a quarry, twice) and writes no
    // terrain, so it inherits the world `ai-reach` handed on and hands the same
    // one to `ai-slope`.
    "ai-pursue",
    // ...and the same group's sloped-terrain half, appended last in it for the
    // reason above: `ai-slope` writes a real stone ramp and regenerates on the
    // way out, exactly as `ai-approach` does with its wall.
    "ai-slope",
    // ...and the same ramp trick for a body that has no legs left to walk it
    // with. Beside `ai-slope` because it shares the fixture shape and the same
    // teardown discipline (it writes stone and regenerates on the way out), and
    // because the two measure the two halves of "a creature on a hill": the
    // walker must not sink into it, the crawler must not float over it.
    "crawl-slope",
    // Live ragdoll: a blast knocks a creature flying and it gets back up; a
    // long fall does the same. Appended last in the group for the reason
    // above; it restores the world on its way out.
    "ragdoll",
    // Right after it: same fixture shape, and it resets mobs + debris and
    // regenerates the world on both the way in and the way out, so it is
    // order-independent past that.
    "ragdoll-falldamage",
    // ...and the dressed one, last in the group for the reason the AI gates
    // give. Same self-contained shape again — resets mobs + debris and
    // regenerates worldgen on the way in and the way out — but it also WEARS
    // every item in the library, and a gate that equips things perturbs the
    // id-keyed draws of anything after it, so it goes after the two that do not.
    "ragdoll-dress",
    // ---- THE WOUND MODEL ---------------------------------------------------
    // LAST of the mob gates, and the position is a lesson rather than a
    // preference.
    //
    // These four belong with the world-touching mob gates: they spawn creatures
    // at absolute coordinates inside the residency window, and `wound-bleed`
    // pours real blood into the CA and regenerates the world on the way out,
    // exactly as `mob-burn` does. The obvious slot was therefore right after
    // `mob-burn` — and putting them there FAILED `armor-react`, whose acid bath
    // found the bare arm losing 0 skin voxels in 120 ticks where it had lost 18.
    //
    // Nothing about acid, armour or occlusion had changed. A gate that merely
    // SPAWNS CREATURES perturbs every id-keyed draw after it (MobSystem's id
    // counter seeds the gore profile, the crater noise and the burn/dissolve
    // RNG key), and armour's acid arm is already documented in its own source
    // as swinging by an order of magnitude between scopes for that reason.
    // Restoring the id counter (MobSystem::SetNextIdCounter, which these gates
    // do) closes the largest channel but not every one of them: the suite's
    // shared World, page table and tick stream are others.
    //
    // So the rule a NEW gate should follow, and the reason this comment is
    // long: a gate added to a suite that shares one World has to go where it
    // disturbs the fewest gates that were there first, and that is the END of
    // its group — not the middle of it, however well it reads there.
    "wound-chip", "wound-accumulate", "wound-heft", "wound-bleed",
    // ---- NPCs SWINGING, AND BLADES MEETING BLADES (phase C) ---------------
    // APPENDED at the very end of the mob group, following the rule the block
    // above spells out: a new gate in a shared-World suite goes LAST in its
    // group so it inherits state instead of changing what everything after it
    // inherits. These four spawn armed creatures and let them cut each other
    // up, which is about as large a perturbation as this suite has, so they go
    // after even the wound gates. Each restores the id counter and regenerates
    // worldgen on the way out.
    "npc-strike", "npc-block", "npc-styles",
    // The same replay, asking what the pose went THROUGH rather than where it
    // went (game/selfclip.h). Directly after npc-styles because it builds the
    // identical fixture and restores the world the same way.
    "rig-clip", "duel",
    // ---- BLOOD IS HEALTH; BURNS CAP IT (Gore §F/§G, 2026-09-02) -----------
    // Appended after the combat gates by the same rule again. Each spawns one
    // creature inside the window and restores the id counter. bleed-out is
    // CPU only (severs, lets the stump run, no tick submitted); burn-cap's
    // last phase stands the creature in a real fire and regenerates the world
    // on the way out, like mob-burn.
    "bleed-out", "burn-cap",
    // one committed sword blow across three limbs never kills outright and
    // takes nothing off (owner report 2026-09-02; CPU only, no tick).
    "one-hit",
    // ...and the corpse stays in one piece when the stroke keeps going
    // through it (same report, the half one-hit could not see; CPU only).
    "corpse-intact",
    // ...and the blade that keeps going TAKES SOMETHING: a corpse is carved by
    // the same kerf a living limb is, on the art rather than on the collider
    // derived from it (owner report 2026-09-20). Same fixture, same pristine
    // ground, CPU only.
    "corpse-cut",
    // ...and the blow MOVES what it lands on, in all three drive states —
    // standing (a pose spring), limp and dead (an impulse into Jolt). Same
    // fixture and the same two-creature sweep, CPU only.
    "hit-drive",
    // ...and a corpse COMES APART where you cut it, and the room hears it:
    // the joints Die() leaves are cut by the same rule the living sever by,
    // and dead flesh reports its own gore (owner report 2026-09-20). Same
    // fixture, CPU only.
    "corpse-dismember",
    // ...and a mace MARKS a corpse and leaves it crumbling, instead of boring
    // an instant sphere out of it: the same three-rung ladder the living
    // climb (owner report 2026-09-20). Ticks the debris system only.
    "corpse-blunt",
    // Right after it, and for the same reason it exists: `corpse-armor` is
    // `corpse-intact` with a wardrobe on and the head off first. It needs the
    // same pristine ground and leaves the same nothing behind.
    "corpse-armor",
    // ...and every piece of it bleeds from its own end of the cut, the soak
    // lands on what is exposed, and bone stays bone (owner report 2026-09-02).
    // Ticks the world (the corpse needs ground to lie on) and regenerates it
    // on the way out, like wound-bleed.
    "corpse-bleed",
    // ...and blood is SEEN on a body: a cut bloodies what it exposes (bone
    // included), a burst lands on the creature in its way, a pool rubs off
    // on contact and water rinses it (owner report 2026-09-13). Ticks the
    // world for the last two and regenerates it on the way out.
    "body-stain",
    // ...and the DEAD take the same coat: a corpse in blood is bloodied and in
    // water is washed, by the living's own contact pass (owner report
    // 2026-09-22). Ticks the world and regenerates it on the way out.
    "corpse-wash",
    // ...and the three things a corpse did not inherit from the creature it
    // was: heat crossing its joints, its armour shielding it, and a burst of
    // blood landing on it (owner report 2026-09-22). Each ticks the world and
    // regenerates it on the way out.
    "corpse-crossheat", "corpse-worn", "corpse-splatter", "vessel-grid", "vessel-mpm", "vessel-break",
    // ...and what landed there is a SUBSTANCE, not a colour: the per-limb coat
    // ledger names the material, it dries at that material's own authored rate
    // (and does not at the default one), and a coat can be tracked back onto
    // the ground through the ordinary particle path. Same room fixture as
    // body-stain, same world regeneration on the way out.
    "body-coat",
    "mob-rain",
    "rain-oil",
    // ...and a blast bloodies the HOLE IT MADE and nothing else: a limb the
    // crater took no voxel from stays clean, and the limb it did hit gets a
    // chip's worth of blood rather than a repainted surface (owner report
    // 2026-09-13, the second half of the same one body-stain answers).
    "blast-stain",
    // ...and on a LIVING body that soak dries back to flesh rather than
    // evaporating: blood's authored `decay -> air` (a rule about a pool on the
    // ground) was eating the limb outward from every cut until it fell off.
    // Two arms in one gate, gore.woundHeals on and off, because the claim is a
    // difference and the off arm is both the undead setting and the proof the
    // decay ran at all (owner report 2026-09-14).
    "wound-heal",
    // ---- THE OTHER TWO KINDS OF BLOW (docs/PLAN_impact_unarmed.md) --------
    // A strike is three parts now (game/impact.h): the wound gates above own
    // the CUT, and these four own the BLUNT and the BITE. Appended after them
    // by the rule the long note in this block states — a new gate in a
    // shared-World suite goes LAST in its group, so it inherits state instead
    // of changing what everything after it inherits.
    //
    // They belong beside the wound gates rather than beside the combat ones
    // because they are fabricated blows against a standing fixture, not
    // strokes through the AI: same perturbation, same scale, same "spawn one
    // creature, hit it, reset" shape. Each regenerates worldgen on the way in
    // and resets mobs and debris on every exit, and each restores the id
    // counter (mob ids seed gore variance).
    "impact-blunt", "impact-armor", "impact-fist", "bite-rot", "bite-infect",
    // ...and the structural consequence the rot had none of until 2026-09-19:
    // a limb whose ATTACHMENT has been eaten comes off, whatever ate it.
    "joint-rot",
    // ...and the DISTRIBUTION over limbs, which every gate above it is blind to
    // because they all bite the one biggest severable limb: a bite has to infect
    // a forearm and a hand as well as a thigh (owner report 2026-09-19).
    "bite-limbs",
    // ...and a corpse that died alight keeps burning: every piece advances
    // its embers, keeps emitting fire, and its brick agrees with its lattice
    // (owner report 2026-09-02: the corpse pulsed at its death colour for
    // good). Same world fire as burn-cap, regenerated on the way out.
    "corpse-burn",
    // ...and a laser through a head kills it in place and bores a hole: hp
    // does not decapitate, and dead flesh takes the flesh bore, not the rock
    // melt (owner report 2026-09-22). Pristine ground, CPU only.
    "laser-head",
    // ...and the overlap where a limb meets its parent is ONE cell of flesh in
    // two lattices: rot, a coat or a hole in either copy is in both (owner
    // report 2026-09-23). Pristine ground, CPU only.
    "joint-twins",
    // ...and a CORROSIVE coat (acid) eats the limb it is on, a blood coat does
    // not, acid displaces blood, and the coat is spent. Pose ticks only.
    "acid-coat",
    // ...and on the DEAD: a burst of acid coats a corpse torso, eats it, and
    // is spent. Ticks the world; regenerated on the way out.
    "corpse-acid",
    // ...and a HOT coat (lava) sets the limb alight and eats it, a FUEL coat
    // (oil) is inert until heat reaches it and then flashes. Pose ticks only.
    "lava-oil-coat",
    // ...and a part that comes off keeps its hand: a split forearm drops the
    // hand with the wrist end, a severed arm stays jointed. Pose ticks only.
    "severed-hand",
    // ...and a corpse is a dead Mob that costs nothing once it has settled
    // (asleep: no anchor, no read-back, no garment activation) and wakes on a
    // cut; and the dead are bounded by their own cap, the oldest decaying to
    // debris (docs/PLAN_corpse_is_a_mob.md). The first ticks the world and
    // regenerates it on the way out; the second is one PreTick, CPU only.
    "corpse-sleep", "corpse-cap",
    // The player's corpse is a dead Mob too (P2b): moved into mobs_ at the
    // PreTick after the death, whole respawn, sleeps, a cut reaches it. Ticks
    // the world and regenerates it on the way out, as corpse-sleep does.
    "player-corpse",
    // ---- NOTHING IS LEFT HANGING (2026-09-03) -----------------------------
    // LAST of everything that touches the shared World except `voxregion`, and
    // that position was EARNED rather than chosen. It first sat at the end of
    // the phys group — which is what the rule in the wound block above says to
    // do, and which was still wrong, because "the end of its group" is only the
    // end of the RUN for the group that happens to be last. From the middle of
    // the suite this gate builds a stone pad, drops two bodies on it, stamps
    // matter into the grid and advances the tick stream, and the first
    // full-suite run came back with `wound-accumulate` red beside it: exactly
    // the id-keyed perturbation the wound block warns about, committed by the
    // gate added to obey it.
    //
    // So the rule is really: a new gate goes as late as it can, and "its group"
    // is a tiebreak, not the constraint. This one regenerates worldgen on the
    // way out like its neighbours, and from here there is nothing left for it
    // to disturb.
    "floaters",
    // ...and the same question asked of a TREE, which is where the owner
    // actually sees it: burn one down and lone voxels hang in the air, cut one
    // through the trunk and it keeps standing. `floaters` sweeps its own
    // 3-voxel-block fixture box; this one plants an oak-sized tree (92 tall,
    // which is past kMaxRegionCells) and sweeps what the fire and the axe leave
    // behind. Directly after `floaters` for the same reason `floaters` is here:
    // it burns a fixture, advances the tick stream hard, and regenerates
    // worldgen on the way out.
    "tree-fell",
    // A saguaro-sized fixture that fits every cap tree-fell crosses: if THIS
    // stays standing the handoff chain itself is at fault, not a limit.
    "cactus-fell",
    // ---- FISTS, JAWS AND A LUNGE (docs/PLAN_impact_unarmed.md §8) ---------
    // APPENDED HERE, as late as they can go, by the rule the wound block above
    // spells out and the `floaters` block sharpens: a new gate in a
    // shared-World suite goes where it disturbs the fewest gates that were
    // there first, and "its group" is a tiebreak rather than the constraint.
    // All four spawn creatures (which perturbs every id-keyed draw after
    // them), `lunge` throws bodies through the air, and `player-unarmed`
    // stands an avatar on real terrain — so they go after everything except
    // `voxregion`, which owns the residency window. Each restores the id
    // counter and regenerates worldgen on the way out.
    "unarmed-attack", "lunge", "bite-target", "zombie-draw", "limb-alias",
    "player-unarmed",
    // ...and the directional flinch, appended by the same rule as the four
    // above and for the same reason: it spawns a creature. It is the most
    // easily perturbed of the group — its whole measurement is a differential
    // against the rig's own idle wobble — so it goes where nothing else can
    // leave a body standing in its fixture.
    "hit-react",
    // ...and `levitate` after it, by that same rule: it spawns TWO creatures
    // and lays one of them down, so it is the most disturbing member of the
    // group rather than the most easily disturbed.
    "levitate",
    // ...and a splash at a plated torso (MobSystem::SplatterView's worn-shell
    // test), appended by the same rule: it spawns two creatures and
    // regenerates worldgen on the way out.
    "splatter-armor",
    // ---- THE SNAPSHOT LATENCY IS A CONSTANT (PLAN_multiplayer_now N1) ----
    // As late as it can go, by the rule the `floaters` block above spells out.
    // It regenerates worldgen three times (once per pacing arm and once on the
    // way out) and runs 64 ticks of the selftest op stream, so it disturbs the
    // shared World about as much as `determinism` does -- and from here there
    // is nothing left for it to disturb but `voxregion`, which resets the
    // window and the page table itself.
    "snapshot-latency",
    // LAST of the world-touching gates, and it must be: BuildVoxRegion moves
    // the residency window and resets the page table, which is the state every
    // other gate's fixture placement assumes. It restores both before it
    // returns, but running it early would make any bug in that restore look
    // like a failure somewhere else (CLAUDE.md rule 7).
    "voxregion",
    "perf",
};

const std::vector<Gate>& Registry() {
  static std::vector<Gate> all = [] {
    std::vector<Gate> pool;
    for (const auto* g : {&TerrainGates(), &TreeGates(), &BiomeGates(), &EnvTruthGates(), &GeneratorGates(), &ScaleGates(),
                          &SimGates(), &CaGates(), &GasGates(), &WindGates(), &WaterGates(),
                          &RenderGates(),
                          &PlayerGates(),
                          &MobGates(), &BodyGates(), &FloaterGates(),
                          &WorldIoGates(), &AudioGates(),
                          &VoxRegionGates(),
                          &SpellGates(), &PlayerKitGates(), &VesselGates(), &GrimoireGates(), &SpellGraphGates(),
                          &SwingGates(),
                          &EquipmentGates(), &DyeGates(), &WoundGates(), &ImpactGates(),
                          &CombatGates(),
                          &NetGates()})
      pool.insert(pool.end(), g->begin(), g->end());

    std::vector<Gate> v;
    for (const char* name : kOrder)
      for (const Gate& g : pool)
        if (std::strcmp(name, g.name) == 0) v.push_back(g);
    // Anything defined but not ordered would never run: say so loudly rather
    // than dropping it.
    for (const Gate& g : pool) {
      bool listed = false;
      for (const char* name : kOrder)
        if (std::strcmp(name, g.name) == 0) listed = true;
      if (!listed)
        std::fprintf(stderr,
                     "selftest: gate '%s' is not in kOrder and will not run\n",
                     g.name);
    }
    return v;
  }();
  return all;
}

std::string Format(const char* fmt, ...) {
  va_list a, b;
  va_start(a, fmt);
  va_copy(b, a);
  int n = std::vsnprintf(nullptr, 0, fmt, a);
  va_end(a);
  std::string out;
  if (n > 0) {
    out.resize((size_t)n);
    std::vsnprintf(&out[0], (size_t)n + 1, fmt, b);
  }
  va_end(b);
  return out;
}

namespace {

const Gate* Find(const std::string& name) {
  for (const Gate& g : Registry())
    if (name == g.name) return &g;
  return nullptr;
}

// Expand the requested gates with their transitive dependencies, then emit
// them in REGISTRY order. Registry order is the order the gates were written
// to run in, and several gates depend on world state left by an earlier one
// without saying so — keeping registry order means a subset run reproduces the
// same sequence the full run would, just with the irrelevant gates removed.
std::vector<const Gate*> Plan(const std::vector<std::string>& only) {
  std::unordered_set<std::string> want;
  if (only.empty()) {
    for (const Gate& g : Registry()) want.insert(g.name);
  } else {
    std::vector<std::string> stack = only;
    while (!stack.empty()) {
      std::string n = stack.back();
      stack.pop_back();
      if (!want.insert(n).second) continue;
      const Gate* g = Find(n);
      if (!g) {
        std::fprintf(stderr, "selftest: no such gate '%s' (try --list)\n",
                     n.c_str());
        continue;
      }
      for (const char* d : g->deps) stack.push_back(d);
    }
  }
  std::vector<const Gate*> plan;
  for (const Gate& g : Registry())
    if (want.count(g.name)) plan.push_back(&g);
  return plan;
}

// Every non-pass/fail value from the baseline, keyed by name — the golden world
// hash is just the first inhabitant. File-scope because gates live in other TUs
// and take no options argument; Run() sets it before the first gate runs.
std::unordered_map<std::string, std::string> g_baselineVals;

// Baseline: gate name -> was it failing at the recorded commit. Hand-editable
// JSON, deliberately a flat object so a human can read a diff of it.
//
// Also picks up every value that is NOT "pass"/"fail" — "determinismHash", and
// any threshold a gate pins — into g_baselineVals. Same flat string->string
// shape, so the scanner below needs no new syntax, only a second place to put
// the value. Keys starting with '_' are prose (`_about`, `_smoke_about`) and
// are kept out of the map so nothing can accidentally read one as a threshold.
std::unordered_map<std::string, bool> LoadBaseline(const std::string& path) {
  std::unordered_map<std::string, bool> known;
  g_baselineVals.clear();
  std::ifstream f(path);
  if (!f) return known;
  std::string text((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  // Scan for `"name" : "pass"|"fail"` pairs. No JSON dependency on purpose —
  // this is read before anything else is initialised, and the file is a flat
  // string->string map by design.
  //
  // STRICT about what sits between the key and the value: only whitespace and
  // one colon. A lenient version of this (search forward for the next quoted
  // token) silently paired a prose key with a LATER gate's verdict when the
  // file still carried comment arrays, which is how a "known failure" quietly
  // becomes an unnoticed regression. Keep it strict; keep prose in
  // tests/BASELINE.md instead.
  size_t i = 0;
  while ((i = text.find('"', i)) != std::string::npos) {
    size_t e = text.find('"', i + 1);
    if (e == std::string::npos) break;
    std::string key = text.substr(i + 1, e - i - 1);

    size_t p = e + 1;
    while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
    if (p >= text.size() || text[p] != ':') { i = e + 1; continue; }
    p++;
    while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
    if (p >= text.size() || text[p] != '"') { i = e + 1; continue; }

    size_t ve = text.find('"', p + 1);
    if (ve == std::string::npos) break;
    std::string val = text.substr(p + 1, ve - p - 1);
    if (val == "fail" || val == "pass") known[key] = (val == "fail");
    else if (!key.empty() && key[0] != '_') g_baselineVals[key] = val;
    i = ve + 1;
  }
  return known;
}

const char* StatusWord(Status s) {
  return s == Status::Pass ? "PASS" : s == Status::Fail ? "FAIL" : "SKIP";
}

void WriteJson(const std::string& path, const std::vector<Result>& results) {
  std::ofstream f(path);
  if (!f) {
    std::fprintf(stderr, "selftest: cannot write %s\n", path.c_str());
    return;
  }
  f << "{\n  \"gates\": {\n";
  for (size_t i = 0; i < results.size(); i++) {
    const Result& r = results[i];
    std::string detail;
    for (char ch : r.detail) {  // escape for JSON
      if (ch == '"' || ch == '\\') detail += '\\';
      if (ch == '\n') { detail += "\\n"; continue; }
      detail += ch;
    }
    f << "    \"" << r.name << "\": {\"status\": \""
      << (r.status == Status::Pass ? "pass"
          : r.status == Status::Fail ? "fail" : "skip")
      << "\", \"seconds\": " << (int)(r.seconds * 100) / 100.0
      << ", \"detail\": \"" << detail << "\"";
    // What the gate MEASURED (RecordObserved), always -- not only under
    // --rebaseline, which is the one run that writes them into baseline.json.
    // A gate that reports numbers (terrain, gen-settle) is otherwise only
    // readable back out of its prose detail line.
    if (!r.observed.empty()) {
      f << ", \"observed\": {";
      for (size_t k = 0; k < r.observed.size(); k++)
        f << (k ? ", " : "") << "\"" << r.observed[k].first << "\": \""
          << r.observed[k].second << "\"";
      f << "}";
    }
    f << "}" << (i + 1 < results.size() ? "," : "") << "\n";
  }
  f << "  },\n";
  // ---- THE OP STREAM'S REFUSALS, ALWAYS (docs/PLAN_multiplayer_now.md N3) --
  //
  // An op that does not fit its per-tick cap is dropped at the choke point,
  // and until this line the only evidence was a voxel that never appeared. A
  // count per stream turns "the blood stopped showing up" into a number a
  // later reader can find without re-running anything, which is the whole
  // argument for build/last_run.json.
  {
    const sandvox::opstream::StreamCounts& oc = sandvox::opstream::Counts();
    f << "  \"opstream\": {\"brushTrunc\": " << oc.brushTrunc
      << ", \"expTrunc\": " << oc.expTrunc
      << ", \"cellTrunc\": " << oc.cellTrunc
      << ", \"spawnTrunc\": " << oc.spawnTrunc
      << ", \"fluidTrunc\": " << oc.fluidTrunc
      << ", \"cellDupes\": " << oc.cellDupes
      << ", \"ticksWithDupes\": " << oc.ticksWithDupes
      << ", \"firstDupeTick\": " << oc.firstDupeTick
      << ", \"firstDupeCell\": " << (int64_t)(int32_t)oc.firstDupeCell
      // M9.4-A's single-producer ledger. Zero on every single-player run —
      // the SubmitTick walk only happens when net::Hook() is non-null — so a
      // non-zero here is by itself the statement "two machines were emitting
      // into one chunk", with the first offender named beside it.
      << ", \"authorityViolations\": " << oc.authorityViolations
      << ", \"unknownProducers\": " << oc.unknownProducers
      << ", \"firstViolTick\": " << oc.firstViolTick
      << ", \"firstViolProducer\": " << (int64_t)(int32_t)oc.firstViolProducer
      << ", \"firstViolOwner\": " << (int64_t)(int32_t)oc.firstViolOwner
      << ", \"firstViolChunk\": [" << oc.firstViolChunk[0] << ", "
      << oc.firstViolChunk[1] << ", " << oc.firstViolChunk[2] << "]"
      << "},\n";
  }
  // ---- WHAT THIS RUN'S SIM CONSTANTS WERE (PLAN_multiplayer_now N6) -------
  //
  // tuning.json / materials.json / reactions.json all hot-reload and none is
  // in the save, so a hash that differs between two machines "on the same
  // build" is most cheaply explained by one of these three. Recorded next to
  // the result rather than printed only at boot, so the explanation is still
  // there when somebody reads last_run.json a day later.
  f << "  \"tuningStamp\": " << sandvox::StampTuning(AssetDir()).Json() << ",\n";
  // What the driver charged for each pipeline this run, always
  // (docs/PLAN_shader_compile.md package A item 4). A cold worldgen compile is
  // minutes and the entry point that took them is not otherwise recorded
  // anywhere a later reader can find.
  f << PipelineTimingJson("  ") << "\n}\n";
  std::printf("wrote %s\n", path.c_str());
}

// Replace the quoted value of `"key": "..."` in place, leaving comments, key
// order and prose untouched. Returns false when the key is absent — which is
// SILENT AND DELIBERATE for gate verdicts (the baseline records only gates
// worth pinning), but means a gate's new observed key must be seeded into the
// file by hand once or --rebaseline will appear to work and write nothing.
bool ReplaceJsonValue(std::string& text, const std::string& key,
                      const std::string& val, std::string* oldOut) {
  const std::string keyPat = "\"" + key + "\"";
  size_t p = text.find(keyPat);
  if (p == std::string::npos) return false;
  size_t colon = text.find(':', p + keyPat.size());
  if (colon == std::string::npos) return false;
  size_t q1 = text.find('"', colon + 1);
  if (q1 == std::string::npos) return false;
  size_t q2 = text.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  if (oldOut) *oldOut = text.substr(q1 + 1, q2 - q1 - 1);
  text = text.substr(0, q1 + 1) + val + text.substr(q2);
  return true;
}

void RebaselineSelftest(const std::string& path,
                        const std::vector<Result>& results) {
  // Read the determinism gate's observed hash from its detail string.
  // The determinism gate's detail is "hash <hex> (N ticks, ...)" — the hash
  // is the second token.
  std::string newHash;
  for (const Result& r : results) {
    // Pass OR pinnedOnly. pinnedOnly means the twice-run comparison was
    // IDENTICAL and only the golden pin differs -- which is precisely the run
    // whose hash we are here to record. Requiring Pass made this function
    // unreachable in the only case it exists for.
    if (r.name == "determinism" &&
        (r.status == Status::Pass || r.pinnedOnly)) {
      // Parse "hash XXXXXXXX ..." from the detail
      size_t p = r.detail.find("hash ");
      if (p != std::string::npos) {
        p += 5;
        size_t e = r.detail.find(' ', p);
        if (e == std::string::npos) e = r.detail.size();
        newHash = r.detail.substr(p, e - p);
      }
    }
  }

  // Read baseline, replace values
  std::ifstream fi(path);
  if (!fi) {
    std::fprintf(stderr, "rebaseline: cannot read %s\n", path.c_str());
    return;
  }
  std::string text((std::istreambuf_iterator<char>(fi)),
                   std::istreambuf_iterator<char>());
  fi.close();

  // Update determinismHash if we have a new one
  const std::string oldHash = GoldenDeterminismHash();
  if (!newHash.empty() && newHash != oldHash) {
    std::string was;
    if (ReplaceJsonValue(text, "determinismHash", newHash, &was))
      std::printf("\n*** determinismHash: %s -> %s ***\n", was.c_str(),
                  newHash.c_str());
  }

  // Update gate pass/fail status
  int changed = 0;
  for (const Result& r : results) {
    if (r.status == Status::Skip) continue;
    // A pinnedOnly failure records as PASS, and it has to: the write above just
    // updated the pin that made it fail, so it will pass on the next run.
    // Recording "fail" would enter it in the known-failing set and mask the
    // very regression the gate exists to catch — a rebaseline that quietly
    // disarms `determinism` is worse than one that refuses.
    const char* newVal =
        (r.status == Status::Pass || r.pinnedOnly) ? "pass" : "fail";
    std::string oldVal;
    if (!ReplaceJsonValue(text, r.name, newVal, &oldVal)) continue;
    if (oldVal != newVal) {
      std::printf("  %s: %s -> %s\n", r.name.c_str(), oldVal.c_str(), newVal);
      changed++;
    }
  }

  // Update measured values (Result::observed). These are what make a threshold
  // tunable without a rebuild — but only for keys that already exist in the
  // file, so a gate that adds one must seed it there by hand once. Say so out
  // loud rather than dropping it silently, because a value that never lands is
  // indistinguishable from a value that never moved.
  int vals = 0;
  for (const Result& r : results) {
    for (const auto& kv : r.observed) {
      std::string oldVal;
      if (!ReplaceJsonValue(text, kv.first, kv.second, &oldVal)) {
        std::printf("  %s: NOT IN BASELINE (add \"%s\": \"%s\" by hand)\n",
                    kv.first.c_str(), kv.first.c_str(), kv.second.c_str());
        continue;
      }
      if (oldVal != kv.second) {
        std::printf("  %s: %s -> %s\n", kv.first.c_str(), oldVal.c_str(),
                    kv.second.c_str());
        vals++;
      }
    }
  }
  changed += vals;

  std::ofstream fo(path);
  if (!fo) {
    std::fprintf(stderr, "rebaseline: cannot write %s\n", path.c_str());
    return;
  }
  fo << text;
  std::printf("\n*** REBASELINED %s (%d gate%s changed) ***\n", path.c_str(),
              changed, changed == 1 ? "" : "s");
}

}  // namespace

// Values the CURRENT gate has recorded, drained by Run() when it returns.
std::vector<std::pair<std::string, std::string>> g_observed;
bool g_pinnedOnly = false;

void RecordObserved(const char* key, const std::string& value) {
  g_observed.emplace_back(key, value);
}

// See Result::pinnedOnly. A gate calls this after it has proved its own
// invariant still holds and found that only the RECORDED value differs.
void MarkPinnedOnly() { g_pinnedOnly = true; }

void RecordObserved(const char* key, double value) {
  // Integers as integers: a threshold reading "1364" is diffable in a way that
  // "1364.000000" is not, and every value pinned so far is a count or a voxel.
  char buf[64];
  if (value == (double)(long long)value)
    std::snprintf(buf, sizeof buf, "%lld", (long long)value);
  else
    std::snprintf(buf, sizeof buf, "%.3f", value);
  g_observed.emplace_back(key, buf);
}

const std::string* BaselineValue(const char* key) {
  auto it = g_baselineVals.find(key);
  return it == g_baselineVals.end() ? nullptr : &it->second;
}

double BaselineNumber(const char* key, double fallback) {
  const std::string* v = BaselineValue(key);
  if (!v || v->empty()) return fallback;
  try {
    size_t used = 0;
    const double d = std::stod(*v, &used);
    return used == 0 ? fallback : d;
  } catch (...) {
    return fallback;
  }
}

const std::string& GoldenDeterminismHash() {
  static const std::string kEmpty;
  const std::string* v = BaselineValue("determinismHash");
  return v ? *v : kEmpty;
}

void Ctx::Grab(const char* path) {
  rhi::Buffer shot =
      CreateBuffer(ctx.device, (uint64_t)width * height * 4,
                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                   "screenshot");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  rhi::TexelCopyTexture srcT{};
  srcT.texture = offscreen;
  rhi::TexelCopyBuffer dstB{};
  dstB.buffer = shot;
  dstB.bytesPerRow = width * 4;
  dstB.rowsPerImage = height;
  rhi::Extent3D ext{width, height, 1};
  enc.CopyTextureToBuffer(srcT, dstB, ext);
  ctx.queue.Submit(enc.Finish());
  std::vector<uint8_t> pixels((size_t)width * height * 4);
  bool got = false;
  got = rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(), (size_t)(pixels.size()));
  if (got && WriteBmpFile(path, pixels, width, height))
    std::printf("wrote %s\n", path);
}

int List() {
  std::string group;
  for (const Gate& g : Registry()) {
    if (group != g.group) {
      group = g.group;
      std::printf("\n%s:\n", group.c_str());
    }
    std::printf("  %-24s", g.name);
    if (!g.deps.empty()) {
      std::printf(" needs:");
      for (const char* d : g.deps) std::printf(" %s", d);
    }
    if (g.advisory) std::printf("  [advisory]");
    // Declared, not inferred — see the Gate::needsRender comment. Printing it
    // is what makes "which gates could run before the Vulkan render path
    // exists" a question with an answer in the binary rather than in a commit
    // message.
    if (g.needsRender) std::printf("  [needs-render]");
    std::printf("\n");
  }
  std::printf("\nrun one:  sandvox --selftest --gate <name>\n");
  std::printf("[needs-render] = drives the offscreen target / a draw; the rest\n"
              "are compute + readback only. All 23 run on both backends since\n"
              "phase 4b; the flag remains as documentation.\n");
  return 0;
}

int Run(Ctx& c, const Options& opt) {
  // Make a harness tick behave like a game frame: block for the readback map
  // after a kicked readback so World::Snap() actually becomes valid. See the
  // block comment on SetHarnessSnapshotDrain (test/support.h) for why the
  // harnesses need this and the game does not.
  SetHarnessSnapshotDrain(true);
  // SANDVOX_RECORD_OPS=<file>: record every tick this run submits
  // (docs/PLAN_multiplayer_now.md N3). Here rather than in main.cpp because
  // this is the harness that owns both a material table and a tick loop; the
  // --record-ops flag will call the same function from argv when main.cpp is
  // free to edit. The `ops-replay` gate takes the recorder over for its own
  // scene, so the two do not compose — the gate says so when it does.
  sandvox::opstream::ResetCounts();
  if (!sandvox::opstream::EnvRecordPath().empty()) {
    std::string err;
    if (sandvox::opstream::StartRecording(sandvox::opstream::EnvRecordPath(),
                                          sandvox::kDefaultSeed, c.mats, err))
      std::printf("op record: writing %s\n",
                  sandvox::opstream::EnvRecordPath().c_str());
    else
      std::fprintf(stderr, "op record: %s\n", err.c_str());
  }
  std::vector<const Gate*> plan = Plan(opt.only);
  if (plan.empty()) {
    std::fprintf(stderr, "selftest: nothing to run\n");
    return 2;
  }
  // Resolve the baseline next to the ASSETS dir rather than the CWD: the exe
  // is normally run as ./build/Release/sandvox.exe from the checkout root, but
  // the tuner's Play button and a CI runner both invoke it from elsewhere, and
  // a baseline that silently fails to load reports every known failure as a
  // fresh regression.
  std::string bpath = opt.baselinePath;
  if (bpath.empty()) {
    namespace fs = std::filesystem;
    fs::path assets(AssetDir());
    fs::path guess = assets.parent_path() / "tests" / "baseline.json";
    bpath = fs::exists(guess) ? guess.string() : std::string("tests/baseline.json");
  }
  auto known = LoadBaseline(bpath);

  // UNBUFFERED STDOUT FOR THE REST OF THE RUN, and it is a diagnostic decision
  // rather than a style one. A redirected stdout is FULLY buffered under MSVC
  // (4 KiB, and _IOLBF is treated as _IOFBF on Windows), so a gate that dies
  // hard takes the last four kilobytes of the log with it — the log then ends
  // at the engine's startup banner whatever gate actually crashed, which makes
  // the one question worth asking ("where did it die") unanswerable from the
  // artefact. Measured: a W1 crash inside `waterbody` produced a 14-line log
  // whose last line was "physics, debris, mobs, far-field init", and the full
  // suite's log ended mid-word inside the PREVIOUS gate's output. The cost is
  // a write syscall per printf in a harness that spends its time on the GPU.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("=== selftest === (%zu gate%s, backend vulkan)\n", plan.size(),
              plan.size() == 1 ? "" : "s");

  // The shared offscreen target every render-touching gate draws into.
  // Gate::needsRender remains declared (and printed by --list) as
  // documentation of which gates drive the render path, but nothing skips on
  // it: all 23 gates run.
  c.offscreen = c.ctx.device.CreateTexture({c.width, c.height, 1}, rhi::TextureFormat::RGBA8Unorm, rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc, "offscreen");
  c.view = c.offscreen.CreateView();

  std::vector<Result> results;
  std::unordered_map<std::string, Status> outcome;

  for (const Gate* g : plan) {
    // A gate whose dependency did not pass cannot produce a meaningful
    // verdict. Report SKIP so the run does not blame it for upstream damage.
    const char* blockedBy = nullptr;
    for (const char* d : g->deps) {
      auto it = outcome.find(d);
      if (it != outcome.end() && it->second != Status::Pass) blockedBy = d;
    }
    Result r;
    r.name = g->name;
    if (blockedBy) {
      r.status = Status::Skip;
      r.detail = std::string("depends on ") + blockedBy + ", which did not pass";
      std::printf("%s: SKIP (%s)\n", r.name.c_str(), r.detail.c_str());
    } else {
      // The gate bodies still print their own "name: PASS (numbers)" lines,
      // including the nested sub-gate lines ("mob gait: ...", "body blast:
      // ..."). Those ARE the diagnostic output, so the harness does not
      // reprint a verdict — it only records the status for the baseline diff
      // and the JSON. Timing is printed here since no gate measured itself.
      double t0 = NowSeconds();
      std::string detail;
      g_observed.clear();
      g_pinnedOnly = false;
      r.status = g->fn(c, detail);
      r.seconds = NowSeconds() - t0;
      r.detail = detail;
      r.observed = std::move(g_observed);
      g_observed.clear();
      r.pinnedOnly = g_pinnedOnly;
      g_pinnedOnly = false;
    }
    outcome[r.name] = r.status;
    results.push_back(std::move(r));
  }

  if (sandvox::opstream::Recording()) {
    std::printf("op record: %u frames, %llu bytes\n",
                sandvox::opstream::RecordedFrames(),
                (unsigned long long)sandvox::opstream::RecordedBytes());
    sandvox::opstream::StopRecording();
  }
  if (!opt.jsonPath.empty()) WriteJson(opt.jsonPath, results);
  WriteJson("build/last_run.json", results);

  // Verdict. A gate already failing in the baseline is reported but does not
  // turn the run red — that is the whole point: an agent sees at a glance
  // whether it introduced a failure or inherited one.
  std::vector<std::string> regressions, fixed, inherited, pinnedMoved;
  for (const Result& r : results) {
    const Gate* g = Find(r.name);
    if (g && g->advisory) continue;
    bool wasFailing = known.count(r.name) && known[r.name];
    // A gate that failed ONLY because its pinned value moved is not a
    // regression to a run that was invoked to move it. Outside --rebaseline it
    // still is one: `--selftest` must go red when the world changes under you.
    if (r.status == Status::Fail && r.pinnedOnly && opt.rebaseline) {
      pinnedMoved.push_back(r.name);
      continue;
    }
    if (r.status == Status::Fail && !wasFailing) regressions.push_back(r.name);
    if (r.status == Status::Fail && wasFailing) inherited.push_back(r.name);
    if (r.status == Status::Pass && wasFailing) fixed.push_back(r.name);
  }

  if (!inherited.empty()) {
    std::printf("\nknown-failing at baseline (not yours): ");
    for (size_t i = 0; i < inherited.size(); i++)
      std::printf("%s%s", inherited[i].c_str(),
                  i + 1 < inherited.size() ? ", " : "");
    std::printf("\n");
  }
  if (!fixed.empty()) {
    std::printf("FIXED since baseline: ");
    for (size_t i = 0; i < fixed.size(); i++)
      std::printf("%s%s", fixed[i].c_str(), i + 1 < fixed.size() ? ", " : "");
    std::printf("\n  (update tests/baseline.json to lock these in)\n");
  }
  // Vulkan runs with --vk-validation: print whatever the messenger collected
  // during the whole suite (nothing pops a scope mid-run), and let a hazard
  // turn the run red — a sync-validation message IS a barrier bug (§6.2).
  const size_t vkMsgs = c.ctx.ReportVkValidation("selftest");

  // Page faults, over the WHOLE suite (PLAN_page_table.md §2.4, §4.4). The
  // counter is monotonic and unconditional, so this one read covers every gate
  // that ran — it is what turns "a kernel can never write through a sentinel"
  // from a structural claim into a measurement made on every run. Non-zero
  // means some chunk a kernel wrote was not materialized before its dispatch,
  // which is risk 1 and is always a bug.
  uint32_t pageFaults[kPageFaultWords] = {};
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.pageFaults, 0,
                        pageFaults, kPageFaultBytes, "pageFaults");
  // WHAT WAS LOST AND WHERE, from voxStore's three spare words: [2] the widest
  // word it dropped, [1]/[3] the highest and lowest refusing chunk slot.
  //
  // The count on its own names nothing. Finding the 58 faults this reporting
  // was written for cost a day of turning worldgen features off one at a time —
  // ponds, shores, ruins, caves, the sediment wedge, evaporation, the MPM seam
  // — because "58 voxels went missing somewhere" is compatible with all of
  // them. The word decoded to `stone, stain wet/1` and the span to a single
  // chunk, and that is the whole answer in one line: a pond's water staining
  // the rock behind its bank, into a chunk still held as a JITTER sentinel.
  std::string lost;
  if (pageFaults[0]) {
    const uint32_t m = pageFaults[2] & 0xFFFu;
    const char* nm = m == 0 ? "air"
                     : m < c.mats.size() ? c.mats[m].name.c_str() : "?";
    lost += Format(" | lost %s (id %u, word 0x%08x: state %u stamp %u"
                   " stain %u/%u)", nm, m, pageFaults[2],
                   (pageFaults[2] >> 12) & 0xF, (pageFaults[2] >> 16) & 0x7,
                   (pageFaults[2] >> 24) & 0xF, (pageFaults[2] >> 28) & 0x7);
    // ---- WHO, WHERE, WHEN (world.h's page-fault record) ------------------
    //
    // THE SLOT SPAN THAT USED TO BE PRINTED HERE WAS FICTION on any gate that
    // flies. It decoded pageFaults[1]/[3] — the highest and lowest refusing
    // SLOT — through SlotToWorldChunk AT REPORT TIME, i.e. with whatever window
    // origin the run happened to end on. The window is toroidal and the
    // streaming gate shifts it 227 times, so those coordinates named chunks
    // with no relationship to the fault, and they cost an hour of chasing them
    // (RESEARCH_streaming_hitch.md §6, "one reporter defect found on the way").
    //
    // The record now carries the WORLD CHUNK resolved inside voxStore, under
    // the origin the faulting dispatch actually ran with, plus the tick and the
    // kernel. Those three cannot be reconstructed after the fact, so they are
    // the ones worth the four words.
    static const char* const kKernelName[] = {
        "?", "sim_step", "sim_mutate", "sim_explode", "sim_particle",
        "sim_occupancy", "sim_pick", "sim_fluid_seam", "sim_waterbody",
        "worldgen:main", "worldgen:list", "worldgen:pagefill", "sim_gas"};
    auto kname = [&](uint32_t idPlus1) {
      const uint32_t id = idPlus1 - 1;
      return id < (uint32_t)(sizeof kKernelName / sizeof *kKernelName)
                 ? kKernelName[id]
                 : "?";
    };
    if (pageFaults[4] != 0) {
      lost += Format(" | FIRST %s tick %u chunk (%d,%d,%d) word 0x%08x "
                     "entry 0x%08x local %u",
                     kname(pageFaults[4]), pageFaults[10],
                     (int32_t)pageFaults[5] * 16, (int32_t)pageFaults[6] * 16,
                     (int32_t)pageFaults[7] * 16, pageFaults[8],
                     pageFaults[16], pageFaults[17]);
    }
    if (pageFaults[11] != 0) {
      lost += Format(" | LAST %s tick %u chunk (%d,%d,%d) entry 0x%08x",
                     kname(pageFaults[11]), pageFaults[15],
                     (int32_t)pageFaults[12] * 16, (int32_t)pageFaults[13] * 16,
                     (int32_t)pageFaults[14] * 16, pageFaults[18]);
    }
    // The per-kernel tally is the rule-6 line: it answers "which writer" in one
    // run instead of one writer switched off per run.
    std::string byK;
    for (uint32_t i = 0; i < kPageFaultWords - kPageFaultKernelBase; i++) {
      const uint32_t n = pageFaults[kPageFaultKernelBase + i];
      if (!n) continue;
      byK += Format("%s%s %u", byK.empty() ? "" : ", ", kname(i + 1), n);
    }
    if (!byK.empty()) lost += " | by kernel: " + byK;
  }
  std::printf("page faults over the suite: %u%s%s\n", pageFaults[0],
              pageFaults[0] == 0 ? " (a sentinel write is a lost voxel: 0 is the"
                                   " only acceptable value)"
                                 : "  *** SENTINEL WRITES LOST VOXELS ***",
              lost.c_str());

  // Pool high-water over the WHOLE suite. Under §3.8's fatal-exhaustion policy
  // kPoolPages is safety-critical rather than advisory, and §3.8 requires the
  // margin to be a TRACKED NUMBER rather than an assumption: the suite is the
  // worst case the gates can produce, so its high-water is what kPoolPages must
  // be sized against. Reported unconditionally in paged mode so a change that
  // eats the headroom shows up as a moving number long before it shows up as an
  // abort. Dense is the identity map and has no pool, so it is omitted there.
  if (c.world.residency == World::Residency::Paged && c.world.pages) {
    const uint32_t hw = c.world.pages->PagesHighWater();
    std::printf("page pool high water over the suite: %u of %u (%.1f%%, "
                "%.1f MiB of %.1f MiB reserved)\n",
                hw, kPoolPages, 100.0 * (double)hw / (double)kPoolPages,
                (double)hw * kChunkVol * 4.0 / (1024.0 * 1024.0),
                (double)kPoolPages * kChunkVol * 4.0 / (1024.0 * 1024.0));
  }

  if (!pinnedMoved.empty()) {
    std::printf("\nPINNED VALUES MOVED (%zu): ", pinnedMoved.size());
    for (size_t i = 0; i < pinnedMoved.size(); i++)
      std::printf("%s%s", pinnedMoved[i].c_str(),
                  i + 1 < pinnedMoved.size() ? ", " : "");
    std::printf("\n  each of these verified its own invariant and differs from "
                "the baseline only in a RECORDED value, which is what this run "
                "was invoked to update.\n");
  }

  if (!regressions.empty() || vkMsgs > 0 || pageFaults[0] != 0) {
    if (!regressions.empty()) {
      std::printf("\nREGRESSIONS (%zu): ", regressions.size());
      for (size_t i = 0; i < regressions.size(); i++)
        std::printf("%s%s", regressions[i].c_str(),
                    i + 1 < regressions.size() ? ", " : "");
    }
    if (vkMsgs > 0)
      std::printf("\nvulkan validation reported %zu message%s (see above)",
                  vkMsgs, vkMsgs == 1 ? "" : "s");
    if (pageFaults[0] != 0)
      std::printf("\n%u page fault%s: a sim kernel wrote through a sentinel and"
                  " the voxel was LOST (PLAN_page_table.md risk 1)",
                  pageFaults[0], pageFaults[0] == 1 ? "" : "s");
    if (opt.rebaseline) {
      std::printf("\n*** REFUSING to rebaseline: the run has errors ***\n");
    }
    std::printf("\n=== selftest FAIL ===\n");
    return 1;
  }

  if (opt.rebaseline) {
    RebaselineSelftest(bpath, results);
    std::printf("*** THIS WAS A REBASELINE, NOT A PASS. ***\n");
  }

  std::printf("=== selftest PASS === (%zu known failure%s carried)\n",
              inherited.size(), inherited.size() == 1 ? "" : "s");
  return 0;
}

}  // namespace selftest
