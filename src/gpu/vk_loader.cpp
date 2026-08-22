#include "gpu/vk_loader.h"

#include <windows.h>

// windows.h (included above, after the header) re-defines this; see the note
// in vk_loader.h.
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif

namespace vkl {
namespace {

HMODULE g_lib = nullptr;

}  // namespace

bool LoadGlobal(GlobalFns& out, std::string& err) {
  if (!g_lib) {
    // Same DLL Dawn loads. No path search games: the system copy is the only
    // correct one, and a driver-less machine should say so plainly.
    g_lib = LoadLibraryA("vulkan-1.dll");
    if (!g_lib) {
      err = "cannot load vulkan-1.dll (no Vulkan driver installed?)";
      return false;
    }
  }
  out.GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)GetProcAddress(g_lib, "vkGetInstanceProcAddr");
  if (!out.GetInstanceProcAddr) {
    err = "vulkan-1.dll has no vkGetInstanceProcAddr";
    return false;
  }
  auto get = [&](const char* n) { return out.GetInstanceProcAddr(VK_NULL_HANDLE, n); };
  out.CreateInstance = (PFN_vkCreateInstance)get("vkCreateInstance");
  out.EnumerateInstanceExtensionProperties =
      (PFN_vkEnumerateInstanceExtensionProperties)get(
          "vkEnumerateInstanceExtensionProperties");
  out.EnumerateInstanceLayerProperties =
      (PFN_vkEnumerateInstanceLayerProperties)get("vkEnumerateInstanceLayerProperties");
  // Absent on a Vulkan 1.0 loader; that is a supported outcome, and the caller
  // treats null as "assume 1.0".
  out.EnumerateInstanceVersion =
      (PFN_vkEnumerateInstanceVersion)get("vkEnumerateInstanceVersion");
  if (!out.CreateInstance) {
    err = "vulkan-1.dll has no vkCreateInstance";
    return false;
  }
  return true;
}

