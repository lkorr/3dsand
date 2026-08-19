# sandvox — 3D falling-sand voxel engine (v0.3)

First-person 3D Noita-like: a GPU-resident cellular automaton over a 256³ voxel
world, raymarched directly from the sim buffers, with a walkable player, a
JSON-driven material/reaction system, GPU particles, explosions, and
Jolt-powered debris rigidbodies cut from the terrain by island detection.
Architecture and rationale live in `DESIGN.md` (source of truth).

**Stack:** C++20 + WebGPU (Dawn, Vulkan backend natively; Emscripten/browser
build planned) + WGSL. GLFW, Dear ImGui, nlohmann/json, Jolt Physics.

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
| G | throw grenade (bounces, 2.2 s fuse) |
| X | detonate at crosshair |
| 1–8, or overlay dropdown | select material |
| `[` `]` | brush radius |
| P / N | pause / single-step sim |
| R / F5 | hot-reload materials.json / WGSL shaders |
| F9 / F10 | save / load world (`world.svx`, RLE) |
| F1 / Esc | toggle overlay / release cursor |

## Adding a material

Edit `assets/materials/materials.json`, press **R** in-game. Classes:
`solid | powder | liquid | gas`. Density drives displacement (oil floats on
water at 900 vs 1000; air is 10). Gases take `decayPerMille`. Reactions/tags
land at M3 (see DESIGN.md §6/§13).

## Engine facts (v0.3)

- Sim: fixed 30 Hz, integer-only, bit-deterministic (verified by twice-run hash
  in selftest — including explosions and particles). 27 cell-color passes × 2
  gravity substeps per tick, race-free without atomics on voxel data.
- All world writes flow through the MutationQueue (brush + explosion + exact-
  cell ops → `sim_mutate.wgsl`/`sim_explode.wgsl`) — also the save/replay/
  network format.
- Particles: fixed-point GPU voxels-in-flight; reinsertion resolved by
  state-keyed claim priority, so buffer order never affects the grid.
- Explosions: per-voxel occlusion rays vs material `hardness`, two-phase
  (mark/apply) for determinism; destroyed voxels partially eject as particles.
- Debris: destruction events → async region readback → bounded island
  detection → Jolt rigidbodies with voxel payload (rendered voxel-crisp),
  colliding against cached marching-cubes chunk meshes; <8-voxel islands
  crumble to their `rubble` material.
- Sleeping everywhere: settled world simulates ~nothing, settled bodies sleep
  in Jolt, dead particle pages cost nothing.
- CPU sees the world via an async 3×3×3-chunk mirror + on-demand chunk fetch
  cache (`mapAsync`, one tick latent, ≤1 MB/tick).
- Measured on RTX 3060 Ti: sim ≈ 1.0 ms/tick active (explosions + particles),
  render ≈ 4 ms @1080p with sun shadows.
- Fast shader check without a rebuild: `bash scripts/check_shaders.sh`.
