// vk_info.cpp — the `--vk-info` headless smoke mode (Vulkan port phase 3a exit).
//
// See vk_info.h for what this proves and why it exists. In one line: phase 3a
// builds foundations that execute nothing, and foundations nobody runs are
// foundations nobody has tested — so this runs each of them exactly once.
//
// It also PRINTS THE CAPABILITY RECORD, which is a deliverable rather than
// decoration: whether phase 7's sparse-residency plan (a 4 GiB virtual voxels
// buffer, ~83% of the dense 512 MiB saved) is viable at all is decided by
// residencyNonResidentStrict and maxStorageBufferRange on the actual device.

#include "gpu/vk_info.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/resources.h"
#include "gpu/rhi_vulkan.h"
#include "gpu/vk_spirv.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "test/support.h"  // AssetDir()

namespace sandvox {
namespace {

bool ReadFileText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// Every (file, entry point) pair Simulation::BuildPipelines creates a COMPUTE
// pipeline from. Kept in this order so the output reads like the build does.
//
// Deliberately a literal list rather than something derived: phase 3c wires the
// real Simulation through the Vulkan backend and this file goes away. A
// derivation built now would be a second source of truth for exactly one
// release of exactly one smoke test.
struct PipelineSpec {
  const char* file;
  const char* entry;
  const char* label;
};

const PipelineSpec kComputePipelines[] = {
    {"worldgen.wgsl", "main", "worldgen"},
    {"worldgen.wgsl", "list", "worldgenList"},
    {"worldgen.wgsl", "far", "farFill"},
    {"worldgen.wgsl", "fardown", "farDown"},
    {"sim_mutate.wgsl", "main", "mutate"},
    {"sim_mutate.wgsl", "cells", "mutateCells"},
    {"sim_compact.wgsl", "main", "compact"},
    {"sim_compact.wgsl", "mainNext", "compactNext"},
    {"sim_step.wgsl", "main", "step"},
    {"sim_occupancy.wgsl", "main", "occupancy"},
    {"sim_occupancy.wgsl", "mainDirty", "occupancyDirty"},
    {"sim_pick.wgsl", "main", "pick"},
    {"sim_explode.wgsl", "mark", "explodeMark"},
    {"sim_explode.wgsl", "apply", "explodeApply"},
    {"sim_particle.wgsl", "args1", "pArgs1"},
    {"sim_particle.wgsl", "spawn", "pSpawn"},
    {"sim_particle.wgsl", "integrate", "pIntegrate"},
    {"sim_particle.wgsl", "args2", "pArgs2"},
    {"sim_particle.wgsl", "resolve", "pResolve"},
    {"sim_fluid.wgsl", "spawn", "fluidSpawn"},
    {"sim_fluid.wgsl", "mark", "fluidMark"},
    {"sim_fluid.wgsl", "alloc", "fluidAlloc"},
    {"sim_fluid.wgsl", "clearGrid", "fluidClear"},
    {"sim_fluid.wgsl", "p2g", "fluidP2g"},
    {"sim_fluid.wgsl", "gridUpdate", "fluidGridUp"},
    {"sim_fluid.wgsl", "g2p", "fluidG2p"},
};

// The render-only shaders. Phase 3a creates NO render pipelines (no swapchain,
// no raster state), but their WGSL must still compile to SPIR-V or phase 4
// discovers it the hard way — so they are compiled and reported, then dropped.
const PipelineSpec kRenderShaders[] = {
    {"raymarch.wgsl", "vs", "raymarch.vs"},
    {"raymarch.wgsl", "fs", "raymarch.fs"},
    {"debris.wgsl", "vsParticle", "debris.vsParticle"},
    {"debris.wgsl", "vsBody", "debris.vsBody"},
    {"debris.wgsl", "vsSprite", "debris.vsSprite"},
    {"debris.wgsl", "vsFluid", "debris.vsFluid"},
    {"debris.wgsl", "fs", "debris.fs"},
    {"microbody.wgsl", "vs", "microbody.vs"},
    {"microbody.wgsl", "fs", "microbody.fs"},
    {"debug_lines.wgsl", "vsBox", "debug_lines.vsBox"},
    {"debug_lines.wgsl", "fsBox", "debug_lines.fsBox"},
};

const char* YesNo(bool b) { return b ? "YES" : "no"; }

void PrintCaps(const vk::Caps& c) {
  std::printf("\n=== device ===\n");
  std::printf("  name                  : %s\n", c.deviceName.c_str());
  std::printf("  type                  : %s\n", c.discrete ? "discrete" : "integrated/other");
  std::printf("  vendor/device         : 0x%04x / 0x%04x\n", c.vendorId, c.deviceId);
  std::printf("  api version           : %u.%u.%u\n", VK_API_VERSION_MAJOR(c.apiVersion),
              VK_API_VERSION_MINOR(c.apiVersion), VK_API_VERSION_PATCH(c.apiVersion));
  std::printf("  driver version        : %u\n", c.driverVersion);

  // These five decide phase 7. Printed together, loudly, because the sparse
  // plan lives or dies on them and "we'll check later" is how a phase gets
  // planned against hardware that cannot do it.
  std::printf("\n=== phase 7 (sparse residency) gates ===\n");
  std::printf("  sparseBinding                 : %s\n", YesNo(c.sparseBinding));
  std::printf("  sparseResidencyBuffer         : %s\n", YesNo(c.sparseResidencyBuffer));
  std::printf("  residencyNonResidentStrict    : %s%s\n",
              YesNo(c.residencyNonResidentStrict),
              c.residencyNonResidentStrict
                  ? "   <- unbound pages read as ZERO; sparse is VIABLE"
                  : "   <- unbound reads UNDEFINED; sparse must stay DISABLED");
  std::printf("  maxStorageBufferRange         : %llu bytes (%.2f GiB)\n",
              (unsigned long long)c.maxStorageBufferRange,
              (double)c.maxStorageBufferRange / (1024.0 * 1024.0 * 1024.0));
  std::printf("  maxMemoryAllocationSize       : %llu bytes (%.2f GiB)\n",
              (unsigned long long)c.maxMemoryAllocationSize,
              (double)c.maxMemoryAllocationSize / (1024.0 * 1024.0 * 1024.0));
  // The plan wants a 4 GiB virtual voxels buffer; say plainly whether one
  // binding can cover it.
  const uint64_t kWant4GiB = 4ull * 1024 * 1024 * 1024;
  std::printf("  -> a single 4 GiB storage binding: %s\n",
              c.maxStorageBufferRange >= kWant4GiB
                  ? "FITS"
                  : "DOES NOT FIT (the voxels buffer must be split or the window capped)");

  std::printf("\n=== compute limits ===\n");
  std::printf("  maxComputeWorkGroupInvocations: %u\n", c.maxComputeWorkGroupInvocations);
  std::printf("  maxComputeWorkGroupSize       : %u x %u x %u\n",
              c.maxComputeWorkGroupSize[0], c.maxComputeWorkGroupSize[1],
              c.maxComputeWorkGroupSize[2]);
  std::printf("  maxComputeWorkGroupCount      : %u x %u x %u\n",
              c.maxComputeWorkGroupCount[0], c.maxComputeWorkGroupCount[1],
              c.maxComputeWorkGroupCount[2]);
  std::printf("  maxBoundDescriptorSets        : %u\n", c.maxBoundDescriptorSets);
  std::printf("  maxPerStageDescStorageBuffers : %u%s\n",
              c.maxPerStageDescriptorStorageBuffers,
              c.maxPerStageDescriptorStorageBuffers > 16
                  ? "   (Dawn's 16-per-stage layout limit does NOT apply here;"
                    " simSlimBGL_ could collapse - separate hash-gated change)"
                  : "");

  std::printf("\n=== alignment (upload path + dynamic offsets) ===\n");
  std::printf("  minStorageBufferOffsetAlign   : %llu\n",
              (unsigned long long)c.minStorageBufferOffsetAlignment);
  std::printf("  minUniformBufferOffsetAlign   : %llu%s\n",
              (unsigned long long)c.minUniformBufferOffsetAlignment,
              c.minUniformBufferOffsetAlignment <= 256
                  ? "   (passUBO's 256 B stride is legal)"
                  : "   <- LARGER THAN passUBO's 256 B stride; the 54 dynamic"
                    " offsets would be misaligned");
  std::printf("  nonCoherentAtomSize           : %llu\n",
              (unsigned long long)c.nonCoherentAtomSize);

  std::printf("\n=== synchronization2 (phase 3b barriers) ===\n");
  std::printf("  synchronization2              : %s%s\n", YesNo(c.synchronization2),
              c.synchronization2
                  ? "   (vkCmdPipelineBarrier2 usable)"
                  : "   <- the generated-barrier recorder CANNOT run");

  std::printf("\n=== measurement / validation ===\n");
  std::printf("  timestampQuery                : %s (period %.4f ns)\n",
              YesNo(c.timestampQuery), c.timestampPeriodNs);
  std::printf("  validation layer available    : %s\n", YesNo(c.validationAvailable));
  std::printf("  validation enabled            : %s\n", YesNo(c.validationEnabled));
  std::printf("  synchronization validation    : %s\n", YesNo(c.syncValidationEnabled));
}

}  // namespace

int RunVkInfo(bool lowPower) {
  // Unbuffered: this mode is diagnostic, and a crash in a driver call must not
  // swallow the lines that say how far it got.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("=== sandvox --vk-info (Vulkan port phase 3a) ===\n");
  bool ok = true;

  // Tuning first: LoadShader bakes tuning constants into every shader's prelude,
  // so they must be live before a single shader is assembled — the same
  // ordering main.cpp uses. Compiling against defaults would compile a source
  // string the engine never actually builds.
  std::string assetDir = AssetDir();
  Tuning tuning;
  if (LoadTuning(assetDir + "/materials/tuning.json", tuning)) SetCurrentTuning(tuning);

  vk::Backend be;
  std::string err;
  // Validation + synchronization validation ON for this mode: it exists to find
  // problems, and it submits so little that the layer cost is irrelevant.
  if (!be.Init(lowPower, /*validation=*/true, /*syncValidation=*/true, err)) {
    std::printf("device init: FAIL (%s)\n", err.c_str());
    std::printf("\n=== --vk-info FAIL ===\n");
    return 1;
  }
  std::printf("device init: OK\n");
  PrintCaps(be.GetCaps());

  // ---- shaders: compile EVERY WGSL file the engine loads ----
  //
  // Assembled exactly as LoadShader does — prelude + tuning + common + body —
  // because a compiler that succeeds on a different string than the engine
  // feeds it has proven nothing.
  std::string shaderDir = assetDir + "/shaders";
  std::string common;
  if (!ReadFileText(shaderDir + "/common.wgsl", common)) {
    std::printf("\nshaders: FAIL (cannot read common.wgsl)\n");
    std::printf("\n=== --vk-info FAIL ===\n");
    return 1;
  }
  std::string prefix =
      ShaderConstantPrelude() + "\n" + TuningWgslBlock(CurrentTuning()) + "\n" + common + "\n";
  // Lines contributed ahead of the body, so a diagnostic points at the file the
  // author edits rather than at line 900 of a concatenation. Same arithmetic
  // scripts/check_shaders.sh does.
  const uint32_t bodyLineOffset = vkspv::CountLines(prefix);

  std::printf("\n=== shaders (WGSL -> SPIR-V via Tint) ===\n");
  std::printf("  prelude+tuning+common = %u lines ahead of every body\n", bodyLineOffset);

  auto compileAll = [&](const PipelineSpec* specs, size_t n, bool makePipeline,
                        std::vector<VkShaderModule>* out) {
    std::string lastFile;
    std::string body;
    for (size_t i = 0; i < n; i++) {
      const PipelineSpec& s = specs[i];
      if (lastFile != s.file) {
        if (!ReadFileText(shaderDir + "/" + s.file, body)) {
          std::printf("  %-24s FAIL (cannot read %s)\n", s.label, s.file);
          ok = false;
          if (out) out->push_back(VK_NULL_HANDLE);
          continue;
        }
        lastFile = s.file;
      }
      std::string src = prefix + body;
      std::string diag;
      VkShaderModule m = be.GetShaderModule(src, s.file, s.entry, bodyLineOffset, diag);
      if (m == VK_NULL_HANDLE) {
        std::printf("  %-24s FAIL\n%s\n", s.label, diag.c_str());
        ok = false;
      } else {
        std::printf("  %-24s OK   (%s :: %s)%s\n", s.label, s.file, s.entry,
                    makePipeline ? "" : "  [compile only, no render pipelines in 3a]");
      }
      if (out) out->push_back(m);
    }
  };

  std::vector<VkShaderModule> computeModules;
  compileAll(kComputePipelines, std::size(kComputePipelines), true, &computeModules);
  compileAll(kRenderShaders, std::size(kRenderShaders), false, nullptr);

  // ---- descriptor set layouts ----
  //
  // The three the sim actually uses, reproduced from Simulation::Init. The
  // dynamic-offset uniform at binding 5 is passUBO's 54 x 256 B windows (27
  // colour phases x 2 substeps) — the one binding whose alignment the capability
  // record above is checked against.
  auto entry = [](uint32_t binding, rhi::BufferBindingType type, bool dynamic = false) {
    rhi::BindGroupLayoutEntry e{};
    e.binding = binding;
    e.visibility = rhi::ShaderStage::Compute;
    e.type = type;
    e.hasDynamicOffset = dynamic;
    return e;
  };
  using T = rhi::BufferBindingType;
  rhi::BindGroupLayoutEntry simEntries[] = {
      entry(0, T::Storage),         entry(1, T::Storage),
      entry(2, T::Storage),         entry(3, T::ReadOnlyStorage),
      entry(4, T::Uniform),         entry(5, T::Uniform, true),  // passUBO, dynamic
      entry(6, T::ReadOnlyStorage), entry(7, T::Storage),
      entry(8, T::Storage),         entry(9, T::Storage),
      entry(10, T::Uniform),        entry(11, T::ReadOnlyStorage),
      entry(12, T::Storage),        entry(13, T::Storage),
      entry(14, T::ReadOnlyStorage), entry(15, T::Storage),
      entry(16, T::ReadOnlyStorage),
  };
  rhi::BindGroupLayoutEntry slimEntries[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::Storage),
      entry(3, T::ReadOnlyStorage), entry(4, T::Uniform),
  };
  rhi::BindGroupLayoutEntry partEntries[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::Storage),
      entry(3, T::Storage), entry(4, T::Storage), entry(5, T::ReadOnlyStorage),
      entry(6, T::Storage), entry(7, T::ReadOnlyStorage),
  };
  // The far-field set, bound as GROUP 1 by worldgen.wgsl's `far`/`fardown`
  // entry points. Getting this wrong is not a soft failure: a pipeline layout
  // that omits a descriptor set the SPIR-V references is undefined behaviour,
  // and on a machine with no validation layer (there is no Vulkan SDK here by
  // design) the NVIDIA ICD faults inside vkCreateComputePipelines rather than
  // returning an error. That is exactly how this was found.
  rhi::BindGroupLayoutEntry farEntries[] = {
      entry(0, T::Storage),          // farVox
      entry(1, T::Storage),          // farOcc
      entry(2, T::ReadOnlyStorage),  // farList
      entry(3, T::Uniform),          // FarParams
      entry(4, T::ReadOnlyStorage),  // dirtyList
  };

  std::printf("\n=== descriptor set layouts + pipeline layouts ===\n");
  VkDescriptorSetLayout simBGL = be.CreateSetLayout(simEntries, std::size(simEntries));
  VkDescriptorSetLayout slimBGL = be.CreateSetLayout(slimEntries, std::size(slimEntries));
  VkDescriptorSetLayout partBGL = be.CreateSetLayout(partEntries, std::size(partEntries));
  VkDescriptorSetLayout farBGL = be.CreateSetLayout(farEntries, std::size(farEntries));
  if (!simBGL || !slimBGL || !partBGL || !farBGL) {
    std::printf("  FAIL (descriptor set layout creation)\n");
    ok = false;
  } else {
    std::printf("  simBGL      OK (%zu bindings, 1 dynamic uniform @5 = passUBO)\n",
                std::size(simEntries));
    std::printf("  simSlimBGL  OK (%zu bindings)\n", std::size(slimEntries));
    std::printf("  particleBGL OK (%zu bindings)\n", std::size(partEntries));
    std::printf("  farBGL      OK (%zu bindings)\n", std::size(farEntries));
  }

  VkPipelineLayout simPL = be.CreatePipelineLayout(&simBGL, 1);
  VkDescriptorSetLayout pair[] = {slimBGL, partBGL};
  VkPipelineLayout simPL2 = be.CreatePipelineLayout(pair, 2);
  // Same shape Simulation::Init builds: the far kernels take the slim group 0
  // plus the far group 1.
  VkDescriptorSetLayout farPair[] = {simBGL, farBGL};
  VkPipelineLayout farPL = be.CreatePipelineLayout(farPair, 2);
  if (!simPL || !simPL2 || !farPL) {
    std::printf("  FAIL (pipeline layout creation)\n");
    ok = false;
  } else {
    std::printf("  simPL OK (1 set)  simPL2 OK (slim+particle)  farPL OK (sim+far)\n");
  }

  // ---- compute pipelines ----
  std::printf("\n=== compute pipelines ===\n");
  int made = 0;
  for (size_t i = 0; i < std::size(kComputePipelines) && i < computeModules.size(); i++) {
    const PipelineSpec& s = kComputePipelines[i];
    if (computeModules[i] == VK_NULL_HANDLE) continue;
    // WHICH LAYOUT: the same split Simulation::BuildPipelines makes, and it is
    // not cosmetic — a pipeline layout must declare every descriptor set the
    // SPIR-V references, or the driver faults (see the farBGL comment above).
    //   far/fardown            -> farPL  (sim group 0 + far group 1)
    //   particle/explode       -> simPL2 (slim group 0 + particle group 1)
    //   everything else        -> simPL  (sim group 0 only)
    std::string file = s.file;
    std::string ep = s.entry;
    bool isFar = file == "worldgen.wgsl" && (ep == "far" || ep == "fardown");
    bool slim = file == "sim_particle.wgsl" || file == "sim_explode.wgsl";
    VkPipelineLayout use = isFar ? farPL : (slim ? simPL2 : simPL);
    // The label is printed BEFORE the call, on an unbuffered stdout. With no
    // validation layer on this machine (no Vulkan SDK, by design) a malformed
    // pipeline description faults inside the ICD instead of returning an error,
    // so the last name printed is the only evidence of which one did it. That
    // is how the far/group-1 layout bug above was located.
    std::printf("  %-24s ", s.label);
    VkPipeline p = be.CreateComputePipeline(use, computeModules[i], s.entry, s.label);
    if (p == VK_NULL_HANDLE) {
      std::printf("FAIL\n");
      ok = false;
    } else {
      std::printf("OK   (%s :: %s)\n", s.file, s.entry);
      made++;
    }
  }
  std::printf("  %d / %zu compute pipelines created\n", made,
              std::size(kComputePipelines));
  if (made != (int)std::size(kComputePipelines)) ok = false;

  // ---- buffers, zero-init, and one fenced submit ----
  //
  // A representative set rather than the whole world: the point is to exercise
  // CreateBuffer -> registry -> ZeroInitAll -> fenced submit, not to allocate
  // 512 MiB in a smoke test.
  std::printf("\n=== buffers + zero-init + fenced submit ===\n");
  struct BufSpec { const char* label; uint64_t size; rhi::BufferUsage usage; };
  const BufSpec bufs[] = {
      {"voxels(sample)", 16u << 20, rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc},
      {"dirty0", (uint64_t)kNumChunks * 4, rhi::BufferUsage::Storage},
      {"dirtyList", (uint64_t)kNumChunks * 4, rhi::BufferUsage::Storage},
      // The five with no CopyDst today — exactly the ones a hand-written
      // zero-init list forgets, which is why the registry exists.
      {"argsStage", 16, rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc},
      {"pArgsStage", 32, rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc},
      {"farOcc", (uint64_t)kNumChunks * 4, rhi::BufferUsage::Storage},
      {"tickUBO", 64, rhi::BufferUsage::Uniform},
      {"passUBO", 54 * 256, rhi::BufferUsage::Uniform},
      {"readback", 1u << 20, rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst},
  };
  std::vector<vk::Buffer*> made_bufs;
  for (const BufSpec& b : bufs) {
    vk::Buffer* p = be.CreateBuffer(b.size, b.usage, b.label);
    if (!p) {
      std::printf("  %-16s FAIL (allocation)\n", b.label);
      ok = false;
    } else {
      std::printf("  %-16s OK  %8llu B%s\n", b.label, (unsigned long long)b.size,
                  p->mapped ? "  (persistently mapped)" : "");
    }
    made_bufs.push_back(p);
  }

  if (!be.ZeroInitAll(err)) {
    std::printf("  ZeroInitAll: FAIL (%s)\n", err.c_str());
    ok = false;
  } else {
    std::printf("  ZeroInitAll: OK (vkCmdFillBuffer over every registered buffer,"
                " one fenced submit)\n");
  }

  // The readback buffer is host-visible and was just zero-filled on the GPU, so
  // reading it back verifies the fill actually happened AND that a fence-waited
  // submit makes device writes visible to the host — the readback path's whole
  // contract, tested without a readback ring existing yet.
  if (!made_bufs.empty()) {
    vk::Buffer* rb = made_bufs.back();
    if (rb && rb->mapped) {
      const uint32_t* words = (const uint32_t*)rb->mapped;
      bool allZero = true;
      for (size_t i = 0; i < 4096; i++)
        if (words[i] != 0) allZero = false;
      std::printf("  host-visible readback after fill: %s\n",
                  allZero ? "all zero (fill + fence visibility CONFIRMED)"
                          : "NON-ZERO -- zero-init or host visibility is broken");
      if (!allZero) ok = false;
    }
  }

  // One EMPTY fenced command buffer, submitted and waited. Trivial, and it is
  // the thing phase 3b builds on: if this cannot round-trip, nothing recorded
  // later can either.
  VkCommandBuffer cmd = be.BeginCommands("empty");
  if (cmd == VK_NULL_HANDLE) {
    std::printf("  empty submit: FAIL (could not begin)\n");
    ok = false;
  } else if (be.SubmitCommands(cmd, err) == VK_NULL_HANDLE) {
    std::printf("  empty submit: FAIL (%s)\n", err.c_str());
    ok = false;
  } else if (!be.WaitIdle(err)) {
    std::printf("  empty submit: FAIL (wait: %s)\n", err.c_str());
    ok = false;
  } else {
    std::printf("  empty submit: OK (submitted with a fence, waited, retired)\n");
  }

  // ---- upload path ----
  //
  // Both classes, through the pending queue, flushed by the next BeginCommands.
  // Class A is a small 4-aligned payload (vkCmdUpdateBuffer, captured at record
  // time); Class B exceeds 65536 B and rides the staging ring.
  std::printf("\n=== upload path (barrier_graph 4.1) ===\n");
  std::vector<uint32_t> small(16, 0xA5A5A5A5u);
  std::vector<uint32_t> big(40000, 0x5A5A5A5Au);  // 160000 B > 65536 -> Class B
  if (made_bufs.size() > 6 && made_bufs[6]) be.QueueWrite(made_bufs[6], 0, small.data(), small.size() * 4);
  if (made_bufs[0]) be.QueueWrite(made_bufs[0], 0, big.data(), big.size() * 4);
  std::printf("  queued %zu uploads (1 Class A <= 65536 B, 1 Class B via staging ring)\n",
              be.PendingUploadCount());
  VkCommandBuffer up = be.BeginCommands("uploads");
  bool drained = be.PendingUploadCount() == 0;
  std::printf("  flushed at the head of the next command buffer: %s\n",
              drained ? "OK (queue drained)" : "FAIL (queue not drained)");
  if (!drained) ok = false;
  if (up != VK_NULL_HANDLE) {
    if (be.SubmitCommands(up, err) == VK_NULL_HANDLE || !be.WaitIdle(err)) {
      std::printf("  upload submit: FAIL (%s)\n", err.c_str());
      ok = false;
    } else {
      std::printf("  upload submit: OK\n");
    }
  }

  // ---- validation verdict ----
  const auto& msgs = be.ValidationMessages();
  std::printf("\n=== validation ===\n");
  if (msgs.empty()) {
    std::printf("  no errors or warnings\n");
  } else {
    // Validation messages are reported but do NOT by themselves fail the run:
    // phase 3a records no barriers, so any synchronization finding here would
    // be about work that does not exist yet. Phase 3b makes these fatal.
    std::printf("  %zu message(s):\n", msgs.size());
    for (const auto& m : msgs) std::printf("    %s\n", m.c_str());
  }

  std::printf("\n=== --vk-info %s ===\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

}  // namespace sandvox
