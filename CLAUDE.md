# sandvox — working rules

3D falling-sand voxel engine. GPU CA over a 256³ toroidal window into an infinite world, raymarched from sim buffers. C++20 / Vulkan / WGSL (Tint compiles to SPIR-V at load+F5). Vulkan-only since 2026-08-22; Dawn checkout stays for Tint only. **`DESIGN.md` is architecture truth** — read before changing a system, update in the same commit if contradicted.

## Parallel sessions

Multiple Claude sessions work this repo concurrently. Coordinate via `AGENTS_BOARD.md` (gitignored, append-only) using **only** `scripts/board.sh` (never Edit/Write the board — rewrites drop concurrent appends):

```bash
bash scripts/board.sh active                     # who holds what
bash scripts/board.sh claim "<files>" "<what>"   # before editing shared files
bash scripts/board.sh done "<what landed>"       # when you stop
bash scripts/board.sh note "<heads-up>"          # cross-cutting info
```

Claim before editing: `world.h`, `common.wgsl`, `simulation.cpp`, `main.cpp`, `CLAUDE.md`, `DESIGN.md`, `assets/materials/`, `assets/spells/`, `assets/tuner*`. Re-check board before build/commit/selftest. Stale claims (hours old, untouched files) are abandoned — note and proceed. Also `ls --time-style=full-iso` hub files you depend on.

## Three inviolable rules

### 1. Bit-deterministic simulation
Same seed+tick+inputs → same world hash everywhere. Integer-only sim math (no f32 in CA). Stateless counter-based RNG: `hash3(seed,tick,cellIndex)`. No scheduling-dependent outcomes (no atomics-CAS, no subgroup ops). Write reach ≤1 cell. **Gate:** `--selftest` runs sim twice, compares hashes. Pinned hash: `7cfa2420`. If it moves, you broke the sim — stop and report.

### 2. Cost scales with activity, not world size
Every system sleeps when idle. Dispatch over compacted dirty-chunk list via `DispatchWorkgroupsIndirect`, never full world. Chunks clear dirty flags and sleep when settled. Reaction growth must be subcritical (expected offspring/tick < 1). Selftest asserts ≤32 active chunks at rest. Bound every emergent process.

### 3. All mutations flow through MutationQueue
Brush edits, spells, explosions, worldgen — everything CPU→GPU via `sim_mutate.wgsl`/`sim_explode.wgsl`. No direct voxel buffer writes. Exceptions: snapshot restores (worldgen, `LoadWorld`). The queue is also save format, replay log, and future network stream. Keep CPU↔GPU traffic <1 MB/tick, async, one-tick latent. Never add synchronous readback to frame path.

## Build and verify

```bash
bash scripts/build.sh                    # Release build (mutex-protected, 6-core cap)
bash scripts/build.sh --selftest         # build + selftest
bash scripts/build.sh --configure       # force cmake reconfigure
```

**Always use `scripts/build.sh`**, never raw cmake — it holds a machine-global mutex. Kill stale instances: `taskkill //F //IM sandvox.exe`. Set `export SANDVOX_NO_CRASH_DIALOG=1` in every shell. Read `crash.log` after crashes. Verify exe mtime before trusting results.

```bash
./build/Release/sandvox.exe --selftest --gate <name>      # one gate (~4-20s vs ~50s full)
./build/Release/sandvox.exe --selftest --list             # list gates
./build/Release/sandvox.exe --selftest --json out.json    # machine-readable
./build/Release/sandvox.exe --vk-smoke-loud --vk-validation  # 19 pinned hash probes + sync validation
./build/Release/sandvox.exe --frames 400                  # windowed N frames then exit
bash scripts/check_shaders.sh                             # validate WGSL without rebuild
python scripts/check_invariants.py                        # "two places must agree" checks
python scripts/check_pass_table.py                        # pass table vs WGSL bindings
```

`--vk-validation` enables sync validation; **a validation message FAILS the run**. `tests/baseline.json` records known-failing gates (exit 0); new failures are regressions (exit 1). `--backend dawn` refuses with exit 2.

### Build gotchas
- Dawn checkout stays for Tint; `DAWN_ENABLE_*` all OFF. Turning one ON re-declares the whole Dawn engine (~156 TUs).
- `TINT_BUILD_SPV_WRITER`/`SPV_READER` default to `${DAWN_ENABLE_VULKAN}`, forced ON independently. `DAWN_USE_GLFW` stays ON (supplies `glfw` target). `Vulkan::Headers` has its own `add_subdirectory`.
- Shared dep cache at `C:/sv-deps` (`FETCHCONTENT_BASE_DIR`), set BEFORE `include(FetchContent)`. Keep path short (Dawn filenames hit MAX_PATH).
- Jolt: `USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF` or 139 `/MT` vs `/MD` link errors.
- `simSlimBGL_` is a vestige of Dawn's 16-storage-buffer layout limit — can collapse now, but is its own hash-gated change.
- `synchronization2`/`dynamicRendering` must be explicitly ENABLED even in Vulkan 1.3. `target` is reserved in WGSL. Mark+apply two-phase for kernels that read+write neighborhoods.

## Layout (key paths)

