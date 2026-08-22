#include "gpu/vk_spirv.h"

#include <cstdio>
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

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vkspv {
namespace {

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
  r.ok = true;
  return r;
}

}  // namespace vkspv
