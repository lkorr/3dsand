// =============================================================================
// distant-mp -- THE DISTANT TIER ACROSS MACHINES (docs/PLAN_distant_npcs.md
// 8.3 / 8.6, package D7a; game/distant.h's D7a paragraph; net/distantsync.h)
// =============================================================================
//
// Two machines in one process, joined by bytes (net-melee's construction):
// machine A is the suite's own MobSystem + DebrisSystem + Physics -- THE HOST:
// it ticks THE tick (support::TickCursor, with an EntityParking in phase H and
// a park function bound: its ChunkStore is the world's); machine B is a second
// MobSystem + DebrisSystem on its own Physics -- THE CLIENT: no park function,
// its phases called directly (DIRECT PHASE CALLS ON PURPOSE (W2-O): B has no
// session; these are the calls TickAuthority and the frame loop make for a
// client, in their order: the entity apply, the handoff scan, phase H's
// Unpark, PreTick, the physics step). Every record crosses as encoded BYTES
// both ways through net::EntitySync (EntityBatch v5), in main.cpp's order
// around the tick: A applies B's batch labelled tick+1 before running it, B
// applies A's batch labelled with the tick it is about to run.
//
// THE GEOMETRY, and why B's promotions still find ground. B shares A's World
// (one GPU world per process), so B's fetch cache IS A's: a chunk is fetchable
// only inside A's window. B's own window is a TEST SEAM on B's MobSystem
// (MobSystem::SetTierWindowForTest): every border rule B applies -- its park
// rule, its promotion line, its band -- reads B's window, while its near tier
// and its promotion snap read the World as it is. B's window is A's shifted
// +16 chunks in x, so the two OVERLAP in A's x chunks [16, 32), every y and z:
// a promotion on B lands where both windows hold the ground, in the overlap.
// The players' chunks the authority arithmetic reads (the PeerViews) stand at
// z chunk 28, A's at x 16 and B's at x 32, so a creature in the overlap's far
// half (x chunk > 24) is NEARER B: the ownership rule names B there and A's
// own promotion line yields it to B. Past A's x chunk 32, B's window holds
// chunks the shared fetch cache never will -- what W uses on purpose.
//
//   H. A HAND-OUT. A creature B owns stands on A's outermost x plane (chunk 31:
//      no authority margin for A there, so B alone owns it), hurt in an arm,
//      with a brain target. B's window jumps to +33 chunks: the creature is
//      past B's pad and B's park rule HANDS IT OUT -- with no park function it
//      used to be erased. Applied on A: A's tier holds it under the SAME id,
//      the brain's target and every base limb's hp bit-identical to B's live
//      limbs at the hand-out, its bleed rate the one CaptureDistantVitals read,
//      and A's ghost of it gone. B's window comes back: the creature is over
//      B's promotion line only, A hands it IN, and B places it again under the
//      same id (the round trip, within distantMp.roundTripMaxTicks).
//   L. THE BORN-THIS-TICK LATCH. A creature A spawns in the overlap's far half
//      AFTER the handoff scan and before the tick -- where phase H's
//      promotions and unparks make creatures live -- is NOT made a ghost by
//      that tick's RefreshOwnership (it would have been one with no handoff
//      ever built, and expired); the next scan hands it to B WITH its record:
//      B owns it, A holds a ghost.
//   P. A REMOTE PROMOTION. A distant walker in A's tier on a Goto from past
//      the +z face into the overlap's far half: over both lines, the ownership
//      rule names B, so A HANDS IT IN (A never promotes it), and B promotes it
//      ON B under the same id -- every base limb's hp bit-identical to A's
//      in-transit copy, origin x / z bit-identical (y snapped onto B's real
//      ground), the brain's profile and target; the "promoted" answer closes
//      A's transit. B DREW IT FROM THE VIEW before: its mirror held the shape
//      and B's statue list an item exactly where the view put it.
//   R. RETIRE AND ACTIVATION OVER W. Two distant creatures 1,600 voxels either
//      side of A's centre in x (past A's radius + hysteresis, 1,536): the one
//      toward B (1,344 from B's centre) is KEPT, the other RETIRED (parked).
//      Two parked records 1,350 either side (past A's radius, 1,280): the one
//      toward B (1,094 from B) is ACTIVATED, the other stays parked.
//   W. A GIVE-BACK. A parked record inside B's window and outside A's (x chunk
//      40) is activated by A and handed in (B's line alone); B has no ground
//      for it in the shared fetch cache and gives it back after
//      kHandInMaxWaitTicks: A's tier holds it again under the same id, with
//      nothing in transit and nothing in B's waiting room.
//   D. A DISCONNECT. A second walker is handed in; the link drops after B took
//      it into its waiting room and before B's pass: A puts its in-transit copy
//      back in its tier, B keeps its waiting room and promotes it (two worlds
//      now, each holding its own copy).
//   T. TWICE-RUN. H..D from two fresh worlds (same ticks, same ids): the digest
//      of every distant record on the wire both ways (hand-outs, hand-ins,
//      answers, views, room reports) and of A's tier after every tick is equal.
//
// LEAVES NOTHING: both ownership closures and the role cleared from the
// suite's MobSystem and DebrisSystem, the park function removed, machine B
// destroyed, mobs / debris / tier / store reset, tuning and the id counter
// restored, worldgen regenerated at home. The window is never moved.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/distant.h"
#include "game/mob.h"
#include "game/persist.h"
#include "net/distantsync.h"
#include "net/entitysync.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/chunkstore.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

