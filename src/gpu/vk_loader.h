// vk_loader.h — volk-style dynamic loading of the Vulkan entry points.
//
// WHY NOT LINK A LOADER
// ---------------------
// The repo deliberately has no system Vulkan SDK dependency, and Dawn's
// dependency tree — which is where our Vulkan-Headers come from — checks out
// vulkan-loader but never BUILDS it. There is therefore no vulkan-1.lib to link
// against, and requiring one would mean every contributor installs the SDK.
//
// We do not need it. The engine already runs on Dawn's Vulkan backend, which
// means vulkan-1.dll is present and loadable on any machine that can run
// sandvox today (that is what DAWN_FORCE_SYSTEM_COMPONENT_LOAD in CMakeLists.txt
// is about). So we do exactly what the loader-linking would have done: open the
// DLL, take vkGetInstanceProcAddr, and walk the function tables ourselves.
//
// The build compiles with VK_NO_PROTOTYPES, so vulkan.h declares no vk*
// functions at all. Every entry point in the engine resolves through the tables
// below, and a direct call to a vk* symbol is a COMPILE error rather than a
// link-time surprise or an accidental static bind. That is the property worth
// having: it is mechanically impossible to half-use a loader we do not ship.
//
// THREE TIERS, because Vulkan has three.
//   - Global: resolved from a null instance (vkCreateInstance, and the two
//     enumerations you need BEFORE an instance exists).
//   - Instance: resolved from the VkInstance; covers physical-device queries.
//   - Device: resolved from the VkDevice via vkGetDeviceProcAddr. Going through
//     the device tier rather than the instance tier skips the loader's dispatch
//     trampoline on every call — free, and the reason volk exists.

#pragma once

#include <vulkan/vulkan.h>

#include <string>

// windows.h defines CreateSemaphore as a macro (CreateSemaphoreA/W). Any TU
// that includes both windows.h and this header would otherwise have the struct
// member below silently renamed by the preprocessor.
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif

namespace vkl {

// Global-tier entry points: everything callable before/without a VkInstance.
struct GlobalFns {
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkCreateInstance CreateInstance = nullptr;
  PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
  PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
  PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
};

// Instance-tier: physical device enumeration/queries, and the debug messenger
// that carries validation output back to us.
struct InstanceFns {
  PFN_vkDestroyInstance DestroyInstance = nullptr;
  PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
  PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkCreateDevice CreateDevice = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  // VK_EXT_debug_utils — only non-null when the extension was enabled.
  PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT = nullptr;
  PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT = nullptr;
  // VK_KHR_surface — only non-null when the instance enabled the surface
  // extensions (windowed phase 4b; headless runs leave these null).
  PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
};

// Device-tier: everything the compute backend actually issues per frame.
struct DeviceFns {
  PFN_vkDestroyDevice DestroyDevice = nullptr;
  PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;

  // Memory / buffers. VMA needs most of these handed to it explicitly, since we
  // build with VMA_STATIC_VULKAN_FUNCTIONS=0.
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
  PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
  // Vulkan 1.1 memory requirement queries VMA prefers when available.
  PFN_vkGetBufferMemoryRequirements2 GetBufferMemoryRequirements2 = nullptr;
  PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2 = nullptr;
  PFN_vkBindBufferMemory2 BindBufferMemory2 = nullptr;
  PFN_vkBindImageMemory2 BindImageMemory2 = nullptr;

  // Command recording.
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkResetCommandPool ResetCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;

  // Fills (zero-init), updates (Class A uploads), and — from phase 3b — the
  // recording commands and the generated barriers.
  PFN_vkCmdFillBuffer CmdFillBuffer = nullptr;
  PFN_vkCmdUpdateBuffer CmdUpdateBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  // vkCmdPipelineBarrier2 is CORE VULKAN 1.3, but a core-1.3 command is only
  // usable when the corresponding feature is ENABLED at device creation —
  // VkPhysicalDeviceVulkan13Features::synchronization2. Promotion to core makes
  // the symbol resolvable; it does not make the command legal. With no
  // validation layer on this machine, calling it on a device that did not
  // request the feature is undefined behaviour that faults inside the ICD
  // rather than erroring, so Backend::Init enables the feature explicitly and
  // refuses to run if the device cannot offer it.
  PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2 = nullptr;
  PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
  PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
  PFN_vkCmdDispatch CmdDispatch = nullptr;
  PFN_vkCmdDispatchIndirect CmdDispatchIndirect = nullptr;

  // Sync.
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  PFN_vkGetFenceStatus GetFenceStatus = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;

  // Pipelines + descriptors.
  PFN_vkCreateShaderModule CreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
  PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
  PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
  PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
  PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
  PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
  PFN_vkDestroyPipeline DestroyPipeline = nullptr;
  PFN_vkCreatePipelineCache CreatePipelineCache = nullptr;
  PFN_vkDestroyPipelineCache DestroyPipelineCache = nullptr;
  PFN_vkGetPipelineCacheData GetPipelineCacheData = nullptr;
  PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;

  // Render path (phase 4b). Dynamic rendering is core 1.3 (its feature bit is
  // MANDATORY in 1.3, unlike synchronization2's pre-1.3 history), but the same
  // rule applies: the entry point resolving does not make the call legal until
  // the feature is enabled at device creation — Backend::Init does both.
  PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
  PFN_vkCmdEndRendering CmdEndRendering = nullptr;
  PFN_vkCmdDraw CmdDraw = nullptr;
  PFN_vkCmdDrawIndirect CmdDrawIndirect = nullptr;
  PFN_vkCmdSetViewport CmdSetViewport = nullptr;
  PFN_vkCmdSetScissor CmdSetScissor = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;

  // VK_KHR_swapchain — only non-null when the device enabled it (windowed).
  PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
  PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
  PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
  PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
  PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
  PFN_vkCreateSemaphore CreateSemaphore = nullptr;
  PFN_vkDestroySemaphore DestroySemaphore = nullptr;

  // Timestamps (capability-gated).
  PFN_vkCreateQueryPool CreateQueryPool = nullptr;
  PFN_vkDestroyQueryPool DestroyQueryPool = nullptr;
  PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
  PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
  PFN_vkCmdWriteTimestamp2 CmdWriteTimestamp2 = nullptr;
  PFN_vkCmdCopyQueryPoolResults CmdCopyQueryPoolResults = nullptr;
  PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;
};

// Open vulkan-1.dll and resolve the global tier. Returns false with `err` set
// if the library is missing or too old — a machine with no Vulkan driver is a
// clean, reportable condition, not a crash.
bool LoadGlobal(GlobalFns& out, std::string& err);

// Resolve the instance tier. `debugUtils` selects whether the messenger entry
// points are looked up (they are absent unless the extension was enabled).
void LoadInstance(const GlobalFns& g, VkInstance inst, bool debugUtils, InstanceFns& out);

// Resolve the device tier through vkGetDeviceProcAddr.
void LoadDevice(const InstanceFns& i, VkDevice dev, DeviceFns& out);

// Human-readable VkResult, for error messages.
const char* ResultName(VkResult r);

}  // namespace vkl
