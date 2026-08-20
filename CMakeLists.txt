cmake_minimum_required(VERSION 3.24)
project(sandvox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
# sandvox
# ---------------------------------------------------------------------------
add_executable(sandvox
  src/main.cpp
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
  src/game/anim.cpp
  src/game/avatar.cpp
  src/game/thirdperson.cpp
  src/phys/physics.cpp
  src/phys/debris.cpp
  src/phys/marching_cubes.cpp
  src/ui/overlay.cpp
)
target_include_directories(sandvox PRIVATE src)
target_compile_definitions(sandvox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(sandvox PRIVATE
  dawn::webgpu_dawn
  webgpu_cpp
  webgpu_glfw
  glfw
  imgui_lib
  nlohmann_json::nlohmann_json
  Jolt
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