// FNV-1a 64 over raw bytes: the twice-run digest.
struct Digest {
  uint64_t h = 1469598103934665603ull;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void F(float v) { Bytes(&v, sizeof v); }
  void U(uint64_t v) { Bytes(&v, sizeof v); }
};

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// The tier with the gate's own numbers, whatever tuning.json says.
void SetTier() {
  Tuning t = CurrentTuning();
  t.distant.enabled = true;
  t.distant.radius = 1280.0f;
  t.distant.hysteresis = 256.0f;
  t.distant.idleStride = 4;
  t.distant.surfaceSnapMaxVox = 24.0f;
  t.distant.promoteMaxDropVox = 48.0f;
  SetCurrentTuning(t);
}

constexpr uint32_t kMobsSection = 'M' | ('O' << 8) | ('B' << 16) | ((uint32_t)'S' << 24);
constexpr uint32_t kPeer = 1;

// Every distant record a batch carries, as its bytes, in field order.
void DigestDistant(Digest& dg, const ::net::EntityBatch& b) {
  std::vector<uint8_t> buf;
  ByteWriter w{buf};
  for (const ::net::DistantHandOut& h : b.distantHandOuts) ::net::Encode(w, h);
  for (const ::net::DistantHandInDone& d : b.distantDones) ::net::Encode(w, d);
  for (const ::net::DistantHandIn& h : b.distantHandIns) ::net::Encode(w, h);
  ::net::Encode(w, b.distantView);
  w.U32(b.livingFree);
  dg.Bytes(buf.data(), buf.size());
}

// A's tier after a tick: ids, origins, every limb's hp, life -- and the
// creatures in transit by id.
void DigestTier(Digest& dg, const MobSystem& mobs) {
  for (const DistantMob& d : mobs.Distant().All()) {
    dg.U(d.id);
    dg.F(d.origin.x);
    dg.F(d.origin.y);
    dg.F(d.origin.z);
    dg.F(d.steer.heading);
    for (const float h : d.limbHp) dg.F(h);
    dg.U(d.dead ? 1u : 0u);
  }
  for (const DistantTransit& t : mobs.DistantInTransit()) dg.U(t.d.id ^ 0xA5A5ull);
}

bool InTransit(const MobSystem& mobs, uint64_t id) {
  for (const DistantTransit& t : mobs.DistantInTransit())
    if (t.d.id == id) return true;
  return false;
}

struct MpRun {
  std::vector<std::string> fails;
  uint64_t digest = 0;
  // ---- measurements (the detail line, last_run.json) ----
  int hRoundTrip = -1;          // ticks from B's window coming back to H near on B again
  float hBleed = 0.0f;
  int pWalk = -1;               // ticks from the walk's start to A's hand-in
  int pPromote = -1;            // ticks from the hand-in to B's promotion
  float pSnapDy = 0.0f;
  int wWait = -1;               // ticks from W's hand-in to its return on A
  int dPromote = -1;            // ticks from the disconnect to B's promotion of the waiting walker
  uint64_t viewEntries = 0, viewShapes = 0, viewGones = 0, viewBytes = 0, viewBatches = 0;
  uint64_t ticks = 0;
  uint64_t handOuts = 0, handInsSent = 0, handInsPromoted = 0, handInsReturned = 0;
  uint64_t transitRestored = 0;
};

