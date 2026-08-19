#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Material classes — must match common.wgsl.
enum MatClass : uint32_t {
  CLASS_SOLID = 0,
  CLASS_POWDER = 1,
  CLASS_LIQUID = 2,
  CLASS_GAS = 3,
};

// GPU-side layout, 32 bytes — must match struct Material in common.wgsl.
struct MaterialGpu {
  uint32_t klass;
  int32_t density;
  uint32_t color0, color1, color2;  // RGBA8
  uint32_t decayPerMille;
  uint32_t flags;
  uint32_t pad;
};

struct MaterialDef {
  std::string name;
  MaterialGpu gpu{};
  std::vector<std::string> tags;  // validated now, consumed by reactions at M3
};

// Loads materials.json. Returns false (with errors filled) on validation
// failure — modders get diagnostics, not silent breakage (DESIGN.md §6).
// Index in the returned vector == 12-bit material ID; slot 0 is air.
bool LoadMaterials(const std::string& path, std::vector<MaterialDef>& out,
                   std::string& errors);
