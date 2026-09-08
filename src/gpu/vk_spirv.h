// vk_spirv.h — WGSL -> SPIR-V, in process, via Tint.
//
// WHY IN-PROCESS AND NOT OFFLINE
// ------------------------------
// WGSL stays the single shader source of truth for the port
// (docs/PLAN_vulkan_port.md, "Shader strategy"). The alternative — running
// tint.exe at build time and shipping .spv files — would work for a static
// build and cannot work for F5. `LoadShader` reassembles the prelude from LIVE
// tuning values on every load, which is exactly what makes F5 re-tune the
// renderer without a rebuild; a precompiled blob freezes those constants at
// build time. So the compiler has to be a library call.
//
// The string compiled here is byte-for-byte the one LoadShader assembles:
//
//     ShaderConstantPrelude() + "\n" + TuningWgslBlock(...) + "\n"
//         + common.wgsl + "\n" + <body>
//
// which is also what scripts/check_shaders.sh reproduces. Three consumers of one
// concatenation is already two too many; if a fourth appears, hoist it.
//
// DIAGNOSTIC LINE NUMBERS. Tint reports lines in the COMBINED source, where the
// body starts hundreds of lines in. Reporting those raw sends the reader to a
// line that does not exist in the file they edited. `Compile` takes the number
// of lines contributed ahead of the body and rewrites diagnostics the same way
// check_shaders.sh's awk pass does: below the offset it is a common.wgsl/prelude
// problem and is labelled so; at or above it, subtract and point at the body.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkspv {

struct CompileResult {
  bool ok = false;
  std::vector<uint32_t> spirv;
  // Diagnostics with line numbers already remapped to the authored files.
  std::string diagnostics;
};

// Compile one WGSL module to SPIR-V.
//
// `wgsl`         the fully assembled source (prelude + tuning + common + body)
// `label`        the body's file name, used in diagnostics
// `entryPoint`   the entry point to generate; Tint emits a single-entry module
// `bodyLineOffset` number of lines ahead of the body in `wgsl`; 0 disables
//                remapping (diagnostics then carry combined-source lines).
CompileResult Compile(const std::string& wgsl, const std::string& label,
                      const std::string& entryPoint, uint32_t bodyLineOffset);

// Count the lines a prefix contributes, for `bodyLineOffset`. Kept next to
// Compile so the two cannot disagree about whether a trailing newline counts.
uint32_t CountLines(const std::string& s);

// SPIR-V POST-PROCESSING (`SANDVOX_SPIRV_OPT`).
//
// Tint hands the driver unoptimized SPIR-V: every WGSL local is an OpVariable
// in the Function storage class with load/store traffic around it, and every
// helper is a real OpFunctionCall. On the NVIDIA ICD the front end that has to
// undo all of that is where a cold `vkCreateComputePipelines` on
// worldgen.wgsl's `far` entry spends ~750 s (docs/PLAN_shader_compile.md).
// Running SPIRV-Tools' optimizer in between is the experiment: same semantics,
// far fewer instructions to chew through.
//
// Measured 2026-09-07 (two cold arms, table in vk_spirv.cpp): worldgen `far`
// 622 s -> 171 s, whole worldgen set 861 s -> 299 s, same world hash both ways.
// So it is ON BY DEFAULT and the env var is an opt-OUT.
//
// Values: unset / "1" / "perf" = RegisterPerformancePasses (the default).
// "0" / "off" = disabled, the raw Tint blob. "legal" = the LunarG legalization
// recipe (inline-exhaustive, private-to-local, scalar replacement, SSA rewrite,
// simplify, aggressive DCE). Anything the optimizer refuses falls through to the
// unoptimized blob with a stderr note — this must never be able to break a boot.
//
// `OptimizerCacheTag()` is 0 when disabled and a small non-zero id per recipe
// otherwise. The SPIR-V disk cache MUST mix it into its key: the cache
// otherwise keys on WGSL source alone and an optimized blob written by one run
// would be served to a run that asked for the unoptimized one (and vice versa),
// which would silently corrupt any A/B of the two.
uint32_t OptimizerCacheTag();

}  // namespace vkspv
