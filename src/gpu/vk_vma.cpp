// vk_vma.cpp — the single translation unit that compiles VulkanMemoryAllocator.
//
// VMA is a header-only library whose implementation is guarded by
// VMA_IMPLEMENTATION, so exactly one TU may define it. This is that TU, and it
// deliberately contains nothing else — the same pattern src/audio/device.cpp
// uses for miniaudio.
//
// The build sets VMA_STATIC_VULKAN_FUNCTIONS=0 and VMA_DYNAMIC_VULKAN_FUNCTIONS=0
// (CMakeLists.txt): with VK_NO_PROTOTYPES there are no vk* symbols to bind
// statically, and we do not want VMA opening the DLL a second time to bind them
// dynamically either. Instead rhi_vulkan.cpp hands VMA an explicit
// VmaVulkanFunctions table filled from our own loader, so there is ONE place
// entry points come from.

#include <vulkan/vulkan.h>

// MSVC warns copiously inside VMA's generated code; none of it is ours to fix.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)  // unreferenced formal parameter
#pragma warning(disable : 4127)  // conditional expression is constant
#pragma warning(disable : 4189)  // local variable initialized but not referenced
#pragma warning(disable : 4324)  // structure padded due to alignment specifier
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
