# sandvox — working rules

3D falling-sand voxel engine. GPU-resident cellular automaton over a 256³
toroidal residency window into an infinite streamed world, raymarched from the
sim buffers. C++20 + WebGPU (Dawn) + WGSL.

**`DESIGN.md` is the source of truth for architecture and rationale.** Read the
relevant section before changing a system; if a change contradicts DESIGN.md,
update DESIGN.md in the same commit or don't make the change.

---

## You are probably not alone in this tree

Several Claude sessions routinely work this repo **at the same time**. They have
already, on separate occasions: dropped an authored block out of
`materials.json` while every shader still compiled, swept another session's
half-finished work into an unrelated commit, and left conflict markers inside
JSON via a stash/pop. All of it was silent — green build, normal `git status`.

`AGENTS_BOARD.md` (gitignored, append-only) is the shared scratchpad the
sessions use to see each other. A `SessionStart` hook prints the open claims and
the last 12 hours into your context automatically, so you start already knowing
who is holding what.

**Write to it only through `scripts/board.sh`** — it appends one line, which is
safe under concurrent writes. Editing the board with Edit/Write rewrites the
whole file and silently drops whatever another agent appended a second earlier.

```bash
bash scripts/board.sh active                     # who holds what right now
bash scripts/board.sh claim "<files>" "<what>"   # BEFORE you start editing
bash scripts/board.sh done "<what landed>"       # when you stop, or hand off
bash scripts/board.sh note "<heads-up>"          # no claim, just information
```

What is actually required of you:

- **Claim before editing anything shared** — `world.h`, `common.wgsl`,
  `simulation.cpp`, `main.cpp`, `CLAUDE.md`, `DESIGN.md`, and every file under
  `assets/materials/`, `assets/spells/`, `assets/tuner*`. `claim` warns you if
  someone already holds an overlapping path; treat that warning as a reason to
  re-read the file and reconcile, not as something to ride past.
- **Re-check the board before a build, a commit, or a `--selftest` run.** These
  are the three operations that collide hardest: `LNK1104` means someone's
  `sandvox.exe` is live, and a selftest failure may belong to their tree rather
  than yours (see the attribution rule below).
- **Post a `done` with what actually landed**, not "finished". The next session
  reads that line to know whether the thing it depends on exists yet.
- **`note` anything cross-cutting the moment you decide it** — a constant you
  changed, a JSON block you added, a file you are about to regenerate. The
  `kVoxelMeters` and `stain`-block incidents were both one `note` away from
  being non-events.
- **A stale claim is not a lock.** If a claim is hours old and the file looks
  untouched, it is abandoned; say so with a `note` and proceed. The board is
  information, never a mutex — nothing blocks on it.

This does not replace the freshness checks. Still run
`ls --time-style=full-iso` on a hub file whose content you are depending on, and
still re-`grep` an authored JSON block back out after a parallel-edit warning.
The board tells you who is nearby; the mtime tells you what actually changed.

---

## The three rules that outrank everything

Every one of these is cheap to honor now and near-impossible to retrofit. A change
that violates one is wrong even if it looks correct and runs fast.

### 1. The simulation must be bit-deterministic

Same seed + same tick + same inputs → same world hash, on every machine and every
GPU vendor. This is what keeps lockstep multiplayer and replay debugging viable
(DESIGN.md §2, §4, §10). Concretely, inside any sim shader:

- **Integer-only sim state and math.** No `f32` anywhere a value can influence
  voxel state. Floats are fine for rendering (`raymarch.wgsl`) and for CPU-side
  camera/player, never for the CA. Shader compilers diverge across vendors on FMA
  contraction, fast-math, and transcendentals — integers sidestep it entirely.
- **Stateless counter-based RNG only**: `hash3(seed, tick, cellIndex)` as used in
  `sim_step.wgsl`. Never a stateful stream, never anything seeded by thread or
  dispatch order.
- **No scheduling-dependent outcomes.** No atomics-CAS to claim cells, no subgroup
  or wave ops, no order-dependent reductions, no append-buffer ordering leaking
  into sim state. Conflict resolution is by the color lattice or by a fixed
  priority rule — never first-come-first-served.
- **Write reach must stay ≤ 1 cell.** The whole race-freedom argument depends on
  it. A rule that writes 2 cells away silently breaks the lattice guarantee.

**The gate:** `--selftest` runs the sim twice and compares world hashes. It must
pass before any sim change is considered done. Use `--adapter low` to run on a
different adapter for cross-vendor hash comparison.

Determinism is still only verified on one vendor (RTX 3060 Ti) — DESIGN.md risk
#3 is open. Don't add anything that would make it harder to close.

