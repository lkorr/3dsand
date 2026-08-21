#include "audio/occlusion.h"

#include <algorithm>
#include <cmath>

#include "sim/world.h"

namespace audio {
namespace {

// Ray step, in voxels. The ray is a sampler, not a solver: it asks "how much
// material is on this line", and 1 voxel is the natural quantum of that
// question. Half-voxel steps would double the cost to resolve detail finer
// than the world itself has.
constexpr float kStepVox = 1.0f;

// Hard cap on samples per ray, independent of maxRayM. At 0.10 m voxels a 40 m
// ray is 400 steps; this is the backstop that keeps a mis-set tuning value from
// turning one voice into a millisecond of CPU.
constexpr int kMaxSteps = 448;

// Frequency the broadband gain is evaluated at, and the open cutoff. 20 kHz is
// the "no filter" sentinel Voice::ApplyOcclusion bypasses on.
constexpr float kOpenCutoffHz = 20000.0f;

}  // namespace

OcclusionResult ComputeOcclusion(World& world, const Vec3& listenerVox,
                                 const Vec3& sourceVox,
                                 const std::vector<MaterialAcoustics>& acoustics,
                                 const OcclusionTuning& tuning) {
  OcclusionResult r;
  if (!tuning.enabled || acoustics.empty()) return r;

  const Vec3 d = sourceVox - listenerVox;
  const float distVox = d.len();
  if (distVox < 1e-4f) return r;

  const float maxVox = tuning.maxRayM / kVoxelMeters;
  const float traceVox = std::min(distVox, maxVox);
  const Vec3 dir = d * (1.0f / distVox);

  // Walk from the LISTENER toward the source. Direction matters for the early
  // exit below: material right next to your ear is what dominates the result,
  // so accumulating from this end lets a fully-blocked path bail early.
  const int steps = std::min(kMaxSteps, (int)(traceVox / kStepVox));
  float solidVox = 0.0f;   // voxels of solid crossed, weighted by dbPerM
  float dbLoss = 0.0f;     // accumulated broadband loss
  float hfDepth = 0.0f;    // accumulated fraction toward the cutoff floor
  float rawSolidVox = 0.0f;

  // Skip the first sample: it is the listener's own cell. Standing with your
  // head inside a leaf block should not silence the world.
  for (int i = 1; i < steps; i++) {
    const float t = (float)i * kStepVox;
    const Vec3 p = listenerVox + dir * t;
    const IVec3 cell{ifloor(p.x), ifloor(p.y), ifloor(p.z)};

    // Unknown space is treated as AIR, not as solid. The chunk cache trails
    // the player, so assuming solid would make every sound past the cached
    // radius cut out -- a far worse artifact than under-occluding.
    if (!world.CellInWindow(cell)) continue;
    const IVec3 wc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
    const CachedChunk* cc = world.Cached(wc);
    if (cc == nullptr || cc->voxels.size() != kChunkVol) {
      // Ask for it so the NEXT solve is better; this one treats it as open.
      // Bounded by World::kFetchPerTick, so a long ray cannot flood the queue.
      world.RequestChunkFetch(wc);
      continue;
    }

    const uint32_t lx = (uint32_t)(cell.x & 15), ly = (uint32_t)(cell.y & 15),
                   lz = (uint32_t)(cell.z & 15);
    const uint32_t mat = cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
    if (mat == 0 || mat >= acoustics.size()) continue;  // air / unknown id

    const MaterialAcoustics& a = acoustics[mat];
    if (a.dbPerM <= 0.0f) continue;  // acoustically transparent (gases)

    const float segM = kStepVox * kVoxelMeters;
    dbLoss += a.dbPerM * segM;
    hfDepth += segM / std::max(0.05f, a.hfDepthM);
    rawSolidVox += kStepVox;
    solidVox += kStepVox;

    // Early out: past the attenuation cap AND past the cutoff floor, more
    // material cannot change the answer. A ray into bedrock stops after a few
    // samples instead of tracing 400 of them.
    if (dbLoss * tuning.attenScale >= tuning.maxAttenDb && hfDepth >= 1.0f) break;
  }

  r.solidM = rawSolidVox * kVoxelMeters;
  if (rawSolidVox <= 0.0f) return r;  // clear line of sight

  const float db = std::min(dbLoss * tuning.attenScale, tuning.maxAttenDb);
  r.gain = std::pow(10.0f, -db / 20.0f);

  // Cutoff sweeps from open to the floor in LOG frequency as depth accumulates
  // -- the mass-law tilt. hfDepth == 1 means "as dull as this material gets".
  const float k = std::clamp(hfDepth, 0.0f, 1.0f);
  const float lo = std::max(60.0f, tuning.minCutoffHz);
  r.cutoffHz = std::clamp(kOpenCutoffHz * std::pow(lo / kOpenCutoffHz, k) * tuning.cutoffScale,
                          lo, kOpenCutoffHz);

  // The reverb send survives occlusion better than the dry path: sound that
  // reaches you through/around a barrier still excites the room you are
  // standing in. Blend the dry gain toward unity by wetKeep.
  r.wetGain = r.gain + (1.0f - r.gain) * std::clamp(tuning.wetKeep, 0.0f, 1.0f);
  return r;
}

OcclusionResult ScaleOcclusion(const OcclusionResult& r, float scale) {
  if (scale >= 1.0f) return r;
  OcclusionResult o = r;
  const float s = std::max(0.0f, scale);
  // Scale the dB duck rather than the linear gain, so "half as occluded" means
  // half the decibels -- which is what the ear actually halves.
  const float db = -20.0f * std::log10(std::max(1e-6f, r.gain));
  o.gain = std::pow(10.0f, -(db * s) / 20.0f);
  const float dbWet = -20.0f * std::log10(std::max(1e-6f, r.wetGain));
  o.wetGain = std::pow(10.0f, -(dbWet * s) / 20.0f);
  // Relax the cutoff toward open by the same fraction, in log frequency.
  o.cutoffHz = std::min(kOpenCutoffHz,
                        r.cutoffHz * std::pow(kOpenCutoffHz / std::max(1.0f, r.cutoffHz), 1.0f - s));
  return o;
}

}  // namespace audio
