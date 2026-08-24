# sandvox

3D falling-sand voxel engine: a GPU cellular automaton over a 512^3 toroidal
window into an infinite world, raymarched directly from the sim buffers.
C++20 / Vulkan / WGSL (Tint compiles WGSL to SPIR-V at load and on F5).
GLFW, Dear ImGui, nlohmann/json, Jolt Physics.

Architecture and rationale: **`DESIGN.md`** (source of truth).
Working rules for contributors: **`CLAUDE.md`**.

## Build (Windows, VS2022)

```bash
bash scripts/build.sh                   # Release build (mutex-protected)
bash scripts/build.sh --selftest        # build + run selftest suite
bash scripts/build.sh --configure       # force cmake reconfigure
```

Always use `scripts/build.sh` -- it holds a machine-global mutex that
serializes builds and runs across concurrent sessions. Never call cmake
directly.

To run the engine outside of `build.sh --selftest`:

```bash
bash scripts/run.sh ./build/Release/sandvox.exe               # windowed game
bash scripts/run.sh ./build/Release/sandvox.exe --selftest     # headless selftest
bash scripts/run.sh ./build/Release/sandvox.exe --help         # list all flags
```

Shader validation without a rebuild: `bash scripts/check_shaders.sh`.

## Controls

| Input | Action |
|---|---|
| WASD + mouse | move / look |
| V | toggle fly / walk |
| Space | jump / swim up / fly up |
| Ctrl | descend (fly) / down |
| Shift | sprint |
| LMB | paint voxels (brush) / place (prefab) / swing (melee) / pour (fluid) |
| RMB | erase voxels (brush) / cast spell (magic mode) |
| Tab | cycle tool (brush / laser / prefab / mob / melee / fluid) |
| 1-8 | select material (brush) / speak glyph (magic mode) |
| `[` `]` | brush radius |
| F | laser (hold) |
| G | throw grenade |
| X | detonate at crosshair |
| Z | toggle magic mode |
| Backspace | abandon spell |
| C | cycle camera (first / third / over-shoulder) |
| M | spawn mob at crosshair |
| B | place prefab |
| K | spawn rolling sphere |
| U | clear MPM fluid |
| P / N | pause / single-step sim |
| R | hot-reload materials.json |
| F5 | hot-reload WGSL shaders |
| F9 / F10 | save / load world |
| F1 | toggle debug overlay |
| F3 | toggle collision-box wireframes |
| Esc | release cursor |