### 2. Cost must scale with activity, not world size

A settled world costs ~nothing. This is the product, not an optimization
(DESIGN.md §11).

- Every system needs a "costs nothing when idle" state. CA passes, reactions,
  occupancy — all dispatch over the compacted dirty-chunk list via
  `DispatchWorkgroupsIndirect`, never over the full world. Full-world scans happen
  only on hash ticks.
- Anything that changes marks its chunk dirty; a chunk with no movement and no
  active reactions clears its flag and sleeps. A voxel moving across a chunk
  boundary must set the *neighbor's* dirty flag.
- **Reaction-driven growth must be decisively subcritical.** A rule whose expected
  offspring per tick is ≥1 means chunks never sleep and the sim never settles —
  it looks like a perf bug but it's a content bug. See the stem note in
  `reactions.json`.
- The selftest asserts a settled world stays under 32 active chunks (currently
  measures 4). If that number climbs, something stopped sleeping.
- **Bound every emergent process** — flood fills, reaction cascades, particle
  counts. Unbounded emergence kills perf and level design both.

### 3. All world mutations flow through the MutationQueue

Brush edits, spells, explosions, worldgen — everything CPU→GPU goes through the
queue and lands via `sim_mutate.wgsl` (brush + exact-cell ops) or
`sim_explode.wgsl` (explosion ops). No direct writes to voxel buffers from
anywhere else. The two sanctioned exceptions are snapshot restores: worldgen
and `LoadWorld` (worldio.cpp), which replace the whole world rather than
mutating it.

This queue is also the save format, the replay log, and the network replication
stream (DESIGN.md §2, §10). Building every new tool against it from day one is the
entire anti-tech-debt play; a bypass is a future networking bug.

Related: keep CPU↔GPU traffic under ~1 MB/tick, always batched, always async (one
tick latent via `mapAsync`). Never add a synchronous readback to the frame path —
the selftest's blocking hash read is the one sanctioned exception.

---

## Architecting a new system: guidelines

Distilled from John Lin's *The Perfect Voxel Engine* (voxely.net, 2021),
mirrored at **`docs/refs/perfect_voxel_engine.md`** — read it before a big
architectural call. His thesis, in one line: **voxel engines die from their
data format, not their renderer** — developers pick the structure that renders
fastest, build every other system on top of it, and then discover it is hostile
to collision, pathfinding, GI, dynamic objects, and new per-voxel attributes,
by which point changing it means rewriting everything.

These are guidelines for *designing* a feature, not invariants like the three
rules above. Where this engine deliberately departs from the post, that is said
outright below — sandvox is a specific game with a hard 16 bpv budget, not a
general-purpose engine, and some of Lin's advice is wrong when applied here.

### 1. Design the data format before the renderer, and design it for the system that will hate it most

Lin's core observation: *"It's important to answer these questions ahead of
time because most of the systems we build need to incorporate the format into
their designs."* Sparse voxel octrees look brilliant until you ask them for
collision detection, and *"storage and rendering are the only things they are
acceptable (not even great) at."*

Before adding a representation, write down how it answers the awkward
questions, not the easy one:

- How does a **physics query** hit it? A single-cell point sample and a swept
  capsule are different questions.
- How does it **serialize**, and does it round-trip bit-exactly? (The stain
  bits shipped broken because the save format silently truncated hashed state.)
- How does it **replicate** — can it be expressed as MutationQueue ops, or
  does it need a second channel? A representation that cannot be expressed as
  ops is a future networking bug (rule 3).
- What does **worldgen** or an importer have to emit to produce one?
- How does it **LOD out**, and what does it look like at the handoff?
- What happens when it is **half-loaded** at the residency-window edge?

If the answer to one of those is "we'll figure it out later", that is the
system that will force the rewrite.

### 2. Attributes go in a side table keyed by where they exist — never widened onto the base voxel

This is the post's sharpest point, via a thought experiment: given a ray-tracing
API taking `struct vertex_t { vec3 position; vec3 normal; }`, do you (A) add
`vec3 color` to the vertex and update every consumer, or (B) accept opaque data
with an offset/stride and bake *indices* instead of attributes? B is correct,
and *"imagine replacing Vertex with Voxel and suddenly with answer A you've
described Efficient Sparse Voxel Octrees."* The failure mode has a name in the
post — `dumb_voxel_grid_t`, a grid carrying `material_ids`, `albedo`, `normal`,
`vegetation_type`, `vegetation_state`, `redstone_strength` for every cell,
everywhere, forever. His test: *"We don't want to store vegetation growth state
deep underground in some cave where vegetation isn't growing anyway."*

