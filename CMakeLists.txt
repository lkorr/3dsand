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
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)

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
  src/sim/simulation.cpp
  src/sim/materials.cpp
  src/game/player.cpp
  src/game/camera.cpp
  src/game/brush.cpp
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
)

# Run from any cwd: bake the asset dir absolute path for dev builds.
target_compile_definitions(sandvox PRIVATE SANDVOX_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
