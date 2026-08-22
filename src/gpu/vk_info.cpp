// vk_info.cpp — deliverable 3 replaces this with the --vk-info smoke mode.
//
// For now it exists to PROVE the deliverable-1 build plumbing: that Tint's WGSL
// reader and SPIR-V writer link into sandvox, that VMA compiles against the
// dynamically-loaded Vulkan headers, and that the loader finds vulkan-1.dll.
// A link that is only exercised by code written later is a link nobody has
// tested.

#include "gpu/vk_info.h"

#include <cstdio>
#include <string>

#include "gpu/vk_loader.h"
#include "gpu/vk_spirv.h"

namespace sandvox {

// Compile a trivial WGSL compute shader to SPIR-V and load the Vulkan library.
// Returns 0 on success. Deliverable 3 replaces this body with the real thing.
int RunVkInfo(bool /*lowPower*/) {
  const char* kWgsl =
      "@group(0) @binding(0) var<storage, read_write> out_buf : array<u32>;\n"
      "@compute @workgroup_size(64)\n"
      "fn main(@builtin(global_invocation_id) gid : vec3<u32>) {\n"
      "  out_buf[gid.x] = gid.x * 2u;\n"
      "}\n";
  vkspv::CompileResult r = vkspv::Compile(kWgsl, "plumbing_probe.wgsl", "main", 0);
  if (!r.ok) {
    std::printf("vk plumbing: Tint FAILED\n%s\n", r.diagnostics.c_str());
    return 1;
  }
  std::printf("vk plumbing: Tint OK (%zu SPIR-V words, magic %08x)\n", r.spirv.size(),
              r.spirv.empty() ? 0u : r.spirv[0]);

  vkl::GlobalFns g;
  std::string err;
  if (!vkl::LoadGlobal(g, err)) {
    std::printf("vk plumbing: loader FAILED (%s)\n", err.c_str());
    return 1;
  }
  uint32_t ver = VK_API_VERSION_1_0;
  if (g.EnumerateInstanceVersion) g.EnumerateInstanceVersion(&ver);
  std::printf("vk plumbing: loader OK (instance version %u.%u.%u)\n",
              VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver),
              VK_API_VERSION_PATCH(ver));
  return 0;
}

}  // namespace sandvox
