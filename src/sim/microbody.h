#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/voxload.h"
#include "sim/world.h"

// Dynamic microvoxel BODIES (docs/PLAN_voxel_editor.md §C, DESIGN.md §9).
//
// A mob def may declare `"scale": 2` (or 4): its limb .vox models are authored
// at that many voxels per WORLD voxel, so a limb whose .vox box is 6x2x2 at
// scale 2 occupies 3x1x1 world voxels. Physics builds the same limb with a
// voxel pitch of 1/scale, so the creature is the same physical size as a
// scale-1 one with a coarser skin — the extra resolution buys detail, not bulk.
//
// RENDERING is where they diverge. The cube path (debris.wgsl `vsBody`) emits
// one 36-vertex cube per voxel; at scale 4 that is 64x the instances for the
// same silhouette, which is exactly the wrong trade. Instead each micro body
// rasterizes ONE box — its oriented bounding box, 36 vertices total — and the
// fragment shader marches the limb's brick in object space. Cost then scales
// with SCREEN AREA rather than with voxel count, which is what makes 4x
// resolution affordable (rule 2: cost tracks activity, not content size).
//
// DETERMINISM (rule 1): everything here is render-only, exactly like the static
// micro pool. The pool and the tables are bound to the microbody render
// pipeline and to nothing else; the sim never sees them, and the voxels never
// change after load (damage does not edit a limb's micro model in v1, so there
// is no copy-on-write and no per-instance storage at all).

// ---- GPU layout ------------------------------------------------------------

// One limb model, 16 bytes — must match struct MicroBodyModel in common.wgsl.
struct MicroBodyModelGpu {
  // Word index into the pool where this model's payload starts. The payload is
  // dims.x*dims.y*dims.z micro voxels, 4 packed 8-bit palette indices per word,
  // x-major then y then z (idx = (z*dy + y)*dx + x), padded to a whole word.
  //
  // Palette index == material ID (the project-wide .vox convention), so a micro
  // voxel shades through the ordinary material table. 8 bits caps a micro body
  // at material ids 1..255, checked at load.
  uint32_t base;
  // bits 0..9 dims.x, bits 10..19 dims.y, bits 20..29 dims.z (micro voxels).
  // 10 bits each is 1023 per axis, far past the +-127 DebrisVoxel bound that
  // limits a limb anyway; packing them keeps the record at 16 bytes.
  uint32_t dims;
  uint32_t scale;  // micro voxels per world voxel: 2 or 4
  uint32_t _pad;   // padding to 16 bytes; no flag bits are defined
};
static_assert(sizeof(MicroBodyModelGpu) == 16,
              "must match common.wgsl MicroBodyModel");

// One micro body to draw this frame, 16 bytes — must match MicroBodyInst in
// microbody.wgsl. COMPACTED: the draw's instance count is exactly the number
// of micro bodies on screen, so a world with none dispatches no vertices at
// all (rule 2). Slot indexes the shared bodyXforms buffer, which both the cube
// pass and this one read — the two passes partition the slots, never duplicate.
struct MicroBodyInstGpu {
  uint32_t slot;
  uint32_t model;
  uint32_t pad0 = 0, pad1 = 0;
};
static_assert(sizeof(MicroBodyInstGpu) == 16,
              "must match microbody.wgsl MicroBodyInst");

// ---- CPU side --------------------------------------------------------------

// The pool plus the per-model records. Built at mob-def load, uploaded once,
// shared by every instance of every def (voxels never change post-load).
struct MicroBodySet {
  std::vector<MicroBodyModelGpu> models;
  std::vector<uint32_t> pool;
};

// Packs one limb's voxel model into `set` and returns its model index, or -1 on
// failure (pool full, out-of-range material, empty model). `voxels` are in
// MICRO units with a min corner already at 0 (PrefabModel-local coords).
//
// `log` collects diagnostics. A limb that fails to pack does NOT fall back to
// the cube path — cube instances are one WORLD voxel each, so a scale-2 limb
// drawn that way would be twice its real size — it simply does not render,
// which the loader says out loud. A broken limb must not stop the mob from
// loading (DESIGN.md §6).
int MicroBodyPack(MicroBodySet& set, const std::vector<PrefabVoxel>& voxels,
                  IVec3 dims, uint32_t scale, const std::string& label,
                  std::string& log);

// A body's micro rendering, passed along when ownership of the body moves.
//
// This travels as an explicit argument (MobSystem hands it to
// DebrisSystem::AdoptBody) rather than living in a side table keyed by physics
// handle. A side table would have to be kept in sync at every spawn, sever,
// death, cull and reset — five agreements enforced by nothing — and a recycled
// Jolt BodyID could then paint unrelated debris as somebody's leg. As a
// parameter the invariant "this description belongs to that body" is not
// merely maintained, it is unrepresentable-if-violated.
//
// The routing key is still the BODY, not "is a mob limb": that is what makes a
// severed micro limb keep its detail with no special case at the adoption site.
struct MicroBodyRef {
  // kMicroBodyNoModel = ordinary cube path (plain debris, scale-1 limbs).
  uint32_t model = kMicroBodyNoModel;
  uint32_t scale = 1;  // micro voxels per world voxel; the body's voxels are in these
  bool Valid() const { return model != kMicroBodyNoModel; }
};