void LoadInstance(const GlobalFns& g, VkInstance inst, bool debugUtils, InstanceFns& o) {
  auto get = [&](const char* n) { return g.GetInstanceProcAddr(inst, n); };
#define VKL_I(name) o.name = (PFN_vk##name)get("vk" #name)
  VKL_I(DestroyInstance);
  VKL_I(EnumeratePhysicalDevices);
  VKL_I(GetPhysicalDeviceProperties);
  VKL_I(GetPhysicalDeviceProperties2);
  VKL_I(GetPhysicalDeviceFeatures);
  VKL_I(GetPhysicalDeviceFeatures2);
  VKL_I(GetPhysicalDeviceMemoryProperties);
  VKL_I(GetPhysicalDeviceQueueFamilyProperties);
  VKL_I(EnumerateDeviceExtensionProperties);
  VKL_I(CreateDevice);
  VKL_I(GetDeviceProcAddr);
#undef VKL_I
  if (debugUtils) {
    o.CreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT)get("vkCreateDebugUtilsMessengerEXT");
    o.DestroyDebugUtilsMessengerEXT =
        (PFN_vkDestroyDebugUtilsMessengerEXT)get("vkDestroyDebugUtilsMessengerEXT");
  }
  // VK_KHR_surface entry points resolve to null when the instance did not
  // enable the extension (headless), which is the supported outcome.
  o.DestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)get("vkDestroySurfaceKHR");
  o.GetPhysicalDeviceSurfaceSupportKHR =
      (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)get("vkGetPhysicalDeviceSurfaceSupportKHR");
  o.GetPhysicalDeviceSurfaceCapabilitiesKHR =
      (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)get(
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  o.GetPhysicalDeviceSurfaceFormatsKHR =
      (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)get("vkGetPhysicalDeviceSurfaceFormatsKHR");
  o.GetPhysicalDeviceSurfacePresentModesKHR =
      (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)get(
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
}

void LoadDevice(const InstanceFns& i, VkDevice dev, DeviceFns& o) {
  auto get = [&](const char* n) { return i.GetDeviceProcAddr(dev, n); };
#define VKL_D(name) o.name = (PFN_vk##name)get("vk" #name)
  VKL_D(DestroyDevice);
  VKL_D(GetDeviceQueue);
  VKL_D(QueueSubmit);
  VKL_D(QueueWaitIdle);
  VKL_D(DeviceWaitIdle);

  VKL_D(AllocateMemory);
  VKL_D(FreeMemory);
  VKL_D(MapMemory);
  VKL_D(UnmapMemory);
  VKL_D(FlushMappedMemoryRanges);
  VKL_D(InvalidateMappedMemoryRanges);
  VKL_D(BindBufferMemory);
  VKL_D(BindImageMemory);
  VKL_D(GetBufferMemoryRequirements);
  VKL_D(GetImageMemoryRequirements);
  VKL_D(CreateBuffer);
  VKL_D(DestroyBuffer);
  VKL_D(CreateImage);
  VKL_D(DestroyImage);
  VKL_D(CmdCopyBuffer);
  VKL_D(GetBufferMemoryRequirements2);
  VKL_D(GetImageMemoryRequirements2);
  VKL_D(BindBufferMemory2);
  VKL_D(BindImageMemory2);

  VKL_D(CreateCommandPool);
  VKL_D(DestroyCommandPool);
  VKL_D(ResetCommandPool);
  VKL_D(AllocateCommandBuffers);
  VKL_D(FreeCommandBuffers);
  VKL_D(BeginCommandBuffer);
  VKL_D(EndCommandBuffer);
  VKL_D(ResetCommandBuffer);

  VKL_D(CmdFillBuffer);
  VKL_D(CmdUpdateBuffer);
  VKL_D(CmdPipelineBarrier);
  // Core 1.3. Resolvable on a 1.3 device regardless of features; LEGAL to call
  // only if synchronization2 was enabled. Backend::Init checks both.
  VKL_D(CmdPipelineBarrier2);
  VKL_D(CmdBindPipeline);
  VKL_D(CmdBindDescriptorSets);
  VKL_D(CmdDispatch);
  VKL_D(CmdDispatchIndirect);

  VKL_D(CreateFence);
  VKL_D(DestroyFence);
  VKL_D(ResetFences);
  VKL_D(GetFenceStatus);
  VKL_D(WaitForFences);

  VKL_D(CreateShaderModule);
  VKL_D(DestroyShaderModule);
  VKL_D(CreateDescriptorSetLayout);
  VKL_D(DestroyDescriptorSetLayout);
  VKL_D(CreatePipelineLayout);
  VKL_D(DestroyPipelineLayout);
  VKL_D(CreateComputePipelines);
  VKL_D(DestroyPipeline);
  VKL_D(CreateDescriptorPool);
  VKL_D(DestroyDescriptorPool);
  VKL_D(AllocateDescriptorSets);
  VKL_D(UpdateDescriptorSets);

  VKL_D(CmdBeginRendering);
  VKL_D(CmdEndRendering);
  VKL_D(CmdDraw);
  VKL_D(CmdDrawIndirect);
  VKL_D(CmdSetViewport);
  VKL_D(CmdSetScissor);
  VKL_D(CmdCopyImageToBuffer);
  VKL_D(CreateImageView);
  VKL_D(DestroyImageView);
  VKL_D(CreateGraphicsPipelines);

  // VK_KHR_swapchain: null unless the device enabled it (windowed only).
  VKL_D(CreateSwapchainKHR);
  VKL_D(DestroySwapchainKHR);
  VKL_D(GetSwapchainImagesKHR);
  VKL_D(AcquireNextImageKHR);
  VKL_D(QueuePresentKHR);
  VKL_D(CreateSemaphore);
  VKL_D(DestroySemaphore);

  VKL_D(CreateQueryPool);
  VKL_D(DestroyQueryPool);
  VKL_D(CmdResetQueryPool);
  VKL_D(CmdWriteTimestamp);
  VKL_D(CmdWriteTimestamp2);
  VKL_D(CmdCopyQueryPoolResults);
  VKL_D(GetQueryPoolResults);
#undef VKL_D
}

const char* ResultName(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    default: return "VK_ERROR_<other>";
  }
}

}  // namespace vkl
