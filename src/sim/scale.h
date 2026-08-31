#pragma once
#include "sim/world.h"

// The ONE metres<->cells vocabulary (DESIGN.md §3b).
//
// The engine runs in voxel units, but almost nothing it simulates is *about*
// voxels: a player is 1.7 m, a step-up is 58 cm, a sword reaches 45 cm. Those
// are physical facts, and `kVoxelMeters` is the only thing that turns them into
// cell counts. Write the fact, convert at use.
//
// THE RULE THIS HEADER EXISTS TO ENFORCE:
//
//   No authored quantity representing a physical length may be expressed in
//   cells without also recording the cells-per-metre it was authored at.
//
// A bare voxel literal is not wrong because it is ugly — it is wrong because it
// silently means a DIFFERENT PHYSICAL THING at a different `kVoxelMeters`, and
// nothing anywhere will tell you. That is exactly how the tree atlas came to
// bake half-height forests and the avatar came to stand half as tall as the
// capsule it drives: both were correct voxel counts, authored at 10 cm, with no
// mechanical link back to the constant that gave them meaning.
//
// `Player` has done this correctly since v0.2 (`player.h:176-178` is
// `0.85f / kVoxelMeters`, not `8.5f`), which is why the player controller was
// the one system that survived 0.10 -> 0.05 untouched. This header is that
// pattern given a name so the rest of the engine can share it.
//
// SCOPE. This is for NON-worldgen code, which authors in metres. The `worldgen`
// tuning group solves the same problem differently and deliberately: its rows
// are authored in voxels at `worldgen.refVoxelsPerMetre` and rescaled by
// `LoadTuning` (`tuning.cpp:1843`), because a terrain octave amplitude has no
// natural metre value a human would want to type. Do not introduce a second
// authoring baseline here — one `refVoxelsPerMetre`, one metres rule, and a
// clear line between them.
//
// WHAT DOES *NOT* BELONG HERE. Three kinds of number look like lengths and are
// not, and converting them is a bug:
//   - Sub-cell sampling rates. `player.cpp`'s 0.45-cell sweep substep and
//     `kSkin = 1/512` are fractions OF A CELL and must stay <= 1 cell at every
//     voxel size, so they are correct as bare cell values.
//   - Ratios and relative measures. The avatar's gait is authored in LEG
//     LENGTHS and seconds (`avatar.cpp:34-115`), so it follows the rig for
//     free.
//   - Gradients, chances and thresholds. A slope is dimensionless; a 0..255
//     noise threshold is not a distance. (`tuning.cpp:1884` makes the same
//     point for the worldgen group.)

// Metres -> cells. The conversion, and the reason every other name in this file
// exists: so the call site reads as a unit change rather than as arithmetic.
inline constexpr float MetresToCells(float metres) {
  return metres / kVoxelMeters;
}

// Cells -> metres. For reporting a measured voxel distance in physical terms —
// diagnostics, gate messages, impact speeds.
inline constexpr float CellsToMetres(float cells) { return cells * kVoxelMeters; }

// Metres -> whole cells, rounded, and FLOORED AT ONE.
//
// The floor is the point. Reaches, probe depths and step-up budgets are all
// "at least this far", and a budget that rounds to zero does not become a
// slightly worse budget — it turns a rule off. `Player::kMaxStepUpVoxels`
// (`player.h:190-192`) already spells this exact clamp out inline; this is that
// expression, named, so the next one cannot forget it.
//
// Use the float form instead when the quantity is compared against a
// continuous position — rounding a collision margin to a whole cell throws away
// the sub-cell precision the sweep is built on.
inline constexpr int MetresToCellsI(float metres) {
  const int cells = (int)(metres / kVoxelMeters + 0.5f);
  return cells < 1 ? 1 : cells;
}

// Speeds. Dimensionally identical to MetresToCells (a per-second denominator
// cancels), but distinct at the call site: a reader checking units wants to see
// that a speed was converted as a speed, and a m/s value silently used as
// cells/s is one of the easier mistakes to make here.
inline constexpr float MetresPerSecToCells(float mps) {
  return mps / kVoxelMeters;
}
inline constexpr float CellsPerSecToMetres(float cps) {
  return cps * kVoxelMeters;
}

// Accelerations, same argument (m/s^2 -> cells/s^2). Gravity is the one that
// bites: a fall that "felt right" at 10 cm is twice as fast in cells at 5 cm
// and reads as an entirely different physics.
inline constexpr float MetresPerSec2ToCells(float mps2) {
  return mps2 / kVoxelMeters;
}

// ---- authored-asset resolution ---------------------------------------------

// The voxels-per-metre every sidecar predating `artVoxelsPerMetre` was drawn
// at. Those files said `skinScale: 8` meaning "8 art voxels per WORLD voxel",
// which only fixed a physical size because the world happened to be 10 cm — so
// this is the constant that reading was hiding, written down.
//
// It is frozen history, NOT a current default: it must not follow
// kVoxelsPerMetre, or a legacy asset would keep changing size, which is the
// entire bug. New art declares artVoxelsPerMetre.
inline constexpr int kLegacyAuthoringVoxelsPerMetre = 10;


// Voxel art (`.vox` mobs, items, armour) is a grid with no intrinsic size: a
// 136-tall figure is 1.7 m only if you also say the art is 80 voxels/metre.
// That declaration is `artVoxelsPerMetre` in the sidecar, and this is the
// derivation from it to `MobDef::skinScale` — art voxels per WORLD cell.
//
// Returns 0 when the art is COARSER than the world (the ratio would be < 1 and
// skinScale is a positive integer), which is the caller's signal to upsample the
// art grid first — see NeededArtUpsample. Returns 0 as well for a ratio that is
// not a whole number, which is an authoring error rather than a scale to round.
inline constexpr unsigned SkinScaleFor(int artVoxelsPerMetre) {
  if (artVoxelsPerMetre <= 0) return 0;
  if (artVoxelsPerMetre % kVoxelsPerMetre != 0) return 0;
  return (unsigned)(artVoxelsPerMetre / kVoxelsPerMetre);
}

// The power-of-two factor the art grid must be block-replicated by before
// SkinScaleFor can produce a legal scale — 1 when the art is already fine
// enough. Replication is exact and lossless (it preserves world size and adds
// no detail, `gen_mina.py:566` calls the same operation "the shape it was,
// sampled twice as finely"), so this direction is always safe.
//
// The reverse direction — art FINER than needed — is deliberately not handled:
// it would be a lossy downsample needing a deterministic tie-break, and it
// cannot arise while every asset is authored at or above the coarsest voxel
// size in use. 10 cm is that baseline today.
inline constexpr unsigned NeededArtUpsample(int artVoxelsPerMetre) {
  if (artVoxelsPerMetre <= 0) return 1;
  unsigned u = 1;
  while ((unsigned)artVoxelsPerMetre * u < (unsigned)kVoxelsPerMetre) u *= 2;
  return u;
}