The existing rule "don't grow the 16-bit voxel; extra per-voxel state goes in
an optional sparse auxiliary layer keyed by chunk" (DESIGN.md §3) is exactly
answer B, and it holds. Concretely, when a feature wants new per-voxel state:

- **Ask where it actually exists.** If it is meaningful in <10% of cells, it is
  a sparse layer keyed by chunk, not a field.
- **Ask whether it must be world state at all.** The cheapest attribute is one
  that is *derived*: `microvox` gives grass and flowers per-cell sub-geometry
  with *zero* per-instance storage, zero new world state and zero sim cost,
  because the world cell stays an ordinary 16-bit voxel and only the raymarcher
  substitutes the finer model. Reach for that shape first.
- **The state nibble is not spare space.** Its meaning is already per-material
  (variant / fullness / burn stage). Overloading it for a sixth meaning is
  widening the voxel by stealth.

**Where we depart from Lin:** he wants runtime-typed attributes with a name,
bit width and type enum in a header, so modders can add fields the engine never
heard of. We do not, and should not, put that indirection on the base voxel —
16 bpv with a compile-time-known layout is what buys 100M+ resident voxels and
integer-only deterministic kernels (rule 1). Our extensibility seam is
different and already exists: **behavior is data via materials/reactions/tags,
not via runtime-typed voxel fields.** Adding behavior means JSON and tags, not
a new attribute. Keep the modding surface there.

### 3. Use the format that fits the job, and make the conversion the seam

*"The solution is actually rather obvious: to use whatever voxel format is best
for the job! This means not having one or two, but as many as are necessary."*
The precedent Lin cites is meshes: engines import obj/ply/fbx through a common
interchange, then convert to whatever the GPU wants. Multiple representations
are fine — *unowned, undocumented, silently-diverging* representations are not.

This engine already runs several by design: the 16-bit world grid (sim truth),
RLE region files (storage), far-field cascades (render-only LOD), micro bricks
(render-only detail), `.vox` prefabs (authoring), microbody skins vs. derived
colliders (skin authoritative, collider by majority-fill), and the MutationQueue
op stream (mutation, save, replay, network). That is the pattern working.

So when a system wants a different layout, add the *conversion*, don't fork the
truth:

- **Name one authoritative source per fact.** Every other representation is
  derived, and says so in a header comment the way `farfield.h` does
  ("Derived data only: no readbacks, no persistence, no sim interaction, not
  hashed"). Two things that can both be edited are a desync waiting to happen.
- **Derived data must be reconstructible and disposable.** If it cannot be
  rebuilt from the authoritative source, it is not derived — it is a second
  copy of the truth, and it belongs in the save format and the world hash.
- **Put the conversion in one function with an obvious name.** Lin's point that
  conversion operators are *"black boxes"* is the useful part: voxelizing a
  mesh, importing a map, generating collision data, building a compressor and
  baking an LOD are all the same shape of thing. A new importer or exporter
  should be a new converter against an existing format, never a new format.
- **A conversion is a natural test seam.** Round-trip it in a gate.

### 4. No system should be closed-ended

*"My main goal was to design everything in such a way that no system was
closed-ended, and that developers could always iterate on or expand the engine
without any codebase overhauls."* The Jobs quote he leans on is the test:
*"what happens when you think of a great idea six months from now?"*

Applied here — a new system should be open along the axis it will actually grow:

- **Author content by name, resolve to ids at load.** Materials, glyphs, sound
  sets and mob defs all do this; it is what makes hot-reload possible.
- **Take a callback where you would otherwise take a concrete owner.** The
  spell VM is not player-coupled — health arrives via `CasterHealth`, so a mob
  casts through the identical path. `ApplySpellEffect` is one
  position-parameterized payload, so casting at the muzzle and backfiring into
  your own chest are the same call with different arguments. Prefer that shape.
- **One authoring surface per kind of thing**, listed in exactly one place
  (`sound_schema.js` for sound slots, `tuning_params.def` for `TUNE_*`). A
  second list is the "two places that must agree" bug the invariant checker
  exists to catch.
- **Don't hardcode material IDs in shaders.** Shaders test authored *fields*
  and tags; that is why behavior can be added as data.

**Also note where Lin is honest about the cost.** His own reflection layer
"requires pre-coding the supported type combos" and he calls it *"a little
weak"*. Generality has a price, and this engine pays it selectively:
determinism, the 16-bit voxel and integer-only sim kernels all beat flexibility
where they conflict. Rules 1–3 above win every time. Openness is for the
*authoring* and *composition* layers, not for the inner loop.

### 5. Rendering is the small part — budget for the rest

The post opens on why micro-voxel engines are *"basically synonymous with
vaporware"*: the showcase renders billions of voxels and then goes quiet,
because lighting, serialization, network sync, physics, AI, dynamic objects,
procgen, per-voxel interaction and voxel characters were never designed for.

Practical use here: when a feature looks done because it looks right on screen,
it isn't. Ask what it does about persistence (does it survive save/load and
chunk eviction?), determinism (does it touch hashed state — rule 1), idle cost
(does it let chunks sleep — rule 2), the CPU mirror (does it query the grid
outside the 3×3×3 window), and mutation (does it go through the queue — rule 3).
A visually-correct system that fails those is the vaporware pattern in
miniature.