MpRun RunMp(Ctx& c) {
  MpRun out;
  auto check = [&](bool ok, const std::string& why) {
    if (!ok) out.fails.push_back(why);
    return ok;
  };
  const Tuning tuneWas = CurrentTuning();
  // ---- a fresh world, the tier on with the gate's numbers ----
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(7000);   // both runs issue the same ids (every hash salt)
  c.mobs.SetItems(&c.items);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  SetTier();
  c.mobs.SetDistantStream(&c.stream);
  const int human = c.mobs.FindDef("human");
  if (human < 0) {
    out.fails.push_back("no 'human' def");
    SetCurrentTuning(tuneWas);
    return out;
  }
  const MobDef& hdef = c.mobs.Defs()[(size_t)human];
  const Vec3 hs = hdef.worldSize;
  const int base = (int)hdef.limbs.size();
  const IVec3 Awo = c.world.WindowOrigin();
  auto vox = [](int chunk) { return chunk * (int)kChunk; };
  const float aCx = (float)vox(Awo.x) + 0.5f * (float)kWorldN;
  const float aCz = (float)vox(Awo.z) + 0.5f * (float)kWorldN;
  // The standing height under a body box whose min corner is at (x, z): the
  // highest TerrainHeight over its footprint, plus one.
  auto standY = [&](int x, int z) {
    int hi = INT32_MIN;
    for (int dx = 0; dx <= (int)hs.x; dx += 2)
      for (int dz = 0; dz <= (int)hs.z; dz += 2)
        hi = std::max(hi, World::TerrainHeight(x + dx, z + dz, kDefaultSeed));
    return hi + 1;
  };

  // ---- machine B (net-melee's construction) ---------------------------------
  Physics farPhys;
  farPhys.Init();
  DebrisSystem farDebris;
  farDebris.Init(&farPhys, &c.world, c.mats, c.reactions);
  farDebris.SetMicroSet(c.debris.MicroSet());
  farDebris.SetLocalPlayerId(kPeer);
  MobSystem far;
  far.Init(&farPhys, &c.world, &farDebris, c.mats, c.reactions);
  far.SetMicroSet(c.mobs.MicroSet());
  far.SetDefFactory(c.mobs.DefFactory());
  far.SetDefs(std::vector<MobDef>(c.mobs.Defs()));
  far.SetBehaviors(ai::Library(c.mobs.Behaviors()));
  far.SetAttackStyles(StyleLibrary(c.mobs.AttackStyles()));
  far.SetItems(&c.items);
  far.SetLocalPlayerId(kPeer);
  far.SetIdBand(kPeer);   // the game's rule: B mints ids in its own band
  IVec3 Bwo{Awo.x + 16, Awo.y, Awo.z};
  far.SetTierWindowForTest(&Bwo);

  // ---- parking: A's store is the world's; B parks nothing -------------------
  EntityParking aParking;
  aParking.Bind(c.mobs, c.stream.Store(), true);
  EntityParking bParking;   // never bound: Unpark runs its waiting-room branch

  // ---- the roles and the link -----------------------------------------------
  c.mobs.SetDistantNetRole(DistantNetRole::Host);
  far.SetDistantNetRole(DistantNetRole::Client);
  ::net::EntitySync entA, entB;
  entA.Connect(0, kPeer);
  entB.Connect(kPeer, 0);
  bool linked = true;
  const int pz = Awo.z + 28;
  const int pcy = World::TerrainHeight(vox(Awo.x + 28) + 8, vox(Awo.z + 30) + 8, kDefaultSeed) >> 4;
  auto seat = [&]() {
    if (!linked) return;
    ::net::PeerView a{0, {Awo.x + 16, pcy, pz}, Awo, true};
    ::net::PeerView b{kPeer, {Bwo.x + 16, pcy, pz}, Bwo, true};
    entA.SetPeers(a, &b);
    entB.SetPeers(b, &a);
  };

  auto cleanup = [&]() {
    far.SetOwnershipFn(nullptr);
    farDebris.ClearOwnershipFn();
    farDebris.SetChunkOwnedFn(nullptr);
    far.Reset();
    farDebris.Reset();
    c.mobs.SetOwnershipFn(nullptr);
    c.debris.ClearOwnershipFn();
    c.debris.SetChunkOwnedFn(nullptr);
    c.mobs.SetDistantNetRole(DistantNetRole::Off);
    aParking.Bind(c.mobs, c.stream.Store(), false);   // the park function captured aParking
    c.mobs.Reset();
    c.debris.Reset();
    c.stream.Store().Clear();
    SetCurrentTuning(tuneWas);
  };

  // ---- ONE TICK OF BOTH MACHINES, the wire running both ways ----------------
  uint32_t tick = 9800;
  const uint32_t t0 = tick;
  support::TickCursor stepA{c, tick, IVec3{Awo.x + 16, pcy, Awo.z + 16}};
  stepA.Rig().Authority().parking = &aParking;   // THE tick's phase-H door
  ::net::EntityBatch toA;
  Digest dg;
  // The arms' hooks: before A's tick (after A's apply and scan), after it,
  // after B's apply, after B's phase H.
  std::function<void()> preA, postA, midB, postBUnpark;
  auto cross = [&](::net::EntitySync& from, MobSystem& m, DebrisSystem& d, uint32_t label) {
    ::net::EntityBatch o;
    from.Build(m, d, label, o);
    std::vector<uint8_t> bytes;
    o.Encode(bytes);
    ::net::EntityBatch in;
    if (!in.Decode(bytes.data(), bytes.size())) check(false, "an entity batch did not decode");
    DigestDistant(dg, in);
    return in;
  };
  auto step = [&]() {
    seat();
    // ---- A: B's batch for this label, the scans, THE tick ----
    entA.NoteRemote(::net::EntityBatch(toA));
    toA.Clear();
    entA.ApplyForTick(tick + 1, c.mobs, c.debris, nullptr);
    entA.ScanHandoffs(c.mobs, c.debris, tick + 1);
    entA.ExpireGhosts(tick + 1, c.mobs, c.debris);
    if (preA) preA();
    stepA();
    DigestTier(dg, c.mobs);
    if (postA) postA();
    // ---- A -> B, applied at the tick B is about to run ----
    entB.NoteRemote(cross(entA, c.mobs, c.debris, tick));
    entB.ApplyForTick(tick, far, farDebris, nullptr);
    if (midB) midB();
    entB.ScanHandoffs(far, farDebris, tick);
    entB.ExpireGhosts(tick, far, farDebris);
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): B has no session; this is phase
    // H's Unpark (its waiting-room branch) and the tick's body for B.
    bParking.Unpark(far, c.stream.Store(), c.world, tick);
    if (postBUnpark) postBUnpark();
    std::vector<ParticleSpawn> bSp;
    std::vector<CellOp> bCells;
    std::vector<BrushOp> bOps;
    far.PreTick(tick, c.world, bOps, bCells, bSp);
    farDebris.PreTick(tick, c.world, bCells, bSp);
    farPhys.Step(kTickDt);
    farDebris.PostStep();
    far.PostStep();
    // ---- B -> A, for A's next tick ----
    toA = cross(entB, far, farDebris, tick + 1);
  };
  // A distant creature of `profile` in A's tier: built near at A's window
  // centre and demoted through the park rule's own door (DemoteForTest), then
  // stood at column (x, z) on its ground. 0 on refusal.
  auto makeDistant = [&](const char* profile, int x, int z) -> uint64_t {
    const int sx = (int)aCx, sz = (int)aCz;
    const uint64_t id = c.mobs.Spawn(human, {sx, standY(sx, sz), sz});
    if (id == 0) return 0;
    c.mobs.SetMobBehavior(id, profile);
    if (!c.mobs.DemoteForTest(id)) return 0;
    DistantMobs& tier = c.mobs.DistantMut();
    const float gy = (float)(c.mobs.DistantGroundMut().TerrainTop(x + (int)(hs.x * 0.5f),
                                                                  z + (int)(hs.z * 0.5f)) + 1);
    tier.Relocate(id, Vec3{(float)x, gy, (float)z});
    tier.MarkMoved();
    return id;
  };
  // ...and a PARKED record of one, at column (x, z): made distant, stood there,
  // its patched record put in A's store bucket (the very record a retire
  // parks), the creature gone from the tier (records carry no id).
  auto makeParked = [&](int x, int z, Vec3& at) -> bool {
    const uint64_t id = makeDistant("dummy", x, z);
    const DistantMob* d = c.mobs.Distant().Find(id);
    if (d == nullptr) return false;
    ChunkStore::EntityRecord r;
    r.section = kMobsSection;
    r.version = MobSystem::kSaveVersion;
    r.pos = d->origin;
    at = d->origin;
    if (!c.mobs.DistantRecord(*d, r.bytes)) return false;
    c.mobs.DistantMut().Erase(id);
    c.stream.Store().DormantEntities(ChunkStore::RegionOfVoxel(r.pos)).push_back(std::move(r));
    return true;
  };
  auto parkedNear = [&](Vec3 at) {
    int k = 0;
    for (const ChunkStore::EntityRecord& r :
         c.stream.Store().DormantEntities(ChunkStore::RegionOfVoxel(at)))
      if (r.section == kMobsSection && std::fabs(r.pos.x - at.x) < 2.0f &&
          std::fabs(r.pos.z - at.z) < 2.0f)
        k++;
    return k;
  };
  auto goTo = [&](uint64_t id, Vec3 goal) {
    ai::Brain* b = c.mobs.BrainAnyTierMut(id);
    if (b == nullptr) return;
    b->routine = ai::Routine{};
    b->routine.active = true;
    b->routine.verb = ai::Intent::Goto;
    b->routine.goal = goal;
    b->routine.final = true;
    b->routine.arriveRadius = 4.0f;
    b->routine.speed = 1.0f;
  };
  // A's counters live on the suite's MobSystem and carry every earlier gate's
  // (and run 1's): read as deltas from here. B's are this run's own.
  const DistantMobs::Stats& aSt = c.mobs.Distant().GetStats();
  const DistantMobs::Stats& bSt = far.Distant().GetStats();
  const DistantMobs::Stats aRun0 = aSt;

  // ======== setup: the link speaks (B reports its room, A learns its window) ==
  for (int i = 0; i < 3; i++) step();
  check(entA.Stats().batchesIn > 0 && entB.Stats().batchesIn > 0,
        "setup: no entity batch crossed the wire");

  // ======== H: a hand-out, and the round trip =================================
  {
    const int hx = vox(Awo.x + 31) + 2, hz = vox(Awo.z + 16) + 4;
    const uint64_t hId = far.Spawn(human, {hx, standY(hx, hz), hz});
    if (hId != 0) far.SetMobBehavior(hId, "dummy");
    for (int i = 0; i < 12; i++) step();
    const Mob* hm = far.FindMobById(hId);
    const Mob* hg = c.mobs.FindMobById(hId);
    check(hId != 0 && hm != nullptr && !hm->IsGhost() && hg != nullptr && hg->IsGhost(),
          Format("H fixture: B owns its creature %d, A holds a ghost %d", hm && !hm->IsGhost(),
                 hg && hg->IsGhost()));
    // Hurt in an arm; a target in its brain.
    int arm = -1;
    for (int li = 0; li < base && arm < 0; li++)
      if (hdef.limbs[(size_t)li].tag == "arm" && hdef.limbs[(size_t)li].severable) arm = li;
    if (arm >= 0 && far.LimbBody(hId, arm) != 0)
      far.Damage(far.LimbBody(hId, arm), 0.3f * std::max(1.0f, hdef.limbs[(size_t)arm].hp),
                 far.LimbAnchorPos(hId, arm));
    if (ai::Brain* b = far.BrainAnyTierMut(hId)) {
      b->targetId = 4242;
      b->hasTarget = true;
      b->targetPos = Vec3{aCx, 0.0f, aCz};
    }
    std::vector<float> hHp;
    for (int li = 0; li < base; li++) hHp.push_back(far.LimbHp(hId, li));
    const uint64_t sent0 = bSt.handOutsSent, taken0 = aSt.handOutsTaken;
    // B's window jumps +17 chunks: the creature is past B's pad.
    Bwo = IVec3{Awo.x + 33, Awo.y, Awo.z};
    far.SetTierWindowForTest(&Bwo);
    step();   // B's park rule hands it out; B -> A carries it
    const bool goneOnB = far.FindMobById(hId) == nullptr && bSt.handOutsSent == sent0 + 1;
    float wireBleed = -1.0f;
    for (const ::net::DistantHandOut& h : toA.distantHandOuts)
      if (h.id == hId) wireBleed = h.status.bleedRate;
    bool onA = false, hpSame = false, targetSame = false, bleedSame = false, ghostGone = false;
    int hpDiffAt = -1;
    preA = [&]() {
      if (onA) return;
      const DistantMob* d = c.mobs.Distant().Find(hId);
      onA = d != nullptr;
      if (!onA) return;
      hpSame = d->limbHp.size() >= (size_t)base;
      for (int li = 0; li < base && hpSame; li++)
        if (!SameBits(d->limbHp[(size_t)li], hHp[(size_t)li])) {
          hpSame = false;
          hpDiffAt = li;
        }
      targetSame = d->brain.hasTarget && d->brain.targetId == 4242;
      bleedSame = SameBits(d->bloodLossPerTick, wireBleed);
      out.hBleed = d->bloodLossPerTick;
      ghostGone = c.mobs.FindMobById(hId) == nullptr;
    };
    step();   // A applies it (preA, before A's tick)
    preA = nullptr;
    check(goneOnB && wireBleed >= 0.0f && onA && hpSame && targetSame && bleedSame && ghostGone &&
              aSt.handOutsTaken == taken0 + 1,
          Format("H: handed out by B %d (on the wire %d), in A's tier %d (taken %llu), base hp "
                 "bit-identical %d (first diff limb %d), target kept %d, bleed %.4f carried %d, "
                 "A's ghost gone %d",
                 goneOnB, wireBleed >= 0.0f, onA,
                 (unsigned long long)(aSt.handOutsTaken - taken0), hpSame, hpDiffAt, targetSame,
                 (double)out.hBleed, bleedSame, ghostGone));
    // B's window comes back: over B's line only -> handed in -> placed on B.
    Bwo = IVec3{Awo.x + 16, Awo.y, Awo.z};
    far.SetTierWindowForTest(&Bwo);
    const int rtMax = (int)BaselineNumber("distantMp.roundTripMaxTicks", 90);
    for (int i = 0; i < rtMax; i++) {
      step();
      const Mob* m = far.FindMobById(hId);
      if (m != nullptr && !m->IsGhost()) {
        out.hRoundTrip = i + 1;
        break;
      }
    }
    // B's "promoted" answer rides B's batch of that step and is applied by A
    // at the top of the next one: the transit closes a step after B placed it.
    for (int i = 0; i < 2; i++) step();
    check(out.hRoundTrip > 0 && c.mobs.Distant().Find(hId) == nullptr &&
              !InTransit(c.mobs, hId),
          Format("H: the round trip -- back near on B after %d ticks (max %d), off A's tier %d, "
                 "transit closed %d",
                 out.hRoundTrip, rtMax, c.mobs.Distant().Find(hId) == nullptr,
                 !InTransit(c.mobs, hId)));
  }

  // ======== L: the born-this-tick latch =======================================
  {
    const int lx = vox(Awo.x + 25) + 4, lz = vox(Awo.z + 26) + 4;
    uint64_t lId = 0;
    preA = [&]() {
      // Phase H's position: after the handoff scan, before the tick.
      lId = c.mobs.Spawn(human, {lx, standY(lx, lz), lz});
      if (lId != 0) c.mobs.SetMobBehavior(lId, "dummy");
    };
    step();
    preA = nullptr;
    const Mob* lm = c.mobs.FindMobById(lId);
    const bool localAfterTick = lm != nullptr && !lm->IsGhost();
    const uint64_t handoffsIn0 = entB.Stats().handoffsIn;
    for (int i = 0; i < 8; i++) step();
    const Mob* lb = far.FindMobById(lId);
    const Mob* la = c.mobs.FindMobById(lId);
    check(lId != 0 && localAfterTick && lb != nullptr && !lb->IsGhost() && la != nullptr &&
              la->IsGhost() && entB.Stats().handoffsIn > handoffsIn0,
          Format("L: spawned in phase H's place, still A's after the tick %d; then B owns it %d "
                 "(handed over with its record, B's handoffs in %llu), A holds a ghost %d",
                 localAfterTick, lb != nullptr && !lb->IsGhost(),
                 (unsigned long long)entB.Stats().handoffsIn, la != nullptr && la->IsGhost()));
  }

  // ======== P + R + W: a remote promotion; retire and activation over W; a
  // give-back -- set up together, watched tick by tick ==========================
  {
    const DistantMobs::Stats a0 = aSt;
    // P: a walker past the +z face, at the overlap's far half.
    const int px = vox(Awo.x + 28) + 8 - (int)(hs.x * 0.5f);
    const int pz0 = vox(Awo.z + (int)kNChunk) + 20;
    const uint64_t pId = makeDistant("duelist", px, pz0);
    goTo(pId, Vec3{(float)px + hs.x * 0.5f, 0.0f, (float)vox(Awo.z + 27)});
    // R: two distant creatures +-1,600 in x, two parked records +-1,350.
    const int rz = (int)aCz;
    const uint64_t r1 = makeDistant("dummy", (int)(aCx + 1600.0f), rz);
    const uint64_t r2 = makeDistant("dummy", (int)(aCx - 1600.0f), rz);
    Vec3 r3At{}, r4At{}, wAt{};
    const bool recs = makeParked((int)(aCx + 1350.0f), rz, r3At) &&
                      makeParked((int)(aCx - 1350.0f), rz, r4At) &&
                      // W: inside B's window, past A's.
                      makeParked(vox(Awo.x + 40) + 4, rz, wAt);
    check(pId != 0 && r1 != 0 && r2 != 0 && recs, "P/R/W fixture: a creature or record refused");
    const Vec3 r2At = c.mobs.Distant().Find(r2) != nullptr ? c.mobs.Distant().Find(r2)->origin : Vec3{};
    // Watchers.
    int stepNo = 0;
    int pHandedAt = -1, pPromotedAt = -1;
    DistantMob pCopy;
    bool pDrawn = false, pPromoted = false, pLocalOnA = false;
    bool pHpSame = false, pXzSame = false, pBrainSame = false, pNotOnA = false;
    uint64_t wId = 0;
    int wHandedAt = -1, wReturnedAt = -1;
    bool wBackClean = false;
    bool rChecked = false, r1Kept = false, r2Retired = false, r3Activated = false,
         r4Parked = false;
    postA = [&]() {
      if (pHandedAt < 0)
        for (const DistantTransit& t : c.mobs.DistantInTransit())
          if (t.d.id == pId) {
            pHandedAt = stepNo;
            pCopy = t.d;
          }
      // W's id is the activation's (a fresh one): found in the tier, or
      // already in transit (activation and the hand-in share one Unpark).
      auto isW = [&](const DistantMob& d) {
        return d.activated && std::fabs(d.origin.x - wAt.x) < 2.0f &&
               std::fabs(d.origin.z - wAt.z) < 2.0f;
      };
      if (wId == 0) {
        for (const DistantMob& d : c.mobs.Distant().All())
          if (isW(d)) wId = d.id;
        for (const DistantTransit& t : c.mobs.DistantInTransit())
          if (isW(t.d)) wId = t.d.id;
      }
      if (wId != 0 && wHandedAt < 0 && InTransit(c.mobs, wId)) wHandedAt = stepNo;
      // ATTRIBUTION for "near on A": the walker's last state in A's tier, the
      // answers the remote-promotion choice reads, and the host pass's
      // counters, printed once at the tick it turned up near on A.
      static DistantMob pLast;
      static bool pLastValid = false;
      if (const DistantMob* d = c.mobs.Distant().Find(pId)) {
        pLast = *d;
        pLastValid = true;
      }
      if (const Mob* m = c.mobs.FindMobById(pId); m != nullptr && !m->IsGhost()) {
        if (!pLocalOnA && pLastValid) {
          const Vec3 feet{pLast.origin.x + pLast.worldSize.x * 0.5f, pLast.origin.y,
                          pLast.origin.z + pLast.worldSize.z * 0.5f};
          const uint32_t own = c.mobs.OwnershipAnswerForTest(pId, feet);
          std::printf("distant-mp: P near on A at step %d: last distant origin (%.1f,%.1f,%.1f) "
                      "chunk (%d,%d,%d), A wo (%d,%d,%d), B wo (%d,%d,%d); owner answer %u; goes to "
                      "peer %d; A promoted +%llu handInsSent +%llu overflowFilled +%llu peers %zu "
                      "peerView %d\n",
                      stepNo, (double)pLast.origin.x, (double)pLast.origin.y, (double)pLast.origin.z,
                      (int)std::floor(feet.x / 16), (int)std::floor(feet.y / 16),
                      (int)std::floor(feet.z / 16), Awo.x, Awo.y, Awo.z, Bwo.x, Bwo.y, Bwo.z, own,
                      c.mobs.DistantGoesToPeer(pLast) ? 1 : 0,
                      (unsigned long long)(aSt.promoted - a0.promoted),
                      (unsigned long long)(aSt.handInsSent - a0.handInsSent),
                      (unsigned long long)(aSt.overflowFilled - a0.overflowFilled),
                      c.mobs.DistantPeerCountForTest(), entA.HavePeerView() ? 1 : 0);
        }
        pLocalOnA = true;
      }
    };
    preA = [&]() {
      // W comes back: A's apply of B's "returned" just put it in the tier.
      if (wId != 0 && wHandedAt >= 0 && wReturnedAt < 0 &&
          c.mobs.Distant().Find(wId) != nullptr && !InTransit(c.mobs, wId)) {
        wReturnedAt = stepNo;
        wBackClean = far.Distant().Find(wId) == nullptr && aSt.donesReturned > a0.donesReturned;
      }
    };
    midB = [&]() {
      // B DREW IT FROM THE VIEW: the mirror's entry, and a statue item there.
      if (pDrawn || pHandedAt >= 0) return;
      for (const DistantViewMob& v : far.DistantViewMobs()) {
        if (v.id != pId || !v.haveShape) continue;
        std::vector<DistantDrawItem> items;
        far.AppendDistantViewDrawItems(items);
        for (const DistantDrawItem& it : items)
          if (it.look.Valid() && SameBits(it.origin.x, v.origin.x) &&
              SameBits(it.origin.z, v.origin.z))
            pDrawn = true;
      }
    };
    postBUnpark = [&]() {
      if (pPromoted) return;
      const Mob* m = far.FindMobById(pId);
      if (m == nullptr || m->IsGhost()) return;
      pPromoted = true;
      pPromotedAt = stepNo;
      pHpSame = pCopy.limbHp.size() >= (size_t)base;
      for (int li = 0; li < base && pHpSame; li++)
        pHpSame = SameBits(far.LimbHp(pId, li), pCopy.limbHp[(size_t)li]);
      const Vec3 o = m->Origin();
      pXzSame = SameBits(o.x, pCopy.origin.x) && SameBits(o.z, pCopy.origin.z);
      out.pSnapDy = o.y - pCopy.origin.y;
      const ai::Brain* b = far.MobBrain(pId);
      const ai::Profile* pa = b != nullptr ? far.Behaviors().At(b->profile) : nullptr;
      const ai::Profile* pb = c.mobs.Behaviors().At(pCopy.brain.profile);
      pBrainSame = b != nullptr && pa != nullptr && pb != nullptr && pa->name == pb->name &&
                   b->hasTarget == pCopy.brain.hasTarget && b->targetId == pCopy.brain.targetId;
      pNotOnA = c.mobs.Distant().Find(pId) == nullptr && c.mobs.FindMobById(pId) == nullptr;
    };
    const int maxSteps = (int)BaselineNumber("distantMp.pwStepsMax", 420);
    for (stepNo = 1; stepNo <= maxSteps; stepNo++) {
      step();
      if (stepNo == 6 && !rChecked) {
        rChecked = true;
        r1Kept = c.mobs.Distant().Find(r1) != nullptr;
        r2Retired = c.mobs.Distant().Find(r2) == nullptr && parkedNear(r2At) == 1;
        int nearR3 = 0;
        for (const DistantMob& d : c.mobs.Distant().All())
          if (d.activated && std::fabs(d.origin.x - r3At.x) < 2.0f &&
              std::fabs(d.origin.z - r3At.z) < 2.0f)
            nearR3++;
        r3Activated = nearR3 == 1 && parkedNear(r3At) == 0;
        r4Parked = parkedNear(r4At) == 1;
      }
      if (pPromoted && wReturnedAt >= 0 && rChecked && stepNo >= pPromotedAt + 3) break;
    }
    preA = postA = midB = postBUnpark = nullptr;
    out.pWalk = pHandedAt;
    out.pPromote = pPromoted && pHandedAt >= 0 ? pPromotedAt - pHandedAt : -1;
    out.wWait = wReturnedAt >= 0 && wHandedAt >= 0 ? wReturnedAt - wHandedAt : -1;
    check(rChecked && r1Kept && r2Retired && r3Activated && r4Parked,
          Format("R: kept toward B %d, retired away %d, activated toward B %d, the record away "
                 "still parked %d",
                 r1Kept, r2Retired, r3Activated, r4Parked));
    const int walkMax = (int)BaselineNumber("distantMp.walkMaxTicks", 300);
    const int promoteMax = (int)BaselineNumber("distantMp.promoteMaxTicks", 60);
    check(pHandedAt > 0 && pHandedAt <= walkMax && pPromoted && out.pPromote <= promoteMax &&
              pDrawn && !pLocalOnA && pHpSame && pXzSame && pBrainSame && pNotOnA &&
              !InTransit(c.mobs, pId),
          Format("P: handed in after %d ticks (max %d), promoted ON B %d after %d more (max %d), "
                 "drawn on B from the view first %d, never near on A %d, base hp bit-identical %d, "
                 "x/z bit-identical %d (y snapped %+.2f), brain profile + target %d, off A %d, "
                 "transit closed %d",
                 pHandedAt, walkMax, pPromoted, out.pPromote, promoteMax, pDrawn, !pLocalOnA,
                 pHpSame, pXzSame, (double)out.pSnapDy, pBrainSame, pNotOnA,
                 !InTransit(c.mobs, pId)));
    const int giveBackSlack = (int)BaselineNumber("distantMp.giveBackSlackTicks", 12);
    check(wId != 0 && wHandedAt > 0 && wReturnedAt > 0 &&
              out.wWait >= (int)kHandInMaxWaitTicks &&
              out.wWait <= (int)kHandInMaxWaitTicks + giveBackSlack && wBackClean,
          Format("W: activated in B's window %d, handed in at %d, given back after %d ticks "
                 "(%u..%u), clean on both sides %d",
                 wId != 0, wHandedAt, out.wWait, kHandInMaxWaitTicks,
                 kHandInMaxWaitTicks + (uint32_t)giveBackSlack, wBackClean));
  }

  // ======== D: a disconnect with a creature in transit =========================
  {
    // x chunk 30: in the overlap's far half (B nearer), and two chunks clear
    // of P's creature, which stands near on B at x chunk 28 by now -- at 26
    // the walker spent 300 ticks trying to get round it at the face.
    const int xx = vox(Awo.x + 30) + 8 - (int)(hs.x * 0.5f);
    const int xz0 = vox(Awo.z + (int)kNChunk) + 20;
    const uint64_t xId = makeDistant("duelist", xx, xz0);
    goTo(xId, Vec3{(float)xx + hs.x * 0.5f, 0.0f, (float)vox(Awo.z + 27)});
    bool dropNow = false, inTransitAtDrop = false, waitingAtDrop = false, restoredOnA = false;
    int droppedAt = -1, stepNo = 0;
    const uint64_t restored0 = aSt.transitRestored;
    postA = [&]() {
      if (!dropNow && InTransit(c.mobs, xId)) dropNow = true;
    };
    midB = [&]() {
      if (!dropNow || !linked) return;
      inTransitAtDrop = InTransit(c.mobs, xId);
      const DistantMob* w = far.Distant().Find(xId);
      waitingAtDrop = w != nullptr && w->handIn;
      // THE TWO netDrops (main.cpp's order: the entity half, then the tier,
      // then "everything is mine" closures).
      entA.Disconnect();
      c.mobs.DistantNetDisconnect(tick);
      c.mobs.SetOwnershipFn([](uint64_t, Vec3) { return 0u; });
      c.debris.ClearOwnershipFn();
      c.debris.SetChunkOwnedFn(nullptr);
      entB.Disconnect();
      far.DistantNetDisconnect(tick);
      far.SetOwnershipFn([](uint64_t, Vec3) { return kPeer; });
      farDebris.ClearOwnershipFn();
      farDebris.SetChunkOwnedFn(nullptr);
      linked = false;
      droppedAt = stepNo;
      restoredOnA = c.mobs.Distant().Find(xId) != nullptr && !InTransit(c.mobs, xId) &&
                    aSt.transitRestored > restored0;
    };
    const int maxSteps = (int)BaselineNumber("distantMp.dStepsMax", 400);
    const int dMax = (int)BaselineNumber("distantMp.disconnectPromoteMaxTicks", 60);
    for (stepNo = 1; stepNo <= maxSteps; stepNo++) {
      step();
      // ATTRIBUTION while it has not been handed in: where it is, in which
      // list, and the waits the host's hand-in pass counted.
      if (droppedAt < 0 && stepNo % 50 == 0) {
        const DistantMob* d = c.mobs.Distant().Find(xId);
        const Mob* na = c.mobs.FindMobById(xId);
        const Mob* nb = far.FindMobById(xId);
        const auto& pw = c.mobs.DistantPeers();
        std::printf("distant-mp: D step %d: tier %d at (%.1f,%.1f,%.1f) asleep %d goal %d, transit %d, "
                    "near A %d (ghost %d), near B %d (ghost %d); A waits room %llu budget %llu flip %llu, "
                    "peer free %u (have %d), sent %llu\n",
                    stepNo, d != nullptr, d ? (double)d->origin.x : 0.0, d ? (double)d->origin.y : 0.0,
                    d ? (double)d->origin.z : 0.0, d ? (int)d->asleep : -1,
                    d ? (int)d->brain.routine.active : -1, InTransit(c.mobs, xId), na != nullptr,
                    na && na->IsGhost(), nb != nullptr, nb && nb->IsGhost(),
                    (unsigned long long)aSt.handInRoomWaits, (unsigned long long)aSt.handInBudgetWaits,
                    (unsigned long long)aSt.handInFlipWaits, pw.empty() ? 0u : pw[0].livingFree,
                    pw.empty() ? -1 : (int)pw[0].haveFree, (unsigned long long)aSt.handInsSent);
      }
      if (droppedAt > 0) {
        const Mob* m = far.FindMobById(xId);
        if (m != nullptr && !m->IsGhost()) {
          out.dPromote = stepNo - droppedAt;
          break;
        }
        if (stepNo - droppedAt > dMax) break;
      }
    }
    postA = midB = nullptr;
    check(droppedAt > 0 && inTransitAtDrop && waitingAtDrop && restoredOnA && out.dPromote >= 0 &&
              out.dPromote <= dMax,
          Format("D: dropped with it in transit on A %d and waiting on B %d; A restored it %d; B "
                 "promoted its waiting room after %d ticks (max %d)",
                 inTransitAtDrop, waitingAtDrop, restoredOnA, out.dPromote, dMax));
  }

  // ---- the numbers, then nothing left behind ----
  {
    const ::net::EntitySync::Counters& s = entA.Stats();
    out.viewEntries = s.viewEntriesOut;
    out.viewShapes = s.viewShapesOut;
    out.viewGones = s.viewGonesOut;
    out.viewBytes = s.viewBytesOut;
    out.viewBatches = s.viewBatchesOut;
  }
  out.ticks = tick - t0;
  out.handOuts = bSt.handOutsSent;
  out.handInsSent = aSt.handInsSent - aRun0.handInsSent;
  out.handInsPromoted = bSt.handInsPromoted;
  out.handInsReturned = bSt.handInsReturned;
  out.transitRestored = aSt.transitRestored - aRun0.transitRestored;
  // NEVER LOST: the tier's `failed` counts every creature a refusal could not
  // park (and every record that would not patch) -- it must not move here.
  check(aSt.failed == aRun0.failed && bSt.failed == 0,
        Format("a creature lost: A failed %llu, B failed %llu",
               (unsigned long long)(aSt.failed - aRun0.failed), (unsigned long long)bSt.failed));
  out.digest = dg.h;
  cleanup();
  return out;
}

