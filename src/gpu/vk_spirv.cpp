#include "gpu/vk_spirv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

// Tint's headers are noisy under MSVC's default warning level and are not ours
// to fix; the project itself builds clean.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#pragma warning(disable : 4127)
#pragma warning(disable : 4244)
#pragma warning(disable : 4267)
#pragma warning(disable : 4324)
#endif

#include "src/tint/lang/spirv/writer/writer.h"
#include "src/tint/lang/wgsl/reader/reader.h"
#include "src/tint/utils/diagnostic/formatter.h"

#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vkspv {
namespace {

// Which optimizer recipe `SANDVOX_SPIRV_OPT` selected. Read once: an env var
// that changes meaning between two pipelines of the same boot would make the
// disk-cache tag a lie.
enum class OptRecipe : uint32_t { kOff = 0, kPerformance = 1, kLegalization = 2 };

OptRecipe ReadOptRecipe() {
  const char* v = std::getenv("SANDVOX_SPIRV_OPT");
  // DEFAULT ON since 2026-09-07, on the measurement in
  // docs/PLAN_shader_compile.md package B: two cold arms, fresh CWD each (which
  // is what makes both the SPIR-V disk cache and the driver's pipeline cache
  // cold), --frames 60, SANDVOX_SHADER_TIMING=1.
  //
  //   entry              off        on   (on includes the optimizer's own time)
  //   worldgen main    84.3 s    37.4 s
  //   worldgen list    69.2 s    36.2 s
  //   pagefill          0.09 s    0.12 s
  //   far             622.1 s   170.7 s     -72.6 %
  //   fardown          85.6 s    54.7 s
  //   ---------------------------------
  //   worldgen        861.2 s   299.0 s     -65.3 %
  //   all 58 compute  864.2 s   318.4 s
  //   render pipes     32.9 s    29.5 s
  //
  // The counter-intuitive part, recorded so nobody "fixes" it: the optimizer
  // makes the module MUCH BIGGER (far: 66,416 -> 1,607,872 words, because
  // inline-exhaustive expands genColumn's several call sites), and the driver
  // still compiles it 3.6x faster. What is expensive on the NVIDIA ICD is the
  // front-end work of inlining and promoting Tint's function-scope var
  // load/store soup, not the instruction count — so doing that work once in
  // SPIRV-Tools beats making the driver redo it per entry point.
  //
  // Determinism: the `determinism` gate produced the SAME world hash with the
  // optimizer off and on (ef83fd47 both), so this is not a hashed-state change.
  //
  // Opt OUT with SANDVOX_SPIRV_OPT=0 — which is also the arm to try first if a
  // driver ever miscompiles an optimized module.
  if (!v || !*v) return OptRecipe::kPerformance;
  if (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0) return OptRecipe::kOff;
  if (std::strcmp(v, "legal") == 0) return OptRecipe::kLegalization;
  return OptRecipe::kPerformance;  // "1", "perf", anything else truthy
}

OptRecipe CurrentRecipe() {
  static const OptRecipe r = ReadOptRecipe();
  return r;
}

bool TimingOn() {
  static const bool on = std::getenv("SANDVOX_SHADER_TIMING") != nullptr;
  return on;
}

// The SPIR-V environment. Tint's writer defaults to `SpvVersion::kSpv13`
// (spirv/writer/common/options.h) and vk_spirv.cpp does not override it, so the
// modules are SPIR-V 1.3 — which is Vulkan 1.1's dialect. Telling the optimizer
// Vulkan 1.3 would let it emit instructions this module's header does not claim
// a version for; telling it 1.0 would make the validator reject legal 1.3
// constructs. If `opts.spirv_version` above ever changes, change this with it.
constexpr spv_target_env kSpvEnv = SPV_ENV_VULKAN_1_1;

// Run SPIRV-Tools over `spirv` in place. Returns true only if the optimizer
// produced a module that still validates; on ANY failure the caller keeps the
// blob Tint produced. The optimizer is an experiment and must not be able to
// take a boot down with it.
bool Optimize(std::vector<uint32_t>& spirv, OptRecipe recipe, const std::string& label,
              const std::string& entryPoint) {
  const auto t0 = std::chrono::steady_clock::now();
  const size_t before = spirv.size();

  std::string msgs;
  auto consumer = [&msgs](spv_message_level_t level, const char*, const spv_position_t&,
                          const char* m) {
    if (level == SPV_MSG_ERROR || level == SPV_MSG_INTERNAL_ERROR ||
        level == SPV_MSG_FATAL) {
      msgs += m ? m : "";
      msgs += "\n";
    }
  };

  spvtools::Optimizer opt(kSpvEnv);
  opt.SetMessageConsumer(consumer);
  if (recipe == OptRecipe::kLegalization) {
    // LunarG's white-paper recipe, in their order: inline everything so the
    // scalar passes can see through helper boundaries, hoist Private vars into
    // functions, break aggregates apart, then mem2reg (SSA rewrite) and clean
    // up. This is the shape that turns Tint's load/store soup into values.
    opt.RegisterPass(spvtools::CreateInlineExhaustivePass())
        .RegisterPass(spvtools::CreatePrivateToLocalPass())
        .RegisterPass(spvtools::CreateScalarReplacementPass())
        .RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass())
        .RegisterPass(spvtools::CreateLocalAccessChainConvertPass())
        .RegisterPass(spvtools::CreateSSARewritePass())
        .RegisterPass(spvtools::CreateSimplificationPass())
        .RegisterPass(spvtools::CreateDeadBranchElimPass())
        .RegisterPass(spvtools::CreateBlockMergePass())
        .RegisterPass(spvtools::CreateAggressiveDCEPass(/*preserve_interface=*/true));
  } else {
    // `preserve_interface = true`: the engine's descriptor set layouts are
    // built from the WGSL's own @group/@binding decorations (see Compile()
    // below), and a pass that drops an "unused" interface variable would make
    // the module and the layout disagree about what set 0 contains.
    opt.RegisterPerformancePasses(/*preserve_interface=*/true);
  }

