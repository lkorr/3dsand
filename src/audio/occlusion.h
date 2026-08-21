#pragma once
// Sound occlusion against the voxel world (game thread).
//
// WHY THIS IS NOT THE WEBGAME'S OCCLUSION. audio_webgame solves Maekawa
// knife-edge diffraction analytically against a list of cylinders and boxes:
// it can name an obstacle's edges, so it can compute the exact detour around
// them. A voxel world has no such list -- it has 100M+ cells and arbitrary
// topology, and "the edges of the obstacle" is not a question with a cheap
// answer. So the model here is different in kind:
//
//   Sample the straight path emitter->listener, accumulate how much solid
//   material it crosses, and convert that into (broadband gain, low-pass
//   cutoff) via a per-material acoustic transmission loss.
//
// That is the model most 3D games actually ship, and it captures the effects
// players read: more wall = quieter and duller; a thin partition muffles less
// than bedrock; a doorway lets sound through. It deliberately gives up true
// diffraction imaging. See "What this does not model" at the bottom.
//
// PHYSICALLY, the low-pass is the load-bearing half. Transmission loss through
// a partition rises with frequency (mass law: ~6 dB per doubling), so material
// between you and a source removes highs far faster than lows -- which is why
// a wall does not merely make a sound quieter, it makes it *dull*. Getting the
// tilt right matters more for readability than getting the absolute level
// right, so the cutoff falls fast with depth while the broadband gain is
// deliberately gentle.
//
// COST. One DDA ray per audible voice per frame, over the SAME one-tick-latent
// chunk cache the avatar's foot probe uses -- no GPU work, no readback, no new
// synchronization. Rays are capped in length and in step count, and voices
// beyond audible range never solve at all, so this obeys CLAUDE.md rule 2:
// a settled world with nothing playing costs nothing.

#include <cstdint>
#include <vector>

#include "math3d.h"

class World;

namespace audio {

// Per-material acoustic properties, indexed by 12-bit material ID. Built once
// from materials.json (see cues.cpp) so that "how does lava sound through"
// stays data, not a switch statement in the audio code.
struct MaterialAcoustics {
  // Transmission loss in dB per meter of this material crossed, evaluated at
  // the reference frequency. Air and gases are ~0; stone is high.
  float dbPerM = 0.0f;
  // Depth in meters at which the high-frequency roll-off reaches the cutoff
  // floor. Small = this material kills highs fast (dense solids); large = it
  // is acoustically thin (foliage, snow).
  float hfDepthM = 1.0f;
};

struct OcclusionTuning {
  bool enabled = true;
  float maxAttenDb = 24.0f;   // cap on the broadband duck
  float minCutoffHz = 320.0f; // fully-occluded low-pass floor
  float attenScale = 1.0f;    // multiplies the accumulated dB
  float cutoffScale = 1.0f;   // multiplies the cutoff (<1 = darker)
  float maxRayM = 40.0f;      // never trace further than this
  // Reverb send retained when the path is blocked but the spaces still
  // connect. 1 = the send fully survives occlusion, 0 = it ducks with the dry.
  // Sound reaching you around a corner still excites the room you are in, so
  // a muffled source should keep more of its reverb than its dry signal --
  // without this a sound behind a wall goes dry AND quiet, which reads as
  // "switched off" rather than "behind something".
  float wetKeep = 0.7f;
};

struct OcclusionResult {
  float cutoffHz = 20000.0f;  // 20 kHz = open
  float gain = 1.0f;          // broadband, linear, dry path
  float wetGain = 1.0f;       // broadband, linear, reverb send (>= gain)
  float solidM = 0.0f;        // meters of solid crossed (diagnostics/HUD)
};

// Traces listener->source through the voxel mirror and returns the muffling.
// `acoustics` is indexed by material id; ids past its end are treated as air.
//
// Positions are in VOXELS (the coordinate system World speaks); the result's
// distances are in meters. Cells the mirror does not cover are treated as
// UNKNOWN and skipped rather than assumed solid: the chunk cache trails the
// player, and assuming solid would make every distant sound cut out.
OcclusionResult ComputeOcclusion(World& world, const Vec3& listenerVox,
                                 const Vec3& sourceVox,
                                 const std::vector<MaterialAcoustics>& acoustics,
                                 const OcclusionTuning& tuning);

// Softens a solve for a spatially EXTENDED source (Voice::occlScale < 1): the
// dB duck is scaled and the low-pass relaxes toward open by the same fraction
// in log-frequency. A wide lava lake is never fully shadowed by one pillar, so
// point-source muffling would read as the lake switching off behind cover.
OcclusionResult ScaleOcclusion(const OcclusionResult& r, float scale);

// ---------------------------------------------------------------------------
// What this does NOT model, so the next person does not assume it does:
//
//   * Diffraction imaging. An occluded sound is muffled but still arrives from
//     its true direction, not from around the corner it actually came through.
//     audio_webgame's Occlusion.cpp does this (OcclusionResult::apparentPos)
//     and it is a real upgrade, but it needs the obstacle's edges, which is
//     what a voxel field does not hand you cheaply. If it is wanted later, the
//     tractable version is a small fan of probe rays toward offsets around the
//     blocked direction, taking the least-occluded as the apparent position.
//   * Portals. Sound does not route through a corridor; it takes the straight
//     line. A cave system will therefore leak sound through its walls rather
//     than around its passages.
//   * Reflections off specific geometry. The engine's early reflections are a
//     generic shoebox, not the actual room.
// ---------------------------------------------------------------------------

}  // namespace audio