---

## Build and verify

```bash
bash scripts/build.sh                    # build sandvox (Release)
bash scripts/build.sh --selftest         # build + run selftest
bash scripts/build.sh --configure        # force cmake configure first
```

**Always use `scripts/build.sh`, never raw `cmake --build`.** The script
acquires a machine-global mutex (`C:/sv-build-lock`) so only one MSVC
compilation runs at a time — multiple simultaneous unbounded MSBuild instances
will brick the machine. It also caps parallelism to 6 `cl.exe` jobs (half of
16 cores), leaving headroom for editing, searching, and the OS. Agents keep
working while waiting for the lock; only the compile+link serializes.

The underlying commands, for reference or manual use:
```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # configure (first run fetches Dawn ~15 min)
cmake --build build --config Release --target sandvox    # DO NOT run this directly from agents
./build/Release/sandvox.exe --selftest
```

Third-party sources (Dawn, Jolt, ImGui, …) are fetched into a **shared** cache
outside the checkout — `C:/sv-deps`, set by `FETCHCONTENT_BASE_DIR` in
CMakeLists.txt — so a fresh configure does not re-clone or re-build Dawn. Only
the first configure on the machine pays the ~15 min. Override with
`-DFETCHCONTENT_BASE_DIR=...` or `$SANDVOX_DEPS` for a private cache; delete
`C:/sv-deps` to force a clean refetch.

Two things about that path are load-bearing, both learned by breaking them:

- **It is set before `include(FetchContent)`.** The module defines
  `FETCHCONTENT_BASE_DIR` itself at include time, so a guard placed after the
  include never fires and every build silently gets a private cache — the
  ~15 min this exists to avoid.
- **It is short (`C:/sv-deps`, not under your home dir).** Dawn's tint test
  corpus has ~200-char filenames; a deep path exceeds the Windows 260-char
  `MAX_PATH` and the fetch dies with `Filename too long`. Don't lengthen it.

`--selftest` is the acceptance gate: it checks the twice-run world hash, the sleep
assertion, perf, a walk test, and writes `screenshot.bmp`. Run it after any sim,
shader, or material change and report the actual result — never claim a sim change
works without it.

### Run ONE gate, not all twenty

The suite is a registry of named gates (`src/test/selftest*.cpp`). The full run
is ~50 s; a single gate is usually 4–20 s, so iterate on one and run the whole
thing once before you land:

```bash
./build/Release/sandvox.exe --selftest --list          # names + deps, no GPU needed
./build/Release/sandvox.exe --selftest --gate prefab   # just this one (~4 s)
./build/Release/sandvox.exe --selftest --json out.json # machine-readable results
```

`--gate` pulls in whatever that gate declares as a dependency, and gates run in
the order `kOrder` (selftest.cpp) pins — **which is load-bearing**. Gates share
one `World`/`Simulation` and several rely on state an earlier gate left behind;
the streaming gate in particular leaves the window origin ~20 chunks out. Adding
a gate means a row in its domain file *and* a name in `kOrder`; one that is
missing from `kOrder` prints a warning and never runs.

### A red run tells you whether it is yours

`tests/baseline.json` records which gates already fail. A gate failing there is
reported as *known-failing at baseline (not yours)* and the run still exits 0;
anything else that fails is a **REGRESSION** and exits 1. Fix one, flip it to
`"pass"` in the same commit — the run prints `FIXED since baseline` to remind
you. `tests/BASELINE.md` says why each known failure is there and how to
reproduce it.

This replaces the old ritual of rebuilding clean `HEAD` to prove a failure was
not yours. Reach for that only when the baseline itself is in doubt.

**A running `sandvox.exe` may be killed without asking.** The link step fails with
`LNK1104: cannot open file ...sandvox.exe` whenever an instance still holds the
binary. That instance is essentially always me poking at the build, and there is
no unsaved state to lose — worlds live in `.svx` files written explicitly. So kill
it and rebuild rather than stopping to ask:

```bash
taskkill //F //IM sandvox.exe   # git-bash needs the doubled slashes
```

Validate shaders without a full rebuild (seconds, not minutes):

```bash
bash scripts/check_shaders.sh
```

This concatenates `common.wgsl + <file>` exactly the way `LoadShader` does and runs
each through `tint --validate`. It runs automatically on WGSL edits via the
PostToolUse hook in `.claude/settings.json`.

That same hook (`scripts/post_edit_check.sh`) also runs:

```bash
python scripts/check_invariants.py          # all pairs
python scripts/check_invariants.py <file>   # just the ones that file can break
```

which mechanically enforces the "two places that must agree" pairs listed below
— sound slots, `TUNE_*` constants, `RENDER_PATHS`, world constants. Every one of
those is a *silent* failure otherwise: green build, working tuner, wrong
behaviour at runtime. It is quiet unless something actually disagrees.

Tuning by eye/ear goes through the tuner, which finds `assets/` itself and
whose **Build** / **Play** buttons run the two commands above:

```bash
./sandvox_tuner.exe                   # desktop app (build it once, below)
python scripts/tuner_server.py        # same UI in a browser, for devtools
python scripts/build_tuner_exe.py     # (re)build sandvox_tuner.exe
```

The exe must sit in the project root: it edits this checkout in place, so
`assets/` and `build/` are deliberately NOT bundled into it. Building it needs
`pip install pywebview pyinstaller` once; it is gitignored.

### Build gotchas (learned the hard way)

- Dawn needs `DAWN_FORCE_SYSTEM_COMPONENT_LOAD=ON` or its `vulkan-1.dll` load
  fails with Windows error 87.
- ImGui must be ≥1.92 for the current `imgui_impl_wgpu` backend.
- The compile log is enormous (Dawn). Filter for `src/` and `assets/` paths; don't
  read `build_compile.log` whole.
- Jolt needs `USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF` or the link fails with 139
  `/MT` vs `/MD` runtime mismatches.
- Dawn's pipeline-layout limit counts **layout entries, not shader usage**: max
  16 storage buffers per stage across all bind groups in one layout. That's why
  the particle/explosion pipelines pair a slim group-0 (`simSlimBGL_`, bindings
  0–4) with group 1 instead of reusing the full sim layout.
- `target` is a reserved word in WGSL; a buffer used as `Indirect` must not be
  bound in any bind group of the same pass (stage via a Storage copy).
- A kernel that both reads a neighborhood and writes into it races itself and
  breaks determinism — split into mark (pure read) + apply phases. This bit the
  explosion kernel; the two-phase structure in `sim_explode.wgsl` is the fix.

---

## Layout

