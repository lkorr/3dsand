# sandvox — working rules

3D falling-sand voxel engine. GPU-resident cellular automaton over a 256³
toroidal residency window into an infinite streamed world, raymarched from the
sim buffers. C++20 + WebGPU (Dawn) + WGSL.

**`DESIGN.md` is the source of truth for architecture and rationale.** Read the
relevant section before changing a system; if a change contradicts DESIGN.md,
update DESIGN.md in the same commit or don't make the change.

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

## Build and verify

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # first run fetches+builds Dawn (~15 min)
cmake --build build --config Release --target sandvox
./build/Release/sandvox.exe --selftest                  # determinism, perf, sleep, walk, screenshot
```

`--selftest` is the acceptance gate: it checks the twice-run world hash, the sleep
assertion, perf, a walk test, and writes `screenshot.bmp`. Run it after any sim,
shader, or material change and report the actual result — never claim a sim change
works without it.

Validate shaders without a full rebuild (seconds, not minutes):

```bash
bash scripts/check_shaders.sh
```

This concatenates `common.wgsl + <file>` exactly the way `LoadShader` does and runs
each through `tint --validate`. It runs automatically on WGSL edits via the
PostToolUse hook in `.claude/settings.json`.

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
| `src/main.cpp` | frame loop, arg parsing, selftest harness, BMP writer |
| `src/sim/` | world storage, sim dispatch, JSON material/reaction compilation |
| `src/gpu/` | Dawn context, buffer/shader/pipeline helpers |
| `src/game/` | player controller, camera, brush |
| `assets/shaders/*.wgsl` | `common.wgsl` is prepended to every other shader by `LoadShader` — shared constants and helpers live there, and it is not a standalone module |
| `assets/materials/*.json` | materials and reactions, hot-reloadable (R in-game) |

**Two invariants that have already cost debugging time — don't rediscover them:**

- **The 3×3×3 color lattice is GLOBAL in WORLD coordinates, not chunk- or
  slot-local.** Chunk-local dispatch must offset by the WORLD chunk coordinate
  (16 ≡ 1 mod 3), and coloring by slot coords instead of world coords races at
  the toroidal wrap (world-adjacent cells whose slots are WORLD_N apart share a
  color). Getting this wrong produces a sim that looks right and is subtly race-y.
- **Kernels think in world coords; memory is slot-indexed.** World cell → slot
  is a bitmask (`cellIndexW`/`chunkIndexW`); every neighbor access must bounds-
  check against the residency window (`inWindow` with the origin uniform), not
  a fixed 0..N box. Unloaded space is solid and inert.
- **`WORLD_N` in `common.wgsl` must match `world.h`.** They are two independent
  declarations of the same number.

## Conventions

- Materials and reactions are data, not code. Adding behavior means editing JSON
  and adding tags — reactions target tags (`flammable`, `organic`) rather than
  enumerating materials. This is the guard against the N×M interaction explosion;
  don't hardcode material IDs in shaders.
- Match surrounding style: 2-space indent, lowercase-underscore WGSL, `CamelCase`
  C++ functions. Comment density in the sim shaders is deliberately high because
  the rules encode non-obvious invariants — keep it.
- Don't grow the 16-bit voxel. Extra per-voxel state goes in an optional sparse
  auxiliary layer keyed by chunk (DESIGN.md §3). 16 bpv is what makes 100M+
  resident voxels affordable.
