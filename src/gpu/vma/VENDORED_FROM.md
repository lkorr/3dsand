# VulkanMemoryAllocator — vendored single header

Copied from https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
tag `v3.1.0`, commit `009ecd192c1289c7529bff248a16cfe896254816`.

Vendored into sandvox: 2026-08-22 (Vulkan port phase 3a).

Files: `vk_mem_alloc.h` (the entire library) and `LICENSE.txt` (MIT).

## Why vendored rather than FetchContent

FetchContent was the first choice and it does not work in this repo, for a
reason worth writing down so nobody re-tries it:

**`FETCHCONTENT_FULLY_DISCONNECTED` is `ON` in the configured build tree.** That
setting exists here to stop CMake re-checking the network for Dawn/Jolt/ImGui on
every configure — which is exactly what you want for the ~15-minute Dawn fetch —
but it also means a **newly declared** dependency is never downloaded at all.
CMake does not fail on it either: `FetchContent_Populate` emits a warning,
leaves `${vulkanmemoryallocator_SOURCE_DIR}` pointing at a directory that does
not exist, and configure exits 0. The empty include directory that produced then
broke the build in a place with nothing to do with VMA (an ImGui/Dawn header
ordering failure), which cost real time to trace back.

So a new FetchContent dependency in this tree is only correct for someone who
also deletes their build directory. Vendoring is honest about that: one file,
no configure-time network, no interaction with the disconnected flag, and every
worktree gets it from git like any other source file.

The precedent is `src/audio/xyzpan/` — but note the reasoning differs. xyzpan is
vendored because we may EDIT it. VMA is vendored because it CANNOT be fetched
here. Both end up in the same place; only one of them is about ownership.

## Do not edit this header

It is 18,676 lines of generated/upstream code. If it needs a behaviour change,
that is a configuration macro set at the include site
(`src/gpu/vk_vma.cpp` and the `VMA_*` defines in `CMakeLists.txt`), not an edit
here — an edited copy silently diverges from the tag above and the next person
to bump the version loses the change.

## How it is configured

`CMakeLists.txt` sets, for the whole `sandvox` target:

  * `VK_NO_PROTOTYPES` — vulkan.h declares no `vk*` functions, so nothing can
    accidentally static-link against a Vulkan loader we do not ship.
  * `VMA_STATIC_VULKAN_FUNCTIONS=0` — there are no static symbols to bind.
  * `VMA_DYNAMIC_VULKAN_FUNCTIONS=0` — VMA must NOT open vulkan-1.dll itself.

With all three off, VMA requires an explicit `VmaVulkanFunctions` table, which
`rhi_vulkan.cpp` fills from the one loader in `src/gpu/vk_loader.h`. That is the
point: entry points have exactly one source.

`VMA_IMPLEMENTATION` is defined in exactly one TU, `src/gpu/vk_vma.cpp`, the same
single-TU pattern `src/audio/device.cpp` uses for miniaudio.

## Upgrading

Replace `vk_mem_alloc.h` (and `LICENSE.txt` if it changed), update the tag and
commit hash above, and rebuild. There is no patch to re-apply — the file is
verbatim upstream, and keeping it that way is what makes an upgrade a copy.
