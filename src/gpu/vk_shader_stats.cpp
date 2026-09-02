// vk_shader_stats.cpp — `--shader-stats`. See vk_shader_stats.h for why.

#include "gpu/vk_shader_stats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"

namespace sandvox {
namespace {

// Case-insensitive substring test. Used ONLY for the sort key and the
// highlight, never to decide whether to print a statistic.
bool ContainsNoCase(const std::string& hay, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0 || hay.size() < n) return false;
  for (size_t i = 0; i + n <= hay.size(); i++) {
    size_t k = 0;
    while (k < n && std::tolower((unsigned char)hay[i + k]) ==
                        std::tolower((unsigned char)needle[k]))
      k++;
    if (k == n) return true;
  }
  return false;
}

// THE ONE NUMBER THIS MODE EXISTS FOR: how much of this executable's working
// set fell out of registers. Vendors name it differently — NVIDIA reports
// "Local Memory Bytes" / "Spill Count", AMD's ISA path reports "Scratch Memory
// Size", Intel reports "Spill Count" — so the key is the largest value among
// statistics whose NAME contains local / spill / scratch, and 0 when the driver
// reports nothing of the kind. That is a sort key, not a truth: a driver with
// other names still prints every counter it has, just unsorted.
bool IsPressureName(const std::string& n) {
  return ContainsNoCase(n, "local") || ContainsNoCase(n, "spill") ||
         ContainsNoCase(n, "scratch");
}

// THE DEVICE FLOOR, and why a readout that only printed raw values would be
// useless on the hardware this repo actually runs.
//
// Measured on an RTX 3060 Ti: every executable reports
// `Local Memory Size = 0x10'00000000 + n`, including a 16-register, 640-byte
// kernel that cannot possibly touch 64 GiB. The constant high dword is a tag
// the driver puts in the value, not magnitude — the differences (0, 64, 144,
// 256) are the bytes. It is NOT this file's business to know that about NVIDIA,
// so the correction is derived from the data instead of hardcoded: for each
// statistic NAME, the smallest value any executable reports is the floor, and
// the headline reports the EXCESS over it.
//
// On a driver that reports honest zeros the floor is 0 and this is the identity
// function, which is the property that makes it safe to apply everywhere. The
// table above still prints every value verbatim; only the summary subtracts,
// and it names the floor it subtracted so nothing is hidden.
std::map<std::string, double> StatFloors(
    const std::vector<vk::PipelineExecutable>& rows) {
  std::map<std::string, double> floors;
  for (const vk::PipelineExecutable& e : rows)
    for (const vk::PipelineStat& s : e.stats) {
      if (!IsPressureName(s.name)) continue;
      auto it = floors.find(s.name);
      if (it == floors.end())
        floors.emplace(s.name, s.value);
      else
        it->second = std::min(it->second, s.value);
    }
  return floors;
}

const vk::PipelineStat* PressureStat(const vk::PipelineExecutable& e) {
  const vk::PipelineStat* worst = nullptr;
  for (const vk::PipelineStat& s : e.stats)
    if (IsPressureName(s.name))
      if (!worst || s.value > worst->value) worst = &s;
  return worst;
}

// How far above the device floor this executable's worst spill statistic sits.
// 0 means "fits in registers"; the sort key and the summary both use it.
double PressureKey(const vk::PipelineExecutable& e,
                   const std::map<std::string, double>& floors) {
  double worst = 0.0;
  for (const vk::PipelineStat& s : e.stats) {
    if (!IsPressureName(s.name)) continue;
    auto it = floors.find(s.name);
    const double base = it == floors.end() ? 0.0 : it->second;
    worst = std::max(worst, s.value - base);
  }
  return worst;
}

// Numbers as the driver meant them: integers stay integers (a register count
// printed as 128.000000 is noise), everything else keeps three decimals.
std::string FormatValue(const vk::PipelineStat& s) {
  char buf[64];
  if (s.isBool) return s.value != 0.0 ? "YES" : "no";
  if (s.value == (double)(long long)s.value && std::abs(s.value) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%lld", (long long)s.value);
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "%.3f", s.value);
  return buf;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20)
          o += ' ';
        else
          o += c;
    }
  }
  return o;
}

void WriteJson(const std::string& path,
               const std::vector<vk::PipelineExecutable>& rows,
               const std::map<std::string, double>& floors) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "shader-stats: could not write %s\n", path.c_str());
    return;
  }
  std::fprintf(f, "[\n");
  for (size_t i = 0; i < rows.size(); i++) {
    const vk::PipelineExecutable& e = rows[i];
    std::fprintf(f,
                 "  {\"pipeline\": \"%s\", \"stage\": \"%s\", \"executable\": \"%s\",\n"
                 "   \"subgroupSize\": %u, \"pressure\": %.6g, \"stats\": {",
                 JsonEscape(e.pipeline).c_str(), JsonEscape(e.stage).c_str(),
                 JsonEscape(e.name).c_str(), e.subgroupSize, PressureKey(e, floors));
    for (size_t k = 0; k < e.stats.size(); k++)
      std::fprintf(f, "%s\"%s\": %.6g", k ? ", " : "",
                   JsonEscape(e.stats[k].name).c_str(), e.stats[k].value);
    std::fprintf(f, "}}%s\n", i + 1 < rows.size() ? "," : "");
  }
  std::fprintf(f, "]\n");
  std::fclose(f);
  std::printf("\nwrote %s (%zu executables)\n", path.c_str(), rows.size());
}

}  // namespace

