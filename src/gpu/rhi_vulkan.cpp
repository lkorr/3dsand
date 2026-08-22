#include "gpu/rhi_vulkan.h"

#include <cstring>
#include <functional>

#include "gpu/vk_spirv.h"
#include "sim/microvox.h"  // MicroBrickGpu — Class A boundary assert
#include "sim/world.h"     // kMaxDebugBoxes / DebugBox — Class A boundary assert
#include "vk_mem_alloc.h"

namespace vk {

// ---------------------------------------------------------------------------
// CLASS A BOUNDARY. vkCmdUpdateBuffer's limit is 65536 bytes, and TWO engine
// buffers land exactly on it. Both are legal by one byte, and both are one
// constant bump from becoming an illegal update that Vulkan reports at runtime
// as a validation error inside an upload — a long way from the `constexpr` that
// caused it. Asserting here converts that into a build failure at the bump.
//
// The barrier document is explicit that the fix is NOT to move these to Class B
// "for safety": a size-derived rule with two hand-made exceptions is a rule the
// next person applies wrong.
// ---------------------------------------------------------------------------
static_assert((uint64_t)kMaxDebugBoxes * sizeof(DebugBox) <= kClassAMaxBytes,
              "debugBoxes exceeded vkCmdUpdateBuffer's 65536-byte limit: it was "
              "EXACTLY at it. Either lower kMaxDebugBoxes or move debugBoxes to "
              "the Class B staging path (barrier_graph 4.1).");
static_assert((uint64_t)kMaterialSlots * sizeof(MicroBrickGpu) <= kClassAMaxBytes,
              "microTableBuf_ exceeded vkCmdUpdateBuffer's 65536-byte limit: it "
              "was EXACTLY at it. Either lower kMaterialSlots or move it to the "
              "Class B staging path (barrier_graph 4.1).");

namespace {

// Big enough for the worst tick the barrier doc enumerates (cellOps 512 KiB +
// spawnOps 128 KiB + bodyInstances 4 MiB, plus 4 MiB pool buffers on a hot
// reload). 16 MiB is ample and costs nothing that is not touched.
constexpr uint64_t kStagingRingBytes = 16ull * 1024 * 1024;

// Class B copies must respect the alignment vkCmdCopyBuffer wants; 16 is
// generous and keeps every payload naturally aligned.
constexpr uint64_t kStagingAlign = 16;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

VkBufferUsageFlags ToVkUsage(rhi::BufferUsage u) {
  VkBufferUsageFlags f = 0;
  if (rhi::Any(u, rhi::BufferUsage::CopySrc)) f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (rhi::Any(u, rhi::BufferUsage::CopyDst)) f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (rhi::Any(u, rhi::BufferUsage::Index)) f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (rhi::Any(u, rhi::BufferUsage::Vertex)) f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (rhi::Any(u, rhi::BufferUsage::Uniform)) f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (rhi::Any(u, rhi::BufferUsage::Storage)) f |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (rhi::Any(u, rhi::BufferUsage::Indirect)) f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  // MapRead/MapWrite are memory properties, not usage bits; they steer the VMA
  // allocation below rather than adding a VkBufferUsageFlag.
  return f;
}

VkDescriptorType ToVkDescriptorType(rhi::BufferBindingType t, bool dynamic) {
  switch (t) {
    case rhi::BufferBindingType::Uniform:
      return dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                     : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case rhi::BufferBindingType::Storage:
    case rhi::BufferBindingType::ReadOnlyStorage:
      // Vulkan has no read-only storage descriptor type: read-only-ness is a
      // property of the SPIR-V (Tint emits NonWritable), not of the descriptor.
      // Both map to STORAGE_BUFFER, which is correct and is also why the pass
      // table — not the descriptor — is what tells the barrier generator whether
      // a pass reads or writes a buffer.
      return dynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                     : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

VkShaderStageFlags ToVkStages(rhi::ShaderStage s) {
  VkShaderStageFlags f = 0;
  if ((uint32_t)s & (uint32_t)rhi::ShaderStage::Vertex) f |= VK_SHADER_STAGE_VERTEX_BIT;
  if ((uint32_t)s & (uint32_t)rhi::ShaderStage::Fragment) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if ((uint32_t)s & (uint32_t)rhi::ShaderStage::Compute) f |= VK_SHADER_STAGE_COMPUTE_BIT;
  return f;
}

}  // namespace

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  if (!data || !data->pMessage) return VK_FALSE;
  // Errors and warnings only: info/verbose from the validation layers is
  // thousands of lines and drowns the thing you are looking for.
  if (!(severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)))
    return VK_FALSE;
  auto* be = (Backend*)user;
  if (be) be->validationMsgs_.push_back(data->pMessage);
  return VK_FALSE;  // never abort the call
}

Backend::~Backend() { Shutdown(); }

bool Backend::Init(bool lowPower, bool validation, bool syncValidation,
                   std::string& err) {
  if (!vkl::LoadGlobal(gfn_, err)) return false;

  // ---- layers/extensions ----
  std::vector<const char*> layers;
  std::vector<const char*> exts;

  // LAYER ENUMERATION. Two properties this loop must have, both learned here:
  //
  //  1. It requests exactly ONE layer, by exact name. Installing the LunarG SDK
  //     registered EIGHT explicit layers (api_dump, gfxreconstruct,
  //     synchronization2, monitor, screenshot, profiles, shader_object,
  //     validation) — enabling whatever enumerates would silently put an API
  //     dumper or a capture layer in the path of a determinism run. Only
  //     khronos_validation is ever asked for, and only when the toggle is on.
  //  2. It handles VK_INCOMPLETE. The two-call idiom races against a layer set
  //     that can change between the count call and the fill call, and with a
  //     dozen registered layers that is no longer hypothetical. On INCOMPLETE
  //     the vector holds a valid PREFIX, so scanning it is still correct — but
  //     the loop must not assume `layerCount` entries were written.
  uint32_t layerCount = 0;
  if (gfn_.EnumerateInstanceLayerProperties) {
    VkResult lr = gfn_.EnumerateInstanceLayerProperties(&layerCount, nullptr);
    if ((lr == VK_SUCCESS || lr == VK_INCOMPLETE) && layerCount) {
      std::vector<VkLayerProperties> props(layerCount);
      uint32_t got = layerCount;
      lr = gfn_.EnumerateInstanceLayerProperties(&got, props.data());
      if (lr == VK_SUCCESS || lr == VK_INCOMPLETE) {
        if (got > layerCount) got = layerCount;
        for (uint32_t i = 0; i < got; i++)
          if (std::strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation") == 0)
            caps_.validationAvailable = true;
      }
    }
  }
  // The debug messenger is how validation output reaches us at all, so a
  // validation run without VK_EXT_debug_utils is a validation run whose findings
  // go nowhere. Check the extension is actually present rather than assuming the
  // layer brings it.
  bool debugUtilsAvailable = false;
  if (gfn_.EnumerateInstanceExtensionProperties) {
    uint32_t n = 0;
    VkResult er = gfn_.EnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    if ((er == VK_SUCCESS || er == VK_INCOMPLETE) && n) {
      std::vector<VkExtensionProperties> eprops(n);
      uint32_t got = n;
      er = gfn_.EnumerateInstanceExtensionProperties(nullptr, &got, eprops.data());
      if (er == VK_SUCCESS || er == VK_INCOMPLETE) {
        if (got > n) got = n;
        for (uint32_t i = 0; i < got; i++)
          if (std::strcmp(eprops[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
            debugUtilsAvailable = true;
      }
    }
  }
  if (validation && caps_.validationAvailable) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
    if (debugUtilsAvailable) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    caps_.validationEnabled = true;
  }

  // Synchronization validation is the PRIMARY detector for a missing barrier
  // (barrier_graph §6.2's detection ladder puts it above cross-backend hash
  // equality and far above the sledgehammer A/B, because it reports a hazard
  // from the RECORDED COMMANDS without needing a divergence to actually occur).
  // Live since phase 3b, when the LunarG SDK was installed on this machine —
  // before that the layer did not enumerate and this toggle did nothing.
  VkValidationFeatureEnableEXT enables[] = {
      VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
  VkValidationFeaturesEXT valFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
  valFeatures.enabledValidationFeatureCount = 1;
  valFeatures.pEnabledValidationFeatures = enables;
  if (syncValidation && caps_.validationEnabled) caps_.syncValidationEnabled = true;

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "sandvox";
  app.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledLayerCount = (uint32_t)layers.size();
  ici.ppEnabledLayerNames = layers.data();
  ici.enabledExtensionCount = (uint32_t)exts.size();
  ici.ppEnabledExtensionNames = exts.data();
  if (caps_.syncValidationEnabled) ici.pNext = &valFeatures;

  VkResult r = gfn_.CreateInstance(&ici, nullptr, &instance_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateInstance failed: ") + vkl::ResultName(r);
    return false;
  }
  vkl::LoadInstance(gfn_, instance_, caps_.validationEnabled, ifn_);

  if (caps_.validationEnabled && ifn_.CreateDebugUtilsMessengerEXT) {
    VkDebugUtilsMessengerCreateInfoEXT mi{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = DebugCallback;
    mi.pUserData = this;
    ifn_.CreateDebugUtilsMessengerEXT(instance_, &mi, nullptr, &messenger_);
  }

  if (!PickPhysicalDevice(lowPower, err)) return false;
  QueryCaps();
  if (!CreateLogicalDevice(err)) return false;
  if (!InitAllocator(err)) return false;

  // Command pool. RESET_COMMAND_BUFFER because command buffers are recycled as
  // their fences retire.
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = queueFamily_;
  r = dfn_.CreateCommandPool(device_, &cpi, nullptr, &cmdPool_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateCommandPool failed: ") + vkl::ResultName(r);
    return false;
  }

  // Descriptor pool sized for the engine's ~25 pipelines across a handful of
  // bind groups, with headroom. Phase 3c can size this from the pass table.
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 64},
  };
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 128;
  dpi.poolSizeCount = (uint32_t)std::size(sizes);
  dpi.pPoolSizes = sizes;
  r = dfn_.CreateDescriptorPool(device_, &dpi, nullptr, &descPool_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateDescriptorPool failed: ") + vkl::ResultName(r);
    return false;
  }

  // The Class B staging ring: host-visible, coherent, persistently mapped.
  // Coherent means host writes are visible to the device at the next queue
  // submit with no explicit flush, so the common upload path needs no
  // HOST_WRITE barrier (barrier_graph §4.1).
  stagingRing_ = CreateBuffer(kStagingRingBytes,
                              rhi::BufferUsage::CopySrc | rhi::BufferUsage::MapWrite,
                              "stagingRing");
  if (!stagingRing_ || !stagingRing_->mapped) {
    err = "failed to create the persistently-mapped staging ring";
    return false;
  }
  return true;
}

bool Backend::PickPhysicalDevice(bool lowPower, std::string& err) {
  uint32_t n = 0;
  ifn_.EnumeratePhysicalDevices(instance_, &n, nullptr);
  if (n == 0) {
    err = "no Vulkan physical devices";
    return false;
  }
  std::vector<VkPhysicalDevice> devs(n);
  ifn_.EnumeratePhysicalDevices(instance_, &n, devs.data());

  // Prefer discrete, unless lowPower asked for the opposite. `--adapter low`
  // exists so the world hash can be compared across GPU vendors on one machine
  // (DESIGN.md risk 3, still open), so honouring it here is not cosmetic.
  VkPhysicalDevice best = VK_NULL_HANDLE;
  int bestScore = -1;
  for (VkPhysicalDevice d : devs) {
    VkPhysicalDeviceProperties p{};
    ifn_.GetPhysicalDeviceProperties(d, &p);
    bool discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    bool integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    // A compute queue is mandatory: this backend exists to run the CA.
    uint32_t qn = 0;
    ifn_.GetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    ifn_.GetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
    bool hasCompute = false;
    for (const auto& q : qs)
      if (q.queueFlags & VK_QUEUE_COMPUTE_BIT) hasCompute = true;
    if (!hasCompute) continue;

    int score = 1;
    if (lowPower) {
      if (integrated) score = 3;
      else if (!discrete) score = 2;
    } else {
      if (discrete) score = 3;
      else if (integrated) score = 2;
    }
    if (score > bestScore) {
      bestScore = score;
      best = d;
    }
  }
  if (!best) {
    err = "no Vulkan device exposes a compute queue";
    return false;
  }
  phys_ = best;

  uint32_t qn = 0;
  ifn_.GetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qs(qn);
  ifn_.GetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qs.data());
  // Prefer a family with both graphics and compute: v1 is single-queue by
  // design (barrier_graph §5.1), and phase 4 will want graphics on it too.
  queueFamily_ = UINT32_MAX;
  for (uint32_t i = 0; i < qn; i++) {
    if ((qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
        (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      queueFamily_ = i;
      break;
    }
  }
  if (queueFamily_ == UINT32_MAX)
    for (uint32_t i = 0; i < qn; i++)
      if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        queueFamily_ = i;
        break;
      }
  return true;
}

void Backend::QueryCaps() {
  VkPhysicalDeviceProperties p{};
  ifn_.GetPhysicalDeviceProperties(phys_, &p);
  caps_.deviceName = p.deviceName;
  caps_.apiVersion = p.apiVersion;
  caps_.driverVersion = p.driverVersion;
  caps_.vendorId = p.vendorID;
  caps_.deviceId = p.deviceID;
  caps_.discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

  const VkPhysicalDeviceLimits& L = p.limits;
  caps_.maxStorageBufferRange = L.maxStorageBufferRange;
  caps_.maxComputeWorkGroupInvocations = L.maxComputeWorkGroupInvocations;
  for (int i = 0; i < 3; i++) {
    caps_.maxComputeWorkGroupSize[i] = L.maxComputeWorkGroupSize[i];
    caps_.maxComputeWorkGroupCount[i] = L.maxComputeWorkGroupCount[i];
  }
  caps_.maxBoundDescriptorSets = L.maxBoundDescriptorSets;
  caps_.maxPerStageDescriptorStorageBuffers = L.maxPerStageDescriptorStorageBuffers;
  caps_.minStorageBufferOffsetAlignment = L.minStorageBufferOffsetAlignment;
  caps_.minUniformBufferOffsetAlignment = L.minUniformBufferOffsetAlignment;
  caps_.nonCoherentAtomSize = L.nonCoherentAtomSize;
  caps_.timestampQuery = L.timestampComputeAndGraphics != 0;
  caps_.timestampPeriodNs = L.timestampPeriod;

  // THE PHASE 7 GATE. residencyNonResidentStrict is a sparseProperties field,
  // not a feature bit, and it is the one that decides whether sparse residency
  // is usable at all here: without it, a read of an unbound page is undefined
  // rather than zero, and sim kernels read unbound pages by design.
  caps_.residencyNonResidentStrict = p.sparseProperties.residencyNonResidentStrict != 0;

  VkPhysicalDeviceFeatures f{};
  ifn_.GetPhysicalDeviceFeatures(phys_, &f);
  caps_.sparseBinding = f.sparseBinding != 0;
  caps_.sparseResidencyBuffer = f.sparseResidencyBuffer != 0;

  // maxMemoryAllocationSize is a Vulkan 1.1 (maintenance3) property.
  if (ifn_.GetPhysicalDeviceProperties2) {
    VkPhysicalDeviceMaintenance3Properties m3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &m3;
    ifn_.GetPhysicalDeviceProperties2(phys_, &p2);
    caps_.maxMemoryAllocationSize = m3.maxMemoryAllocationSize;
  }
}

bool Backend::CreateLogicalDevice(std::string& err) {
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  // Request only what is present. Sparse features are requested when available
  // so phase 7 does not need a device recreate; nothing uses them yet.
  VkPhysicalDeviceFeatures want{};
  want.sparseBinding = caps_.sparseBinding;
  want.sparseResidencyBuffer = caps_.sparseResidencyBuffer;

  // SYNCHRONIZATION2 IS MANDATORY FOR PHASE 3b, and asking for it is not
  // optional decoration: vkCmdPipelineBarrier2 is core in Vulkan 1.3, which
  // makes the entry point RESOLVE, but calling it on a device that never
  // enabled the feature is undefined behaviour. With no validation layer here
  // that is an access violation inside the ICD, not an error return — the same
  // class of failure that cost phase 3a a debugging session on pipeline
  // layouts. Query first, enable explicitly, and refuse to run without it.
  VkPhysicalDeviceVulkan13Features feat13{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  if (ifn_.GetPhysicalDeviceFeatures2) {
    VkPhysicalDeviceVulkan13Features probe{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &probe;
    ifn_.GetPhysicalDeviceFeatures2(phys_, &f2);
    caps_.synchronization2 = probe.synchronization2 != 0;
  }
  if (!caps_.synchronization2) {
    err =
        "device does not support VkPhysicalDeviceVulkan13Features::"
        "synchronization2, which the generated-barrier recorder requires "
        "(docs/vulkan_barrier_graph.md §3.2 is written in Flags2 scopes)";
    return false;
  }
  feat13.synchronization2 = VK_TRUE;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &want;
  dci.pNext = &feat13;

  VkResult r = ifn_.CreateDevice(phys_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateDevice failed: ") + vkl::ResultName(r);
    return false;
  }
  vkl::LoadDevice(ifn_, device_, dfn_);
  dfn_.GetDeviceQueue(device_, queueFamily_, 0, &queue_);
  if (!dfn_.CmdPipelineBarrier2) {
    err = "vkCmdPipelineBarrier2 did not resolve despite synchronization2";
    return false;
  }
  return true;
}

bool Backend::InitAllocator(std::string& err) {
  // VMA gets its entry points from OUR loader — the build disables both its
  // static and its dynamic lookup, so this table is the only way it can reach
  // Vulkan. One source of entry points, by construction.
  VmaVulkanFunctions vf{};
  vf.vkGetInstanceProcAddr = gfn_.GetInstanceProcAddr;
  vf.vkGetDeviceProcAddr = ifn_.GetDeviceProcAddr;
  vf.vkGetPhysicalDeviceProperties = ifn_.GetPhysicalDeviceProperties;
  vf.vkGetPhysicalDeviceMemoryProperties = ifn_.GetPhysicalDeviceMemoryProperties;
  vf.vkAllocateMemory = dfn_.AllocateMemory;
  vf.vkFreeMemory = dfn_.FreeMemory;
  vf.vkMapMemory = dfn_.MapMemory;
  vf.vkUnmapMemory = dfn_.UnmapMemory;
  vf.vkFlushMappedMemoryRanges = dfn_.FlushMappedMemoryRanges;
  vf.vkInvalidateMappedMemoryRanges = dfn_.InvalidateMappedMemoryRanges;
  vf.vkBindBufferMemory = dfn_.BindBufferMemory;
  vf.vkBindImageMemory = dfn_.BindImageMemory;
  vf.vkGetBufferMemoryRequirements = dfn_.GetBufferMemoryRequirements;
  vf.vkGetImageMemoryRequirements = dfn_.GetImageMemoryRequirements;
  vf.vkCreateBuffer = dfn_.CreateBuffer;
  vf.vkDestroyBuffer = dfn_.DestroyBuffer;
  vf.vkCreateImage = dfn_.CreateImage;
  vf.vkDestroyImage = dfn_.DestroyImage;
  vf.vkCmdCopyBuffer = dfn_.CmdCopyBuffer;
  vf.vkGetBufferMemoryRequirements2KHR = dfn_.GetBufferMemoryRequirements2;
  vf.vkGetImageMemoryRequirements2KHR = dfn_.GetImageMemoryRequirements2;
  vf.vkBindBufferMemory2KHR = dfn_.BindBufferMemory2;
  vf.vkBindImageMemory2KHR = dfn_.BindImageMemory2;

  VmaAllocatorCreateInfo aci{};
  aci.physicalDevice = phys_;
  aci.device = device_;
  aci.instance = instance_;
  aci.vulkanApiVersion = VK_API_VERSION_1_3;
  aci.pVulkanFunctions = &vf;

  VkResult r = vmaCreateAllocator(&aci, &allocator_);
  if (r != VK_SUCCESS) {
    err = std::string("vmaCreateAllocator failed: ") + vkl::ResultName(r);
    return false;
  }
  return true;
}

Buffer* Backend::CreateBuffer(uint64_t size, rhi::BufferUsage usage, const char* label) {
  auto b = std::make_unique<Buffer>();
  b->size = size;
  b->label = label ? label : "";

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  // UNCONDITIONAL TRANSFER_DST: it is what ZeroInitAll's vkCmdFillBuffer needs,
  // and it is free on device-local memory. This is the price of the zero-init
  // POLICY, and paying it everywhere is what makes the policy a mechanism
  // rather than a list somebody has to maintain correctly (barrier_graph §4.8).
  bci.usage = ToVkUsage(usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci{};
  const bool hostVisible =
      rhi::Any(usage, rhi::BufferUsage::MapRead | rhi::BufferUsage::MapWrite);
  if (hostVisible) {
    // Persistently mapped and coherent: the readback slots and the staging ring
    // both want a pointer that stays valid, and coherent memory needs no
    // explicit flush before a submit reads it.
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.flags |= rhi::Any(usage, rhi::BufferUsage::MapRead)
                     ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                     : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  } else {
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }

  VmaAllocationInfo info{};
  VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &b->buf, &b->alloc, &info);
  if (r != VK_SUCCESS) return nullptr;
  b->mapped = info.pMappedData;

  Buffer* raw = b.get();
  // The zero-init REGISTRY. Every buffer, no exceptions, no opt-out.
  buffers_.push_back(std::move(b));
  return raw;
}

bool Backend::ZeroInitAll(std::string& err) {
  VkCommandBuffer cmd = BeginCommands("zeroInitAll");
  if (!cmd) {
    err = "ZeroInitAll: could not begin a command buffer";
    return false;
  }
  // Iterate the REGISTRY, never a hand-written list. The barrier document's own
  // draft enumerated "the buffers that need zeroing", missed two of them, and
  // one of the misses (pArgsStage) would have fed garbage into an indirect draw
  // count on the first tick — a device hang. Anything created through
  // CreateBuffer is covered here whether or not anyone remembered it.
  for (const auto& b : buffers_) {
    if (!b->buf || b->size == 0) continue;
    dfn_.CmdFillBuffer(cmd, b->buf, 0, VK_WHOLE_SIZE, 0);
  }
  VkFence fence = SubmitCommands(cmd, err);
  if (fence == VK_NULL_HANDLE) return false;
  return WaitIdle(err);
}

void Backend::QueueWrite(Buffer* dst, uint64_t offset, const void* data, size_t size) {
  if (!dst || size == 0) return;
  Pending p;
  p.dst = dst;
  p.dstOffset = offset;
  p.size = size;

  // THE CLASS RULE, and it is exactly one rule: Class A iff the payload fits
  // vkCmdUpdateBuffer's 65536-byte limit AND its size is 4-aligned (the command
  // requires a 4-aligned offset and size). Nothing else. An earlier draft of the
  // design applied this inconsistently by buffer identity; a size-derived rule
  // with hand-made exceptions is one the next person applies wrong.
  p.classA = size <= kClassAMaxBytes && (size % 4) == 0 && (offset % 4) == 0;

  if (p.classA) {
    // vkCmdUpdateBuffer captures the data into the command buffer at RECORD
    // time, so holding a copy until the flush is all the lifetime management
    // needed — no staging allocation, no fence tracking. It is also why the
    // --shot far-fill loop is safe: each iteration records its own payload into
    // its own command buffer, so rapid overwrites of one small buffer cannot
    // alias.
    p.inlineData.resize(size);
    std::memcpy(p.inlineData.data(), data, size);
  } else {
    // Class B: memcpy into the persistently-mapped ring now, copy on the GPU at
    // flush time. The region is reclaimed by the fence of the submit that
    // consumes it — which is why §4.2 gives EVERY submit a fence.
    uint64_t off = AlignUp(stagingHead_, kStagingAlign);
    if (off + size > stagingRing_->size) off = 0;  // wrap
    std::memcpy((uint8_t*)stagingRing_->mapped + off, data, size);
    p.stagingOffset = off;
    stagingHead_ = off + size;
  }
  // ISSUE ORDER, preserved exactly. Never sorted, never coalesced: last write to
  // a range before a submit must win, and coalescing is what would break that.
  pending_.push_back(std::move(p));
}

void Backend::FlushUploads(VkCommandBuffer cmd) {
  for (const Pending& p : pending_) {
    if (p.classA) {
      dfn_.CmdUpdateBuffer(cmd, p.dst->buf, p.dstOffset, p.size, p.inlineData.data());
    } else {
      VkBufferCopy region{};
      region.srcOffset = p.stagingOffset;
      region.dstOffset = p.dstOffset;
      region.size = p.size;
      dfn_.CmdCopyBuffer(cmd, stagingRing_->buf, p.dst->buf, 1, &region);
    }
  }
  pending_.clear();
}

VkFence Backend::AcquireFence(std::string& err) {
  if (!freeFences_.empty()) {
    VkFence f = freeFences_.back();
    freeFences_.pop_back();
    dfn_.ResetFences(device_, 1, &f);
    return f;
  }
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence f = VK_NULL_HANDLE;
  VkResult r = dfn_.CreateFence(device_, &fci, nullptr, &f);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateFence failed: ") + vkl::ResultName(r);
    return VK_NULL_HANDLE;
  }
  return f;
}

VkCommandBuffer Backend::BeginCommands(const char* /*label*/) {
  PollFences();  // recycle anything already finished
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = cmdPool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (dfn_.AllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn_.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;

  // Uploads flush at the HEAD of whichever command buffer is recorded next,
  // from whichever code path records it. That is what reproduces WebGPU's
  // "deferred to the start of the next submit" — including the case where the
  // path that issued the writes submits nothing at all (Stream::FillSlots when
  // every slot hits the store), whose writes then belong to the next tick.
  FlushUploads(cmd);
  return cmd;
}

VkFence Backend::SubmitCommands(VkCommandBuffer cmd, std::string& err) {
  if (dfn_.EndCommandBuffer(cmd) != VK_SUCCESS) {
    err = "vkEndCommandBuffer failed";
    return VK_NULL_HANDLE;
  }
  VkFence fence = AcquireFence(err);
  if (fence == VK_NULL_HANDLE) return VK_NULL_HANDLE;

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  // EVERY submit carries a fence. Not for the readback — for the staging ring:
  // a fenceless submit can never retire, so its ring region is never reclaimed
  // and a loop that submits without reading back (the --shot far-fill loop)
  // exhausts the ring and deadlocks.
  VkResult r = dfn_.QueueSubmit(queue_, 1, &si, fence);
  if (r != VK_SUCCESS) {
    err = std::string("vkQueueSubmit failed: ") + vkl::ResultName(r);
    return VK_NULL_HANDLE;
  }
  inFlight_.push_back({fence, cmd, stagingHead_});
  return fence;
}

void Backend::PollFences() {
  for (size_t i = 0; i < inFlight_.size();) {
    if (dfn_.GetFenceStatus(device_, inFlight_[i].fence) == VK_SUCCESS) {
      VkFence f = inFlight_[i].fence;
      dfn_.FreeCommandBuffers(device_, cmdPool_, 1, &inFlight_[i].cmd);
      // A RETAINED fence must not go back to the pool: a borrower (a readback
      // slot, an eviction batch) still holds the handle and still needs
      // vkGetFenceStatus on it to mean THIS submit. Park it until the last
      // ReleaseFence. See the RetainFence comment in rhi_vulkan.h for what
      // recycling it under a borrower actually corrupts.
      auto it = fenceRetain_.find(f);
      if (it != fenceRetain_.end() && it->second > 0)
        retiredRetained_.push_back(f);
      else
        freeFences_.push_back(f);
      inFlight_.erase(inFlight_.begin() + i);
    } else {
      i++;
    }
  }
}

void Backend::RetainFence(VkFence f) {
  if (f == VK_NULL_HANDLE) return;
  fenceRetain_[f]++;
}

void Backend::ReleaseFence(VkFence f) {
  if (f == VK_NULL_HANDLE) return;
  auto it = fenceRetain_.find(f);
  if (it == fenceRetain_.end()) return;
  if (it->second > 0) it->second--;
  if (it->second != 0) return;
  fenceRetain_.erase(it);
  // If the submit already retired while retained, the fence was parked rather
  // than pooled; hand it back now that nobody holds it.
  for (size_t i = 0; i < retiredRetained_.size(); i++) {
    if (retiredRetained_[i] == f) {
      retiredRetained_.erase(retiredRetained_.begin() + i);
      freeFences_.push_back(f);
      return;
    }
  }
}

VkResult Backend::FenceStatus(VkFence f) const {
  if (f == VK_NULL_HANDLE) return VK_ERROR_UNKNOWN;
  return dfn_.GetFenceStatus(device_, f);
}

bool Backend::WaitFence(VkFence f, std::string& err) {
  if (f == VK_NULL_HANDLE) return true;
  VkResult r = dfn_.WaitForFences(device_, 1, &f, VK_TRUE, UINT64_MAX);
  if (r != VK_SUCCESS) {
    err = std::string("vkWaitForFences failed: ") + vkl::ResultName(r);
    return false;
  }
  PollFences();
  return true;
}

bool Backend::WaitIdle(std::string& err) {
  // vkQueueWaitIdle rather than vkDeviceWaitIdle: equivalent on the single
  // queue v1 uses, but it will not silently widen into a whole-device drain if
  // phase 8 adds an async queue.
  VkResult r = dfn_.QueueWaitIdle(queue_);
  if (r != VK_SUCCESS) {
    err = std::string("vkQueueWaitIdle failed: ") + vkl::ResultName(r);
    return false;
  }
  PollFences();
  return true;
}

VkShaderModule Backend::GetShaderModule(const std::string& wgsl, const std::string& label,
                                        const std::string& entryPoint,
                                        uint32_t bodyLineOffset,
                                        std::string& diagnostics) {
  // Cache by (label, entry point, source hash). The entry point is part of the
  // key because Tint emits a SINGLE-entry-point module: the engine builds
  // several pipelines from one .wgsl file (worldgen.wgsl alone yields main /
  // list / far / fardown), and those are genuinely different SPIR-V modules.
  std::string key = label + "\x1f" + entryPoint + "\x1f" +
                    std::to_string(std::hash<std::string>{}(wgsl));
  auto it = moduleCache_.find(key);
  if (it != moduleCache_.end()) return it->second;

  vkspv::CompileResult cr = vkspv::Compile(wgsl, label, entryPoint, bodyLineOffset);
  diagnostics = cr.diagnostics;
  if (!cr.ok) return VK_NULL_HANDLE;

  VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sci.codeSize = cr.spirv.size() * sizeof(uint32_t);
  sci.pCode = cr.spirv.data();
  VkShaderModule m = VK_NULL_HANDLE;
  if (dfn_.CreateShaderModule(device_, &sci, nullptr, &m) != VK_SUCCESS) {
    diagnostics += "vkCreateShaderModule rejected the SPIR-V Tint produced\n";
    return VK_NULL_HANDLE;
  }
  moduleCache_[key] = m;
  return m;
}

VkDescriptorSetLayout Backend::CreateSetLayout(const rhi::BindGroupLayoutEntry* entries,
                                               size_t count) {
  std::vector<VkDescriptorSetLayoutBinding> bindings(count);
  for (size_t i = 0; i < count; i++) {
    bindings[i].binding = entries[i].binding;
    bindings[i].descriptorType =
        ToVkDescriptorType(entries[i].type, entries[i].hasDynamicOffset);
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = ToVkStages(entries[i].visibility);
  }
  VkDescriptorSetLayoutCreateInfo ci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ci.bindingCount = (uint32_t)bindings.size();
  ci.pBindings = bindings.data();
  VkDescriptorSetLayout l = VK_NULL_HANDLE;
  if (dfn_.CreateDescriptorSetLayout(device_, &ci, nullptr, &l) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  setLayouts_.push_back(l);
  return l;
}

VkPipelineLayout Backend::CreatePipelineLayout(const VkDescriptorSetLayout* sets,
                                               size_t count) {
  VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ci.setLayoutCount = (uint32_t)count;
  ci.pSetLayouts = sets;
  VkPipelineLayout l = VK_NULL_HANDLE;
  if (dfn_.CreatePipelineLayout(device_, &ci, nullptr, &l) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  pipeLayouts_.push_back(l);
  return l;
}

VkPipeline Backend::CreateComputePipeline(VkPipelineLayout layout, VkShaderModule module,
                                          const char* entry, const char* /*label*/) {
  if (!dfn_.CreateComputePipelines || layout == VK_NULL_HANDLE ||
      module == VK_NULL_HANDLE || !entry)
    return VK_NULL_HANDLE;
  VkPipelineShaderStageCreateInfo stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = module;
  stage.pName = entry;

  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = stage;
  ci.layout = layout;

  VkPipeline p = VK_NULL_HANDLE;
  if (dfn_.CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p) !=
      VK_SUCCESS)
    return VK_NULL_HANDLE;
  pipelines_.push_back(p);
  return p;
}

VkDescriptorSet Backend::CreateDescriptorSet(VkDescriptorSetLayout layout,
                                             const rhi::BindGroupLayoutEntry* layoutEntries,
                                             const rhi::BindGroupEntry* entries,
                                             size_t count,
                                             const std::vector<Buffer*>& buffers) {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = descPool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn_.AllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS)
    return VK_NULL_HANDLE;

  std::vector<VkDescriptorBufferInfo> infos(count);
  std::vector<VkWriteDescriptorSet> writes(count);
  for (size_t i = 0; i < count; i++) {
    Buffer* b = i < buffers.size() ? buffers[i] : nullptr;
    infos[i].buffer = b ? b->buf : VK_NULL_HANDLE;
    infos[i].offset = entries[i].offset;
    // SIZE 0 MEANS "the rest of the buffer from offset" — a wgpu semantic the
    // seam preserves, and the reason Buffer caches its size at all. Vulkan
    // spells the same thing VK_WHOLE_SIZE, but resolving it explicitly keeps
    // the descriptor honest about how many bytes it actually covers.
    infos[i].range = entries[i].size ? entries[i].size
                     : (b ? b->size - entries[i].offset : VK_WHOLE_SIZE);

    // THE DESCRIPTOR TYPE COMES FROM THE LAYOUT, matched by binding number
    // rather than by array position — the two arrays are written independently
    // at every call site and nothing guarantees they are ordered alike.
    // Hardcoding STORAGE_BUFFER here (which this did until phase 3b) is
    // undefined behaviour for every uniform binding, and with no validation
    // layer it does not error: it corrupts the descriptor and faults later, in
    // a dispatch, a long way from the cause.
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (size_t j = 0; j < count; j++) {
      if (layoutEntries[j].binding == entries[i].binding) {
        type = ToVkDescriptorType(layoutEntries[j].type, layoutEntries[j].hasDynamicOffset);
        break;
      }
    }

    writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = entries[i].binding;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = type;
    writes[i].pBufferInfo = &infos[i];
  }
  dfn_.UpdateDescriptorSets(device_, (uint32_t)writes.size(), writes.data(), 0, nullptr);
  return set;
}

void Backend::PushValidationScope() {
  validationScopeOpen_ = true;
  validationScopeMark_ = validationMsgs_.size();
}

bool Backend::PopValidationScope(std::string& messages) {
  if (!validationScopeOpen_) return false;
  validationScopeOpen_ = false;
  bool any = validationMsgs_.size() > validationScopeMark_;
  for (size_t i = validationScopeMark_; i < validationMsgs_.size(); i++)
    messages += validationMsgs_[i] + "\n";
  return any;
}

void Backend::Shutdown() {
  if (!device_) {
    if (instance_ && ifn_.DestroyInstance) {
      ifn_.DestroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
    }
    return;
  }
  std::string err;
  WaitIdle(err);

  for (VkPipeline p : pipelines_) dfn_.DestroyPipeline(device_, p, nullptr);
  pipelines_.clear();
  for (VkPipelineLayout l : pipeLayouts_) dfn_.DestroyPipelineLayout(device_, l, nullptr);
  pipeLayouts_.clear();
  for (VkDescriptorSetLayout l : setLayouts_)
    dfn_.DestroyDescriptorSetLayout(device_, l, nullptr);
  setLayouts_.clear();
  for (auto& kv : moduleCache_) dfn_.DestroyShaderModule(device_, kv.second, nullptr);
  moduleCache_.clear();

  for (auto& f : inFlight_) {
    dfn_.FreeCommandBuffers(device_, cmdPool_, 1, &f.cmd);
    dfn_.DestroyFence(device_, f.fence, nullptr);
  }
  inFlight_.clear();
  for (VkFence f : freeFences_) dfn_.DestroyFence(device_, f, nullptr);
  freeFences_.clear();
  // Fences whose submit retired while a borrower still held them. WaitIdle
  // above drained the queue, so these are all signalled and unreferenced by the
  // GPU; the borrowers are going away with the backend.
  for (VkFence f : retiredRetained_) dfn_.DestroyFence(device_, f, nullptr);
  retiredRetained_.clear();
  fenceRetain_.clear();

  if (descPool_) dfn_.DestroyDescriptorPool(device_, descPool_, nullptr);
  descPool_ = VK_NULL_HANDLE;
  if (cmdPool_) dfn_.DestroyCommandPool(device_, cmdPool_, nullptr);
  cmdPool_ = VK_NULL_HANDLE;

  for (auto& b : buffers_)
    if (b->buf) vmaDestroyBuffer(allocator_, b->buf, b->alloc);
  buffers_.clear();
  stagingRing_ = nullptr;

  if (allocator_) vmaDestroyAllocator(allocator_);
  allocator_ = nullptr;

  dfn_.DestroyDevice(device_, nullptr);
  device_ = VK_NULL_HANDLE;

  if (messenger_ && ifn_.DestroyDebugUtilsMessengerEXT)
    ifn_.DestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
  messenger_ = VK_NULL_HANDLE;
  if (instance_) ifn_.DestroyInstance(instance_, nullptr);
  instance_ = VK_NULL_HANDLE;
}

}  // namespace vk
