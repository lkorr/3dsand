cmake_minimum_required(VERSION 3.24)
project(sandvox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Shared dependency cache — MUST be set before include(FetchContent).
#
# Every parallel agent session gets its own worktree (CLAUDE.md "Parallel
# sessions"), and a worktree is a fresh checkout with an empty build/. Left
# alone, FetchContent would re-clone and re-BUILD Dawn per worktree: ~15 min and
# several GB each, which is what makes worktrees unusable on this project rather
# than merely slow. Pointing FETCHCONTENT_BASE_DIR at one location outside every
# checkout shares the immutable third-party sources; each worktree still gets
# its own build/ for OUR objects, so agents never fight over sandvox.exe.
#
# The path is deliberately SHORT (C:/sv-deps, not ~/.sandvox-deps). Dawn's tint
# test corpus has ~200-char filenames, and under a worktree
# (…/.claude/worktrees/<name>/build/_deps/…) they blow the Windows 260-char
# MAX_PATH limit — the fetch fails with "Filename too long" and no amount of
# cache sharing helps. A short root keeps every worktree under the limit.
#
# Ordering matters: FetchContent's module sets FETCHCONTENT_BASE_DIR itself at
# include() time, so a `if(NOT DEFINED ...)` guard placed after the include
# never fires and every worktree silently gets a private cache. Guard on the
# CACHE entry so an explicit -DFETCHCONTENT_BASE_DIR=... still wins.
# ---------------------------------------------------------------------------
if(NOT DEFINED CACHE{FETCHCONTENT_BASE_DIR})
  if(DEFINED ENV{SANDVOX_DEPS})
    set(_sv_deps "$ENV{SANDVOX_DEPS}")
  elseif(WIN32)
    set(_sv_deps "C:/sv-deps")
  elseif(DEFINED ENV{HOME})
    set(_sv_deps "$ENV{HOME}/.sandvox-deps")
  else()
    set(_sv_deps "${CMAKE_BINARY_DIR}/_deps")  # last resort: per-build, as before
  endif()
  set(FETCHCONTENT_BASE_DIR "${_sv_deps}" CACHE PATH
      "Shared FetchContent cache, so worktrees don't each rebuild Dawn")
endif()
message(STATUS "sandvox: dependency cache = ${FETCHCONTENT_BASE_DIR}")

include(FetchContent)

# ---------------------------------------------------------------------------
# Dawn (WebGPU implementation; Vulkan backend on Windows native).
# DAWN_FETCH_DEPENDENCIES pulls Dawn's own third-party deps without depot_tools.
# ---------------------------------------------------------------------------
set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
# Without this, Dawn's LoadLibraryExA(vulkan-1.dll) uses search flags that
# require an absolute path and fails with Windows error 87.
set(DAWN_FORCE_SYSTEM_COMPONENT_LOAD ON CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN ON  CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D12  OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D11  OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL   OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES   OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW ON CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The `tint` CLI is our fast WGSL validator (scripts/check_shaders.sh) — seconds
# of feedback on a shader edit instead of a full link + launch.
set(TINT_BUILD_CMD_TOOLS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  dawn
  GIT_REPOSITORY https://dawn.googlesource.com/dawn
  GIT_TAG        main
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(dawn)

# ---------------------------------------------------------------------------
# Dear ImGui (GLFW + WebGPU backends)
# ---------------------------------------------------------------------------
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.4
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
target_link_libraries(imgui_lib PUBLIC glfw dawn::webgpu_dawn)

# ---------------------------------------------------------------------------
# Jolt Physics (rigid bodies for debris islands, DESIGN.md §7)
# ---------------------------------------------------------------------------
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
# DESIGN.md §10: keep lockstep viable — Jolt supports cross-platform determinism
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
# match the rest of the project (/MD); Jolt defaults to /MT and the link fails
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG        v5.3.0
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  Build
)
FetchContent_MakeAvailable(JoltPhysics)

# ---------------------------------------------------------------------------
# nlohmann/json
# ---------------------------------------------------------------------------
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# miniaudio (playback device + wav/flac/mp3 decoding for src/audio/)
#
# Single header, so it is fetched rather than linked: MINIAUDIO_IMPLEMENTATION
# is defined in exactly one TU (src/audio/device.cpp) and the rest of the audio
# code includes it for declarations only. Populate-only — miniaudio ships a
# CMakeLists that would add targets we do not want.
# ---------------------------------------------------------------------------
FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG        0.11.21
  GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
  FetchContent_Populate(miniaudio)
endif()

# ---------------------------------------------------------------------------
# sandvox
# ---------------------------------------------------------------------------
add_executable(sandvox
  src/main.cpp
  src/crash.cpp
  src/gpu/context.cpp
  src/gpu/resources.cpp
  src/sim/world.cpp
  src/sim/worldio.cpp
  src/sim/stream.cpp
  src/sim/farfield.cpp
  src/sim/chunkstore.cpp
  src/sim/simulation.cpp
  src/sim/materials.cpp
  src/sim/tuning.cpp
  src/sim/voxload.cpp
  src/sim/microvox.cpp
  src/sim/microbody.cpp
  src/game/player.cpp
  src/game/camera.cpp
  src/game/brush.cpp
  src/game/prefab.cpp
  src/game/mob.cpp
  src/game/bodyreg.cpp
  src/game/persist.cpp
  src/game/anim.cpp
  src/game/avatar.cpp
  src/game/thirdperson.cpp
  src/game/spell.cpp
  src/game/melee.cpp
  # The acceptance gate. support.cpp holds the sim/render plumbing that main.cpp
  # and the gates share; each selftest_*.cpp is one domain's gates. Split out of
  # main.cpp so `--gate <name>` can run one in seconds and so two agents editing
  # different domains stop conflicting in one 6k-line file. See test/selftest.h.
  src/test/support.cpp
  src/test/selftest.cpp
  src/test/selftest_sim.cpp
  src/test/selftest_render.cpp
  src/test/selftest_player.cpp
  src/test/selftest_phys.cpp
  src/test/selftest_mob.cpp
  src/test/selftest_worldio.cpp
  src/test/selftest_spell.cpp
  src/audio/device.cpp
  src/audio/library.cpp
  src/audio/voice.cpp
  src/audio/occlusion.cpp
  src/audio/world.cpp
  src/audio/cues.cpp
  # Vendored spatializer (src/audio/xyzpan/VENDORED_FROM.md). Compiled into the
  # sandvox target rather than as its own library: sandvox has no other
  # sub-libraries, and one target keeps the build simple. The upstream project
  # builds these with /fp:fast, which we deliberately do NOT do here — relaxed
  # float must never reach the sim (CLAUDE.md rule 1).
  src/audio/xyzpan/src/Engine.cpp
  src/audio/xyzpan/src/BinauralPipeline.cpp
  src/audio/xyzpan/src/DistancePipeline.cpp
  src/audio/xyzpan/src/ChestPipeline.cpp
  src/audio/xyzpan/src/FloorPipeline.cpp
  src/audio/xyzpan/src/ERPipeline.cpp
  src/audio/xyzpan/src/Coordinates.cpp
  src/audio/xyzpan/src/FDNReverb.cpp
  src/audio/xyzpan/src/LFO.cpp
  src/audio/xyzpan/src/SwarmMode.cpp
  src/phys/physics.cpp
  src/phys/debris.cpp
  src/phys/marching_cubes.cpp
  src/ui/overlay.cpp
  src/telemetry.cpp
)
# src/audio is on the path so the VENDORED engine's own `#include "xyzpan/..."`
# resolves unmodified — the sandvox-side audio code uses "audio/xyzpan/..."
# instead, matching the project's src-relative convention.
target_include_directories(sandvox PRIVATE src src/audio ${miniaudio_SOURCE_DIR})
target_compile_definitions(sandvox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(sandvox PRIVATE
  dawn::webgpu_dawn
  webgpu_cpp
  webgpu_glfw
  glfw
  imgui_lib
  nlohmann_json::nlohmann_json
  Jolt
  ws2_32
  dbghelp
)

# Run from any cwd: bake the asset dir absolute path for dev builds.
target_compile_definitions(sandvox PRIVATE SANDVOX_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")

# ---------------------------------------------------------------------------
# movement_test — CPU-only player controller harness (no GPU, no window).
#
# Covers what --selftest's walk check cannot: crossing NOISY ground. The
# selftest drops the player onto smooth terrain and asserts it lands, which
# still passes when walking over rough ground is completely broken. This one
# runs the controller against synthetic noisy/gritty heightfields and asserts
# distance covered and jumps honoured. Links no Dawn code — it only needs the
# headers that sim/world.h pulls in.
# ---------------------------------------------------------------------------
add_executable(movement_test
  tests/movement_test.cpp
  src/game/player.cpp
  src/sim/tuning.cpp
)
target_include_directories(movement_test PRIVATE src)
target_compile_definitions(movement_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(movement_test PRIVATE webgpu_cpp nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# voxload_test — CPU-only .vox parser harness (no GPU). Asserts the Z-up ->
# Y-up conversion, scene-graph flattening, and palette warnings — the
# properties that silently put every model on its side if they regress.
# ---------------------------------------------------------------------------
add_executable(voxload_test
  tests/voxload_test.cpp
  src/sim/voxload.cpp
)
target_include_directories(voxload_test PRIVATE src)
target_compile_definitions(voxload_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------------------
# lattice_test — CPU-only harness for the skin/collider lattice bridge
# (phys/lattice.h). Header-only dependency, so no engine sources to link.
#
# Asserts what a screenshot cannot: that a carve at skin resolution keeps
# detail the collider cannot express, that the derived collider still agrees
# with the skin about the body's extent, and that the int8 collider bound is
# enforced by DROPPING out-of-range blocks rather than wrapping them to the
# far side of the body.
# ---------------------------------------------------------------------------
add_executable(lattice_test tests/lattice_test.cpp)
target_include_directories(lattice_test PRIVATE src)
target_compile_definitions(lattice_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(lattice_test PRIVATE webgpu_cpp)
