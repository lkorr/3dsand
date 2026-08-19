# sandvox — 3D falling-sand voxel engine (v0)

First-person 3D Noita-like: a GPU-resident cellular automaton over a 256³ voxel
world, raymarched directly from the sim buffers, with a walkable player and a
JSON-driven material system. Architecture and rationale live in `DESIGN.md`
(source of truth).

**Stack:** C++20 + WebGPU (Dawn, Vulkan backend natively; Emscripten/browser
build planned) + WGSL. GLFW, Dear ImGui, nlohmann/json. No other deps.

## Build (Windows, VS2022)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # first run fetches+builds Dawn (~15 min)
cmake --build build --config Release --target sandvox
./build/Release/sandvox.exe
```

Verify with the headless selftest (determinism, perf, walk test, screenshot):

```
./build/Release/sandvox.exe --selftest
```

## Controls

| Input | Action |
|---|---|
| WASD + mouse | move / look |
| V | toggle fly / walk |
| Space / Ctrl | jump-swim-up / down (fly: descend) |
| Shift | sprint |
| LMB / RMB | paint / erase voxels |
| 1–8, or overlay dropdown | select material |
| `[` `]` | brush radius |
| P / N | pause / single-step sim |
| R / F5 | hot-reload materials.json / WGSL shaders |
| F1 / Esc | toggle overlay / release cursor |

## Adding a material

Edit `assets/materials/materials.json`, press **R** in-game. Classes:
`solid | powder | liquid | gas`. Density drives displacement (oil floats on
water at 900 vs 1000; air is 10). Gases take `decayPerMille`. Reactions/tags
land at M3 (see DESIGN.md §6/§13).

## v0 engine facts

- Sim: fixed 30 Hz, integer-only, bit-deterministic (verified by twice-run hash
  in selftest). 27 cell-color passes × 2 gravity substeps per tick, race-free
  without atomics on voxel data.
- All world writes flow through the MutationQueue (brush → `sim_mutate.wgsl`) —
  this is the future save/replay/network format.
- Sleeping: per-chunk dirty flags; a settled world simulates ~nothing.
- CPU sees the world via an async 3×3×3-chunk mirror (`mapAsync`, one tick
  latent) — player collision runs against it.
- Measured on RTX 3060 Ti: sim ≈ 0.9 ms/tick active, render ≈ 2 ms @1080p
  with sun shadows.
