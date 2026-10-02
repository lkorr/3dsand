#include "gpu/rhi_vulkan.h"

#include <algorithm>  // std::sort — DecoratedBindings
#include <atomic>
#include <cctype>   // std::tolower / std::isdigit — SANDVOX_DEVICE matching
#include <chrono>
#include <cstdio>
#include <cstdlib>  // std::abort — the staging ring's unserviceable-write path
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>  // std::make_move_iterator — AbandonCommands re-queue
#include <thread>    // SPIR-V cache temp name; PruneShaderCache's background sweep

// The pipeline cache's temp-file name carries the PID: SANDVOX_PIPELINE_CACHE
// lets several processes share one cache file, and two of them renaming the
// same "<path>.tmp" would hand the survivor a half-written blob.
#ifdef _WIN32
#include <process.h>  // _getpid
#define SANDVOX_GETPID _getpid()
#else
#include <unistd.h>  // getpid
#define SANDVOX_GETPID getpid()
#endif

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
static_assert((uint64_t)kMaxDebugBoxes * sizeof(DebugBox) <= kVkUpdateBufferLimit,
              "debugBoxes exceeded vkCmdUpdateBuffer's 65536-byte limit: it was "
              "EXACTLY at it. Either lower kMaxDebugBoxes or move debugBoxes to "
              "the Class B staging path (barrier_graph 4.1).");
static_assert((uint64_t)kMaterialSlots * sizeof(MicroBrickGpu) <= kVkUpdateBufferLimit,
              "microTableBuf_ exceeded vkCmdUpdateBuffer's 65536-byte limit: it "
              "was EXACTLY at it. Either lower kMaterialSlots or move it to the "
              "Class B staging path (barrier_graph 4.1).");

namespace {

// Sized for the worst UNSUBMITTED batch, which is the only quantity that
// matters: ring space is reclaimed by the fence of the submit that consumed it,
// so bytes queued since the last submit can never be reclaimed no matter how
// long the allocator waits.
//
// The worst batch is a fully-revisited window-shift plane: Stream::FillSlots'
// store-hit branch refills up to 1,024 slots with a 16 KiB voxel page each and
// SUBMITS NOTHING (the gen branch is what submits, and a plane over already
// visited terrain has no gen slots), so 16 MiB of Class B payload accumulates
// before anything can retire. The old 16 MiB ring was exactly that figure, plus
// the ordinary tick traffic the doc enumerates (cellOps 512 KiB, spawnOps
// 128 KiB, bodyInstances 4 MiB, two 4 MiB pool buffers on a hot reload) — i.e.
// it would have run out on the first revisit. 64 MiB is 4x the worst batch and
// costs 64 MiB of host RAM that is only touched when written.
constexpr uint64_t kStagingRingBytes = 64ull * 1024 * 1024;

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

// ---- phase 4b: render-path mappings ---------------------------------------

VkFormat ToVkFormat(rhi::TextureFormat f) {
  switch (f) {
    case rhi::TextureFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::TextureFormat::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case rhi::TextureFormat::Depth32Float: return VK_FORMAT_D32_SFLOAT;
    case rhi::TextureFormat::Undefined: break;
  }
  return VK_FORMAT_UNDEFINED;
}

VkCompareOp ToVkCompare(rhi::CompareFunction f) {
  switch (f) {
    case rhi::CompareFunction::Never: return VK_COMPARE_OP_NEVER;
    case rhi::CompareFunction::Less: return VK_COMPARE_OP_LESS;
    case rhi::CompareFunction::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case rhi::CompareFunction::Greater: return VK_COMPARE_OP_GREATER;
    case rhi::CompareFunction::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case rhi::CompareFunction::Equal: return VK_COMPARE_OP_EQUAL;
    case rhi::CompareFunction::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case rhi::CompareFunction::Always: return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_ALWAYS;
}

VkCullModeFlags ToVkCull(rhi::CullMode m) {
  switch (m) {
    case rhi::CullMode::None: return VK_CULL_MODE_NONE;
    case rhi::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case rhi::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
  }
  return VK_CULL_MODE_NONE;
}

VkPrimitiveTopology ToVkTopology(rhi::PrimitiveTopology t) {
  switch (t) {
    case rhi::PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case rhi::PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case rhi::PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case rhi::PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case rhi::PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  }
  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkBlendFactor ToVkBlendFactor(rhi::BlendFactor f) {
  switch (f) {
    case rhi::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case rhi::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case rhi::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case rhi::BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case rhi::BlendFactor::Src: return VK_BLEND_FACTOR_SRC_COLOR;
    case rhi::BlendFactor::OneMinusSrc: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case rhi::BlendFactor::Dst: return VK_BLEND_FACTOR_DST_COLOR;
    case rhi::BlendFactor::OneMinusDst: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToVkBlendOp(rhi::BlendOperation o) {
  switch (o) {
    case rhi::BlendOperation::Add: return VK_BLEND_OP_ADD;
    case rhi::BlendOperation::Subtract: return VK_BLEND_OP_SUBTRACT;
    case rhi::BlendOperation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case rhi::BlendOperation::Min: return VK_BLEND_OP_MIN;
    case rhi::BlendOperation::Max: return VK_BLEND_OP_MAX;
  }
  return VK_BLEND_OP_ADD;
}

VkShaderStageFlags ToVkStages(rhi::ShaderStage s) {
  VkShaderStageFlags f = 0;
  if ((uint32_t)s & (uint32_t)rhi::ShaderStage::Vertex) f |= VK_SHADER_STAGE_VERTEX_BIT;
  if ((uint32_t)s & (uint32_t)rhi::ShaderStage::Fragment) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if ((uint32_t)s & (uint32_t)rhi::ShaderStage::Compute) f |= VK_SHADER_STAGE_COMPUTE_BIT;
  return f;
}

// Every (set << 16 | binding) a SPIR-V module decorates, sorted. Only a variable
// carrying BOTH decorations is a descriptor; Tint's single-entry-point output
// contains only the globals the entry point reaches. A module too malformed to
// walk yields what was read before the fault: the check it feeds can only
// refuse a pipeline over a binding the SPIR-V really names.
static std::vector<uint32_t> DecoratedBindings(const std::vector<uint32_t>& spirv) {
  constexpr uint32_t kOpDecorate = 71, kDecBinding = 33, kDecDescriptorSet = 34;
  std::unordered_map<uint32_t, uint32_t> set, binding;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffffu;
    if (count == 0 || i + count > spirv.size()) break;
    if (op == kOpDecorate && count >= 4) {
      if (spirv[i + 2] == kDecDescriptorSet) set[spirv[i + 1]] = spirv[i + 3];
      if (spirv[i + 2] == kDecBinding) binding[spirv[i + 1]] = spirv[i + 3];
    }
    i += count;
  }
  std::vector<uint32_t> out;
  for (const auto& [id, b] : binding) {
    auto s = set.find(id);
    if (s != set.end()) out.push_back((s->second << 16) | (b & 0xffffu));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// The shader_cache/ LRU sweep (kShaderCacheMaxAgeDays). Runs once per process
// on a detached thread started by the first GetShaderModule, so the
// directory walk — tens of thousands of entries on a machine that has been
// editing shaders for a month — never sits on a frame or a pipeline build.
//
// Only `*.spv` files older than the cutoff are touched. Racing another process
// is harmless in every direction: an entry deleted here just before someone
// reads it is a cache miss for them (Tint recompiles and rewrites it), an
// entry that process just wrote or touched is newer than the cutoff, and a
// delete that fails because the file is open is skipped.
void PruneShaderCache(std::filesystem::path dir) {
  namespace fs = std::filesystem;
  const auto cutoff = fs::file_time_type::clock::now() -
                      std::chrono::hours(24 * kShaderCacheMaxAgeDays);
  uint64_t removed = 0, bytes = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::directory_entry& e = *it;
    std::error_code fec;
    if (!e.is_regular_file(fec) || e.path().extension() != ".spv") continue;
    const auto t = e.last_write_time(fec);
    if (fec || t >= cutoff) continue;
    const uintmax_t sz = e.file_size(fec);
    if (fs::remove(e.path(), fec)) {
      removed++;
      bytes += fec ? 0 : (uint64_t)sz;
    }
  }
  if (removed)
    std::fprintf(stderr,
                 "shader_cache: pruned %llu entries (%.1f MiB) unused for %d+ "
                 "days (%s)\n",
                 (unsigned long long)removed, (double)bytes / (1024.0 * 1024.0),
                 kShaderCacheMaxAgeDays, dir.string().c_str());
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
  if (be) {
    // The layers call back on whatever thread provoked the message, and since
    // pipeline creation was threaded (docs/PLAN_shader_compile.md package A)
    // that is no longer always the main one.
    std::lock_guard<std::mutex> lock(be->validationMutex_);
    be->validationMsgs_.push_back(data->pMessage);
  }
  // SANDVOX_VK_VALIDATION_ECHO=1: print as it arrives, not only at the end. A
  // run that dies (a driver's heap corruption is a fail-fast no handler sees)
  // never reaches the end-of-run report, and the message that explains the
  // death is exactly the one that would be lost.
  static const bool kEcho = [] {
    const char* e = std::getenv("SANDVOX_VK_VALIDATION_ECHO");
    return e && *e == '1';
  }();
  if (kEcho) {
    std::fprintf(stderr, "[vk-validation live] %s\n", data->pMessage);
    std::fflush(stderr);
  }
  return VK_FALSE;  // never abort the call
}

Backend::~Backend() { Shutdown(); }

bool Backend::Init(bool lowPower, bool validation, bool syncValidation,
                   std::string& err, const char* const* instanceExts,
                   uint32_t instanceExtCount, bool wantSwapchain) {
  if (!vkl::LoadGlobal(gfn_, err)) return false;
  swapchainRequested_ = wantSwapchain;

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
  // Windowed (phase 4b D3): the caller's surface extensions — GLFW's
  // VK_KHR_surface + VK_KHR_win32_surface on this platform.
  for (uint32_t i = 0; i < instanceExtCount; i++) exts.push_back(instanceExts[i]);

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

  // Descriptor pool. THIS IS A BUDGET THAT IS NEVER REFILLED: sets are
  // allocated by CreateDescriptorSet and never freed (no FREE_DESCRIPTOR_SET
  // bit, no reset), so every bind-group rebuild over a process lifetime --
  // and a full --selftest rebuilds the sim groups many times as gates
  // re-create the world -- draws it down. It used to be 512 storage
  // descriptors / 128 sets, "with headroom", and the allocation failure
  // below returned VK_NULL_HANDLE instead of saying so, which would have
  // surfaced as an access violation inside the driver at the first bind,
  // gates away from the cause. Found while chasing exactly such a crash on
  // 2026-09-04 (adding the worldMap binding, 31); that crash turned out to be
  // a stale cross-worktree sccache object with the OLD pass::Buf layout, NOT
  // this pool -- but the budget was one binding-per-set from the same
  // symptom, so it is sized generously now (descriptors are bytes) and the
  // failure path aborts with a count. The sizes are the named kDescPool*
  // constants in rhi_vulkan.h; deriving them from the pass table would be
  // tighter, but whatever sizes it must keep the abort.
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescPoolStorageBuffers},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescPoolUniformBuffers},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kDescPoolUniformDynamic},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, kDescPoolStorageDynamic},
  };
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = kDescPoolMaxSets;
  dpi.poolSizeCount = (uint32_t)std::size(sizes);
  dpi.pPoolSizes = sizes;
  r = dfn_.CreateDescriptorPool(device_, &dpi, nullptr, &descPool_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateDescriptorPool failed: ") + vkl::ResultName(r);
    return false;
  }

  // Pipeline cache: driver-compiled ISA cached across runs so startup skips
  // both Tint WGSL->SPIR-V and the driver's own compilation on subsequent
  // launches with unchanged shaders.
  //
  // SANDVOX_PIPELINE_CACHE overrides the path (absolute, or relative to the
  // CWD). The default is CWD-relative, so every worktree, every fresh run
  // directory and every `cd` paid worldgen's multi-minute compile from cold
  // (docs/PLAN_shader_compile.md item 4). Pointed at one shared file, several
  // processes may write it concurrently: the save is temp+rename, and the
  // blob a loser overwrites is a SUPERSET of what it loaded, so last writer
  // wins and nobody reads a torn file.
  {
    if (const char* env = std::getenv("SANDVOX_PIPELINE_CACHE"))
      pipelineCachePath_ = env;
    else
      pipelineCachePath_ = "sandvox_pipeline_cache.bin";
    // AN EXPLICITLY SELECTED DEVICE GETS ITS OWN CACHE FILE. A blob is valid
    // only for the pipelineCacheUUID that wrote it; another ICD rejects it (one
    // cold compile) and then — the expensive half — writes ITS blob over the
    // shared file at shutdown, so the next native run pays the multi-minute
    // worldgen compile again. Cross-vendor runs (SANDVOX_DEVICE) are exactly
    // the ones that point a second implementation at the same machine-global
    // path run.sh exports, so they are suffixed with the cache UUID.
    if (const char* sel = std::getenv("SANDVOX_DEVICE"); sel && *sel)
      pipelineCachePath_ += "." + caps_.pipelineCacheUuid.substr(0, 12);
    std::vector<uint8_t> blob;
    if (FILE* f = std::fopen(pipelineCachePath_.c_str(), "rb")) {
      // _ftelli64, not ftell: the blob has been measured at 394 MB and a long
      // is 32 bits on this platform, so the cap below must not be judged on a
      // value that wraps at 2 GiB.
#ifdef _WIN32
      _fseeki64(f, 0, SEEK_END);
      const long long sz = _ftelli64(f);
#else
      std::fseek(f, 0, SEEK_END);
      const long long sz = std::ftell(f);
#endif
      if (sz > (long long)kPipelineCacheMaxBytes) {
        // START FRESH (kPipelineCacheMaxBytes says why). The empty cache
        // misses on every create, which marks it dirty, so this launch's
        // live set is what gets written back.
        std::fprintf(stderr,
                     "pipeline cache %s is %.0f MiB, over the %llu MiB cap: "
                     "starting fresh (one cold compile, then it holds only "
                     "what is used)\n",
                     pipelineCachePath_.c_str(), (double)sz / (1024.0 * 1024.0),
                     (unsigned long long)(kPipelineCacheMaxBytes >> 20));
      } else if (sz > 0) {
        blob.resize((size_t)sz);
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_SET);
#else
        std::fseek(f, 0, SEEK_SET);
#endif
        size_t got = std::fread(blob.data(), 1, blob.size(), f);
        if (got != blob.size()) blob.clear();
      }
      std::fclose(f);
    }
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pcci.initialDataSize = blob.size();
    pcci.pInitialData = blob.empty() ? nullptr : blob.data();
    r = dfn_.CreatePipelineCache(device_, &pcci, nullptr, &pipelineCache_);
    if (r != VK_SUCCESS) {
      // Non-fatal: fall back to uncached pipelines.
      std::fprintf(stderr, "pipeline cache creation failed (%s), proceeding uncached\n",
                   vkl::ResultName(r));
      pipelineCache_ = VK_NULL_HANDLE;
    }
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
  devs.resize(n);
  caps_.deviceCount = n;
  const std::vector<VkPhysicalDevice> allDevs = devs;

