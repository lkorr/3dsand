#pragma once
#include <cstddef>
#include <vector>

#include "math3d.h"

// ---- THE INTEREST SET: who the world has to be resident around -------------
//
// Every residency decision in the engine — the toroidal window shift
// (Stream::Update), the far-field cascade recentre (FarField::Update /
// FullRefill) — used to take a single `IVec3 playerChunk`. That signature is
// the shape of a singleplayer assumption, not of the code behind it: both
// functions are "recentre toward a point", and there was no type anywhere in
// the tree that could name a SECOND point (docs/RESEARCH_multiplayer_readiness.md
// S6/S7). The nearest thing was DebrisSystem::AddTerrainAnchor, which only
// feeds the CPU fetch cache.
//
// This type is the seam. It carries the points and says which one the single
// toroidal window is centred on, so today's behaviour is a one-line lookup
// (`Primary()`) and identical to the old code — the point is that the NEXT
// person cannot add a remote player's region without coming through here and
// answering the question the old signature hid: what does residency mean when
// two people are looking at different places?
//
// WHAT IT DELIBERATELY DOES NOT DECIDE. It does not make a second point
// resident. There is ONE window (`kWorldN`^3) and one set of far cascades, and
// making a second region simulate is the chunk-ticket plan's job
// (docs/PLAN_chunk_tickets.md §0-1, P0 landed with kTicketMax = 0) — its
// non-goals (no reduced tick rate, no EMIT/PAIR reactions in tickets) are
// exactly the bound on what a remote player's region could be. Under the model
// of record (DESIGN.md §10: host-authoritative op stream, chunk authority) the
// host may not need a second window at all; that decision is what says whether
// tickets get built. Until then a secondary point is INFORMATION — an
// authority the host owes ops for — and the honest place to record it is here,
// not a second `IVec3` parameter grown onto one call site.
//
// DETERMINISM. Nothing here is sim state. The set is derived per tick from
// entity positions, is not hashed and is not saved. It may only affect WHICH
// chunks are resident, never WHAT a resident chunk computes — CLAUDE.md's
// window-origin rule (no sim-affecting global keyed on the window origin) is
// what keeps a two-point residency policy from moving the world hash.
struct InterestSet {
  // World CHUNK coords (voxel >> 4), one per point of interest. Order is
  // caller-defined and must be STABLE across ticks on every machine that
  // consumes it: an unstable order would make `primary` mean a different
  // point on two clients and move the window differently. Today the game
  // builds it with exactly one entry.
  std::vector<IVec3> chunks;
  // Index into `chunks` of the point the ONE toroidal window follows. Not a
  // preference: the window is a single origin, so somebody has to be the
  // centre, and every other entry is at best a ticket candidate.
  int primary = 0;

  InterestSet() = default;
  // The singleplayer / harness adapter: one point, and it is primary.
  // Non-explicit on purpose so the dozen existing "recentre on the player"
  // call sites (selftest, vk_smoke, perfsuite) keep reading as they did —
  // one player IS a one-element interest set, and spelling that out at every
  // fixture would be ceremony. Anything with two points must name the type.
  InterestSet(IVec3 c) : chunks{c}, primary(0) {}

  bool Empty() const { return chunks.empty(); }
  size_t Size() const { return chunks.size(); }
  // The point residency follows. An empty set has no centre; callers treat
  // that as "do not move the window" rather than defaulting to the origin,
  // because chunk (0,0,0) is a real place and shifting there would evict the
  // whole world.
  IVec3 Primary() const {
    const size_t i = (primary >= 0 && (size_t)primary < chunks.size()) ? (size_t)primary : 0;
    return chunks.empty() ? IVec3{0, 0, 0} : chunks[i];
  }
};
