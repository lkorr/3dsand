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
# TINT — the WGSL compiler, fetched as part of the Dawn repository.
#
# DAWN WAS REMOVED 2026-08-22 (user decision; docs/PLAN_vulkan_port.md phase 6
# decision log). The engine is Vulkan-only: nothing links dawn_native,
# webgpu_dawn, webgpu_cpp or webgpu_glfw any more. What the checkout is STILL
# here for — and the reason this block is not simply deleted — is Tint:
#
#   * `tint_lang_wgsl_reader` + `tint_lang_spirv_writer` are linked into
#     sandvox and are how `src/gpu/vk_spirv.cpp` turns the live WGSL string
#     (constant prelude + tuning block + common.wgsl + body) into SPIR-V at
#     load AND at every F5 hot-reload. Precompiling .spv offline cannot
#     replace this: F5 rebuilds the prelude from live tuning values, so a
#     precompiled blob would freeze them.
#   * `tint.exe` (TINT_BUILD_CMD_TOOLS) is scripts/check_shaders.sh's fast
#     WGSL validator — seconds of feedback on a shader edit instead of a full
#     link + launch, and it runs automatically on every WGSL edit via the
#     PostToolUse hook.
#
# HOW THE DAWN ENGINE IS EXCLUDED, and why this shape rather than a target
# blacklist: Dawn's own CMakeLists gates `add_subdirectory(src/dawn)` (which
# is what DEFINES dawn_native, webgpu_dawn, webgpu_cpp, webgpu_glfw and the
# code generator) on "at least one DAWN_ENABLE_<backend> is ON". With all of
# them OFF those targets are never declared at all — there is nothing left to
# accidentally link, and ~156 dawn_native translation units plus a 178 MB
# webgpu_dawn.lib stop being built. `add_subdirectory(src/tint)` is
# UNCONDITIONAL, so Tint is unaffected.
#
# THE TRAP THIS BLOCK EXISTS TO DOCUMENT: three TINT_BUILD_* options default
# to `${DAWN_ENABLE_VULKAN}` upstream — SPV_WRITER, SPV_READER — so turning
# the Dawn Vulkan backend off ALSO silently turns off the SPIR-V writer we
# depend on, and the build fails much later with an unresolved
# tint::spirv::writer::Generate. They are therefore forced ON here
# INDEPENDENTLY of any DAWN_ENABLE_* value. Do not "simplify" them away.
# ---------------------------------------------------------------------------
set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
# Every Dawn backend OFF => src/dawn is never added => no Dawn engine targets.
set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D12  OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D11  OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL   OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES   OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_METAL      OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
# STAYS ON, and not for Dawn's sake: this flag is what makes Dawn's
# third_party/CMakeLists.txt `add_subdirectory` the GLFW library itself, which
# is where the engine's `glfw` link target comes from. Dawn's own GLFW helper
# (`dawn_glfw`, the wgpu::Surface factory) lives under src/dawn and is excluded
# with the rest of it — we create the VkSurfaceKHR with glfwCreateWindowSurface
# directly. Turning this OFF removes the `glfw` target and breaks the link.
set(DAWN_USE_GLFW ON CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Load-bearing: see "THE TRAP" above. These default to ${DAWN_ENABLE_VULKAN},
# which is now OFF, and they are exactly what the engine's WGSL->SPIR-V path
# is built from.
set(TINT_BUILD_SPV_WRITER  ON CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_READER  ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_WRITER ON CACHE BOOL "" FORCE)
# The `tint` CLI is our fast WGSL validator (scripts/check_shaders.sh) — seconds
# of feedback on a shader edit instead of a full link + launch.
set(TINT_BUILD_CMD_TOOLS ON CACHE BOOL "" FORCE)

# The name stays `dawn` (repository, cache directory, FetchContent id): the
# checkout IS the Dawn repository and renaming it would invalidate every
# existing C:/sv-deps cache for no benefit. CLAUDE.md's notes about that path
# being short and set before include(FetchContent) apply unchanged.
FetchContent_Declare(
  dawn
  GIT_REPOSITORY https://dawn.googlesource.com/dawn
  GIT_TAG        main
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(dawn)

# ---------------------------------------------------------------------------
# Vulkan::Headers — declared HERE, because Dawn only declares it for its own
# Vulkan backend.
#
# Dawn's third_party/CMakeLists.txt guards the Vulkan-Headers add_subdirectory
# on `NOT TARGET Vulkan::Headers AND DAWN_ENABLE_VULKAN`. With every Dawn
# backend off (above) the target is never created, and the engine — which needs
# the headers far more than Dawn ever did — fails to link against a name that
# does not exist. The SOURCES are unaffected: fetch_dawn_dependencies pulls
# vulkan-headers under the `dawn_standalone` condition, not under a backend
# flag, so the checkout is there either way and this just does the
# add_subdirectory Dawn skipped.
#
# `NOT TARGET` guard so that a future Dawn version (or a re-enabled backend)
# declaring it first is not a duplicate-target error.
# ---------------------------------------------------------------------------
if(NOT TARGET Vulkan::Headers)
  set(VULKAN_HEADERS_ENABLE_MODULE OFF)
  add_subdirectory("${dawn_SOURCE_DIR}/third_party/vulkan-headers/src"
                   "${CMAKE_BINARY_DIR}/vulkan-headers" EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# Vulkan backend dependencies (docs/PLAN_vulkan_port.md phase 3).
#
# THREE things, none of which adds a system SDK dependency:
#
# 1. Vulkan HEADERS come from Dawn's own dep tree, which already fetched
#    Vulkan-Headers and exports `Vulkan::Headers` as an INTERFACE target. We do
#    NOT link a loader: Dawn's tree does not build one, and the engine already
#    proves vulkan-1.dll is loadable at runtime (DAWN_FORCE_SYSTEM_COMPONENT_LOAD
#    above exists precisely because Dawn loads it dynamically). src/gpu/vk_loader.*
#    does the same volk-style LoadLibrary + vkGetInstanceProcAddr walk, so a
#    machine with a driver needs nothing installed and a machine without one gets
#    a clean error instead of a link failure. VK_NO_PROTOTYPES enforces this: any
#    direct call to a vk* symbol becomes a COMPILE error rather than an
#    accidental static link against a loader we do not ship.
#
# 2. TINT is Dawn's WGSL compiler, already built here (it is what tint.exe is).
#    Linking `tint_lang_wgsl_reader` + `tint_lang_spirv_writer` gives the Vulkan
#    backend WGSL->SPIR-V in-process, which is what keeps WGSL the single shader
#    source of truth and keeps F5 hot-reload working (the alternative — invoking
#    tint.exe offline at build time — cannot hot-reload). Both targets are
#    declared EXCLUDE_FROM_ALL by tint_add_target, so they only build because we
#    name them here. They carry their own PUBLIC include directories, so no
#    manual path into the dep cache is needed and nothing here hardcodes
#    C:/sv-deps.
#
# 3. VMA (VulkanMemoryAllocator) is VENDORED at src/gpu/vma/, not fetched. It is
#    a single header, so there is nothing to build and no target to link — the
#    include directory below is the whole integration, and VMA_IMPLEMENTATION is
#    compiled in exactly one TU (src/gpu/vk_vma.cpp), the same single-TU pattern
#    miniaudio uses above.
#
#    FetchContent was tried first and is WRONG HERE, for a reason that is not
#    obvious and cost real debugging time: `FETCHCONTENT_FULLY_DISCONNECTED` is
#    ON in a configured build tree (it is what stops CMake re-checking the
#    network for Dawn on every configure). Under that flag a NEWLY declared
#    dependency is never downloaded — and CMake does not fail. It warns, leaves
#    ${vulkanmemoryallocator_SOURCE_DIR} pointing at a directory that does not
#    exist, and exits 0. The empty include path that produced then broke the
#    build somewhere else entirely. A new FetchContent dependency here is only
#    correct for someone who also wipes their build directory, which is not a
#    reasonable thing to require of the next agent.
#
#    See src/gpu/vma/VENDORED_FROM.md for the upstream tag and the upgrade
#    procedure. Dawn's own third_party/vulkan_memory_allocator directory exists
#    but is EMPTY (Dawn does not fetch it), so it cannot be reused either.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Dear ImGui (GLFW platform + Vulkan render backend)
#
# imgui_impl_wgpu.cpp was dropped with Dawn (2026-08-22): src/ui/overlay.cpp is
# Vulkan-only, so the WGPU backend TU and the IMGUI_IMPL_WEBGPU_BACKEND_DAWN
# define it needed are both gone.
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
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# IMGUI_IMPL_VULKAN_NO_PROTOTYPES: the whole engine loads Vulkan dynamically
# (VK_NO_PROTOTYPES; src/gpu/vk_loader.*), so ImGui's Vulkan backend must too —
# overlay.cpp feeds it entry points via ImGui_ImplVulkan_LoadFunctions. PUBLIC
# because the flag must match between imgui_impl_vulkan.cpp and every includer
# of its header.
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(imgui_lib PUBLIC glfw Vulkan::Headers)

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
  src/gpu/rhi.cpp
  src/gpu/context.cpp
  src/gpu/resources.cpp
  src/gpu/passtimer.cpp
  # The Vulkan backend — the ONLY backend since 2026-08-22. rhi_vk.cpp is the
  # single implementation behind the rhi.h seam (rhi_dawn.cpp deleted with
  # Dawn); the seam itself stays, because it is what made the port testable and
  # an abstract impl with one subclass costs nothing.
  src/gpu/vk_loader.cpp
  src/gpu/vk_vma.cpp
  src/gpu/vk_spirv.cpp
  src/gpu/rhi_vulkan.cpp
  src/gpu/rhi_vk.cpp
  src/gpu/vk_record.cpp
  src/gpu/vk_info.cpp
  src/gpu/vk_smoke.cpp
  src/measure/measure.cpp
  # The fluid lab (docs/PLAN_fluid_overhaul.md §4): --lab scenes + the
  # --fluid-bench harness. Same layer as measure — it consumes test/support.h.
  src/lab/lab.cpp
  src/sim/world.cpp
  src/sim/pagetable.cpp
  src/sim/worldio.cpp
  src/sim/worldedit.cpp
  src/sim/stream.cpp
  src/sim/farfield.cpp
  src/sim/faredits.cpp
  src/sim/chunkstore.cpp
  src/sim/simulation.cpp
  src/sim/pass_table.cpp
  src/sim/materials.cpp
  src/sim/tuning.cpp
  src/sim/celestial.cpp
  # Wind primitives: fans / spell gusts / vortices as op-stream parametric
  # objects (docs/RESEARCH_wind.md §4.3).
  src/sim/windprim.cpp
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
  # --voxdump / --voxserve: the tuner's voxel-accurate terrain view. Sits with
  # support.cpp because it drives the same worldgen/readback plumbing the gates
  # do, not because it is a test.
  src/tools/voxregion.cpp
  src/test/support.cpp
  src/test/selftest.cpp
  src/test/selftest_voxregion.cpp
  src/test/selftest_terrain.cpp
  src/test/selftest_sim.cpp
  src/test/selftest_ca.cpp
  src/test/selftest_wind.cpp
  src/test/selftest_render.cpp
  src/test/selftest_player.cpp
  src/test/selftest_phys.cpp
  src/test/selftest_mob.cpp
  src/test/selftest_worldio.cpp
  src/test/selftest_spell.cpp
  src/test/selftest_audio.cpp
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
target_include_directories(sandvox PRIVATE src src/audio ${miniaudio_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/gpu/vma)
target_compile_definitions(sandvox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# VK_NO_PROTOTYPES is load-bearing, not hygiene: it removes the vk* function
# declarations from vulkan.h so every entry point MUST come through the
# dynamically-loaded table in src/gpu/vk_loader.h. Without it, a typo'd direct
# call compiles and then fails to LINK (no loader library) or, worse, silently
# links against a stray import lib. VMA is told the same via
# VMA_STATIC_VULKAN_FUNCTIONS=0, and gets its pointers from the same table.
target_compile_definitions(sandvox PRIVATE
  VK_NO_PROTOTYPES
  VMA_STATIC_VULKAN_FUNCTIONS=0
  VMA_DYNAMIC_VULKAN_FUNCTIONS=0
)
target_link_libraries(sandvox PRIVATE
  glfw
  imgui_lib
  nlohmann_json::nlohmann_json
  Jolt
  ws2_32
  dbghelp
  # Vulkan backend: headers only (no loader — see the block above), plus Tint's
  # WGSL front end and SPIR-V back end for in-process shader compilation. These
  # two tint targets are the ONLY thing still linked out of the Dawn checkout.
  Vulkan::Headers
  tint_lang_wgsl_reader
  tint_lang_spirv_writer
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
# distance covered and jumps honoured. Links no GPU code at all: it only needs
# the headers that sim/world.h pulls in, and since the Dawn removal those are
# plain C++ (the `webgpu_cpp` link line here existed purely to put Dawn's
# headers on the include path for rhi.h).
# ---------------------------------------------------------------------------
add_executable(movement_test
  tests/movement_test.cpp
  src/game/player.cpp
  src/sim/tuning.cpp
  src/sim/celestial.cpp
)
target_include_directories(movement_test PRIVATE src)
target_compile_definitions(movement_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(movement_test PRIVATE nlohmann_json::nlohmann_json)

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
