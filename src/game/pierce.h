#pragma once
// pierce.h -- A POINT ALONG A RAY, THROUGH WHATEVER IT MEETS (2026-10-12,
// docs/PLAN_projectiles.md package B).
//
// The one entry point a flight calls when its tick's segment meets a body:
// arrows (package C), thrown points, solid spell projectiles (package D), and
// the `pierce` gate. The physics of ONE body is phys/kerf.h PierceChannel,
// applied by Mob::PierceLimb (a creature's slot, alive or dead) and
// DebrisSystem::PierceBody (a severed piece, a crate); this file only walks the
// ray from body to body and hands the residual speed on, so a full-draw arrow
// goes through an arm and into the torso behind it, or through one man and
// into the next, by the same rule that stops it in the first.
//
// WHAT IT DOES NOT DO: terrain. The caller's flight owns the grid probe (the
// CPU mirror), and passes `maxDistVox` = how far along the ray the grid is
// clear this tick. Nothing here writes the world: every voxel a channel takes
// leaves through the carve paths' ordinary particles and blood ops (rule 3).
//
// Deterministic for a given physics state: the bodies are met in the order
// Jolt's ray reports them, one cast per body, and everything priced per body
// is a pure function of its lattice and the projectile.

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "phys/kerf.h"

struct ParticleSpawn;
class Physics;
class MobSystem;
class DebrisSystem;
class World;

// One body the ray met.
struct PierceHit {
  uint64_t body = 0;    // the Jolt body that was struck
  uint64_t mobId = 0;   // its creature (0 = a loose body, or nobody's)
  int limb = -1;        // the creature's slot (-1 for a loose body)
  float along = 0.0f;   // world voxels from the ray's origin to the collider hit
  PierceResult r;       // r.hit false = a body nobody prices (a door, a bomb)
};

struct PierceFlight {
  std::vector<PierceHit> hits;  // in the order met
  // The point stopped: embedded or glanced on the last hit, or met a body
  // nothing can carve. False = it is still flying at `speedMps` from `endWorld`.
  bool stopped = false;
  float speedMps = 0.0f;   // left over (0 when stopped)
  float travelled = 0.0f;  // world voxels along the ray to `endWorld`
  Vec3 endWorld{};         // where it stopped, else the end of the segment
};

// Bound on bodies one ray passes through in one call (rule 2): a crowd in a
// line stops the arrow by price long before this does.
inline constexpr int kPierceMaxBodies = 8;
// How far in front of a collider hit the channel starts looking for matter,
// world voxels: the collider is a greedy box merge inflated by a convex
// radius, near the surface but not on it (KerfFrame's `back`, same reason).
inline constexpr float kPierceBackVox = 1.0f;

// Fire `pen` from `fromWorld` along `dirWorld` (any length; normalised here)
// for `maxDistVox` world voxels. `ignore` = bodies the ray must not meet (the
// shooter's own). Every hit is resolved in place -- hp, wounds, gore, death --
// before the next body is cast for.
//
// A BODY ANOTHER MACHINE OWNS (a peer's creature or player, a peer's loose
// body) is NOT carved for real here: the hit is routed to its owner as a
// MeleeHitKind::Pierce record (game/meleehit.h, applied by
// ApplyRemoteMeleeHits) and the channel runs on the ghost as presentation,
// gore discarded -- melee's rule 4. `embedItem` rides that record: the item
// that stays in the limb on the owner if the point embeds there (an arrow,
// Mob::EmbedArrow; "" = nothing). A body this machine owns is the caller's to
// embed in, from the returned hits. An arrow already stuck in a body
// (Mob::IsArrowSlot) is passed by, not struck.
PierceFlight PierceAlongRay(Physics& phys, MobSystem& mobs, DebrisSystem& debris,
                            World& world, std::vector<ParticleSpawn>& spawns,
                            Vec3 fromWorld, Vec3 dirWorld, float maxDistVox,
                            const Penetrator& pen,
                            const std::vector<uint64_t>& ignore = {},
                            const std::string& embedItem = {});

// ---- A SPELL FLIGHT'S MATTER MEETS A BODY (docs/PLAN_projectiles.md D) -------
//
// The owner's half of SpellBodyProbe::matterHit (game/spell.h): the VM says
// what is flying (its matter, mass, speed, point) and over which segment; this
// resolves the strike through the body systems and answers where the flight
// stopped, or how fast it left the far side.
//
//   Pierce  PierceAlongRay with the matter as the Penetrator. Through every
//           body met: `through`, at the residual speed. Stopped inside a
//           creature: the leftover matter acts on the wound -- a matter a body
//           can wear coats the channel's floor (PourOnBody), a tag:hot one
//           lights the struck limb (IgniteLimb) -- else it is the VM's to drop.
//   Scour   the same, its energy capped at `scourBins` bins of skin-hard
//           matter at its tip (priced by this tuning's gore.pierceCellJ), and
//           always stopped: grit does not fly on through a man.
//   Splash  never penetrates. The first creature's skin under the ray takes a
//           disc of coat (MobSystem::PickBody + PourOnBody, the pour brush's
//           writers), and the coat's own rules do the rest; `tookMatter`.
//   Touch   (never sent by the VM, which keeps the old bodyHit rule for a gas)
//           the first body only: where it was met, nothing done to it.
//
// `ignore` = the caster's own bodies. Nothing here writes the world: gore
// leaves through `spawns` (rule 3). Deterministic for a given physics state.
struct SpellBodyShot;
struct SpellBodyStrike;
void SpellMatterStrike(Physics& phys, MobSystem& mobs, DebrisSystem& debris, World& world,
                       std::vector<ParticleSpawn>& spawns, const SpellBodyShot& shot,
                       const std::vector<uint64_t>& ignore, SpellBodyStrike& out);