  spvtools::OptimizerOptions oopts;
  // The validator runs separately below so a failure is reportable as
  // "optimizer output is invalid" rather than a bare false from Run().
  oopts.set_run_validator(false);

  std::vector<uint32_t> out;
  if (!opt.Run(spirv.data(), spirv.size(), &out, oopts) || out.empty()) {
    std::fprintf(stderr, "spirv-opt: %s:%s FAILED, using unoptimized SPIR-V%s%s\n",
                 label.c_str(), entryPoint.c_str(), msgs.empty() ? "" : "\n",
                 msgs.c_str());
    return false;
  }

  spvtools::SpirvTools tools(kSpvEnv);
  tools.SetMessageConsumer(consumer);
  if (!tools.Validate(out)) {
    std::fprintf(stderr,
                 "spirv-opt: %s:%s output failed validation, using unoptimized "
                 "SPIR-V\n%s",
                 label.c_str(), entryPoint.c_str(), msgs.c_str());
    return false;
  }

  spirv = std::move(out);
  if (TimingOn()) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    std::printf("spirv-opt %-16s %-10s %8zu -> %8zu words  %8.1f ms\n", label.c_str(),
                entryPoint.c_str(), before, spirv.size(), ms);
    std::fflush(stdout);
  }
  return true;
}