  // ---- EXPLICIT DEVICE SELECTION: SANDVOX_DEVICE (`--device` sets it) ------
  //
  // Cross-vendor determinism (DESIGN.md §14 risk 3) is checked by running the
  // SAME build on a different Vulkan implementation, and on one machine that
  // means picking among several ICDs: the native driver, a CPU rasteriser
  // (Mesa lavapipe/llvmpipe), Dozen (Vulkan over D3D12, which compiles
  // SPIR-V -> NIR -> DXIL -> the vendor's D3D compiler). The score below can
  // only express "discrete first"; this names one. An index (enumeration
  // order, printed below) or a case-insensitive substring of the device name
  // or driver name ("llvmpipe", "dozen", "basic render"). A selector that
  // matches nothing is FATAL, never a silent fall back to the default device:
  // a fingerprint recorded on the wrong device is a false "match".
  //
  // An environment variable rather than an Init parameter because every
  // headless mode (selftest, smokes, --verify, --voxserve, --fingerprint)
  // constructs its own GpuContext, and the variable reaches all of them
  // through this one function.
  auto lower = [](std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
  };
  auto describe = [&](VkPhysicalDevice d, std::string* drvName) {
    VkPhysicalDeviceProperties p{};
    ifn_.GetPhysicalDeviceProperties(d, &p);
    std::string drv;
    if (ifn_.GetPhysicalDeviceProperties2 && p.apiVersion >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      p2.pNext = &dp;
      ifn_.GetPhysicalDeviceProperties2(d, &p2);
      drv = std::string(dp.driverName) + " " + dp.driverInfo;
    }
    if (drvName) *drvName = drv;
    return std::string(p.deviceName);
  };
  const char* sel = std::getenv("SANDVOX_DEVICE");
  if (sel && *sel) {
    std::printf("vk devices (%u):\n", n);
    for (uint32_t i = 0; i < n; i++) {
      std::string drv;
      const std::string name = describe(devs[i], &drv);
      std::printf("  [%u] %s | %s\n", i, name.c_str(), drv.c_str());
    }
    const std::string want = lower(sel);
    bool numeric = !want.empty();
    for (char ch : want)
      if (!std::isdigit((unsigned char)ch)) numeric = false;
    VkPhysicalDevice pick = VK_NULL_HANDLE;
    uint32_t pickIdx = 0;
    if (numeric) {
      const uint32_t idx = (uint32_t)std::strtoul(want.c_str(), nullptr, 10);
      if (idx < n) { pick = devs[idx]; pickIdx = idx; }
    } else {
      for (uint32_t i = 0; i < n && !pick; i++) {
        std::string drv;
        const std::string name = lower(describe(devs[i], &drv));
        if (name.find(want) != std::string::npos ||
            lower(drv).find(want) != std::string::npos) {
          pick = devs[i];
          pickIdx = i;
        }
      }
    }
    if (!pick) {
      err = std::string("SANDVOX_DEVICE / --device '") + sel +
            "' matches no Vulkan physical device (list above). A CPU or "
            "second ICD is added per-process with VK_ADD_DRIVER_FILES=<icd.json>";
      return false;
    }
    std::printf("vk device: SELECTED [%u] by '%s'\n", pickIdx, sel);
    devs = {pick};
    caps_.deviceIndex = pickIdx;
  }

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
  for (uint32_t i = 0; i < (uint32_t)allDevs.size(); i++)
    if (allDevs[i] == best) caps_.deviceIndex = i;

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
  switch (p.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: caps_.deviceType = "discrete"; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: caps_.deviceType = "integrated"; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: caps_.deviceType = "virtual"; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: caps_.deviceType = "cpu"; break;
    default: caps_.deviceType = "other"; break;
  }
  {
    static const char kHex[] = "0123456789abcdef";
    caps_.pipelineCacheUuid.clear();
    for (uint8_t b : p.pipelineCacheUUID) {
      caps_.pipelineCacheUuid += kHex[b >> 4];
      caps_.pipelineCacheUuid += kHex[b & 15];
    }
  }
  if (ifn_.GetPhysicalDeviceProperties2 && p.apiVersion >= VK_API_VERSION_1_2) {
    VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &dp;
    ifn_.GetPhysicalDeviceProperties2(phys_, &p2);
    caps_.driverName = dp.driverName;
    caps_.driverInfo = dp.driverInfo;
    caps_.driverId = (uint32_t)dp.driverID;
  }

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
  caps_.fragmentStoresAndAtomics = f.fragmentStoresAndAtomics != 0;
  caps_.robustBufferAccessAvailable = f.robustBufferAccess != 0;

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
  // ---- QUEUES: the main queue, and ON REQUEST an ASYNC COMPUTE queue
  // (docs/PLAN_async_compute.md). The main family was chosen in
  // PickPhysicalDevice (graphics+compute). The async queue is created ONLY when
  // asked for (Backend::SetAsyncQueueWanted — main.cpp passes
  // render.asyncCompute at boot — or SANDVOX_ASYNC_COMPUTE=1, or
  // SANDVOX_ASYNC_QUEUE=family|same, or an `on` arm in SANDVOX_PERF_ASYNC_ARMS):
  // MEASURED, merely creating a second queue on the RTX 3060 Ti costs the
  // village-fire scenario ~3.4 ms a frame with nothing ever submitted to it
  // (28.3 -> 31.7 ms p50; forestfire unaffected). A compute-only family is
  // preferred (NVIDIA family 2, AMD's ACE queues), else a second queue of the
  // main family. No async queue (or no device support: lavapipe, WARP) = the
  // single-queue schedule the engine has always had.
  uint32_t qn = 0;
  ifn_.GetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qs(qn);
  ifn_.GetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qs.data());
  asyncFamily_ = UINT32_MAX;
  asyncIndex_ = 0;
  // SANDVOX_ASYNC_QUEUE=same: take the second queue of the MAIN family even
  // when a compute-only family exists (the A/B for which hardware path the
  // overlap rides; a same-family queue also keeps every buffer EXCLUSIVE).
  // =none: create no async queue at all.
  const char* aq = std::getenv("SANDVOX_ASYNC_QUEUE");
  const bool aqSame = aq && std::strcmp(aq, "same") == 0;
  const bool aqFamily = aq && std::strcmp(aq, "family") == 0;
  const char* ac = std::getenv("SANDVOX_ASYNC_COMPUTE");
  const char* arms = std::getenv("SANDVOX_PERF_ASYNC_ARMS");
  const bool wanted = asyncQueueWanted_ || aqSame || aqFamily || (ac && *ac == '1') ||
                      (arms && std::strstr(arms, "on") != nullptr);
  const bool aqNone = !wanted || (aq && std::strcmp(aq, "none") == 0);
  if (!aqSame && !aqNone)
  for (uint32_t i = 0; i < qn; i++)
    if (i != queueFamily_ && (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
        !(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qs[i].queueCount > 0) {
      asyncFamily_ = i;
      break;
    }
  if (!aqNone && asyncFamily_ == UINT32_MAX && queueFamily_ < qn &&
      qs[queueFamily_].queueCount >= 2) {
    asyncFamily_ = queueFamily_;
    asyncIndex_ = 1;
  }
  {
    std::string fams;
    for (uint32_t i = 0; i < qn; i++) {
      char b[64];
      std::snprintf(b, sizeof b, "%s%u:%s%s%s x%u", i ? ", " : "", i,
                    (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "G" : "",
                    (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "C" : "",
                    (qs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ? "T" : "",
                    qs[i].queueCount);
      fams += b;
    }
    caps_.queueFamilies = fams;
  }
  caps_.asyncComputeFamily = asyncFamily_;
  caps_.asyncComputeAvailable = asyncFamily_ != UINT32_MAX;

  const float prios[2] = {1.0f, 1.0f};
  VkDeviceQueueCreateInfo qcis[2]{};
  uint32_t qciCount = 1;
  qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qcis[0].queueFamilyIndex = queueFamily_;
  qcis[0].queueCount = (asyncFamily_ == queueFamily_) ? 2 : 1;
  qcis[0].pQueuePriorities = prios;
  if (asyncFamily_ != UINT32_MAX && asyncFamily_ != queueFamily_) {
    qcis[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qcis[1].queueFamilyIndex = asyncFamily_;
    qcis[1].queueCount = 1;
    qcis[1].pQueuePriorities = prios;
    qciCount = 2;
  }

  // Request only what is present. Sparse features are requested when available
  // so phase 7 does not need a device recreate; nothing uses them yet.
  VkPhysicalDeviceFeatures want{};
  want.sparseBinding = caps_.sparseBinding;
  want.sparseResidencyBuffer = caps_.sparseResidencyBuffer;
  // The shadow cache is the only fragment-stage storage write in the engine.
  // Requested when present rather than demanded: see the cap's note in
  // rhi_vulkan.h for why absence downgrades the renderer instead of failing it.
  want.fragmentStoresAndAtomics = caps_.fragmentStoresAndAtomics;
  // SANDVOX_ROBUST=1: enable robustBufferAccess (cross-vendor diagnosis). OFF
  // by default and deliberately so: Tint's robustness transform already clamps
  // every runtime-array index (vk_spirv.cpp leaves disable_robustness false),
  // so the driver-level feature buys nothing on a correct binding — but on a
  // CPU implementation an access that escapes the clamp (an out-of-spec
  // binding whose arrayLength the driver mis-derives) is a write into the HOST
  // HEAP, and this turns that from heap corruption into a bounded access.
  if (const char* rb = std::getenv("SANDVOX_ROBUST"); rb && *rb == '1')
    want.robustBufferAccess = caps_.robustBufferAccessAvailable;
  caps_.robustBufferAccessEnabled = want.robustBufferAccess != 0;

  // The device extension list, enumerated ONCE. Every consumer below asks the
  // same question of the same array; the enumeration used to live inside the
  // swapchain branch, which meant a headless run never learned what the device
  // offers.
  uint32_t extCount = 0;
  ifn_.EnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> extProps(extCount);
  if (extCount)
    ifn_.EnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, extProps.data());
  auto haveExt = [&extProps](const char* name) {
    for (const VkExtensionProperties& p : extProps)
      if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
  };
  std::vector<const char*> devExts;

  // SYNCHRONIZATION2 IS MANDATORY FOR PHASE 3b, and asking for it is not
  // optional decoration: vkCmdPipelineBarrier2 is core in Vulkan 1.3, which
  // makes the entry point RESOLVE, but calling it on a device that never
  // enabled the feature is undefined behaviour. With no validation layer here
  // that is an access violation inside the ICD, not an error return — the same
  // class of failure that cost phase 3a a debugging session on pipeline
  // layouts. Query first, enable explicitly, and refuse to run without it.
  //
  // TWO WAYS TO HAVE IT (2026-10-02, cross-vendor determinism): core 1.3, or a
  // Vulkan 1.2 device that offers VK_KHR_synchronization2 and
  // VK_KHR_dynamic_rendering as extensions — Mesa's Dozen (Vulkan over D3D12)
  // reports 1.2, and it is the one second shader-compiler back end this
  // machine can run on its own GPU. Same functions, same structs under KHR
  // names (vk_loader resolves the aliases); the feature structs differ, and a
  // VkPhysicalDeviceVulkan13Features in the chain of a 1.2 device is invalid.
  const bool core13 = caps_.apiVersion >= VK_API_VERSION_1_3;
  caps_.khr13Fallback = !core13;
  VkPhysicalDeviceVulkan13Features feat13{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceSynchronization2Features featSync2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
  VkPhysicalDeviceDynamicRenderingFeatures featDynR{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
  VkPhysicalDeviceTimelineSemaphoreFeatures featTimeline{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  if (ifn_.GetPhysicalDeviceFeatures2) {
    if (core13) {
      VkPhysicalDeviceVulkan13Features probe{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &probe;
      ifn_.GetPhysicalDeviceFeatures2(phys_, &f2);
      caps_.synchronization2 = probe.synchronization2 != 0;
      caps_.dynamicRendering = probe.dynamicRendering != 0;
    } else if (haveExt(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) &&
               haveExt(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
      VkPhysicalDeviceSynchronization2Features ps{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
      VkPhysicalDeviceDynamicRenderingFeatures pd{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
      ps.pNext = &pd;
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &ps;
      ifn_.GetPhysicalDeviceFeatures2(phys_, &f2);
      caps_.synchronization2 = ps.synchronization2 != 0;
      caps_.dynamicRendering = pd.dynamicRendering != 0;
    }
    if (caps_.apiVersion >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceTimelineSemaphoreFeatures pt{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &pt;
      ifn_.GetPhysicalDeviceFeatures2(phys_, &f2);
      caps_.timelineSemaphore = pt.timelineSemaphore != 0;
    }
  }
  if (!caps_.synchronization2) {
    err = core13 ? "device does not support VkPhysicalDeviceVulkan13Features::"
                   "synchronization2, which the generated-barrier recorder requires "
                   "(docs/vulkan_barrier_graph.md §3.2 is written in Flags2 scopes)"
                 : "Vulkan 1.2 device without VK_KHR_synchronization2 + "
                   "VK_KHR_dynamic_rendering: the generated-barrier recorder needs "
                   "Flags2 barriers (a HARD minimum: down-converting every scope "
                   "to the 1.0 barrier is a silently weaker barrier)";
    return false;
  }
  // Dynamic rendering is the render path's ONE recording model (phase 4b) —
  // like synchronization2 it is core-1.3 but must still be enabled, and a
  // device without it would need a VkRenderPass-object code path nothing else
  // exercises. Both features are MANDATORY in core 1.3, so a 1.3 device that
  // lacks either is out of spec; refuse rather than fork the recording model.
  if (!caps_.dynamicRendering) {
    err =
        "device does not support dynamicRendering (core 1.3 or "
        "VK_KHR_dynamic_rendering), which the phase-4b render path requires";
    return false;
  }
  // The feature chain handed to vkCreateDevice. `chainTail` is the last
  // struct's pNext, so optional features append without caring which of the
  // two shapes above opened it.
  void* chainHead = nullptr;
  void** chainTail = &chainHead;
  auto append = [&](auto* s) {
    *chainTail = s;
    chainTail = &s->pNext;
  };
  if (core13) {
    feat13.synchronization2 = VK_TRUE;
    feat13.dynamicRendering = VK_TRUE;
    append(&feat13);
  } else {
    featSync2.synchronization2 = VK_TRUE;
    featDynR.dynamicRendering = VK_TRUE;
    append(&featSync2);
    append(&featDynR);
    devExts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    devExts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    // dynamic_rendering's own dependencies, core in 1.2 (named for a driver
    // that checks the list literally).
    if (haveExt(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
      devExts.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
    if (haveExt(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
      devExts.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
  }
  // Timeline semaphores (core 1.2): the async queue's cross-queue ordering.
  // Enabled when present; without them the async queue is simply not used.
  if (caps_.timelineSemaphore) {
    featTimeline.timelineSemaphore = VK_TRUE;
    append(&featTimeline);
  }
  if (!caps_.timelineSemaphore) caps_.asyncComputeAvailable = false;

  // Windowed: VK_KHR_swapchain, verified present rather than assumed. A
  // headless run enables nothing — the device is unchanged from phase 3.
  if (swapchainRequested_) {
    if (!haveExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
      err = "device does not support VK_KHR_swapchain (windowed --backend vulkan)";
      return false;
    }
    devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

  // VK_KHR_pipeline_executable_properties — the `--shader-stats` instrument.
  // Enabled WHENEVER PRESENT, not only in that mode: the mode selects itself
  // through SetCaptureStats (a create flag), and gating device creation on a
  // CLI flag would mean the diagnostic runs on a device the game never uses.
  // The extension on its own changes no behaviour — it only adds two query
  // entry points and one create flag nobody sets by default. Absence is not
  // fatal anywhere; `--shader-stats` reports it and exits 2.
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR fePipeExec{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
  if (haveExt(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) &&
      ifn_.GetPhysicalDeviceFeatures2) {
    // Probe the FEATURE too, not just the extension string: an extension the
    // driver advertises with its feature bit clear is unusable, and enabling it
    // anyway is the kind of half-supported path that turns into an ICD crash
    // with no validation layer running (the synchronization2 note above).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR probe{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &probe;
    ifn_.GetPhysicalDeviceFeatures2(phys_, &f2);
    if (probe.pipelineExecutableInfo) {
      caps_.pipelineExecutableProps = true;
      fePipeExec.pipelineExecutableInfo = VK_TRUE;
      append(&fePipeExec);
      devExts.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    }
  }

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = qciCount;
  dci.pQueueCreateInfos = qcis;
  dci.pEnabledFeatures = &want;
  dci.pNext = chainHead;
  dci.enabledExtensionCount = (uint32_t)devExts.size();
  dci.ppEnabledExtensionNames = devExts.data();

  VkResult r = ifn_.CreateDevice(phys_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateDevice failed: ") + vkl::ResultName(r);
    return false;
  }
  vkl::LoadDevice(ifn_, device_, dfn_);
  dfn_.GetDeviceQueue(device_, queueFamily_, 0, &queue_);
  if (asyncFamily_ != UINT32_MAX)
    dfn_.GetDeviceQueue(device_, asyncFamily_, asyncIndex_, &asyncQueue_);
  if (!dfn_.CmdPipelineBarrier2) {
    err = "vkCmdPipelineBarrier2 did not resolve despite synchronization2";
    return false;
  }
  if (!dfn_.CmdBeginRendering || !dfn_.CmdEndRendering) {
    err = "vkCmdBeginRendering/EndRendering did not resolve despite dynamicRendering";
    return false;
  }
  if (caps_.asyncComputeAvailable &&
      (!dfn_.WaitSemaphores || !dfn_.GetSemaphoreCounterValue || !asyncQueue_))
    caps_.asyncComputeAvailable = false;
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
  // The DEVICE version, capped at the 1.3 VMA was told to target: VMA calls
  // 1.3-core entry points (vkGetDeviceBufferMemoryRequirements) when told 1.3,
  // which a 1.2 device (Dozen) does not have.
  aci.vulkanApiVersion = caps_.apiVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3
                                                               : VK_API_VERSION_1_2;
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
  // UNCONDITIONAL TRANSFER_DST: it is what the queued zero fill's
  // vkCmdFillBuffer needs, and it is free on device-local memory. This is the price of the zero-init
  // POLICY, and paying it everywhere is what makes the policy a mechanism
  // rather than a list somebody has to maintain correctly (barrier_graph §4.8).
  bci.usage = ToVkUsage(usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  b->usage = bci.usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  // ASYNC COMPUTE on a SEPARATE queue family (docs/PLAN_async_compute.md):
  // the async rows read and write ordinary engine buffers from the other
  // family, and an EXCLUSIVE buffer used by a second family without a
  // queue-family ownership transfer has undefined contents. CONCURRENT across
  // the two families instead of transfers: the set of buffers the async rows
  // touch is decided per tick by the recorder, and a transfer pair per buffer
  // per tick would be a second barrier generator to keep honest. Buffers only
  // (images never reach the async queue), and it changes nothing about the
  // CONTENTS — a run that never turns render.asyncCompute on computes exactly
  // what it did; on NVIDIA buffers carry no compression a sharing mode could
  // switch off.
  const uint32_t families[2] = {queueFamily_, asyncFamily_};
  if (asyncFamily_ != UINT32_MAX && asyncFamily_ != queueFamily_) {
    bci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    bci.queueFamilyIndexCount = 2;
    bci.pQueueFamilyIndices = families;
  }

  VmaAllocationCreateInfo aci{};
  const bool mapRead = rhi::Any(usage, rhi::BufferUsage::MapRead);
  const bool hostVisible =
      rhi::Any(usage, rhi::BufferUsage::MapRead | rhi::BufferUsage::MapWrite);
  if (hostVisible) {
    // Persistently mapped: the readback slots and the staging ring both want a
    // pointer that stays valid.
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (mapRead) {
      // READBACK. The CPU READS this memory, and the readers are the worst
      // possible consumers of uncached/write-combined host memory: a 4 MiB
      // memcpy out of an eviction batch, an RLE run scan, a page-table classify
      // pass (Stream::CompleteOldest / HarvestDemotes, World's snapshot
      // callback, PageTable's free-probe harvest). WC reads are uncached with
      // no prefetch and roughly an order of magnitude slower than RAM.
      //
      // So HOST_CACHED is PREFERRED here, explicitly, rather than left to be
      // implied. Coherence is preferred too but NOT required: requiring it
      // would let a device that only offers HOST_VISIBLE|HOST_CACHED (no
      // coherent+cached type) fall back to the uncached type and quietly lose
      // the whole point. `needsInvalidate` below is what makes the uncoherent
      // case correct instead of merely reachable.
      aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
      aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else {
      // UPLOAD (the Class B staging ring). The CPU only ever writes it, in
      // sequential runs, and the device reads it — the textbook case for
      // uncached write-combined memory. HOST_CACHED here would cost cache
      // pollution and a flush; VMA marks it not-preferred for exactly this
      // access pattern. Coherent is REQUIRED, because the no-flush-before-
      // submit argument in barrier_graph §4.1 depends on it.
      aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
      aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
  } else {
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }

  VmaAllocationInfo info{};
  VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &b->buf, &b->alloc, &info);
  if (r != VK_SUCCESS) return nullptr;
  b->mapped = info.pMappedData;
  vmaGetAllocationMemoryProperties(allocator_, b->alloc, &b->memProps);
  b->needsInvalidate =
      mapRead && (b->memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
  if (mapRead) {
    // One line, once, naming what the driver actually gave us. "Prefer cached"
    // is a preference, and a preference that silently did not take is exactly
    // the kind of thing nobody notices for a year — docs/RESEARCH_streaming_
    // hitch.md R6 asserted this memory was HOST_COHERENT-only from reading the
    // create info, which the create info cannot tell you.
    static bool announced = false;
    if (!announced) {
      announced = true;
      std::fprintf(stderr,
                   "readback memory: %s%s%s%s (invalidate before read: %s)\n",
                   (b->memProps & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL|" : "",
                   (b->memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE|" : "",
                   (b->memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT|" : "",
                   (b->memProps & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED" : "(uncached)",
                   b->needsInvalidate ? "yes" : "no");
    }
  }

  Buffer* raw = b.get();
  // The zero-init REGISTRY. Every buffer, no exceptions, no opt-out.
  buffers_.push_back(std::move(b));

  // Zero-init, queued (§4.8, phase 4a form): a whole-buffer fill rides the
  // pending-upload queue in ISSUE ORDER, so it drains at the head of the next
  // recorded command buffer — before any recorded use, and before any data
  // upload queued after creation (which therefore still wins). This is what
  // lets buffers be created at ANY time behind the seam (gate staging, the
  // eviction pool) and still start life zeroed the way WebGPU guarantees.
  // Precondition: no command buffer that uses the new buffer is OPEN at
  // creation time — true for every call site (buffers are always created
  // before their encoder), and the seam keeps it that way.
  //
  // EXCEPTION: MapWrite (host-write staging — the Class B ring). Its contents
  // are host-produced immediately before every GPU read, so a queued GPU fill
  // is not just useless — it drains in the same flush as the copies that read
  // the ring and ZEROES the freshly staged payloads. The material table went
  // flat that way (a frozen world, not a crash); host-write staging is never
  // GPU-zeroed.
  if (!rhi::Any(usage, rhi::BufferUsage::MapWrite)) {
    Pending p;
    p.dst = raw;
    p.dstOffset = 0;
    p.size = size;
    p.zeroFill = true;
    p.classA = false;
    pending_.push_back(std::move(p));
  }
  return raw;
}

void Backend::DestroyBufferDeferred(Buffer* b) {
  if (!b) return;
  for (size_t i = 0; i < buffers_.size(); i++) {
    if (buffers_[i].get() != b) continue;
    // Drop any still-pending upload aimed at it (a queued zero-fill for a
    // buffer nothing ever used, typically).
    for (size_t k = 0; k < pending_.size();) {
      if (pending_[k].dst == b) pending_.erase(pending_.begin() + k);
      else k++;
    }
    if (b->buf) graveyard_.push_back({b->buf, b->alloc, submitSerial_});
    buffers_.erase(buffers_.begin() + i);
    return;
  }
}

void Backend::ReclaimStaging() {
  // The floor is the LOW end of the oldest submit whose fence has not
  // signalled; everything below it has been read by the device already. Bytes
  // queued since the last submit are protected automatically, because
  // stagingSubmitted_ is the ceiling of what any floor can be.
  uint64_t floor = stagingSubmitted_;
  for (const InFlight& f : inFlight_) {
    if (dfn_.GetFenceStatus(device_, f.fence) == VK_SUCCESS) continue;
    if (f.stagingLow < floor) floor = f.stagingLow;
  }
  if (floor > stagingTail_) stagingTail_ = floor;
}

uint64_t Backend::StagingAlloc(uint64_t size) {
  const uint64_t cap = stagingRing_ ? stagingRing_->size : 0;
  if (cap == 0 || size > cap) return kStagingNoRoom;
  for (;;) {
    uint64_t off = AlignUp(stagingHead_, kStagingAlign);
    // A copy region must be contiguous, so pad past the physical end of the
    // ring rather than splitting. The padding is charged to the ring exactly
    // like a payload would be, so it cannot make the occupancy test optimistic.
    const uint64_t phys = off % cap;
    if (phys + size > cap) off += cap - phys;
    // The whole point: the live span is [stagingTail_, off + size) and it must
    // fit in the ring. The pre-2026-09-03 code tested `off + size >
    // ring->size` and wrapped to 0 on failure, which is not an occupancy test
    // at all — it overwrote whatever was at offset 0 whether or not a queued
    // vkCmdCopyBuffer still had to read it.
    if (off + size <= stagingTail_ + cap) {
      stagingHead_ = off + size;
      return off % cap;
    }
    // Full. First try the free reclaim, then BLOCK on the oldest submit still
    // holding space. A stall here is a bad frame; the alternative it replaces
    // is silent corruption.
    const uint64_t before = stagingTail_;
    ReclaimStaging();
    if (stagingTail_ > before) continue;
    const InFlight* oldest = nullptr;
    for (const InFlight& f : inFlight_) {
      if (dfn_.GetFenceStatus(device_, f.fence) == VK_SUCCESS) continue;
      if (!oldest || f.serial < oldest->serial) oldest = &f;
    }
    // Nothing in flight and still no room: the UNSUBMITTED batch alone fills
    // the ring, and no amount of waiting can reclaim it. Caller falls back.
    if (!oldest) return kStagingNoRoom;
    std::string err;
    stagingStalls_++;
    VkFence f = oldest->fence;
    if (!WaitFence(f, err)) return kStagingNoRoom;
    ReclaimStaging();
    // WaitFence returning success means that submit is done; if the floor did
    // not move, some OTHER in-flight submit is older in ring order, and the
    // next iteration waits on it. inFlight_ is finite, so this terminates.
    if (stagingTail_ <= before && inFlight_.empty()) return kStagingNoRoom;
  }
}

void Backend::QueueWrite(Buffer* dst, uint64_t offset, const void* data, size_t size) {
  if (!dst || size == 0) return;
  // A write covering the WHOLE buffer makes a still-queued creation zero fill
  // dead: the fill drains first (issue order) and this write then overwrites
  // every byte of it. Flag it and FlushUploads skips the fill. Harmless if the
  // fill already drained — nothing consults the flag once it has. Big buffers
  // uploaded wholesale right after creation (the material table, the reaction
  // table, a page-table reset) stop paying a fill that nothing can observe.
  if (offset == 0 && size >= dst->size) dst->zeroFillSuperseded = true;
  Pending p;
  p.dst = dst;
  p.dstOffset = offset;
  p.size = size;

  // THE CLASS RULE, and it is exactly one rule: Class A iff the payload is at
  // or under the policy threshold AND its size and offset are 4-aligned (what
  // vkCmdUpdateBuffer requires). Nothing else — no buffer identity, no
  // exceptions. The threshold moved from vkCmdUpdateBuffer's 65536-byte
  // LEGALITY limit to 4096 on 2026-09-03; see kClassAMaxBytes for why those are
  // different questions.
  p.classA = size <= kClassAMaxBytes && (size % 4) == 0 && (offset % 4) == 0;

  if (!p.classA) {
    // Class B: memcpy into the persistently-mapped ring now, copy on the GPU at
    // flush time. The region is reclaimed by the fence of the submit that
    // consumes it — which is why §4.2 gives EVERY submit a fence.
    const uint64_t off = StagingAlloc(size);
    if (off != kStagingNoRoom) {
      std::memcpy((uint8_t*)stagingRing_->mapped + off, data, size);
      p.stagingOffset = off;
    } else if (size <= kVkUpdateBufferLimit && (size % 4) == 0 && (offset % 4) == 0) {
      // The ring cannot be reclaimed (one unsubmitted batch exceeds it) but the
      // payload is still a LEGAL vkCmdUpdateBuffer. Degrade to Class A rather
      // than corrupt or abort: slower, correct, and counted so the ring can be
      // resized instead of guessed at.
      stagingFallbacks_++;
      p.classA = true;
    } else {
      // Neither path can take it. This is unreachable with the current ring
      // (the largest single write in the engine is 4 MiB against a 64 MiB
      // ring), and dropping it silently would lose GPU state, so say so.
      std::fprintf(stderr,
                   "FATAL: staging ring exhausted by an unsubmitted batch and "
                   "the %llu-byte write to '%s' is too large for "
                   "vkCmdUpdateBuffer. Raise kStagingRingBytes.\n",
                   (unsigned long long)size, dst->label.c_str());
      std::abort();
    }
  }
  if (p.classA) {
    // vkCmdUpdateBuffer captures the data into the command buffer at RECORD
    // time, so holding a copy until the flush is all the lifetime management
    // needed — no staging allocation, no fence tracking. It is also why the
    // --shot far-fill loop is safe: each iteration records its own payload into
    // its own command buffer, so rapid overwrites of one small buffer cannot
    // alias.
    p.inlineOffset = inlineArena_.size();
    inlineArena_.insert(inlineArena_.end(), (const uint8_t*)data,
                        (const uint8_t*)data + size);
  }
  // ISSUE ORDER, preserved exactly. Never sorted, never coalesced: last write to
  // a range before a submit must win, and coalescing is what would break that.
  pending_.push_back(std::move(p));
}

void Backend::FlushUploads(VkCommandBuffer cmd) {
  if (pending_.empty()) return;

  // CROSS-SUBMIT ordering for the flush itself (phase 4b, found by sync
  // validation the first time the render gates ran). The §3.4 head barrier is
  // emitted by Recorder::Begin AFTER this flush's commands, so it orders the
  // uploads against everything RECORDED AFTER them — but nothing ordered them
  // against PREVIOUS submits. That gap was unreachable until zero-init moved
  // into the queue (§4.8 phase 4a): a buffer's creation fill can now drain in
  // one command buffer and its first Class B data copy in a LATER one, a WAW
  // across submits that validation reported verbatim as
  // "vkCmdCopyBuffer ... writes to VkBuffer ... previously written by
  // vkCmdFillBuffer (from [another] VkCommandBuffer)". On this GPU the copy
  // could lose the race and the fill zero freshly-uploaded data (the
  // micro-body pool lost half its limbs exactly this way). One barrier at the
  // head of any non-empty flush closes the class: every prior write is made
  // available/visible to these transfer writes. Emitted mechanically for the
  // whole flush — never per call site.
  {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    dfn_.CmdPipelineBarrier2(cmd, &di);
  }

  // INTRA-FLUSH WAW. Two uploads to one buffer in one flush are transfer
  // writes with no implicit ordering — vkCmdFillBuffer / vkCmdUpdateBuffer /
  // vkCmdCopyBuffer may overlap, and "last write wins" (the queue's contract)
  // only holds if a barrier orders them. This became reachable when zero-init
  // moved into the queue (phase 4a): a buffer's creation fill is routinely
  // followed by its first data upload in the SAME flush, and losing that race
  // zeroed the material table — a frozen world, not a crash. The barrier is
  // derived from buffer identity (same rule the §3.3 tracker applies), emitted
  // only when a destination repeats; an ordinary flush emits none.
  //
  // "Seen since the last barrier" is an EPOCH stamp on the buffer rather than a
  // scan of every destination so far: the scan was quadratic in the flush
  // size, and a revisited window-shift plane flushes over a thousand writes.
  ++flushEpoch_;
  for (const Pending& p : pending_) {
    // A creation fill a later whole-buffer write superseded (QueueWrite):
    // recording it would zero bytes the next command in this very flush
    // overwrites. Skipped BEFORE the WAW test, so it does not count as a
    // touch and cannot force a barrier of its own.
    if (p.zeroFill && p.dst->zeroFillSuperseded) continue;
    if (p.dst->flushTouch == flushEpoch_) {
      VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      di.memoryBarrierCount = 1;
      di.pMemoryBarriers = &mb;
      dfn_.CmdPipelineBarrier2(cmd, &di);
      ++flushEpoch_;  // everything touched so far is now ordered
    }
    if (p.zeroFill) {
      dfn_.CmdFillBuffer(cmd, p.dst->buf, 0, VK_WHOLE_SIZE, 0);
    } else if (p.classA) {
      dfn_.CmdUpdateBuffer(cmd, p.dst->buf, p.dstOffset, p.size,
                           inlineArena_.data() + p.inlineOffset);
    } else {
      VkBufferCopy region{};
      region.srcOffset = p.stagingOffset;
      region.dstOffset = p.dstOffset;
      region.size = p.size;
      dfn_.CmdCopyBuffer(cmd, stagingRing_->buf, p.dst->buf, 1, &region);
    }
    p.dst->flushTouch = flushEpoch_;
  }
  // HELD, not dropped. These copies are recorded into `cmd` and will only ever
  // execute if `cmd` is submitted; until then the writes are still owed. See
  // AbandonCommands for the case that made this necessary. Their Class A
  // payloads go with them (heldArena_), and the swap hands the previous held
  // arena's capacity back to the pending side.
  heldFlush_ = std::move(pending_);
  pending_.clear();
  heldArena_.swap(inlineArena_);
  inlineArena_.clear();
  heldFlushCmd_ = cmd;
  // CHARGE THE RING TO THIS COMMAND BUFFER. Everything allocated up to now has
  // had its vkCmdCopyBuffer recorded into `cmd`, so the submit of `cmd` is what
  // releases it — not the submit of whatever command buffer happens to be
  // submitted next, and not "the head at submit time", which would also cover
  // QueueWrites issued DURING the caller's recording whose copies belong to a
  // later command buffer.
  for (FlushMark& m : flushMarks_) {
    if (m.cmd != cmd) continue;
    m.high = stagingHead_;
    return;
  }
  flushMarks_.push_back({cmd, stagingHead_});
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

VkCommandBuffer Backend::BeginCommandsNoFlush() {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!freeCmds_.empty()) {
    cmd = freeCmds_.back();
    freeCmds_.pop_back();
  } else {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (dfn_.AllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
  }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn_.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    freeCmds_.push_back(cmd);
    return VK_NULL_HANDLE;
  }
  return cmd;
}

// ---------------------------------------------------------------------------
// ASYNC COMPUTE (rhi_vulkan.h's block comment; docs/PLAN_async_compute.md).
// ---------------------------------------------------------------------------
void Backend::SetAsyncEnabled(bool on) {
  if (!on || !caps_.asyncComputeAvailable) {
    asyncEnabled_ = false;
    return;
  }
  if (!asyncUsed_) {
    // First switch-on: the async family's command pool and the two timeline
    // semaphores. Created here rather than at Init so a run that never turns
    // the switch on submits exactly what it always did — no timeline chained
    // onto any submit, no second pool.
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = asyncFamily_;
    if (dfn_.CreateCommandPool(device_, &cpi, nullptr, &asyncCmdPool_) != VK_SUCCESS) {
      std::fprintf(stderr, "async compute: vkCreateCommandPool failed; staying single-queue\n");
      caps_.asyncComputeAvailable = false;
      return;
    }
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    ti.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &ti;
    if (dfn_.CreateSemaphore(device_, &sci, nullptr, &mainTimeline_) != VK_SUCCESS ||
        dfn_.CreateSemaphore(device_, &sci, nullptr, &asyncTimeline_) != VK_SUCCESS) {
      std::fprintf(stderr, "async compute: timeline semaphore creation failed; staying single-queue\n");
      caps_.asyncComputeAvailable = false;
      return;
    }
    // Everything submitted to the main queue BEFORE the timeline existed must
    // still be ordered before the first async submit. One empty submit that
    // signals the timeline, behind all of it in queue order, says so.
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    const uint64_t v = ++mainValue_;
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues = &v;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &tsi;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &mainTimeline_;
    dfn_.QueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    asyncUsed_ = true;
  }
  asyncEnabled_ = true;
}

VkCommandBuffer Backend::BeginAsyncCommands() {
  if (!asyncUsed_) return VK_NULL_HANDLE;
  PollFences();
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!freeAsyncCmds_.empty()) {
    cmd = freeAsyncCmds_.back();
    freeAsyncCmds_.pop_back();
  } else {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = asyncCmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (dfn_.AllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
  }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn_.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    freeAsyncCmds_.push_back(cmd);
    return VK_NULL_HANDLE;
  }
  // NO FlushUploads: a pending upload belongs to the MAIN queue's order. An
  // upload drained here would run concurrently with main-queue work that was
  // queued after it — the issue-order contract (semantics 1 in this file's
  // header) would stop holding across queues.
  return cmd;
}

void Backend::AbandonAsyncCommands(VkCommandBuffer cmd, bool ended) {
  if (cmd == VK_NULL_HANDLE) return;
  if (!ended) dfn_.EndCommandBuffer(cmd);
  freeAsyncCmds_.push_back(cmd);
}

VkFence Backend::SubmitAsync(VkCommandBuffer cmd, const std::vector<AsyncTouch>& touched,
                             std::string& err) {
  if (!asyncUsed_ || asyncQueue_ == VK_NULL_HANDLE) {
    err = "SubmitAsync without an async queue";
    return VK_NULL_HANDLE;
  }
  VkFence fence = AcquireFence(err);
  if (fence == VK_NULL_HANDLE) return VK_NULL_HANDLE;
  // WAIT for every main-queue submit so far (the latest value signalled), at
  // ALL_COMMANDS: the async work runs strictly after the tick that produced its
  // inputs, as it would at the end of that tick's command buffer.
  const uint64_t waitV = mainValue_;
  const uint64_t sigV = asyncSubmitted_ + 1;
  VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  tsi.waitSemaphoreValueCount = 1;
  tsi.pWaitSemaphoreValues = &waitV;
  tsi.signalSemaphoreValueCount = 1;
  tsi.pSignalSemaphoreValues = &sigV;
  const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.pNext = &tsi;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &mainTimeline_;
  si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &asyncTimeline_;
  VkResult r = dfn_.QueueSubmit(asyncQueue_, 1, &si, fence);
  if (r != VK_SUCCESS) {
    err = std::string("vkQueueSubmit (async) failed: ") + vkl::ResultName(r);
    return VK_NULL_HANDLE;
  }
  asyncSubmitted_ = sigV;
  // The conflict set ACCUMULATES until a join: two async submits with no main
  // join between them (no conflicting main command in between) are both owed.
  for (const AsyncTouch& t : touched) {
    if (!t.buf) continue;
    bool& w = asyncTouch_[t.buf];
    w = w || t.write;
  }
  InFlight e{fence, cmd, stagingSubmitted_, stagingSubmitted_, ++submitSerial_};
  e.asyncPool = true;
  inFlight_.push_back(e);
  asyncStats_.submits++;
  return fence;
}

bool Backend::AsyncConflict(const Buffer* b, bool write) const {
  if (!AsyncOutstanding() || !b) return false;
  auto it = asyncTouch_.find(b);
  if (it == asyncTouch_.end()) return false;
  // A main-queue WRITE races any async access (WAR or WAW); a main-queue READ
  // races only an async WRITE (RAW). Read/read is the overlap this exists for.
  return write || it->second;
}

VkFence Backend::SubmitMainJoined(VkCommandBuffer head, VkCommandBuffer tail,
                                  bool joinHead, bool joinTail, bool presenting,
                                  std::string& err) {
  return SubmitMainImpl(head, tail, joinHead, joinTail, presenting, err);
}

// The ONE main-queue submit once the timelines exist (asyncUsed_): every main
// submit signals mainTimeline_, and a join waits asyncTimeline_. Before the
// first SetAsyncEnabled(true) the old single-queue paths are used unchanged.
VkFence Backend::SubmitMainImpl(VkCommandBuffer head, VkCommandBuffer tail,
                                bool joinHead, bool joinTail, bool presenting,
                                std::string& err) {
  VkFence fence = AcquireFence(err);
  if (fence == VK_NULL_HANDLE) return VK_NULL_HANDLE;
  const bool present = presenting && pendingAcquireSlot_ != nullptr &&
                       acquiredIndex_ != UINT32_MAX;
  const uint64_t joinV = asyncSubmitted_;
  const bool needJoin = AsyncOutstanding();
  joinHead = joinHead && needJoin;
  // A head join waits in BOTH batches: a semaphore wait's second scope is its
  // OWN batch only, so the tail would otherwise reach the async results only
  // through a barrier chain — legal to reason about, easy to get wrong.
  // Waiting one timeline value twice costs nothing.
  joinTail = (joinTail || joinHead) && needJoin && tail != VK_NULL_HANDLE;

  // Up to two VkSubmitInfo: [head] then [tail]. Semaphore arrays per info.
  struct Part {
    VkSemaphore waits[2];
    uint64_t waitVals[2];
    VkPipelineStageFlags waitStages[2];
    uint32_t nWait = 0;
    VkSemaphore sigs[2];
    uint64_t sigVals[2];
    uint32_t nSig = 0;
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  } parts[2];
  VkSubmitInfo si[2]{};
  const uint32_t nParts = tail != VK_NULL_HANDLE ? 2u : 1u;
  // The swapchain acquire wait goes on the FIRST part (it only gates the
  // colour-attachment / transfer stages, which a compute head never reaches
  // before the render pass anyway), the render-done signal on the LAST.
  if (present) {
    Part& p = parts[0];
    p.waits[p.nWait] = pendingAcquireSlot_->sem;
    p.waitVals[p.nWait] = 0;
    p.waitStages[p.nWait] = kSwapchainAcquireWaitStages;
    p.nWait++;
  }
  if (joinHead) {
    Part& p = parts[0];
    p.waits[p.nWait] = asyncTimeline_;
    p.waitVals[p.nWait] = joinV;
    p.waitStages[p.nWait] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    p.nWait++;
  }
  if (joinTail) {
    Part& p = parts[1];
    p.waits[p.nWait] = asyncTimeline_;
    p.waitVals[p.nWait] = joinV;
    p.waitStages[p.nWait] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    p.nWait++;
  }
  {
    Part& p = parts[nParts - 1];
    p.sigs[p.nSig] = mainTimeline_;
    p.sigVals[p.nSig] = ++mainValue_;
    p.nSig++;
    if (present) {
      p.sigs[p.nSig] = renderDone_[acquiredIndex_];
      p.sigVals[p.nSig] = 0;
      p.nSig++;
    }
  }
  VkCommandBuffer cmds[2] = {head, tail};
  for (uint32_t i = 0; i < nParts; i++) {
    Part& p = parts[i];
    p.tsi.waitSemaphoreValueCount = p.nWait;
    p.tsi.pWaitSemaphoreValues = p.waitVals;
    p.tsi.signalSemaphoreValueCount = p.nSig;
    p.tsi.pSignalSemaphoreValues = p.sigVals;
    si[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si[i].pNext = &p.tsi;
    si[i].waitSemaphoreCount = p.nWait;
    si[i].pWaitSemaphores = p.waits;
    si[i].pWaitDstStageMask = p.waitStages;
    si[i].commandBufferCount = 1;
    si[i].pCommandBuffers = &cmds[i];
    si[i].signalSemaphoreCount = p.nSig;
    si[i].pSignalSemaphores = p.sigs;
  }
  VkResult r = dfn_.QueueSubmit(queue_, nParts, si, fence);
  if (r != VK_SUCCESS) {
    err = std::string("vkQueueSubmit (main, timeline) failed: ") + vkl::ResultName(r);
    return VK_NULL_HANDLE;
  }
  NoteSubmit(fence, head);
  inFlight_.back().cmd2 = tail;
  if (joinHead || joinTail) {
    asyncJoined_ = joinV;
    asyncTouch_.clear();
    asyncStats_.joins++;
    if (joinHead) asyncStats_.headJoins++;
  }
  if (tail != VK_NULL_HANDLE) asyncStats_.splits++;
  if (present) {
    RetainFence(fence);
    pendingAcquireSlot_->lastUse = fence;
    pendingAcquireSlot_ = nullptr;
  }
  return fence;
}

VkCommandBuffer Backend::BeginCommands(const char* /*label*/) {
  PollFences();  // recycle anything already finished
  // Reuse a retired command buffer when there is one (PollFences and
  // AbandonCommands feed freeCmds_). The pool has RESET_COMMAND_BUFFER, which
  // makes vkBeginCommandBuffer an implicit reset, so reuse costs nothing and
  // saves a free + allocate pair per submit.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!freeCmds_.empty()) {
    cmd = freeCmds_.back();
    freeCmds_.pop_back();
  } else {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (dfn_.AllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
  }

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn_.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    freeCmds_.push_back(cmd);
    return VK_NULL_HANDLE;
  }

  // Uploads flush at the HEAD of whichever command buffer is recorded next,
  // from whichever code path records it. That is what reproduces WebGPU's
  // "deferred to the start of the next submit" — including the case where the
  // path that issued the writes submits nothing at all (Stream::FillSlots when
  // every slot hits the store), whose writes then belong to the next tick.
  //
  // ASYNC COMPUTE: an upload is a WRITE recorded at the very head of this
  // buffer, so if it targets anything the outstanding async work touches, this
  // whole command buffer must wait for that work (the seam reads the flag and
  // joins at the head). Decided BEFORE the flush consumes the list.
  lastBeginJoinHead_ = false;
  if (AsyncOutstanding())
    for (const Pending& p : pending_)
      if (asyncTouch_.count(p.dst)) {
        lastBeginJoinHead_ = true;
        break;
      }
  FlushUploads(cmd);
  return cmd;
}

VkFence Backend::SubmitCommands(VkCommandBuffer cmd, std::string& err) {
  if (dfn_.EndCommandBuffer(cmd) != VK_SUCCESS) {
    err = "vkEndCommandBuffer failed";
    return VK_NULL_HANDLE;
  }
  return SubmitEnded(cmd, err);
}

VkFence Backend::SubmitEnded(VkCommandBuffer cmd, std::string& err) {
  // Once the async timelines exist every main submit must signal the main
  // timeline, and a caller that did not decide its own join (anything that is
  // not the seam's Queue::Submit) joins conservatively at the head.
  if (asyncUsed_) return SubmitMainImpl(cmd, VK_NULL_HANDLE, true, false, false, err);
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
  NoteSubmit(fence, cmd);
  return fence;
}

// The command buffer that swallowed the last flush is dead: nothing it recorded
// will ever run, so the uploads it consumed are still owed. Put them back at the
// FRONT of the queue (issue order is the contract — a later write to the same
// range must still win) and drop the ring mark that would otherwise pin the
// staging floor behind a fence that is never coming.
void Backend::AbandonCommands(VkCommandBuffer cmd, bool ended) {
  if (cmd == VK_NULL_HANDLE) return;
  if (heldFlushCmd_ == cmd) {
    if (!heldFlush_.empty()) {
      flushesRecovered_ += (uint64_t)heldFlush_.size();
      // The re-queued writes carry offsets into heldArena_, the ones queued
      // since into inlineArena_. Concatenate held-then-pending (the same order
      // as the lists) and rebase the pending side's offsets past the held
      // bytes, so every Class A payload still points at its own data.
      const uint64_t heldBytes = heldArena_.size();
      for (Pending& p : pending_)
        if (p.classA && !p.zeroFill) p.inlineOffset += heldBytes;
      heldArena_.insert(heldArena_.end(), inlineArena_.begin(), inlineArena_.end());
      inlineArena_.swap(heldArena_);
      heldArena_.clear();
      heldFlush_.insert(heldFlush_.end(), std::make_move_iterator(pending_.begin()),
                        std::make_move_iterator(pending_.end()));
      pending_.swap(heldFlush_);
    }
    heldFlush_.clear();
    heldArena_.clear();
    heldFlushCmd_ = VK_NULL_HANDLE;
    for (size_t i = 0; i < flushMarks_.size(); i++) {
      if (flushMarks_[i].cmd != cmd) continue;
      flushMarks_.erase(flushMarks_.begin() + i);
      break;
    }
  }
  // The buffer was begun by BeginCommands and possibly never ended (an
  // encoder dropped mid-recording); end it so it is not left recording, then
  // hand it straight back for reuse — it was never submitted, so nothing on
  // the GPU can reference it, and the next vkBeginCommandBuffer resets it.
  // A buffer the seam already ended (Finish without Submit) cannot be ended
  // twice, so this asks the caller rather than guessing.
  if (!ended) dfn_.EndCommandBuffer(cmd);
  freeCmds_.push_back(cmd);
}

void Backend::NoteSubmit(VkFence fence, VkCommandBuffer cmd) {
  // Submitted: the held copies will execute, so the debt is settled.
  if (heldFlushCmd_ == cmd) {
    heldFlush_.clear();
    heldArena_.clear();
    heldFlushCmd_ = VK_NULL_HANDLE;
  }
  uint64_t high = stagingSubmitted_;
  for (size_t i = 0; i < flushMarks_.size(); i++) {
    if (flushMarks_[i].cmd != cmd) continue;
    if (flushMarks_[i].high > high) high = flushMarks_[i].high;
    flushMarks_.erase(flushMarks_.begin() + i);
    break;
  }
  inFlight_.push_back({fence, cmd, stagingSubmitted_, high, ++submitSerial_});
  stagingSubmitted_ = high;
}

void Backend::PollFences() {
  // Advance the staging-ring floor BEFORE the erase loop: once an entry is
  // erased its span is gone, and ReclaimStaging derives the floor from what is
  // still in flight.
  ReclaimStaging();
  for (size_t i = 0; i < inFlight_.size();) {
    if (dfn_.GetFenceStatus(device_, inFlight_[i].fence) == VK_SUCCESS) {
      VkFence f = inFlight_[i].fence;
      // Retired: the GPU is done with it, so it goes back for reuse
      // (BeginCommands) rather than to the pool. An async submit's buffer
      // belongs to the async family's pool; a split main buffer's tail rides
      // the same fence.
      if (inFlight_[i].asyncPool)
        freeAsyncCmds_.push_back(inFlight_[i].cmd);
      else
        freeCmds_.push_back(inFlight_[i].cmd);
      if (inFlight_[i].cmd2 != VK_NULL_HANDLE) freeCmds_.push_back(inFlight_[i].cmd2);
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

  // Drain the buffer graveyard: an entry is freeable once every submit that
  // was in flight when its handle was released has retired.
  if (!graveyard_.empty() || !imageGraveyard_.empty()) {
    uint64_t minInFlight = UINT64_MAX;
    for (const auto& f : inFlight_) minInFlight = f.serial < minInFlight ? f.serial : minInFlight;
    for (size_t i = 0; i < graveyard_.size();) {
      if (graveyard_[i].serial < minInFlight) {
        vmaDestroyBuffer(allocator_, graveyard_[i].buf, graveyard_[i].alloc);
        graveyard_.erase(graveyard_.begin() + i);
      } else {
        i++;
      }
    }
    for (size_t i = 0; i < imageGraveyard_.size();) {
      if (imageGraveyard_[i].serial < minInFlight) {
        if (imageGraveyard_[i].view) dfn_.DestroyImageView(device_, imageGraveyard_[i].view, nullptr);
        vmaDestroyImage(allocator_, imageGraveyard_[i].img, imageGraveyard_[i].alloc);
        imageGraveyard_.erase(imageGraveyard_.begin() + i);
      } else {
        i++;
      }
    }
  }
  // The pipeline-side graveyard (DestroyPipelineDeferred and friends): same
  // rule, its own lock because build-pool threads append to it.
  {
    std::lock_guard<std::mutex> lock(handleGraveMutex_);
    if (!handleGraveyard_.empty()) {
      uint64_t minInFlight = UINT64_MAX;
      for (const auto& f : inFlight_) minInFlight = f.serial < minInFlight ? f.serial : minInFlight;
      for (size_t i = 0; i < handleGraveyard_.size();) {
        if (handleGraveyard_[i].serial < minInFlight) {
          DestroyDoomedHandle(handleGraveyard_[i]);
          handleGraveyard_[i] = handleGraveyard_.back();
          handleGraveyard_.pop_back();
        } else {
          i++;
        }
      }
    }
  }
}

void Backend::DestroyDoomedHandle(const DoomedHandle& d) {
  switch (d.kind) {
    case DoomedHandle::Pipeline:
      dfn_.DestroyPipeline(device_, (VkPipeline)d.handle, nullptr);
      break;
    case DoomedHandle::PipelineLayout:
      dfn_.DestroyPipelineLayout(device_, (VkPipelineLayout)d.handle, nullptr);
      break;
    case DoomedHandle::SetLayout:
      dfn_.DestroyDescriptorSetLayout(device_, (VkDescriptorSetLayout)d.handle, nullptr);
      break;
  }
}

// The serial recorded is the LATEST submit at the call, exactly as
// DestroyBufferDeferred records it: every submit up to it may have recorded a
// use, and the handle dies once all of them have retired. Each call looks the
// handle up in its registry first and ignores one it does not find, which is
// what makes a seam handle outliving Shutdown (whose registries are empty) a
// no-op rather than a double destroy.
void Backend::DestroyPipelineDeferred(VkPipeline p) {
  if (p == VK_NULL_HANDLE) return;
  {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    size_t i = 0;
    while (i < pipelines_.size() && pipelines_[i].pipe != p) i++;
    if (i == pipelines_.size()) return;
    pipelines_.erase(pipelines_.begin() + (long)i);
  }
  std::lock_guard<std::mutex> lock(handleGraveMutex_);
  handleGraveyard_.push_back({DoomedHandle::Pipeline, (uint64_t)p, submitSerial_.load()});
}

// pipeLayouts_ / setLayouts_ are appended by CreatePipelineLayout /
// CreateSetLayout (main thread only, at Init) and erased here (any thread);
// handleGraveMutex_ covers both sides.
void Backend::DestroyPipelineLayoutDeferred(VkPipelineLayout l) {
  if (l == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> lock(handleGraveMutex_);
  size_t i = 0;
  while (i < pipeLayouts_.size() && pipeLayouts_[i] != l) i++;
  if (i == pipeLayouts_.size()) return;
  pipeLayouts_.erase(pipeLayouts_.begin() + (long)i);
  handleGraveyard_.push_back({DoomedHandle::PipelineLayout, (uint64_t)l, submitSerial_.load()});
}

void Backend::DestroySetLayoutDeferred(VkDescriptorSetLayout l) {
  if (l == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> lock(handleGraveMutex_);
  size_t i = 0;
  while (i < setLayouts_.size() && setLayouts_[i] != l) i++;
  if (i == setLayouts_.size()) return;
  setLayouts_.erase(setLayouts_.begin() + (long)i);
  handleGraveyard_.push_back({DoomedHandle::SetLayout, (uint64_t)l, submitSerial_.load()});
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

void Backend::InvalidateForRead(Buffer* b, uint64_t offset, uint64_t size) {
  // Fast out on the only case that occurs in practice: coherent memory needs
  // nothing, and the branch is one predictable load per map.
  if (!b || !b->needsInvalidate || !b->alloc) return;
  // vmaInvalidateAllocation, not the raw vkInvalidateMappedMemoryRanges: VMA
  // rounds the range out to nonCoherentAtomSize and knows the allocation's
  // offset inside its memory block. Getting either of those wrong by hand is a
  // validation error at best and a partially-invalidated read at worst.
  vmaInvalidateAllocation(allocator_, b->alloc, offset, size);
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
  // The async queue too, once it has ever been used: "all submitted GPU work"
  // includes the derived passes, and a caller that reads a render-only buffer
  // after WaitIdle (a gate's openness probe) must see them finished.
  if (asyncUsed_ && asyncQueue_ != VK_NULL_HANDLE) {
    r = dfn_.QueueWaitIdle(asyncQueue_);
    if (r != VK_SUCCESS) {
      err = std::string("vkQueueWaitIdle (async) failed: ") + vkl::ResultName(r);
      return false;
    }
  }
  PollFences();
  return true;
}

VkShaderModule Backend::GetShaderModule(const std::string& wgsl, const std::string& label,
                                        const std::string& entryPoint,
                                        uint32_t bodyLineOffset,
                                        std::string& diagnostics,
                                        std::string* cacheKey,
                                        std::vector<uint32_t>* bindings) {
  // THREAD-SAFE, AND CONCURRENT ACROSS KEYS. Threaded pipeline creation
  // (Simulation::BuildPipelines) reaches this from several threads at once.
  // The lock covers the CACHE ONLY, never the compile: the expensive middle —
  // Tint, the SPIR-V optimizer, vkCreateShaderModule — runs unlocked, which is
  // the whole point. Serializing it would have been ~free while Tint was the
  // only cost there, and is not once an optimizer pass sits in the same span
  // (docs/PLAN_shader_compile.md package B measured ~181 s of it).
  //
  // Two threads asking for the SAME key do not both compile: the second waits
  // on `shaderCv_` for the first to publish, then takes its own reference. The
  // compute build asks for each key once, but the render pipelines share
  // their vs/fs modules across variants, so this is the path that keeps the
  // no-duplicate-work property from being an accident of the call pattern.
  //
  // Everything else on this path is already per-key private: the shader_cache
  // file name is a hash of (source, entry point), so no two keys touch the
  // same file, and vkspv::Compile holds no state between calls.
  //
  // Cache by (label, entry point, source hash). The entry point is part of the
  // key because Tint emits a SINGLE-entry-point module: the engine builds
  // several pipelines from one .wgsl file (worldgen.wgsl alone yields main /
  // list / far / fardown), and those are genuinely different SPIR-V modules.
  //
  // The SPIRV-Tools recipe (`SANDVOX_SPIRV_OPT`) is mixed into the source hash,
  // not appended to the key string, because the same number is what names the
  // DISK cache file below. Without it an optimized blob and an unoptimized one
  // built from identical WGSL collide on one filename, and whichever run wrote
  // last silently supplies both — which would make an A/B of the two measure
  // nothing. Tag 0 (the default, opt off) reproduces the historical hash, so no
  // existing shader_cache/ entry is invalidated by this line.
  size_t srcHash = std::hash<std::string>{}(wgsl);
  if (uint32_t optTag = vkspv::OptimizerCacheTag(label))
    srcHash ^= (size_t)optTag * 0x9e3779b97f4a7c15ull;
  std::string key = label + "\x1f" + entryPoint + "\x1f" +
                    std::to_string(srcHash);
  if (cacheKey) *cacheKey = key;
  {
    std::unique_lock<std::mutex> lock(shaderMutex_);
    for (;;) {
      auto it = moduleCache_.find(key);
      if (it != moduleCache_.end()) {
        it->second.refs++;  // the caller's reference (ReleaseShaderModule)
        if (bindings) *bindings = it->second.bindings;
        return it->second.module;
      }
      if (!moduleInFlight_.count(key)) break;  // ours to compile
      shaderCv_.wait(lock);
    }
    moduleInFlight_.insert(key);
  }
  // From here to the publish below, THIS THREAD OWNS `key` and holds no lock.
  // Every early return has to hand the claim back, so the compile body is
  // wrapped in a lambda and there is exactly one exit path.
  VkShaderModule out = VK_NULL_HANDLE;
  std::vector<uint32_t> decorated;  // DecoratedBindings, published with `out`
  auto compile = [&]() -> VkShaderModule {

  // SPIR-V disk cache: skip Tint entirely on subsequent launches when the
  // assembled WGSL hasn't changed. The key is a hash of (source, entry point);
  // any edit to a shader, common.wgsl, or tuning.json changes the assembled
  // source and produces a new hash, so stale cache entries are harmless to
  // correctness — but they are never read again, and they used to accumulate
  // forever (2.5 GB / 35,405 files measured 2026-09-23). So every hit TOUCHES
  // its file, and the first load of a process starts a background sweep that
  // deletes entries untouched for kShaderCacheMaxAgeDays (PruneShaderCache
  // below). The in-memory cache above short-circuits all of this while any
  // seam handle still holds the module (it is refcounted since 2026-09-24, so
  // an F5 whose sources are unchanged re-reads the .spv from here).
  //
  // SANDVOX_SHADER_CACHE overrides the directory, for the same reason
  // SANDVOX_PIPELINE_CACHE overrides the driver blob's path: the default is
  // CWD-relative, so a worktree's first run re-ran Tint and spirv-opt for every
  // entry point the main checkout had already compiled from identical source.
  // Sharing is safe by construction — the file name IS the content key, and
  // the write below is whole-file, so two processes producing the same key
  // write the same bytes.
  namespace fs = std::filesystem;
  static const fs::path cacheDir = [] {
    const char* env = std::getenv("SANDVOX_SHADER_CACHE");
    fs::path d(env && *env ? env : "shader_cache");
    std::error_code ec;
    fs::create_directories(d, ec);
    // Once per process, off the calling thread: the directory can hold tens of
    // thousands of files and this is the pipeline-build path.
    std::thread(PruneShaderCache, d).detach();
    return d;
  }();
  // Combine source hash and entry-point hash into a filename.
  size_t epHash = std::hash<std::string>{}(entryPoint);
  char cacheName[64];
  std::snprintf(cacheName, sizeof(cacheName), "%016zx_%016zx.spv", srcHash, epHash);
  fs::path cachePath = cacheDir / cacheName;

  std::vector<uint32_t> spirv;
  bool fromDisk = false;
  {
    std::error_code ec;
    auto fsize = fs::file_size(cachePath, ec);
    if (!ec && fsize > 0 && (fsize % sizeof(uint32_t)) == 0) {
      if (FILE* f = std::fopen(cachePath.string().c_str(), "rb")) {
        spirv.resize(fsize / sizeof(uint32_t));
        size_t got = std::fread(spirv.data(), 1, fsize, f);
        std::fclose(f);
        // The magic word too: a blob that does not start with it is not SPIR-V
        // and is recompiled rather than handed to the driver.
        if (got == fsize && spirv[0] == 0x07230203u) {
          fromDisk = true;
          // The LRU stamp PruneShaderCache ages entries by. One metadata
          // write per module per launch; failure only means the entry ages
          // from its last successful touch.
          std::error_code tec;
          fs::last_write_time(cachePath, fs::file_time_type::clock::now(), tec);
        } else {
          spirv.clear();
        }
      }
    }
  }

  if (!fromDisk) {
    vkspv::CompileResult cr = vkspv::Compile(wgsl, label, entryPoint, bodyLineOffset);
    diagnostics = cr.diagnostics;
    if (!cr.ok) return VK_NULL_HANDLE;
    spirv = std::move(cr.spirv);
    // WRITE-THEN-RENAME. The cache is shared across processes (the running
    // game, a run.sh gate, a worktree via SANDVOX_SHADER_CACHE), and a reader
    // that opened this path mid-write got a short blob of whole words:
    // "shader compile failed ... unsupported SPIR-V version", the kernel
    // absent, and a gate reporting a defect that did not exist (seen 4x on
    // 2026-09-19 and again 2026-09-22). A rename onto the final name is atomic
    // on one volume, so a reader sees the old file, no file, or the whole new
    // one. The temp name is private to this process and thread.
    char tmpName[96];
    std::snprintf(tmpName, sizeof(tmpName), "%s.%lu.%zx.tmp", cacheName,
                  (unsigned long)SANDVOX_GETPID,
                  std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const fs::path tmpPath = cacheDir / tmpName;
    bool wrote = false;
    if (FILE* f = std::fopen(tmpPath.string().c_str(), "wb")) {
      wrote = std::fwrite(spirv.data(), sizeof(uint32_t), spirv.size(), f) ==
              spirv.size();
      wrote = (std::fclose(f) == 0) && wrote;
    }
    std::error_code rec;
    if (wrote) fs::rename(tmpPath, cachePath, rec);
    if (!wrote || rec) fs::remove(tmpPath, rec);
  }

  decorated = DecoratedBindings(spirv);
  VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sci.codeSize = spirv.size() * sizeof(uint32_t);
  sci.pCode = spirv.data();
  VkShaderModule m = VK_NULL_HANDLE;
  if (dfn_.CreateShaderModule(device_, &sci, nullptr, &m) != VK_SUCCESS) {
    diagnostics += "vkCreateShaderModule rejected the SPIR-V Tint produced\n";
    if (fromDisk) {
      // Stale or corrupt cache entry — remove it and retry via Tint.
      std::error_code ec;
      fs::remove(cachePath, ec);
    }
    return VK_NULL_HANDLE;
  }
  return m;
  };  // compile

  out = compile();
  // Publish and release the claim in one critical section, then wake anyone
  // who was waiting on this key — including on FAILURE, where nothing is
  // cached and the waiter re-runs the compile and gets the same diagnostics.
  {
    std::lock_guard<std::mutex> lock(shaderMutex_);
    moduleInFlight_.erase(key);
    if (out != VK_NULL_HANDLE) moduleCache_[key] = CachedModule{out, 1, decorated};
  }
  shaderCv_.notify_all();
  if (bindings && out != VK_NULL_HANDLE) *bindings = std::move(decorated);
  return out;
}

void Backend::ReleaseShaderModule(const std::string& cacheKey) {
  std::lock_guard<std::mutex> lock(shaderMutex_);
  auto it = moduleCache_.find(cacheKey);
  if (it == moduleCache_.end()) return;  // after Shutdown, or never cached
  if (it->second.refs > 1) {
    it->second.refs--;
    return;
  }
  // Last holder. Destroying NOW is legal: a VkShaderModule is only read during
  // pipeline creation, and every create that used this one held a reference
  // until it returned. Under the lock, so a concurrent GetShaderModule for the
  // same key either found it first (and bumped refs above 1) or will miss and
  // recompile — from shader_cache/ on disk, not from Tint.
  if (device_) dfn_.DestroyShaderModule(device_, it->second.module, nullptr);
  moduleCache_.erase(it);
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
  std::lock_guard<std::mutex> lock(handleGraveMutex_);  // see DestroySetLayoutDeferred
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
  std::lock_guard<std::mutex> lock(handleGraveMutex_);  // see DestroyPipelineLayoutDeferred
  pipeLayouts_.push_back(l);
  return l;
}

// ---------------------------------------------------------------------------
// Pipeline-cache hit/miss, from VkPipelineCreationFeedback (core in 1.3).
//
// SavePipelineCache writes only when this has marked the cache dirty, which is
// the whole of "a warm launch does not rewrite a 400 MB file three to five
// times". A feedback the driver did not fill in (VALID clear) is treated as a
// MISS: saving an unchanged cache costs a write, while believing a real miss
// was a hit costs the next launch a cold compile.
// ---------------------------------------------------------------------------
void Backend::NoteCreationFeedback(const VkPipelineCreationFeedback& fb) {
  const bool valid = (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0;
  const bool hit =
      (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
  if (!valid || !hit) pipelineCacheDirty_.store(true);
}

VkPipeline Backend::CreateComputePipeline(VkPipelineLayout layout, VkShaderModule module,
                                          const char* entry, const char* label) {
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
  // Only with the extension enabled: the flag is invalid without it (found by
  // validation on llvmpipe, which lacks it).
  if (captureStats_ && caps_.pipelineExecutableProps)
    ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
  // Did this create hit the on-disk cache? (NoteCreationFeedback.) One stage
  // feedback per shader stage: the count must be 0 or stageCount, and 0 is
  // only legal on drivers new enough to accept it.
  VkPipelineCreationFeedback fb{}, stageFb{};
  VkPipelineCreationFeedbackCreateInfo fbi{
      VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
  fbi.pPipelineCreationFeedback = &fb;
  fbi.pipelineStageCreationFeedbackCount = 1;
  fbi.pPipelineStageCreationFeedbacks = &stageFb;
  ci.pNext = &fbi;

  VkPipeline p = VK_NULL_HANDLE;
  // A cache HIT hands back an object the driver did not compile this run, and
  // it reports no statistics — so the stats mode compiles for real.
  if (dfn_.CreateComputePipelines(device_,
                                  captureStats_ ? VK_NULL_HANDLE : pipelineCache_, 1,
                                  &ci, nullptr, &p) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  // The stats mode bypassed the cache, so there is nothing to learn about it.
  if (!captureStats_ && pipelineCache_) NoteCreationFeedback(fb);
  // The one piece of shared state on this path (see the header note on why the
  // Vulkan call itself needs no lock).
  {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    pipelines_.push_back({p, label ? label : entry, /*compute=*/true});
  }
  return p;
}

// ---------------------------------------------------------------------------
// Phase 4b: images and graphics pipelines.
// ---------------------------------------------------------------------------

Image* Backend::CreateImage(uint32_t w, uint32_t h, rhi::TextureFormat fmt,
                            rhi::TextureUsage usage, const char* label) {
  VkFormat vfmt = ToVkFormat(fmt);
  if (vfmt == VK_FORMAT_UNDEFINED || w == 0 || h == 0) return nullptr;
  const bool isDepth = fmt == rhi::TextureFormat::Depth32Float;

  VkImageUsageFlags vusage = 0;
  if ((uint32_t)usage & (uint32_t)rhi::TextureUsage::CopySrc)
    vusage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if ((uint32_t)usage & (uint32_t)rhi::TextureUsage::CopyDst)
    vusage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((uint32_t)usage & (uint32_t)rhi::TextureUsage::TextureBinding)
    vusage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if ((uint32_t)usage & (uint32_t)rhi::TextureUsage::StorageBinding)
    vusage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if ((uint32_t)usage & (uint32_t)rhi::TextureUsage::RenderAttachment)
    vusage |= isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  auto im = std::make_unique<Image>();
  im->format = vfmt;
  im->width = w;
  im->height = h;
  im->aspect = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  im->label = label ? label : "";
  // See Image::sampled: a texture something will SAMPLE has to be handed back
  // in SHADER_READ_ONLY_OPTIMAL, which the recorder does at Finish().
  im->sampled = ((uint32_t)usage & (uint32_t)rhi::TextureUsage::TextureBinding) != 0;

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = vfmt;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = vusage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  if (vmaCreateImage(allocator_, &ici, &aci, &im->img, &im->alloc, nullptr) != VK_SUCCESS)
    return nullptr;

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = im->img;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = vfmt;
  vci.subresourceRange = {im->aspect, 0, 1, 0, 1};
  if (dfn_.CreateImageView(device_, &vci, nullptr, &im->view) != VK_SUCCESS) {
    vmaDestroyImage(allocator_, im->img, im->alloc);
    return nullptr;
  }

  Image* raw = im.get();
  images_.push_back(std::move(im));
  return raw;
}

void Backend::DestroyImageDeferred(Image* im) {
  if (!im) return;
  for (size_t i = 0; i < images_.size(); i++) {
    if (images_[i].get() != im) continue;
    if (im->img && im->alloc)  // swapchain-owned images have no allocation
      imageGraveyard_.push_back({im->img, im->alloc, im->view, submitSerial_});
    images_.erase(images_.begin() + i);
    return;
  }
}

VkPipeline Backend::CreateGraphicsPipeline(VkPipelineLayout layout, VkShaderModule vs,
                                           const char* vsEntry, VkShaderModule fs,
                                           const char* fsEntry,
                                           const rhi::RenderPipelineDesc& d,
                                           const char* label) {
  if (!dfn_.CreateGraphicsPipelines || layout == VK_NULL_HANDLE ||
      vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
    return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = vsEntry;
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = fsEntry;

  // No vertex buffers anywhere in the engine: every draw pulls from storage
  // buffers by vertex index (raymarch's fullscreen tri, the cube expanders).
  VkPipelineVertexInputStateCreateInfo vin{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = ToVkTopology(d.topology);

  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  // FRONT FACE: CCW, unchanged from WebGPU's default. The recorder sets a
  // NEGATIVE-HEIGHT viewport (vk_record.cpp BeginRendering), which makes the
  // viewport transform — and therefore framebuffer-space winding — identical to
  // WebGPU's. Passing the front face through unchanged is correct ONLY together
  // with that flip; this is the same pairing Dawn's own Vulkan backend uses.
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = ToVkCull(d.cullMode);
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = d.depth.depthWriteEnabled ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = ToVkCompare(d.depth.depthCompare);

  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (d.blend) {
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = ToVkBlendFactor(d.blend->color.srcFactor);
    att.dstColorBlendFactor = ToVkBlendFactor(d.blend->color.dstFactor);
    att.colorBlendOp = ToVkBlendOp(d.blend->color.operation);
    att.srcAlphaBlendFactor = ToVkBlendFactor(d.blend->alpha.srcFactor);
    att.dstAlphaBlendFactor = ToVkBlendFactor(d.blend->alpha.dstFactor);
    att.alphaBlendOp = ToVkBlendOp(d.blend->alpha.operation);
  }
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;

  // Viewport/scissor are dynamic so one pipeline serves any target size
  // (resize, the offscreen sizes, the swapchain) — the recorder sets both at
  // BeginRendering.
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dstate{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dstate.dynamicStateCount = 2;
  dstate.pDynamicStates = dyn;

  // Dynamic rendering: the attachment formats live on the PIPELINE, not on a
  // render pass object.
  VkFormat colorFmt = ToVkFormat(d.colorFormat);
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &colorFmt;
  ri.depthAttachmentFormat = ToVkFormat(d.depth.format);

  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &ri;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vin;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dstate;
  ci.layout = layout;
  ci.renderPass = VK_NULL_HANDLE;  // dynamic rendering
  // Only with the extension enabled: the flag is invalid without it (found by
  // validation on llvmpipe, which lacks it).
  if (captureStats_ && caps_.pipelineExecutableProps)
    ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
  // Cache hit/miss, as in the compute path; chained after the rendering info.
  VkPipelineCreationFeedback fb{}, stageFb[2]{};
  VkPipelineCreationFeedbackCreateInfo fbi{
      VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
  fbi.pPipelineCreationFeedback = &fb;
  fbi.pipelineStageCreationFeedbackCount = ci.stageCount;
  fbi.pPipelineStageCreationFeedbacks = stageFb;
  ri.pNext = &fbi;

  VkPipeline p = VK_NULL_HANDLE;
  // See the compute path: the on-disk cache is bypassed under captureStats_
  // because a cache hit is a pipeline the driver never compiled.
  if (dfn_.CreateGraphicsPipelines(device_,
                                   captureStats_ ? VK_NULL_HANDLE : pipelineCache_, 1,
                                   &ci, nullptr, &p) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  if (!captureStats_ && pipelineCache_) NoteCreationFeedback(fb);
  // Locked for the same reason the compute path is: EnsureRenderPipelines is
  // still serial and still on the main thread, but a deferred `far` compile
  // may be appending to this vector at the same moment.
  {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    pipelines_.push_back({p, label ? label : "(unlabelled)", /*compute=*/false});
  }
  return p;
}

// ---------------------------------------------------------------------------
// `--shader-stats`: what the driver will tell us about its own compilation.
//
// The KHR API is open-ended BY DESIGN — each vendor reports whichever counters
// it has, under its own names ("Register Count", "SGPRs", "Spill Count",
// "Scratch Memory Size"...). Nothing here may recognise a name: the caller
// prints every statistic generically and only the SORT reads names, by
// substring. Adding a vendor table would silently drop counters on the next GPU
// this repo runs on.
// ---------------------------------------------------------------------------

std::vector<PipelineExecutable> Backend::CollectPipelineStats() const {
  std::vector<PipelineExecutable> out;
  if (!caps_.pipelineExecutableProps || !dfn_.GetPipelineExecutablePropertiesKHR ||
      !dfn_.GetPipelineExecutableStatisticsKHR)
    return out;

  auto stageName = [](VkShaderStageFlags s) {
    std::string n;
    auto add = [&n](const char* w) {
      if (!n.empty()) n += "|";
      n += w;
    };
    if (s & VK_SHADER_STAGE_VERTEX_BIT) add("vertex");
    if (s & VK_SHADER_STAGE_FRAGMENT_BIT) add("fragment");
    if (s & VK_SHADER_STAGE_COMPUTE_BIT) add("compute");
    if (s & VK_SHADER_STAGE_GEOMETRY_BIT) add("geometry");
    if (s & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) add("tessCtrl");
    if (s & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) add("tessEval");
    if (n.empty()) n = "?";
    return n;
  };

  // A snapshot, so the walk cannot be invalidated by a create finishing on
  // another thread. --shader-stats forces the whole build serial anyway (see
  // Simulation::BuildPipelines), but that is a caller's promise, not this
  // function's, and the copy is 60 handles.
  std::vector<PipelineRec> snapshot;
  {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    snapshot = pipelines_;
  }
  for (const PipelineRec& rec : snapshot) {
    if (rec.pipe == VK_NULL_HANDLE) continue;
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = rec.pipe;
    uint32_t execCount = 0;
    if (dfn_.GetPipelineExecutablePropertiesKHR(device_, &pi, &execCount, nullptr) !=
            VK_SUCCESS ||
        execCount == 0)
      continue;
    std::vector<VkPipelineExecutablePropertiesKHR> props(execCount);
    std::memset(props.data(), 0,
                props.size() * sizeof(VkPipelineExecutablePropertiesKHR));
    for (VkPipelineExecutablePropertiesKHR& p : props)
      p.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (dfn_.GetPipelineExecutablePropertiesKHR(device_, &pi, &execCount,
                                                props.data()) != VK_SUCCESS)
      continue;

    for (uint32_t e = 0; e < execCount; e++) {
      // Bounded: a driver string that forgot its terminator must not turn into
      // a 256-byte overread. The arrays were memset above, so the bound is the
      // only thing that can be wrong.
      auto fixed = [](const char* s, size_t cap) {
        return std::string(s, ::strnlen(s, cap));
      };
      PipelineExecutable pe;
      pe.pipeline = rec.label;
      pe.stage = stageName(props[e].stages);
      pe.name = fixed(props[e].name, VK_MAX_DESCRIPTION_SIZE);
      pe.description = fixed(props[e].description, VK_MAX_DESCRIPTION_SIZE);
      pe.subgroupSize = props[e].subgroupSize;

      VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
      ei.pipeline = rec.pipe;
      ei.executableIndex = e;
      uint32_t statCount = 0;
      if (dfn_.GetPipelineExecutableStatisticsKHR(device_, &ei, &statCount, nullptr) ==
              VK_SUCCESS &&
          statCount) {
        // ZERO THE WHOLE ARRAY BY HAND, and do not trust `{sType}` to do it.
        // `value` is a UNION whose first member is a 4-byte VkBool32; aggregate
        // initialisation leaves the other 4 bytes as whatever the heap held,
        // and this driver writes only 32 bits for statistics whose real value
        // fits in 32. Measured on an RTX 3060 Ti: every "Local Memory Size"
        // came back as 0x10'00000000 + the true value, i.e. a stable 64 GiB of
        // garbage in the high dword, which turned the mode's ONE headline
        // ("does this shader spill?") into 74 identical nonsense numbers.
        std::vector<VkPipelineExecutableStatisticKHR> stats(statCount);
        std::memset(stats.data(), 0,
                    stats.size() * sizeof(VkPipelineExecutableStatisticKHR));
        for (VkPipelineExecutableStatisticKHR& s : stats)
          s.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if (dfn_.GetPipelineExecutableStatisticsKHR(device_, &ei, &statCount,
                                                    stats.data()) == VK_SUCCESS) {
          for (uint32_t s = 0; s < statCount; s++) {
            PipelineStat ps;
            ps.name = fixed(stats[s].name, VK_MAX_DESCRIPTION_SIZE);
            ps.description = fixed(stats[s].description, VK_MAX_DESCRIPTION_SIZE);
            switch (stats[s].format) {
              case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                ps.value = stats[s].value.b32 ? 1.0 : 0.0;
                ps.isBool = true;
                break;
              case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                ps.value = (double)stats[s].value.i64;
                break;
              case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                ps.value = (double)stats[s].value.u64;
                break;
              case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
              default:
                ps.value = stats[s].value.f64;
                break;
            }
            pe.stats.push_back(std::move(ps));
          }
        }
      }
      out.push_back(std::move(pe));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Phase 4b D3: the swapchain. Present mode from SetPresentMode (mailbox by
// default since 2026-09-05, FIFO fallback); per-image render-done semaphores;
// a fence-paced ring of acquire semaphores.
// ---------------------------------------------------------------------------

PFN_vkVoidFunction Backend::InstanceProc(const char* name) const {
  return gfn_.GetInstanceProcAddr ? gfn_.GetInstanceProcAddr(instance_, name) : nullptr;
}

void Backend::DestroySwapchainObjects() {
  for (auto& im : swapImages_)
    if (im->view) dfn_.DestroyImageView(device_, im->view, nullptr);
  swapImages_.clear();
  for (VkSemaphore s : renderDone_)
    if (s) dfn_.DestroySemaphore(device_, s, nullptr);
  renderDone_.clear();
  for (auto& slot : acquireSlots_) {
    if (slot.lastUse != VK_NULL_HANDLE) {
      ReleaseFence(slot.lastUse);
      slot.lastUse = VK_NULL_HANDLE;
    }
    if (slot.sem) dfn_.DestroySemaphore(device_, slot.sem, nullptr);
    slot.sem = VK_NULL_HANDLE;
  }
  if (swapchain_ && dfn_.DestroySwapchainKHR)
    dfn_.DestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  pendingAcquireSlot_ = nullptr;
  acquiredIndex_ = UINT32_MAX;
}

bool Backend::ConfigureSwapchain(VkSurfaceKHR surface, uint32_t w, uint32_t h,
                                 std::string& err) {
  if (surface != VK_NULL_HANDLE) surface_ = surface;
  if (surface_ == VK_NULL_HANDLE) {
    err = "ConfigureSwapchain: no surface";
    return false;
  }
  if (!dfn_.CreateSwapchainKHR || !ifn_.GetPhysicalDeviceSurfaceCapabilitiesKHR) {
    err = "swapchain entry points missing (device created without VK_KHR_swapchain?)";
    return false;
  }

  // Present support on the one queue family v1 uses (barrier_graph §5.1). A
  // machine where the graphics+compute family cannot present would need a
  // second queue, which the single-queue rule forbids — refuse and say so.
  VkBool32 canPresent = VK_FALSE;
  ifn_.GetPhysicalDeviceSurfaceSupportKHR(phys_, queueFamily_, surface_, &canPresent);
  if (!canPresent) {
    err = "queue family cannot present to this surface (single-queue rule, §5.1)";
    return false;
  }

  // Recreate: drain first (in-flight frames reference the old images/views).
  std::string werr;
  WaitIdle(werr);
  DestroySwapchainObjects();

  VkSurfaceCapabilitiesKHR caps{};
  ifn_.GetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {  // surface lets us choose
    extent.width = w;
    extent.height = h;
  }
  if (extent.width == 0 || extent.height == 0) {
    // Minimized: leave the swapchain absent; AcquireSwapchainImage returns
    // null and the frame loop skips rendering, same as Dawn's invalid view.
    return true;
  }

  // Present mode: what was asked for if the surface offers it, else FIFO
  // (the one mode every surface must support). Asked for and got are both
  // recorded so the caller can print the truth.
  VkPresentModeKHR wantMode = VK_PRESENT_MODE_FIFO_KHR;
  switch (presentMode_) {
    case rhi::PresentMode::Mailbox: wantMode = VK_PRESENT_MODE_MAILBOX_KHR; break;
    case rhi::PresentMode::Immediate: wantMode = VK_PRESENT_MODE_IMMEDIATE_KHR; break;
    default: break;
  }
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (wantMode != VK_PRESENT_MODE_FIFO_KHR && ifn_.GetPhysicalDeviceSurfacePresentModesKHR) {
    uint32_t pmCount = 0;
    ifn_.GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    if (pmCount)
      ifn_.GetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmCount, pms.data());
    for (VkPresentModeKHR pm : pms)
      if (pm == wantMode) mode = wantMode;
  }
  activePresentMode_ = mode == VK_PRESENT_MODE_MAILBOX_KHR     ? rhi::PresentMode::Mailbox
                       : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? rhi::PresentMode::Immediate
                                                               : rhi::PresentMode::Fifo;

  uint32_t imageCount = caps.minImageCount + 1;
  // Mailbox needs a spare image to swap the newest frame into while one is
  // being scanned out and one is being rendered; two would degrade to FIFO
  // pacing in practice.
  if (mode == VK_PRESENT_MODE_MAILBOX_KHR && imageCount < 3) imageCount = 3;
  if (caps.maxImageCount && imageCount > caps.maxImageCount)
    imageCount = caps.maxImageCount;

  // Format: prefer BGRA8 UNORM (what Dawn negotiates on this platform); fall
  // back to RGBA8, else the first offered.
  uint32_t fmtCount = 0;
  ifn_.GetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmtCount, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
  ifn_.GetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmtCount, fmts.data());
  VkSurfaceFormatKHR chosen = fmts.empty()
                                  ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                       VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                  : fmts[0];
  for (const auto& f : fmts)
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = f;
      break;
    }
  swapFormat_ = chosen.format;

  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = imageCount;
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  // TRANSFER_DST as well as COLOR_ATTACHMENT when the surface allows it: the
  // internal-resolution frame (render.renderScale) reaches the swapchain by
  // a blit, not a draw. Every desktop surface offers it; a surface that does
  // not simply cannot be blitted into, and the frame loop's scale path checks
  // SwapchainBlittable before trying.
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swapBlittable_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
  if (swapBlittable_) sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;

  VkResult r = dfn_.CreateSwapchainKHR(device_, &sci, nullptr, &swapchain_);
  if (r != VK_SUCCESS) {
    err = std::string("vkCreateSwapchainKHR failed: ") + vkl::ResultName(r);
    return false;
  }

  uint32_t n = 0;
  dfn_.GetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  std::vector<VkImage> vkImages(n);
  dfn_.GetSwapchainImagesKHR(device_, swapchain_, &n, vkImages.data());

  VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < n; i++) {
    auto im = std::make_unique<Image>();
    im->img = vkImages[i];
    im->alloc = nullptr;  // swapchain-owned
    im->format = swapFormat_;
    im->width = extent.width;
    im->height = extent.height;
    im->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->presentable = true;
    im->label = "swapchain";
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im->img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = swapFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn_.CreateImageView(device_, &vci, nullptr, &im->view) != VK_SUCCESS) {
      err = "vkCreateImageView failed for a swapchain image";
      return false;
    }
    swapImages_.push_back(std::move(im));
    VkSemaphore s = VK_NULL_HANDLE;
    dfn_.CreateSemaphore(device_, &semCi, nullptr, &s);
    renderDone_.push_back(s);
  }
  for (auto& slot : acquireSlots_) {
    dfn_.CreateSemaphore(device_, &semCi, nullptr, &slot.sem);
    slot.lastUse = VK_NULL_HANDLE;
  }
  acquireCursor_ = 0;
  return true;
}

rhi::TextureFormat Backend::SwapchainFormat() const {
  switch (swapFormat_) {
    case VK_FORMAT_B8G8R8A8_UNORM: return rhi::TextureFormat::BGRA8Unorm;
    case VK_FORMAT_R8G8B8A8_UNORM: return rhi::TextureFormat::RGBA8Unorm;
    default: return rhi::TextureFormat::Undefined;
  }
}

Image* Backend::AcquireSwapchainImage() {
  if (swapchain_ == VK_NULL_HANDLE) return nullptr;
  AcquireSlot& slot = acquireSlots_[acquireCursor_];
  // The semaphore must be unsignaled AND not in use by a previous acquire.
  // The fence of the submit that consumed it is the proof.
  if (slot.lastUse != VK_NULL_HANDLE) {
    std::string err;
    WaitFence(slot.lastUse, err);
    ReleaseFence(slot.lastUse);
    slot.lastUse = VK_NULL_HANDLE;
  }
  uint32_t idx = 0;
  VkResult r = dfn_.AcquireNextImageKHR(device_, swapchain_, UINT64_MAX, slot.sem,
                                        VK_NULL_HANDLE, &idx);
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return nullptr;  // OUT_OF_DATE etc.
  acquiredIndex_ = idx;
  pendingAcquireSlot_ = &slot;
  acquireCursor_ = (acquireCursor_ + 1) % kAcquireSlots;
  return swapImages_[idx].get();
}

VkFence Backend::SubmitEndedPresenting(VkCommandBuffer cmd, std::string& err) {
  if (pendingAcquireSlot_ == nullptr || acquiredIndex_ == UINT32_MAX)
    return SubmitEnded(cmd, err);  // no acquire outstanding: plain submit
  if (asyncUsed_) return SubmitMainImpl(cmd, VK_NULL_HANDLE, true, false, true, err);
  VkFence fence = AcquireFence(err);
  if (fence == VK_NULL_HANDLE) return VK_NULL_HANDLE;

  // Both ways the image is first touched: a colour attachment or a blit
  // destination (rhi_vulkan.h kSwapchainAcquireWaitStages; the recorder's
  // first transition of the image names the same stages as its source).
  VkPipelineStageFlags waitStage = kSwapchainAcquireWaitStages;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &pendingAcquireSlot_->sem;
  si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &renderDone_[acquiredIndex_];
  VkResult r = dfn_.QueueSubmit(queue_, 1, &si, fence);
  if (r != VK_SUCCESS) {
    err = std::string("vkQueueSubmit (present) failed: ") + vkl::ResultName(r);
    return VK_NULL_HANDLE;
  }
  NoteSubmit(fence, cmd);
  // Pin this submit's fence to the acquire slot: reuse of the semaphore waits
  // on it (see AcquireSwapchainImage).
  RetainFence(fence);
  pendingAcquireSlot_->lastUse = fence;
  pendingAcquireSlot_ = nullptr;
  return fence;
}

void Backend::PresentAcquired() {
  if (acquiredIndex_ == UINT32_MAX || swapchain_ == VK_NULL_HANDLE) return;
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &renderDone_[acquiredIndex_];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &acquiredIndex_;
  dfn_.QueuePresentKHR(queue_, &pi);  // SUBOPTIMAL/OUT_OF_DATE: resize handles
  acquiredIndex_ = UINT32_MAX;
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
  // Record at the point of FAILURE, not four frames later. A null set handed
  // back here is bound by the recorder without a check and faults inside the
  // driver at vkCmdBindDescriptorSets, which is how a pool budget (see the
  // pool's creation) read as a NULL-pointer crash in a gate that had nothing
  // to do with it. The pool is never refilled, so this is a hard ceiling on
  // sets-per-process; say so, with the count, and stop.
  static uint32_t setsAllocated = 0;
  VkResult ar = dfn_.AllocateDescriptorSets(device_, &ai, &set);
  if (ar != VK_SUCCESS) {
    std::fprintf(stderr,
                 "FATAL: vkAllocateDescriptorSets failed (%s) after %u sets: the "
                 "descriptor pool (rhi_vulkan.cpp, never refilled) is exhausted. "
                 "Raise its sizes or stop rebuilding bind groups.\n",
                 vkl::ResultName(ar), setsAllocated);
    std::fflush(stderr);
    std::abort();
  }
  setsAllocated++;

  std::vector<VkDescriptorBufferInfo> infos(count);
  std::vector<VkWriteDescriptorSet> writes(count);
  for (size_t i = 0; i < count; i++) {
    Buffer* b = i < buffers.size() ? buffers[i] : nullptr;
    // A descriptor written with VK_NULL_HANDLE is not an error here and not an
    // error at bind time; it is an access violation inside the driver at the
    // first dispatch that touches the set, attributed to whatever pass ran.
    // Refuse it where it happens, naming the binding.
    if (!b || b->buf == VK_NULL_HANDLE) {
      std::fprintf(stderr,
                   "FATAL: CreateDescriptorSet: binding %u of a %zu-entry set has "
                   "a null buffer (rhi::Buffer %s). The buffer was never created "
                   "or its creation failed silently.\n",
                   entries[i].binding, count, b ? "present, VkBuffer null" : "absent");
      std::fflush(stderr);
      std::abort();
    }
    infos[i].buffer = b->buf;
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
    // A STORAGE RANGE OVER THE DEVICE'S LIMIT IS A HARD MINIMUM, NOT A HINT.
    // The voxel page pool (576 MiB at a 512 window) and the far cascade are
    // single bindings, and a device whose maxStorageBufferRange is smaller
    // (Dozen maps storage buffers onto D3D12 raw views, whose element-count
    // ceiling is far below the NVIDIA driver's 4 GiB) would bind a truncated
    // view: reads past the end come back as the robustness behaviour of that
    // implementation and writes vanish — a world hash that differs for a reason
    // that is not a determinism bug at all. Refuse at the point of failure,
    // naming the binding and both numbers.
    if ((type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
         type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) &&
        caps_.maxStorageBufferRange && infos[i].range != VK_WHOLE_SIZE &&
        infos[i].range > caps_.maxStorageBufferRange) {
      // SANDVOX_ALLOW_OVERSIZE_BINDINGS=1: run anyway, OUT OF SPEC, and say
      // so in every record (Caps::oversizeBindings -> the device JSON and the
      // fingerprint). This exists for ONE use: a CPU implementation whose
      // advertised limit is a conservative cap rather than a real one (Mesa
      // llvmpipe advertises 128 MiB; its bounds checks carry the real 32-bit
      // binding size). A hash that then MATCHES the native driver's is still
      // evidence — a truncated binding diverges, it does not coincide — but a
      // hash that differs proves nothing until the binding is in spec.
      // SANDVOX_ALLOW_OVERSIZE_BINDINGS=clamp: the DIAGNOSTIC arm — bind only
      // the first maxStorageBufferRange bytes (in spec, wrong results). If a
      // driver that dies with the oversize binding runs with this, the
      // oversize binding is what killed it.
      if (const char* ov = std::getenv("SANDVOX_ALLOW_OVERSIZE_BINDINGS");
          ov && std::strcmp(ov, "clamp") == 0) {
        caps_.oversizeBindings = true;
        if (infos[i].range > caps_.oversizeMaxRange) caps_.oversizeMaxRange = infos[i].range;
        infos[i].range = caps_.maxStorageBufferRange & ~(uint64_t)3;
      } else if (const char* ov2 = std::getenv("SANDVOX_ALLOW_OVERSIZE_BINDINGS"); ov2 && *ov2 == '1') {
        if (!caps_.oversizeBindings || infos[i].range > caps_.oversizeMaxRange)
          std::fprintf(stderr,
                       "WARNING (out of spec, SANDVOX_ALLOW_OVERSIZE_BINDINGS): storage "
                       "binding %u '%s' covers %llu bytes > maxStorageBufferRange %llu\n",
                       entries[i].binding, b->label.c_str(),
                       (unsigned long long)infos[i].range,
                       (unsigned long long)caps_.maxStorageBufferRange);
        caps_.oversizeBindings = true;
        if (infos[i].range > caps_.oversizeMaxRange) caps_.oversizeMaxRange = infos[i].range;
      } else {
      std::fprintf(stderr,
                   "FATAL: storage binding %u covers %llu bytes (rhi::Buffer '%s') but "
                   "this device's maxStorageBufferRange is %llu (%s). The engine needs "
                   "one binding over the whole page pool / far cascade; this device "
                   "cannot run it.\n",
                   entries[i].binding, (unsigned long long)infos[i].range,
                   b->label.c_str(), (unsigned long long)caps_.maxStorageBufferRange,
                   caps_.deviceName.c_str());
      std::fflush(stderr);
      std::abort();
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

// Both take validationMutex_ because DebugCallback appends from whatever
// thread provoked a message. The SCOPE itself is still single-threaded — one
// F5 rebuild at a time — so the mark is a plain index, not a per-thread one.
void Backend::PushValidationScope() {
  std::lock_guard<std::mutex> lock(validationMutex_);
  validationScopeOpen_ = true;
  validationScopeMark_ = validationMsgs_.size();
}

bool Backend::PopValidationScope(std::string& messages) {
  std::lock_guard<std::mutex> lock(validationMutex_);
  if (!validationScopeOpen_) return false;
  validationScopeOpen_ = false;
  bool any = validationMsgs_.size() > validationScopeMark_;
  for (size_t i = validationScopeMark_; i < validationMsgs_.size(); i++)
    messages += validationMsgs_[i] + "\n";
  return any;
}

// Persist the driver's pipeline cache to disk NOW, not only at Shutdown.
//
// WHY THIS IS CALLED FROM THE FRAME LOOP. Measured 2026-09-02 (startup
// timeline, main.cpp StartupMark): every phase of a windowed launch totals
// ~5 s except the FIRST DRAW, where the lazily created graphics pipelines cost
// 45-52 s of single-threaded driver compile (the 7,500-line raymarch fragment
// shader at the register cap) — the multi-minute white window the tuner's Play
// button showed. The cache that would skip that compile was written only from
// Shutdown, and the processes that pay the compile are exactly the ones that
// never reach Shutdown: build.sh taskkills every live sandvox.exe before it
// links, and the tuner's Play instances are killed that way several times an
// hour. So the on-disk cache lagged the shaders by days (main's file was 12 h
// stale with dozens of launches in between) and every launch after every
// build paid the full compile again. Saving right after the expensive create
// makes the SECOND launch fast whatever happens to the first.
//
// THE FILE IS NOT "A FEW MiB" (what this comment used to say): measured 394 MB
// on 2026-09-23, and written in full by up to five call sites per launch
// (worldgen batch, compute batch, far thread, render pipelines, Shutdown)
// whether or not anything had changed. So a save now happens only when some
// create since the last save MISSED the cache (pipelineCacheDirty_, set from
// VkPipelineCreationFeedback in both create paths). A warm launch sets it
// never and writes nothing; a cold one writes after each batch that compiled.
// The callers stay where they are — "save right after the expensive create"
// is still the right place — and simply cost nothing when clean.
void Backend::SavePipelineCache() {
  if (!device_ || !pipelineCache_ || !dfn_.GetPipelineCacheData ||
      pipelineCachePath_.empty())
    return;
  // Claim the dirty flag BEFORE reading the blob: a create that misses while
  // this save is running sets it again and the next save picks it up, where
  // clearing it afterwards could swallow that create.
  if (!pipelineCacheDirty_.exchange(false)) return;
  auto keepDirty = [this] { pipelineCacheDirty_.store(true); };
  size_t sz = 0;
  if (dfn_.GetPipelineCacheData(device_, pipelineCache_, &sz, nullptr) != VK_SUCCESS ||
      sz == 0) {
    keepDirty();
    return;
  }
  std::vector<uint8_t> blob(sz);
  if (dfn_.GetPipelineCacheData(device_, pipelineCache_, &sz, blob.data()) != VK_SUCCESS) {
    keepDirty();
    return;
  }
  // Write-then-rename, so a process killed mid-write (the whole reason this
  // function exists) cannot leave a truncated cache for the next launch to
  // hand the driver.
  //
  // The temp name is PER PROCESS AND PER CALL. With SANDVOX_PIPELINE_CACHE
  // pointing several worktrees at one file, a shared "<path>.tmp" would let
  // process B's fopen truncate the file process A is about to rename into
  // place — and the result reads as a corrupt cache, i.e. as a full cold
  // compile, which is exactly what the override exists to avoid. Racing
  // RENAMES are fine: each is atomic and each blob is a superset of what its
  // writer loaded, so last writer wins and no reader sees a partial file.
  static std::atomic<uint32_t> saveSeq{0};
  char suffix[48];
  std::snprintf(suffix, sizeof(suffix), ".%d.%u.tmp", (int)SANDVOX_GETPID,
                saveSeq.fetch_add(1));
  const std::string tmp = pipelineCachePath_ + suffix;
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    keepDirty();
    return;
  }
  const size_t wrote = std::fwrite(blob.data(), 1, sz, f);
  std::fclose(f);
  if (wrote != sz) {
    std::remove(tmp.c_str());
    keepDirty();
    return;
  }
  // std::filesystem::rename REPLACES an existing target atomically (MSVC:
  // MoveFileExW with MOVEFILE_REPLACE_EXISTING), so there is no moment at which
  // the cache file does not exist. The old std::remove + std::rename pair (C
  // rename refuses an existing target on Windows) left exactly such a window,
  // in which a concurrent launch sharing SANDVOX_PIPELINE_CACHE found no cache
  // and paid a cold compile.
  std::error_code ec;
  std::filesystem::rename(tmp, pipelineCachePath_, ec);
  if (ec) {
    std::remove(tmp.c_str());
    keepDirty();
  }
}

void Backend::Shutdown() {
  if (!device_) {
    if (instance_ && ifn_.DestroyInstance) {
      ifn_.DestroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
    }
    return;
  }
  // Timed, always, one line: a headless run was measured spending ~45 s
  // between its last printed line and process exit (2026-09-09), and the two
  // candidates in here — the device drain and serializing a 200+ MiB pipeline
  // cache — are the only ones that cannot be seen from outside.
  const auto tShut0 = std::chrono::steady_clock::now();
  auto msSince = [](std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  };
  std::string err;
  WaitIdle(err);
  const double waitMs = msSince(tShut0);
  if (asyncUsed_)
    std::fprintf(stderr,
                 "[shutdown] async compute: %llu async submits, %llu main-queue joins "
                 "(%llu at a buffer head), %llu split command buffers\n",
                 (unsigned long long)asyncStats_.submits,
                 (unsigned long long)asyncStats_.joins,
                 (unsigned long long)asyncStats_.headJoins,
                 (unsigned long long)asyncStats_.splits);

  {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    for (const PipelineRec& p : pipelines_) dfn_.DestroyPipeline(device_, p.pipe, nullptr);
    pipelines_.clear();
  }
  {
    // WaitIdle above drained the queue, so every deferred handle is idle.
    std::lock_guard<std::mutex> lock(handleGraveMutex_);
    for (const DoomedHandle& d : handleGraveyard_) DestroyDoomedHandle(d);
    handleGraveyard_.clear();
    for (VkPipelineLayout l : pipeLayouts_) dfn_.DestroyPipelineLayout(device_, l, nullptr);
    pipeLayouts_.clear();
    for (VkDescriptorSetLayout l : setLayouts_)
      dfn_.DestroyDescriptorSetLayout(device_, l, nullptr);
    setLayouts_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(shaderMutex_);
    for (auto& kv : moduleCache_)
      dfn_.DestroyShaderModule(device_, kv.second.module, nullptr);
    moduleCache_.clear();
  }

  const bool cacheDirty = pipelineCacheDirty_.load();
  const auto tSave0 = std::chrono::steady_clock::now();
  SavePipelineCache();
  const double saveMs = msSince(tSave0);
  if (pipelineCache_) {
    dfn_.DestroyPipelineCache(device_, pipelineCache_, nullptr);
    pipelineCache_ = VK_NULL_HANDLE;
  }
  if (cacheDirty)
    std::fprintf(stderr,
                 "[shutdown] vulkan backend: wait-idle %.0f ms, pipeline cache "
                 "saved in %.0f ms (%s)\n",
                 waitMs, saveMs, pipelineCachePath_.c_str());
  else
    std::fprintf(stderr,
                 "[shutdown] vulkan backend: wait-idle %.0f ms, pipeline cache "
                 "unchanged, not rewritten (%s)\n",
                 waitMs, pipelineCachePath_.c_str());

  for (auto& f : inFlight_) {
    dfn_.FreeCommandBuffers(device_, f.asyncPool ? asyncCmdPool_ : cmdPool_, 1, &f.cmd);
    if (f.cmd2 != VK_NULL_HANDLE) dfn_.FreeCommandBuffers(device_, cmdPool_, 1, &f.cmd2);
    dfn_.DestroyFence(device_, f.fence, nullptr);
  }
  inFlight_.clear();
  if (asyncCmdPool_ != VK_NULL_HANDLE) {
    if (!freeAsyncCmds_.empty())
      dfn_.FreeCommandBuffers(device_, asyncCmdPool_, (uint32_t)freeAsyncCmds_.size(),
                              freeAsyncCmds_.data());
    freeAsyncCmds_.clear();
    dfn_.DestroyCommandPool(device_, asyncCmdPool_, nullptr);
    asyncCmdPool_ = VK_NULL_HANDLE;
  }
  if (mainTimeline_ != VK_NULL_HANDLE) dfn_.DestroySemaphore(device_, mainTimeline_, nullptr);
  if (asyncTimeline_ != VK_NULL_HANDLE) dfn_.DestroySemaphore(device_, asyncTimeline_, nullptr);
  mainTimeline_ = asyncTimeline_ = VK_NULL_HANDLE;
  // Pool destruction below frees these too; explicit so the list is not left
  // holding handles into a dead pool.
  if (!freeCmds_.empty())
    dfn_.FreeCommandBuffers(device_, cmdPool_, (uint32_t)freeCmds_.size(), freeCmds_.data());
  freeCmds_.clear();
  for (VkFence f : freeFences_) dfn_.DestroyFence(device_, f, nullptr);
  freeFences_.clear();
  // Fences whose submit retired while a borrower still held them. WaitIdle
  // above drained the queue, so these are all signalled and unreferenced by the
  // GPU; the borrowers are going away with the backend.
  for (VkFence f : retiredRetained_) dfn_.DestroyFence(device_, f, nullptr);
  retiredRetained_.clear();
  fenceRetain_.clear();

  DestroySwapchainObjects();
  if (surface_ != VK_NULL_HANDLE && ifn_.DestroySurfaceKHR)
    ifn_.DestroySurfaceKHR(instance_, surface_, nullptr);
  surface_ = VK_NULL_HANDLE;

  if (descPool_) dfn_.DestroyDescriptorPool(device_, descPool_, nullptr);
  descPool_ = VK_NULL_HANDLE;
  if (cmdPool_) dfn_.DestroyCommandPool(device_, cmdPool_, nullptr);
  cmdPool_ = VK_NULL_HANDLE;

  // Say it only when it happened. A silent staging ring is the normal case and
  // does not need a line; a ring that STALLED or fell back to Class A is
  // undersized for one unsubmitted batch, and that is invisible from the
  // outside — it shows up as a slow shift with no attribution, which is
  // exactly the kind of number CLAUDE.md's rule 6 says to instrument rather
  // than bisect.
  if (stagingStalls_ || stagingFallbacks_)
    std::fprintf(stderr,
                 "staging ring: %llu blocking waits, %llu Class A fallbacks "
                 "(ring is %llu MiB; raise kStagingRingBytes)\n",
                 (unsigned long long)stagingStalls_,
                 (unsigned long long)stagingFallbacks_,
                 (unsigned long long)(kStagingRingBytes >> 20));

  for (auto& b : buffers_)
    if (b->buf) vmaDestroyBuffer(allocator_, b->buf, b->alloc);
  buffers_.clear();
  // Graveyard remnants: WaitIdle above drained the queue, so these are idle.
  for (auto& g : graveyard_) vmaDestroyBuffer(allocator_, g.buf, g.alloc);
  graveyard_.clear();
  for (auto& im : images_) {
    if (im->view) dfn_.DestroyImageView(device_, im->view, nullptr);
    if (im->img && im->alloc) vmaDestroyImage(allocator_, im->img, im->alloc);
  }
  images_.clear();
  for (auto& g : imageGraveyard_) {
    if (g.view) dfn_.DestroyImageView(device_, g.view, nullptr);
    vmaDestroyImage(allocator_, g.img, g.alloc);
  }
  imageGraveyard_.clear();
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