Status GateDistantMp(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const MpRun a = RunMp(c);
  const MpRun b = RunMp(c);
  // Home as the next gate expects it (the runs' near creatures stood on it).
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const bool same = a.digest == b.digest;
  const double perBatch = a.viewBatches > 0 ? (double)a.viewBytes / (double)a.viewBatches : 0.0;
  const double perTick = a.ticks > 0 ? (double)a.viewBytes / (double)a.ticks : 0.0;
  RecordObserved("distantMp.viewBytesPerBatch", perBatch);
  RecordObserved("distantMp.viewBytesPerTick", perTick);
  RecordObserved("distantMp.roundTripTicks", (double)a.hRoundTrip);
  RecordObserved("distantMp.walkTicks", (double)a.pWalk);
  RecordObserved("distantMp.promoteTicks", (double)a.pPromote);
  RecordObserved("distantMp.giveBackTicks", (double)a.wWait);
  RecordObserved("distantMp.disconnectPromoteTicks", (double)a.dPromote);
  std::string fails;
  for (const std::string& f : a.fails) fails += (fails.empty() ? "" : " | ") + f;
  for (const std::string& f : b.fails) fails += (fails.empty() ? "" : " | ") + ("run 2: " + f);
  detail = Format(
      "%s%sT twice-run %s (%016llx / %016llx) | %llu ticks: hand-outs %llu, hand-ins %llu "
      "(promoted %llu, given back %llu), transits restored %llu, H bleed %.4f | view: %llu "
      "entries (%llu with the shape), %llu gones, %llu bytes in %llu batches = %.1f B/batch, "
      "%.1f B/tick | H round trip %d, P walk %d + promote %d (snap dy %+.2f), W give-back %d, "
      "D promote %d ticks",
      fails.c_str(), fails.empty() ? "" : " || ", same ? "equal" : "DIFFER",
      (unsigned long long)a.digest, (unsigned long long)b.digest,
      (unsigned long long)a.ticks, (unsigned long long)a.handOuts,
      (unsigned long long)a.handInsSent, (unsigned long long)a.handInsPromoted,
      (unsigned long long)a.handInsReturned, (unsigned long long)a.transitRestored,
      (double)a.hBleed, (unsigned long long)a.viewEntries, (unsigned long long)a.viewShapes,
      (unsigned long long)a.viewGones, (unsigned long long)a.viewBytes,
      (unsigned long long)a.viewBatches, perBatch, perTick, a.hRoundTrip, a.pWalk, a.pPromote,
      (double)a.pSnapDy, a.wWait, a.dPromote);
  return a.fails.empty() && b.fails.empty() && same ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DistantMpGates() {
  static const std::vector<Gate> g = {
      {"distant-mp", "mob", {}, false, GateDistantMp, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