// Rewrite "<something>:LINE:COL: kind: message" so LINE points into the file the
// author can actually edit.
//
// This mirrors the awk pass in scripts/check_shaders.sh, deliberately: a
// diagnostic that reads differently depending on whether you found it through
// the hook or through the engine is a diagnostic people learn to distrust. Below
// the offset the text came from the generated prelude or common.wgsl, and saying
// so is the whole value — "line 412 of your 60-line shader" is what this exists
// to prevent.
std::string RemapLines(const std::string& raw, const std::string& label,
                       uint32_t offset) {
  if (offset == 0) return raw;
  std::ostringstream out;
  std::istringstream in(raw);
  std::string line;
  while (std::getline(in, line)) {
    // Find the LAST ":<digits>:<digits>" run that is followed by ':' or space —
    // scanning from the left would hit the "C:" of a Windows drive letter.
    size_t best = std::string::npos;
    for (size_t i = 0; i + 1 < line.size(); i++) {
      if (line[i] != ':' || !std::isdigit((unsigned char)line[i + 1])) continue;
      size_t j = i + 1;
      while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
      if (j >= line.size() || line[j] != ':') continue;
      size_t k = j + 1;
      while (k < line.size() && std::isdigit((unsigned char)line[k])) k++;
      if (k == j + 1) continue;  // no column digits
      best = i;
      break;
    }
    if (best == std::string::npos) {
      out << line << "\n";
      continue;
    }
    size_t j = best + 1;
    while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
    long lineno = std::strtol(line.substr(best + 1, j - best - 1).c_str(), nullptr, 10);
    std::string tail = line.substr(j);  // ":COL: kind: message"
    if ((uint32_t)lineno > offset) {
      out << label << ":" << (lineno - (long)offset) << tail << "\n";
    } else {
      // Prelude/common.wgsl territory: keep the combined-source line, but say
      // where it came from so nobody hunts for it in the body.
      out << "<prelude+common>:" << lineno << tail
          << "  (generated prelude or common.wgsl)\n";
    }
  }
  return out.str();
}

}  // namespace

uint32_t CountLines(const std::string& s) {
  uint32_t n = 0;
  for (char c : s)
    if (c == '\n') n++;
  return n;
}

CompileResult Compile(const std::string& wgsl, const std::string& label,
                      const std::string& entryPoint, uint32_t bodyLineOffset) {
  CompileResult r;

  // 1. WGSL -> AST program. Parse errors surface here with source locations.
  tint::Source::File file(label, wgsl);
  tint::Program program = tint::wgsl::reader::Parse(&file);
  if (!program.IsValid()) {
    tint::diag::Formatter formatter;
    r.diagnostics = RemapLines(formatter.Format(program.Diagnostics()).Plain(), label,
                               bodyLineOffset);
    return r;
  }

  // 2. AST -> lowered core IR. This is the step tint.exe performs between
  //    parsing and any backend writer (see its Generate()); the SPIR-V writer
  //    takes IR, not a Program.
  auto ir = tint::wgsl::reader::ProgramToLoweredIR(program);
  if (ir != tint::Success) {
    r.diagnostics = RemapLines(ir.Failure().reason, label, bodyLineOffset);
    return r;
  }

  // 3. IR -> SPIR-V.
  tint::spirv::writer::Options opts;
  // Generate exactly this entry point. Our shaders declare one @compute
  // function each, but naming it explicitly means a module that later grows a
  // second entry point still produces the pipeline we asked for rather than
  // whichever Tint picked.
  opts.entry_point_name = entryPoint;

  // Binding points pass through UNCHANGED. tint.exe calls GenerateBindings() to
  // invent a flat binding layout because it has no pipeline layout to satisfy;
  // we do, and the WGSL's own @group/@binding decorations are exactly what the
  // descriptor set layouts in rhi_vulkan.cpp are built from. Remapping them
  // here would mean two independent sources of truth for binding numbers — the
  // "two places that must agree" bug the repo has an invariant checker for.
  // Leaving `opts.bindings` default is what preserves the authored numbers.

  // Robustness ON (the default). Dawn clamps out-of-bounds accesses too, so
  // leaving it on is what keeps the two backends' behaviour identical on a
  // buggy index — and under rule 1 an out-of-bounds read that differs between
  // backends is a determinism divergence, not merely a crash risk.

  auto spv = tint::spirv::writer::Generate(ir.Get(), opts);
  if (spv != tint::Success) {
    r.diagnostics = RemapLines(spv.Failure().reason, label, bodyLineOffset);
    return r;
  }

  r.spirv = std::move(spv.Get().spirv);

  // 4. (optional) SPIRV-Tools. Never fatal: Optimize() leaves r.spirv alone and
  //    says so on stderr if anything goes wrong.
  if (OptRecipe recipe = CurrentRecipe(); recipe != OptRecipe::kOff)
    Optimize(r.spirv, recipe, label, entryPoint);

  r.ok = true;
  return r;
}

uint32_t OptimizerCacheTag() { return (uint32_t)CurrentRecipe(); }

}  // namespace vkspv