int RunShaderStats(const rhi::Device& device, const std::string& jsonPath) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  vk::Backend* be = rhi::vkr::NativeBackend(device);
  if (!be) {
    std::printf("--shader-stats requires the Vulkan backend\n");
    return 2;
  }
  if (!be->GetCaps().pipelineExecutableProps) {
    // Not a failure of this code and not something a rerun fixes: the driver
    // simply will not tell us. Say it in one line and get out with a distinct
    // exit code so a caller can tell "unsupported" from "broken".
    std::printf(
        "--shader-stats: this driver does not support "
        "VK_KHR_pipeline_executable_properties (%s).\n"
        "No register / spill counts are available here; the renderer's cost "
        "model stays a guess on this device.\n",
        be->GetCaps().deviceName.c_str());
    return 2;
  }
  if (!be->CaptureStats()) {
    // Belt and braces: without the create flag every query returns zero
    // executables and the table would be a silent, empty lie.
    std::printf(
        "--shader-stats: capture was not armed before pipeline creation; "
        "nothing to report\n");
    return 2;
  }

  std::vector<vk::PipelineExecutable> rows = be->CollectPipelineStats();
  if (rows.empty()) {
    std::printf(
        "--shader-stats: the driver reported no pipeline executables (%s)\n",
        be->GetCaps().deviceName.c_str());
    return 2;
  }

  const std::map<std::string, double> floors = StatFloors(rows);

  // Worst register pressure first. std::stable_sort so pipelines the driver
  // says nothing about (key 0) stay in creation order, which reads like the
  // build does.
  std::stable_sort(rows.begin(), rows.end(),
                   [&floors](const vk::PipelineExecutable& a,
                             const vk::PipelineExecutable& b) {
                     return PressureKey(a, floors) > PressureKey(b, floors);
                   });

  size_t wPipe = 8, wStage = 5, wExec = 10;
  for (const vk::PipelineExecutable& e : rows) {
    wPipe = std::max(wPipe, e.pipeline.size());
    wStage = std::max(wStage, e.stage.size());
    wExec = std::max(wExec, e.name.size());
  }

  std::printf("=== sandvox --shader-stats (%s) ===\n", be->GetCaps().deviceName.c_str());
  std::printf(
      "Sorted by the largest local/spill/scratch statistic the driver reports,\n"
      "measured above that statistic's device-wide floor (see the summary).\n"
      "Statistic names and values are the DRIVER'S, printed verbatim — this "
      "mode knows no vendor's counter set.\n\n");
  std::printf("%-*s  %-*s  %-*s  %s\n", (int)wPipe, "pipeline", (int)wStage, "stage",
              (int)wExec, "executable", "statistics");
  std::printf("%-*s  %-*s  %-*s  %s\n", (int)wPipe, std::string(wPipe, '-').c_str(),
              (int)wStage, std::string(wStage, '-').c_str(), (int)wExec,
              std::string(wExec, '-').c_str(), "----------");

  for (const vk::PipelineExecutable& e : rows) {
    std::string cells;
    for (size_t k = 0; k < e.stats.size(); k++) {
      if (k) cells += ", ";
      cells += e.stats[k].name;
      cells += "=";
      cells += FormatValue(e.stats[k]);
    }
    if (e.stats.empty()) cells = "(driver reported no statistics)";
    std::printf("%-*s  %-*s  %-*s  %s\n", (int)wPipe, e.pipeline.c_str(), (int)wStage,
                e.stage.c_str(), (int)wExec, e.name.c_str(), cells.c_str());
  }

  // The headline, because a 74-row table buries it. Anything with a non-zero
  // local/spill/scratch number is a shader that ran out of fast registers and
  // is paying memory latency for the overflow.
  std::printf("\n--- spilling to memory ---\n");
  for (const auto& kv : floors)
    if (kv.second != 0.0)
      std::printf(
          "  (this driver reports a constant floor of %.0f for '%s' on EVERY "
          "executable — a tag, not magnitude; the excess over it is shown)\n",
          kv.second, kv.first.c_str());
  bool any = false;
  for (const vk::PipelineExecutable& e : rows) {
    const double k = PressureKey(e, floors);
    if (k <= 0.0) continue;
    const vk::PipelineStat* s = PressureStat(e);
    any = true;
    std::printf("  %-*s  %-*s  %s +%.0f\n", (int)wPipe, e.pipeline.c_str(), (int)wStage,
                e.stage.c_str(), s ? s->name.c_str() : "?", k);
  }
  if (!any)
    std::printf("  none — every executable fits in registers on this device\n");

  WriteJson(jsonPath, rows, floors);
  return 0;
}

}  // namespace sandvox