| Path | What |
|---|---|
| `src/main.cpp` | frame loop, arg parsing, `--shot`/`--shot-mob` |
| `src/test/` | selftest harness: `selftest.*` (registry, `kOrder`, baseline), `support.*` (shared sim/render plumbing), one `selftest_<domain>.cpp` per domain |
| `src/sim/` | world storage, sim dispatch, material/reaction JSON compilation, `.vox` loader |
| `src/gpu/` | `rhi.h` = only GPU API the engine sees. `rhi_vk.cpp` = Vulkan backend. `vk_record.cpp` generates ALL barriers from `pass_table.def`. `context.*` = device+swapchain |
| `src/game/` | player, camera, brush, mobs, avatar (`avatar.*`), spells (`spell.*`+`caster.*`) |
| `src/audio/` | `cues.*` = public API (game events). Vendored `xyzpan/` spatializer. Mono assets, meters+Z-up internally |
| `assets/shaders/` | `common.wgsl` prepended to all shaders by `LoadShader` |
| `assets/materials/` | `tuning.json` (F5 hot-reload), materials+reactions JSON (R hot-reload) |
| `assets/tuner.html`+`tuner_schema.js` | browser editor for JSONs, Wiki, Audio, Notes tabs |
| `assets/sound_schema.js` | only list of sound slots; must match `Cues::kSlotPrefix` in `audio/cues.cpp` |
| `assets/spells/glyphs.json` | glyph content, materials by name, hot-reloads with R |
| `assets/prefabs/`, `assets/mobs/` | `.vox` art + mob `.json` sidecars |

## Design guidelines (from [Lin 2021](docs/refs/perfect_voxel_engine.md))

1. **Design data format for the hardest consumer** (physics, serialization, replication), not just rendering.
2. **Attributes in side tables**, not widened onto the voxel. 32 bpv is the budget. Extra state → sparse auxiliary layer keyed by chunk. Prefer derived data (e.g. `microvox`: zero storage, zero sim cost).
3. **Multiple representations are fine; unowned diverging ones aren't.** One authoritative source per fact; derived data is reconstructible+disposable. Conversions in named functions.
4. **No closed-ended systems.** Author content by name, resolve at load. Behavior is data (JSON+tags), not runtime-typed voxel fields. One authoring surface per kind.
5. **Rendering is the small part.** A system isn't done because it looks right — check persistence, determinism, idle cost, CPU mirror, mutation path.

## Voxel word layout (32-bit `u32`)

| Bits | Field | Notes |
|---|---|---|
| 0–11 | material id | 4096 slots |
| 12–15 | state nibble | liquids: fullness 1..8; others: palette jitter |
| 16–18 | tick stamp | substep gate, cycles 1..7; `STAMP_NEVER`=0 for all new voxels |
| 19–23 | FREE | scratch only unless hash mask + `kPersistMask` widened |
| 24–27 | stain amount | 0..15 |
| 28–30 | stain type | 0=clean, 1..7 palette slots |
| 31 | `kCellOpIfAir` | transient CPU→GPU flag |

Allocation must agree in: `common.wgsl` (`voxMat`), `world.h`, and this table. Stamp+bits 19–23 excluded from world hash and stripped on save.

## Critical invariants

- **Color lattice is GLOBAL** (world coords, not chunk-local). Chunk dispatch offsets by world chunk coord.
- **World coords for logic, slot-index for memory.** Neighbor access bounds-checks against residency window (`inWindow`), not fixed 0..N.
- **The voxel buffer is a PAGE POOL, not a dense array** (`docs/PLAN_page_table.md`). `pageTable[slot]` is a page index or an `EMPTY`/`UNIFORM(mat)` sentinel; 77.7 MiB resident vs 512 MiB dense. Address voxels ONLY through `voxWordAt`/`voxWordIndex`/`voxStore` (WGSL) or `World::PageOffsetOfSlot` (C++) — any other subscript reads another chunk's memory. An index used as an **identity** (hash key, RNG key, claim slot) is the SLOT index; only a memory address is the PAGE index. The table is derived data: not hashed, not saved. Pool exhaustion is a **fatal abort**. `--residency dense` is the identity map and the only live differential oracle.
- **`TUNE_*` pipeline:** row in `tuning_params.def` → decl in `tuning.h` → `Read*` in `LoadTuning` → default in `tuning.json` → row in `tuner_schema.js`. `sim.*` values are integer-only (rule 1) — EXCEPT the `sim.fluid*` rows, which are human-unit floats (vox/s², vox²/s, seconds) converted to Q16.16-per-tick integers by WGSL const-eval at the top of `sim_fluid.wgsl` (IEEE-exact, so the kernel stays integer and deterministic). No trailing comments on `.def` rows — `gen_tuning_prelude.py`'s parser chokes.
- **Pass table R/W sets must match WGSL bindings** — omitting a read means NO barrier for that hazard. `check_pass_table.py` catches this.
- **World constants generated from `world.h`** via `ShaderConstantPrelude()`, never redeclared in WGSL.
- **CPU mirror is 3×3×3 chunks around player.** `KindAt` returns `Unknown` past ~48 voxels. Projectiles treat Unknown as passable (unlike player). Selftest anchors to `world.WindowOrigin()`.
- **Budgets charged BEFORE emission**, op refused if it doesn't fit. Trails mark per whole voxel, not per sub-step.
- **`RENDER_PATHS`** in `tuner.html` must match shader predicates in `common.wgsl`.
- **Sound slots** defined in `sound_schema.js` AND `Cues::kSlotPrefix` — must agree.

## Conventions

- Materials/reactions are data (JSON+tags), not code. Don't hardcode material IDs in shaders.
- 2-space indent, `snake_case` WGSL, `CamelCase` C++. High comment density in sim shaders (non-obvious invariants).
- Don't grow the 32-bit voxel. Extra state → sparse auxiliary layer.
- Rotations: Y-up, quats `(x,y,z,w)`, Euler X→Y→Z, heading 0=+Z, `.vox` is Z-up. Use `python scripts/geometry.py`.
- Tuner: `./sandvox_tuner.exe` or `python scripts/tuner_server.py`. Build/Play buttons run build+selftest.