| Path | What |
|---|---|
| `src/main.cpp` | frame loop, arg parsing, `--shot`/`--shot-mob` harnesses |
| `src/test/` | the acceptance gate. `support.*` is the sim/render plumbing shared with `main.cpp` (`SubmitTick`, `WriteRenderParams`, the sync readbacks) — ONE definition, so a test can never pass against behaviour the game does not have. `selftest.*` is the harness: gate registry, `kOrder`, baseline diffing, `--gate`/`--list`/`--json`. One `selftest_<domain>.cpp` per domain, which is also what stops two agents colliding in one hub file |
| `src/sim/` | world storage, sim dispatch, JSON material/reaction compilation, `.vox` prefab loader |
| `src/gpu/` | Dawn context, buffer/shader/pipeline helpers |
| `src/game/` | player controller, camera, brush, prefab placer, mob system, player avatar (`avatar.*`) + third-person rig (`thirdperson.*`) |
| `src/audio/` | spatialized sound (DESIGN.md §12b). `cues.*` is the ONLY header the rest of the game includes — it speaks game events (`Footstep`, `Land`, `Impact`, ambience). Below it: `world.*` voice pools + the one place voxels/Y-up become meters/Z-up, `voice.*` one emitter (lock-free handoff + pre-engine occlusion filter), `occlusion.*` voxel-ray muffling, `library.*` folder-per-set asset registry, `device.*` miniaudio. `xyzpan/` is a VENDORED spatializer — read its `VENDORED_FROM.md` before editing, and keep it dependency-free. Assets are MONO (the panner builds the stereo image); the audio thread must never touch a game object or `CurrentTuning()` |
| `assets/sounds/` | mono one-shots, one FOLDER per sound set (`footsteps/leaf/leaf_01.wav` = set `footsteps/leaf`). A material names sets in its `"sounds"` block (the flat `"footstep"` key still works); a mob names them in its `.json` sidecar. `raw/` holds the uncut source takes and `.trash/` holds tuner-deleted ones — both SKIPPED by the loader; `scripts/split_footsteps.py` cuts raw takes into per-step files on the transient |
| `assets/sound_schema.js` | the only list of sound SLOTS (material `footstep`/`impact`/`break`…, mob `hurt`/`death`/`sever`…). The tuner's Audio tab and wiki Audio section are built entirely from it; each slot's `prefix` must match `Cues::kSlotPrefix` in `audio/cues.cpp` or the tuner writes bindings the engine cannot resolve |
| `src/game/spell.*`, `src/game/caster.*` | the magic system (DESIGN.md §8). `spell.*` is the caster VM: glyphs are functions on a typed stack, `CompileSpell` folds them into a `Spell`, and `ApplySpellEffect` is the ONE position-parameterized payload — casting at the muzzle and backfiring into your own chest are the same call with different arguments. Integer 24.8 fixed point throughout (NOT rule 1 — spell state is outside the hashed domain; it is for lockstep MP + replay, so don't "simplify" it to float). Not player-coupled: health arrives via a `CasterHealth` callback so a mob can cast through the same path. `caster.*` is the player's inventory + spoken stack only |
| `assets/spells/glyphs.json` | glyph CONTENT (materials.json precedent, not tuning.json). Materials by NAME, resolved at load; hot-reloads with R alongside materials — it MUST, since a glyph holds a resolved 12-bit id. Every sustained effect declares a finite budget here and is clamped against engine ceilings at load (rule 2) |
| `assets/prefabs/`, `assets/mobs/` | `.vox` art (palette index == material ID; `scripts/gen_palette.py`), mob `.vox`+`.json` pairs (`scripts/gen_test_mob.py` emits the example dummy; `gen_mina.py` emits the PLAYER AVATAR rig, which is a mob def driven by `game/avatar.cpp` rather than by mob AI — `gen_wizard.py` is a second, unused character). The avatar's height is NOT free: it is derived from `Player::kHalfY`/`kEyeOffset` at the current `kVoxelMeters` (17 world voxels, eyes at voxel 15) and asserted in the generator. `kAvatarDefName` in `main.cpp` picks which def the player wears |
| `assets/shaders/*.wgsl` | `common.wgsl` is prepended to every other shader by `LoadShader` (behind a generated `world.h` constant prelude) — shared structs and helpers live there, and it is not a standalone module |
| `assets/materials/*.json` | materials and reactions, hot-reloadable (R in-game); `tuning.json` is look/feel params, hot-reloadable (F5) |
| `assets/tuner.html` + `tuner_schema.js` | browser editor for all three JSONs; the schema file is the only list of tunable params. Also hosts the **Wiki** tab (every fact about one material/tag/tuning group/shader/sound set, assembled live from all four sources), the **Audio** tab (set browser, waveform audition, drag-drop import, slot bindings — needs the server/app, since a `file://` page cannot touch `assets/sounds/`) and the **Notes** tab (markdown pages autosaved to `notes/`, gitignored) |
| `scripts/tuner_app.py` + `tuner_server.py` | the tuner as a desktop app / local server: auto-loads assets, Build + Play buttons |
| `scripts/build_tuner_exe.py` | packages the above into `sandvox_tuner.exe` |

**Invariants that have already cost debugging time — don't rediscover them.**
The four "two places that must agree" entries below (sound slots, `TUNE_*`,
`RENDER_PATHS`, world constants) are now checked mechanically by
`scripts/check_invariants.py`, which the PostToolUse hook runs on every edit.
The prose stays because it explains *why* each pair exists and what breaks —
the checker only tells you that something disagrees.

- **The 3×3×3 color lattice is GLOBAL in WORLD coordinates, not chunk- or
  slot-local.** Chunk-local dispatch must offset by the WORLD chunk coordinate
  (16 ≡ 1 mod 3), and coloring by slot coords instead of world coords races at
  the toroidal wrap (world-adjacent cells whose slots are WORLD_N apart share a
  color). Getting this wrong produces a sim that looks right and is subtly race-y.
- **Kernels think in world coords; memory is slot-indexed.** World cell → slot
  is a bitmask (`cellIndexW`/`chunkIndexW`); every neighbor access must bounds-
  check against the residency window (`inWindow` with the origin uniform), not
  a fixed 0..N box. Unloaded space is solid and inert.
- **Look/feel constants belong in `tuning.json`, not as literals in shaders.**
  `TuningWgslBlock()` (`sim/tuning.cpp`) emits them as `TUNE_*` WGSL consts into
  the same prelude, so F5 re-tunes the renderer with no rebuild. The `TUNE_*`
  set has ONE source: the table in **`src/sim/tuning_params.def`**, which the
  emitter expands and which `scripts/tuning_prelude.py` is *generated* from
  (`python scripts/gen_tuning_prelude.py`; `check_invariants.py` fails if you
  forget). Adding a shader-visible knob means: a row in the `.def`, the
  declaration + doc comment in `sim/tuning.h`, a `Read*` in `LoadTuning` (that
  is where per-parameter clamping lives, so it stays hand-written), a default in
  `assets/materials/tuning.json`, and a row in `assets/tuner_schema.js`. Values
  under `sim.*` are integer-only and change the world hash — rule 1 applies, and
  `--selftest` must be re-run.
- **`matRow`/`ruleRow`/`condPanel` are shared by two tabs — re-render through
  `rerenderRules()`, never `renderReactions()`.** The Wiki embeds the very same
  row editors the Materials/Reactions tabs use, so an edit made in either place
  is the same edit. A *structural* change (kind switch, duplicate, delete,
  reorder) has to rebuild the list it lives in, and that list differs by tab —
  hardcoding `renderReactions()` there throws you back to the Reactions tab
  mid-edit and silently discards the wiki page you were on.
- **The tuner Wiki's render-path table restates shader predicates — keep it in
  step.** Shaders never name a material (that is the point: behavior is data),
  so the Wiki tab derives "which render path does water take, and why" by
  re-evaluating the same authored-field tests the shaders use —
  `isViscousLiquid` (class+opacity+moveEvery), `isTranslucentSolid` (opacity on
  a solid), `MATF_OPAQUE`, `MATF_MICRO`. That table is `RENDER_PATHS` in
  `assets/tuner.html`, and the flag key names in it must match the ones
  `materials.cpp` reads (`opaque`, `wanders`, a `micro` block). Change a
  predicate in `common.wgsl` without changing `RENDER_PATHS` and the wiki will
  confidently explain the wrong thing.
- **A sound slot is defined in two places that must agree.** `assets/sound_schema.js`
  says which slots exist and which namespace each binds into; `Cues::kSlotPrefix`
  in `audio/cues.cpp` does the same concatenation at runtime. The tuner offers
  set lists per slot from the first table and the engine resolves through the
  second, so a slot present in one and not the other means the tuner writes a
  binding that silently resolves to nothing. Adding a slot = a row in each, plus
  a call site that fires it. The wiki's `resolveMaterialSound()` additionally
  restates `FallbackFootstep()` (cues.cpp) so a material page can say what will
  actually play rather than only what was authored — same standing obligation
  `RENDER_PATHS` carries for the shader predicates.
- **The CPU mirror is 3×3×3 chunks around the PLAYER, so a fast projectile
  leaves it inside one tick.** `World::KindAt` returns `Unknown` past ~48
  voxels. The player controller treats Unknown as blocking, which is right for
  a capsule that never leaves its neighbourhood; a spell projectile must treat
  it as PASSABLE or every bolt detonates in the caster's face. Out-of-window is
  still solid (the residency-window rule) — those are two different tests and
  conflating them is the bug. Anything else that flies fast and queries the
  grid on the CPU inherits this; the real fix is a swept `RequestChunkFetch`
  along the path.
- **A selftest gate that fires into the world must anchor to the LIVE window
  origin, not a fixed world position.** Gates run in sequence and the streaming
  gate leaves the origin ~20 chunks out, so a hardcoded `x=140` lands outside
  the window, where space is solid — the spell gate detonated on tick 1 and
  read as a budget failure. `world.WindowOrigin()` is the fix.
- **A budget must be charged BEFORE the op is emitted, and the op refused if it
  does not fit.** The natural ordering (emit → subtract → check `<= 0`) lets
  the last op overrun by nearly its whole volume, and anything that sub-steps
  within a tick overruns once per sub-step. "Bounded eventually" is not what
  rule 2 asks for. Related: a trail marks per whole VOXEL of travel, not per
  sub-step — the sweep is subdivided for anti-tunneling, so a per-sub-step mark
  spends the entire budget in one tick on a handful of overlapping cells.
- **World constants are generated from `world.h`, never redeclared in WGSL.**
  `ShaderConstantPrelude()` (`gpu/resources.cpp`) emits `WORLD_N`, `CHUNK`,
  `VOXEL_METERS`, the toroidal masks, etc. as WGSL text prepended ahead of
  `common.wgsl`. `world.h` is the single source of truth — adding a constant
  means adding it there and to the prelude, not to `common.wgsl`.

## Conventions

- Materials and reactions are data, not code. Adding behavior means editing JSON
  and adding tags — reactions target tags (`flammable`, `organic`) rather than
  enumerating materials. This is the guard against the N×M interaction explosion;
  don't hardcode material IDs in shaders.
- Match surrounding style: 2-space indent, lowercase-underscore WGSL, `CamelCase`
  C++ functions. Comment density in the sim shaders is deliberately high because
  the rules encode non-obvious invariants — keep it.
- **Don't grow the 32-bit voxel.** The word is a `u32` — 512³ voxels × 4 B =
  512 MiB, which is *exactly* the storage limit `gpu/context.cpp` requests.
  Going to 64 bpv means 1 GiB and blows that limit; extra per-voxel state goes
  in an optional sparse auxiliary layer keyed by chunk (DESIGN.md §3). 32 bpv
  is what makes 134M resident voxels affordable. (This bullet said "16-bit"
  until 2026-08-22 — it had been wrong since the stain layer went in, and the
  layout below is the authoritative version.)

  | Bits | Field | Notes |
  |---|---|---|
  | 0..11 | material id | 4096 slots, ~48 used |
  | 12..15 | state nibble | liquids: fullness 1..8. Others: render palette jitter (`paletteColor`, `state % 3`) |
  | 16..18 | tick stamp | "already acted this substep" gate — see below |
  | 19..23 | **FREE** | the only unallocated span |
  | 24..27 | stain amount | 0..15 |
  | 28..30 | stain type | 0 = clean, 1..7 palette slots |
  | 31 | `kCellOpIfAir` | transient CPU→GPU flag, masked off before the store |

  The allocation table lives in three places that must agree: the comment above
  `voxMat` in `common.wgsl`, the mirror in `src/sim/world.h`, and this table.

- **The tick stamp is 3 bits with a reserved sentinel, and `kStampNever` is the
  only value a CPU path may write.** The stamp answers one question — "was this
  word written during THIS substep?" — so it needs enough phases to make the
  current one distinguishable, not enough to count ticks. `stampFor` cycles
  1..7; `STAMP_NEVER`/`kStampNever` is 0 and no `stampFor` output equals it.

  Everything that ENTERS the world unstamped must use the sentinel: worldgen
  `genCell`, brush paints (`sim_mutate`), particle reinsertion, prefab stamps,
  debris rubble, RLE decode after a stream-in or a load. Writing a live code
  there costs that voxel a substep. This was a `0xFF` byte before 2026-08-22,
  and `0xFF` masked into 3 bits is `7` — a *real* code — so any site still
  writing the old literal is a bug, not a no-op.

  Why the sentinel is load-bearing rather than tidy: a sleeping chunk is not
  dispatched, so its voxels keep their last stamp indefinitely and are compared
  against the current one on wake. With a short cycle, a chunk that slept an
  exact multiple of the cycle would wake with *every* voxel falsely reading
  "already acted" and stall for a substep — correlated and visible, unlike the
  1-in-256 single-voxel skip the byte-wide field had. Birth-at-sentinel is what
  bounds that to one voxel, one substep.

  The stamp is per-tick scheduling scratch, not state: excluded from the world
  hash (`sim_occupancy.wgsl` hashes bits 0..15 + the stain field) and stripped
  on save (`kPersistMask` in `stream.cpp` clears bits 16..23). **Bits 19..23
  inherit both properties** — anything stored there is scratch unless the hash
  mask and `kPersistMask` are both widened to cover it, which is a save-format
  and determinism change.
- **Verify rotations with `scripts/geometry.py` — don't reason about quaternions
  in your head.** The engine is Y-up, quats are `(x,y,z,w)`, Euler order is
  X-then-Y-then-Z, heading 0 = +Z, and .vox files are Z-up (converted by
  `vox_to_engine`). Use the script to confirm any rotation does what you intend:
  ```bash
  python scripts/geometry.py describe_quat 0 0.707 0 0.707   # what does this quat do?
  python scripts/geometry.py qy 90                            # 90° about Y
  python scripts/geometry.py rotate_point 0 1 0 45 -- 1 0 0   # where does (1,0,0) end up?
  python scripts/geometry.py vox_to_engine 10 5 20             # scene -> engine coords
  python scripts/geometry.py euler_to_quat 30 45 0             # euler -> quat
  python scripts/geometry.py look_at 0 0 0 -- 5 0 5            # quat to face a target
  ```
