// selftest_tickets.cpp — chunk tickets (docs/PLAN_chunk_tickets.md §4).
//
//   ticket-settle  P1: a ticket 40 chunks outside the window over real terrain,
//                  2,048 sand poured at its centre by op. The sand is
//                  conserved, the active chunks fall asleep, the ticket
//                  releases itself kTicketIdleTicks later, the store holds the
//                  pile, a second ticket over the same box decodes it back
//                  from the store, and the whole run reproduces bit for bit.
//
// Every gate ticks THE tick (support::TickCursor -> TickAuthority): the ticket
// step lives in Stream::Update, which the real tick calls between ticks. Each
// gate clears the store first (Stream::OnRegen) — a second run that found the
// first run's released pile in the store would be a different world, which is
// the store working, not a determinism failure — and regenerates on the way
// out, so the gates after it find pristine terrain at an unmoved origin.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim/chunkstore.h"
#include "sim/stream.h"
#include "sim/tickets.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

int MatId(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (int)i;
  return -1;
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  // OnRegen BEFORE the worldgen: it drops every ticket and empties the store,
  // so the next run starts from procgen everywhere (see the file comment).
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// Mass of `mat` in eighths (a powder's state nibble is its mass) and an FNV of
// every word with the stamp/excite byte masked, over a ticket's 125 slots in
// local-index order. Read through the page table (ReadVoxelsSync).
struct BoxRead {
  uint64_t mass = 0;
  uint32_t cells = 0;
  uint32_t hash = 2166136261u;
};
BoxRead ReadTicket(Ctx& c, uint32_t ticket, uint32_t mat) {
  BoxRead r;
  std::vector<uint32_t> buf(kChunkVol);
  for (uint32_t l = 0; l < kTicketChunks; l++) {
    ReadVoxelsSync(c.ctx, c.world, World::TicketSlotBase(ticket) + l, 1, buf.data(),
                   "ticketRead");
    for (uint32_t k = 0; k < kChunkVol; k++) {
      const uint32_t w = buf[k];
      r.hash = (r.hash ^ (w & ~0x00FF0000u)) * 16777619u;
      if ((w & 0xFFFu) != mat) continue;
      r.cells++;
      r.mass += PowderMassOfState((w >> 12) & 0xFu);
    }
  }
  return r;
}

// The same count, for a box no ticket holds any more: every chunk the store
// kept is decoded from the store; a chunk it did not keep is what procgen
// makes, whose count was taken at activation (`pristine`, per local index).
bool CountStore(Ctx& c, IVec3 lo, uint32_t mat, const std::vector<uint64_t>& pristine,
                uint64_t& mass, uint32_t& stored) {
  mass = 0;
  stored = 0;
  std::vector<uint32_t> words(kChunkVol);
  for (int z = 0; z < (int)kTicketBoxN; z++)
    for (int y = 0; y < (int)kTicketBoxN; y++)
      for (int x = 0; x < (int)kTicketBoxN; x++) {
        const IVec3 wc{lo.x + x, lo.y + y, lo.z + z};
        const uint32_t l = World::TicketLocalIndex(wc);
        const std::vector<uint32_t>* rle = c.stream.Store().Get(wc);
        if (!rle) {
          mass += pristine[l];
          continue;
        }
        if (!RleDecodeChunk(rle->data(), rle->size() / 2, words.data())) return false;
        stored++;
        for (uint32_t w : words)
          if ((w & 0xFFFu) == mat) mass += PowderMassOfState((w >> 12) & 0xFu);
      }
  return true;
}

// Per-local-index mass of `mat` in a live ticket's slots.
std::vector<uint64_t> PerChunkMass(Ctx& c, uint32_t ticket, uint32_t mat) {
  std::vector<uint64_t> m(kTicketChunks, 0);
  std::vector<uint32_t> buf(kChunkVol);
  for (uint32_t l = 0; l < kTicketChunks; l++) {
    ReadVoxelsSync(c.ctx, c.world, World::TicketSlotBase(ticket) + l, 1, buf.data(),
                   "ticketPristine");
    for (uint32_t w : buf)
      if ((w & 0xFFFu) == mat) m[l] += PowderMassOfState((w >> 12) & 0xFu);
  }
  return m;
}

// Are all 27 ACTIVE chunks of ticket `i` clean in the published snapshot?
bool ActiveAsleep(Ctx& c, uint32_t i) {
  const WorldSnapshot& s = c.world.Snap();
  if (!s.valid || s.dirtyFlags.size() != kNumSlots) return false;
  for (uint32_t l = 0; l < kTicketChunks; l++) {
    const uint32_t slot = World::TicketSlotBase(i) + l;
    if (c.world.TicketSlotActive(slot) && s.dirtyFlags[slot] != 0) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// ticket-settle
// ---------------------------------------------------------------------------
struct SettleRun {
  bool live = false;
  uint32_t ticket = kTicketMax;
  IVec3 centre{}, lo{};
  uint32_t poured = 0;          // sand cells the op stream put down
  uint64_t base = 0;            // natural sand mass in the box before the pour
  uint64_t mass0 = 0, massSettled = 0, massStore = 0, massBack = 0;
  uint32_t storedChunks = 0;
  int asleepAt = -1;            // ticks after the pour the snapshot went quiet
  int releasedAt = -1;          // ticks after the pour the ticket left the table
  uint32_t hashMid = 0, hashSettled = 0;
  std::vector<uint32_t> ops;    // the TicketOp kinds the run recorded (twice-run)
  uint32_t faults = 0;
};

bool RunSettle(Ctx& c, SettleRun& R, std::string& why) {
  const int mSand = MatId(c, "sand");
  if (mSand < 0) { why = "no `sand` material"; return false; }
  Regenerate(c);
  const IVec3 o = c.world.WindowOrigin();
  // 40 chunks past the window's +X face, half way along Z.
  const int cx = o.x + (int)kNChunk - 1 + 40, cz = o.z + (int)kNChunk / 2;
  const int x0 = cx * (int)kChunk, z0 = cz * (int)kChunk;
  int ground = -1 << 30;
  for (int z = z0; z < z0 + (int)kChunk; z++)
    for (int x = x0; x < x0 + (int)kChunk; x++)
      ground = std::max(ground, World::TerrainHeight(x, z, kDefaultSeed));
  const int cy = (ground + 6) >> 4;
  R.centre = {cx, cy, cz};

  uint32_t t = 81000;
  support::TickCursor tick{c, t, R.centre};
  c.stream.TicketSet().Request(R.centre, TicketReason::Gate, t + 1);
  tick();  // the ticket step at the head of this tick activates it
  R.ticket = c.stream.TicketSet().TicketHolding(R.centre);
  if (R.ticket >= kTicketMax) {
    why = Format("no ticket holds the centre chunk (%d,%d,%d) after the request "
                 "(refused cap %llu, placement %llu, in window %llu)",
                 cx, cy, cz, (unsigned long long)c.stream.TicketSet().Stats().refusedCap,
                 (unsigned long long)c.stream.TicketSet().Stats().refusedPlacement,
                 (unsigned long long)c.stream.TicketSet().Stats().inWindow);
    return false;
  }
  R.live = true;
  R.lo = c.stream.TicketSet().BoxLo(R.ticket);
  c.ctx.WaitIdle();
  const std::vector<uint64_t> pristine = PerChunkMass(c, R.ticket, (uint32_t)mSand);
  for (uint64_t m : pristine) R.base += m;

  // 2,048 cells of sand, 16 x 8 x 16, three cells over the highest ground of
  // the centre chunk's footprint, written only into air.
  std::vector<CellOp> pour;
  for (int y = ground + 3; y < ground + 11; y++)
    for (int z = z0; z < z0 + (int)kChunk; z++)
      for (int x = x0; x < x0 + (int)kChunk; x++) {
        const uint32_t ci = c.world.ResidentCellIndex({x, y, z});
        if (ci == World::kTicketSlotNone) {
          why = Format("pour cell (%d,%d,%d) is not resident", x, y, z);
          return false;
        }
        pour.push_back({ci, PackVoxNew((uint32_t)mSand, 0) | kCellOpIfAir});
      }
  tick(std::vector<BrushOp>{}, pour);
  c.ctx.WaitIdle();
  R.mass0 = ReadTicket(c, R.ticket, (uint32_t)mSand).mass;
  R.poured = (uint32_t)((R.mass0 - R.base) / kPowderFull);

  const int kTicks = (int)BaselineNumber("ticketSettle.ticks", 200);
  for (int i = 1; i <= kTicks; i++) {
    tick();
    const Tickets::State st = c.stream.TicketSet().StateOf(R.ticket);
    if (st == Tickets::State::Live && R.asleepAt < 0 && ActiveAsleep(c, R.ticket))
      R.asleepAt = i;
    if (st == Tickets::State::Live && R.asleepAt < 0) {
      // keep looking
    }
    if (i == 40) {
      c.ctx.WaitIdle();
      R.hashMid = ReadTicket(c, R.ticket, (uint32_t)mSand).hash;
    }
    if (st != Tickets::State::Live && R.releasedAt < 0) {
      R.releasedAt = i;
      break;
    }
    // The last look at the slots while the ticket still owns them.
    if (st == Tickets::State::Live) {
      if (R.asleepAt >= 0 && i == R.asleepAt) {
        c.ctx.WaitIdle();
        const BoxRead b = ReadTicket(c, R.ticket, (uint32_t)mSand);
        R.massSettled = b.mass;
        R.hashSettled = b.hash;
      }
    }
  }
  // Let the release's keep decision land (kSnapshotLatency ticks) and the
  // eviction harvest: a dozen more ticks of nothing.
  for (int i = 0; i < 12; i++) tick();
  c.ctx.WaitIdle();
  if (!CountStore(c, R.lo, (uint32_t)mSand, pristine, R.massStore, R.storedChunks)) {
    why = "a stored chunk's RLE did not decode";
    return false;
  }
  // THE WINDOW ARRIVING, through the same store-hit door: a fresh ticket over
  // the same centre decodes the box from the store (FillSlots' store branch,
  // the one a window shift takes).
  c.stream.TicketSet().Request(R.centre, TicketReason::Gate, t + 1);
  tick();
  const uint32_t again = c.stream.TicketSet().TicketHolding(R.centre);
  if (again < kTicketMax) {
    c.ctx.WaitIdle();
    R.massBack = ReadTicket(c, again, (uint32_t)mSand).mass;
  }
  R.faults = c.world.Snap().pageFaults;
  return true;
}

Status GateTicketSettle(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  SettleRun A, B;
  std::string why;
  const bool okA = RunSettle(c, A, why);
  const bool okB = okA && RunSettle(c, B, why);
  Regenerate(c);
  if (!okA || !okB) {
    detail = why;
    return Status::Fail;
  }
  const int asleepMax = (int)BaselineNumber("ticketSettle.asleepByTickMax", 150);
  const int releaseLagMax =
      (int)BaselineNumber("ticketSettle.releaseAfterAsleepMax",
                          (double)(kTicketIdleTicks + 2 * World::kSnapshotLatency + 4));
  const bool poured = A.poured >= 2000;  // IfAir: a grass tuft can refuse a cell
  const bool conserved = A.massSettled == A.mass0 && A.massStore == A.mass0 &&
                         A.massBack == A.mass0;
  const bool asleep = A.asleepAt >= 0 && A.asleepAt <= asleepMax;
  const bool released = A.releasedAt >= 0 && A.asleepAt >= 0 &&
                        A.releasedAt - A.asleepAt <= releaseLagMax;
  const bool stored = A.storedChunks > 0;
  const bool same = A.hashMid == B.hashMid && A.hashSettled == B.hashSettled &&
                    A.asleepAt == B.asleepAt && A.releasedAt == B.releasedAt &&
                    A.massStore == B.massStore;
  const bool noFaults = A.faults == 0 && B.faults == 0;
  const bool zeroAtRest = c.stream.TicketSet().LiveCount() == 0;
  RecordObserved("ticketSettle.asleepAt", (double)A.asleepAt);
  RecordObserved("ticketSettle.releasedAt", (double)A.releasedAt);
  RecordObserved("ticketSettle.hash", Format("%08x", A.hashSettled));
  const bool ok = poured && conserved && asleep && released && stored && same &&
                  noFaults && zeroAtRest;
  detail = Format(
      "ticket #%u box (%d,%d,%d)+5 around chunk (%d,%d,%d), 40 chunks past the window; "
      "poured %u sand cells (natural sand in box %llu eighths); mass %llu at pour, "
      "%llu settled, %llu from the store (%u chunks kept), %llu decoded back by a "
      "second ticket%s; active chunks asleep %d ticks after the pour (allow %d), "
      "released %d ticks after (allow asleep + %d); page faults %u/%u; run twice: %s "
      "(settled hash %08x vs %08x); live after regen %u",
      A.ticket, A.lo.x, A.lo.y, A.lo.z, A.centre.x, A.centre.y, A.centre.z, A.poured,
      (unsigned long long)A.base, (unsigned long long)A.mass0,
      (unsigned long long)A.massSettled, (unsigned long long)A.massStore, A.storedChunks,
      (unsigned long long)A.massBack, conserved ? " - CONSERVED" : " - MASS MOVED",
      A.asleepAt, asleepMax, A.releasedAt, releaseLagMax, A.faults, B.faults,
      same ? "IDENTICAL" : "DIVERGED", A.hashSettled, B.hashSettled,
      c.stream.TicketSet().LiveCount());
  std::printf("ticket-settle: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// ticket-land (P2)
// ---------------------------------------------------------------------------
// Count `mat` over a box no ticket holds any more (store-kept chunks decoded,
// the rest are procgen and hold none of a material procgen never places).
uint32_t CountInStore(Ctx& c, IVec3 lo, uint32_t mat, uint32_t* hash) {
  uint32_t n = 0;
  uint32_t h = 2166136261u;
  std::vector<uint32_t> words(kChunkVol);
  for (int z = 0; z < (int)kTicketBoxN; z++)
    for (int y = 0; y < (int)kTicketBoxN; y++)
      for (int x = 0; x < (int)kTicketBoxN; x++) {
        const std::vector<uint32_t>* rle = c.stream.Store().Get({lo.x + x, lo.y + y, lo.z + z});
        if (!rle || !RleDecodeChunk(rle->data(), rle->size() / 2, words.data())) continue;
        for (uint32_t w : words) {
          h = (h ^ w) * 16777619u;
          if ((w & 0xFFFu) == mat) n++;
        }
      }
  if (hash) *hash = h;
  return n;
}

ParticleSpawn Stone(IVec3 cell, int vx, uint32_t mat) {
  ParticleSpawn s{};
  s.px = cell.x * 256 + 128;
  s.py = cell.y * 256 + 128;
  s.pz = cell.z * 256 + 128;
  s.vx = vx;
  s.payload = mat;
  return s;
}

struct LandRun {
  // arm A: a stone off the window edge
  int landTick = -1, activateTick = -1;
  uint64_t activations = 0;      // tickets opened during arm A (want exactly 1)
  IVec3 boxLo{};
  uint32_t inStore = 0, storeHash = 0;
  bool released = false;
  // arm B: the cap is full, the stone deposits and comes back later
  uint64_t refusedCap = 0, deposited = 0, respawned = 0;
  uint32_t parkedAtFull = 0, landedLater = 0;
  uint32_t faults = 0;
};

bool RunLand(Ctx& c, LandRun& R, std::string& why) {
  int mStone = MatId(c, "glass");
  if (mStone < 0) mStone = MatId(c, "iron");
  if (mStone < 0) { why = "no glass/iron material for the thrown stone"; return false; }
  Regenerate(c);
  const IVec3 o = c.world.WindowOrigin();
  // The far cascade is what a particle outside residency lands on: fill it
  // around the window centre, synchronously, before the throw.
  DrainFullRefill(c.ctx, c.world, c.sim,
                  IVec3{o.x + (int)kNChunk / 2, o.y + (int)kNChunk / 2, o.z + (int)kNChunk / 2});
  const int faceX = (o.x + (int)kNChunk) * (int)kChunk;  // first cell past +X
  const int zMid = (o.z + (int)kNChunk / 2) * (int)kChunk;
  const int hEdge = World::TerrainHeight(faceX - 6, zMid, kDefaultSeed);
  const int vMax = (int)CurrentTuning().sim.partMaxVel;  // 6 voxels/tick, ~20 m/s

  uint32_t t = 82000;
  support::TickCursor tick{c, t, IVec3{(faceX - 6) >> 4, hEdge >> 4, zMid >> 4}};
  const uint64_t act0 = c.stream.TicketSet().Stats().activated;
  // ---- ARM A: one stone off the +X face at full speed --------------------
  {
    support::TickOps ops;
    ops.spawns.push_back(Stone({faceX - 6, hEdge + 12, zMid}, vMax, (uint32_t)mStone));
    tick(ops);
  }
  bool second = false;
  for (int i = 1; i <= 200; i++) {
    tick();
    const WorldSnapshot& s = c.world.Snap();
    if (R.landTick < 0 && s.valid && s.ticketParked > 0) R.landTick = (int)s.tick - 82000;
    const uint64_t acts = c.stream.TicketSet().Stats().activated - act0;
    if (R.activateTick < 0 && acts > 0) {
      R.activateTick = (int)t - 82000;
      for (uint32_t k = 0; k < kTicketMax; k++)
        if (c.stream.TicketSet().StateOf(k) == Tickets::State::Live)
          R.boxLo = c.stream.TicketSet().BoxLo(k);
    }
    // THE SECOND STONE, once the first ticket is live: thrown the same way a
    // few cells over, it lands INSIDE the live box and must not open another.
    if (R.activateTick >= 0 && !second && (int)t - 82000 >= R.activateTick + 2) {
      support::TickOps ops;
      ops.spawns.push_back(Stone({faceX - 6, hEdge + 12, zMid + 3}, vMax, (uint32_t)mStone));
      tick(ops);
      second = true;
    }
    if (second && c.stream.TicketSet().LiveCount() == 0) { R.released = true; break; }
  }
  R.activations = c.stream.TicketSet().Stats().activated - act0;
  for (int i = 0; i < 12; i++) tick();  // the keep decision lands, the batch harvests
  c.ctx.WaitIdle();
  R.inStore = CountInStore(c, R.boxLo, (uint32_t)mStone, &R.storeHash);

  // ---- ARM B: the cap is full -------------------------------------------
  // Sixteen gate tickets along the -X side, then a stone placed just over the
  // far ground past the +X face: its requests are REFUSED, it deposits after
  // PARK_DEPOSIT_TICKS, sits on the CPU as a far landing, and comes back the
  // tick a ticket over its chunk is granted.
  const Tickets& T = c.stream.TicketSet();
  const uint64_t refused0 = T.Stats().refusedCap, dep0 = T.Stats().landingsDeposited,
                 resp0 = T.Stats().landingsRespawned;
  for (uint32_t k = 0; k < kTicketMax; k++) {
    const int cx = o.x - 12, cz = o.z - 40 + 6 * (int)k;
    const int h = World::TerrainHeight(cx * 16 + 8, cz * 16 + 8, kDefaultSeed);
    c.stream.TicketSet().Request({cx, (h + 6) >> 4, cz}, TicketReason::Gate, t + 1);
  }
  const int xB = faceX + 9 * (int)kChunk;   // 9 chunks past the face
  const int zB = zMid + 2 * (int)kChunk;
  const int hB = World::TerrainHeight(xB, zB, kDefaultSeed);
  {
    support::TickOps ops;
    ops.spawns.push_back(Stone({xB, hB + 3, zB}, 0, (uint32_t)mStone));
    tick(ops);
  }
  for (int i = 0; i < 30; i++) tick();
  R.refusedCap = T.Stats().refusedCap - refused0;
  R.deposited = T.Stats().landingsDeposited - dep0;
  R.parkedAtFull = T.Stats().landingsParked;
  // Free the cap the way the world does (they idle out), then ask for the
  // landing's chunk; the landing re-throws the tick it is resident.
  for (int i = 0; i < 120 && c.stream.TicketSet().LiveCount() > 0; i++) tick();
  const IVec3 landChunk{xB >> 4, (hB + 3) >> 4, zB >> 4};
  c.stream.TicketSet().Request(landChunk, TicketReason::Gate, t + 1);
  for (int i = 0; i < 8; i++) tick();
  R.respawned = T.Stats().landingsRespawned - resp0;
  const uint32_t holder = c.stream.TicketSet().TicketHolding(landChunk);
  if (holder < kTicketMax) {
    c.ctx.WaitIdle();
    std::vector<uint32_t> buf(kChunkVol);
    for (uint32_t l = 0; l < kTicketChunks; l++) {
      ReadVoxelsSync(c.ctx, c.world, World::TicketSlotBase(holder) + l, 1, buf.data(),
                     "ticketLandB");
      for (uint32_t w : buf) R.landedLater += (w & 0xFFFu) == (uint32_t)mStone ? 1u : 0u;
    }
  }
  R.faults = c.world.Snap().pageFaults;
  return true;
}

Status GateTicketLand(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  LandRun A, B;
  std::string why;
  const bool okA = RunLand(c, A, why);
  const bool okB = okA && RunLand(c, B, why);
  Regenerate(c);
  if (!okA || !okB) {
    detail = why;
    return Status::Fail;
  }
  const int maxLatency = (int)BaselineNumber("ticketLand.maxLatency", 8);
  const bool landed = A.landTick >= 0;
  const bool quick = landed && A.activateTick >= 0 &&
                     A.activateTick - A.landTick <= maxLatency;
  const bool oneTicket = A.activations == 1;
  const bool stored = A.released && A.inStore == 2;
  const bool refused = B.refusedCap > 0 && A.refusedCap > 0;
  const bool deposited = A.deposited == 1 && A.parkedAtFull >= 1;
  const bool cameBack = A.respawned == 1 && A.landedLater == 1;
  const bool same = A.landTick == B.landTick && A.activateTick == B.activateTick &&
                    A.storeHash == B.storeHash && A.inStore == B.inStore &&
                    A.landedLater == B.landedLater;
  const bool noFaults = A.faults == 0 && B.faults == 0;
  RecordObserved("ticketLand.latency", (double)(A.activateTick - A.landTick));
  const bool ok = quick && oneTicket && stored && refused && deposited && cameBack &&
                  same && noFaults;
  detail = Format(
      "arm A: stone off the +X face parked at t+%d, ticket live at t+%d (latency %d, "
      "allow %d), %llu ticket(s) opened for TWO stones (want 1), box (%d,%d,%d)+5 "
      "%s, %u stone voxel(s) in the store after (want 2); arm B (cap full): %llu "
      "request(s) refused, %llu deposited, %u parked on the CPU, %llu re-thrown "
      "when its chunk was granted, %u landed (want 1); page faults %u/%u; run "
      "twice: %s",
      A.landTick, A.activateTick, A.activateTick - A.landTick, maxLatency,
      (unsigned long long)A.activations, A.boxLo.x, A.boxLo.y, A.boxLo.z,
      A.released ? "released" : "NEVER RELEASED", A.inStore,
      (unsigned long long)A.refusedCap, (unsigned long long)A.deposited,
      A.parkedAtFull, (unsigned long long)A.respawned, A.landedLater, A.faults,
      B.faults, same ? "IDENTICAL" : "DIVERGED");
  std::printf("ticket-land: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// ticket-decay (P3)
// ---------------------------------------------------------------------------
// Embers on a wood slab inside a ticket. In the window this is a fire: wood's
// PAIR rule (neighbour tag:hot) turns it to ember, and ember EMITs fire. In a
// ticket only DECAY runs (sim_step.wgsl gInTicket), so every ember burns out on
// its own authored decay — to ash, smoke or air — and not one wood cell
// catches. The lifetime bound is read off the COMPILED table (the decay rules'
// summed chance), so it follows reactions.json and tuning without a re-pin.
struct DecayRun {
  uint32_t embers = 0, wood0 = 0;
  int goneAt = -1;               // ticks after placement the last ember went
  uint32_t woodKept = 0;         // of the slab's cells, still wood at the end
  uint32_t fireSeen = 0;         // fire / ember cells NOT placed by us, max over the run
  uint32_t ash = 0;
  uint32_t hash = 0;
  bool released = false;
  double meanLife = 0;
};

bool RunDecay(Ctx& c, DecayRun& R, std::string& why) {
  const int mEmber = MatId(c, "ember"), mWood = MatId(c, "wood"), mAsh = MatId(c, "ash"),
            mFire = MatId(c, "fire");
  if (mEmber < 0 || mWood < 0 || mAsh < 0 || mFire < 0) {
    why = "missing one of ember/wood/ash/fire";
    return false;
  }
  // The authored lifetime: 1 / (sum of the ember's ungated DECAY chances).
  {
    const MaterialGpu& g = c.mats[(size_t)mEmber].gpu;
    uint64_t sum = 0;
    for (uint32_t k = 0; k < g.reactCount; k++) {
      const ReactionGpu& r = c.reactions[g.reactOffset + k];
      if ((r.packed & 3u) != kReactDecay) continue;
      if ((r.cond & kCondGateMask) != 0u) continue;  // rain / light-gated
      sum += r.chance;
    }
    if (sum == 0) { why = "ember has no ungated decay rule"; return false; }
    R.meanLife = (double)kReactChanceDen / (double)sum;
  }
  Regenerate(c);
  const IVec3 o = c.world.WindowOrigin();
  // 40 chunks past the window's -Z face, half way along X.
  const int cx = o.x + (int)kNChunk / 2, cz = o.z - 40;
  const int x0 = cx * (int)kChunk + 4, z0 = cz * (int)kChunk + 4;
  int ground = -1 << 30;
  for (int z = z0; z < z0 + 8; z++)
    for (int x = x0; x < x0 + 8; x++)
      ground = std::max(ground, World::TerrainHeight(x, z, kDefaultSeed));
  const IVec3 centre{cx, (ground + 4) >> 4, cz};

  uint32_t t = 83000;
  support::TickCursor tick{c, t, centre};
  c.stream.TicketSet().Request(centre, TicketReason::Gate, t + 1);
  tick();
  const uint32_t tk = c.stream.TicketSet().TicketHolding(centre);
  if (tk >= kTicketMax) { why = "no ticket over the decay site"; return false; }
  // An 8 x 8 wood slab two cells over the highest ground (on a stone plinth
  // down to the ground, so it is not an island), 32 embers on every other
  // cell of its top.
  std::vector<CellOp> build;
  std::vector<IVec3> woodCells;
  auto put = [&](int x, int y, int z, uint32_t w) {
    const uint32_t ci = c.world.ResidentCellIndex({x, y, z});
    if (ci != World::kTicketSlotNone) build.push_back({ci, w});
  };
  const int mStone = MatId(c, "stone");
  for (int z = z0; z < z0 + 8; z++)
    for (int x = x0; x < x0 + 8; x++) {
      for (int y = World::TerrainHeight(x, z, kDefaultSeed) - 1; y <= ground; y++)
        put(x, y, z, PackVoxNew((uint32_t)std::max(mStone, 0), 0));
      put(x, ground + 1, z, PackVoxNew((uint32_t)mWood, 0));
      woodCells.push_back({x, ground + 1, z});
      if (((x - x0) + (z - z0)) % 2 == 0) {
        put(x, ground + 2, z, PackVoxNew((uint32_t)mEmber, 0));
        R.embers++;
      }
    }
  tick(std::vector<BrushOp>{}, build);
  R.wood0 = (uint32_t)woodCells.size();
  auto census = [&](uint32_t& ember, uint32_t& fire, uint32_t& ash, uint32_t* hash) {
    ember = fire = ash = 0;
    uint32_t h = 2166136261u;
    std::vector<uint32_t> buf(kChunkVol);
    for (uint32_t l = 0; l < kTicketChunks; l++) {
      ReadVoxelsSync(c.ctx, c.world, World::TicketSlotBase(tk) + l, 1, buf.data(), "ticketDecay");
      for (uint32_t w : buf) {
        const uint32_t m = w & 0xFFFu;
        h = (h ^ (w & ~0x00FF0000u)) * 16777619u;
        if (m == (uint32_t)mEmber) ember++;
        else if (m == (uint32_t)mFire) fire++;
        else if (m == (uint32_t)mAsh) ash++;
      }
    }
    if (hash) *hash = h;
  };
  const int cap = (int)BaselineNumber("ticketDecay.ticks", 2000);
  for (int i = 1; i <= cap; i++) {
    tick();
    if (c.stream.TicketSet().StateOf(tk) != Tickets::State::Live) break;
    if (i % 10 == 0) {
      c.ctx.WaitIdle();
      uint32_t e = 0, f = 0, a = 0;
      census(e, f, a, nullptr);
      R.fireSeen = std::max(R.fireSeen, f);
      if (e == 0 && R.goneAt < 0) {
        R.goneAt = i;
        R.ash = a;
        // The slab: every wood cell must still be wood.
        R.woodKept = 0;
        std::vector<uint32_t> buf(kChunkVol);
        for (const IVec3& wc : woodCells) {
          const uint32_t ci = c.world.ResidentCellIndex(wc);
          if (ci == World::kTicketSlotNone) continue;
          ReadVoxelsSync(c.ctx, c.world, ci / kChunkVol, 1, buf.data(), "ticketDecayWood");
          if ((buf[ci % kChunkVol] & 0xFFFu) == (uint32_t)mWood) R.woodKept++;
        }
        census(e, f, a, &R.hash);
        break;
      }
    }
  }
  // ...and with nothing left that can act, the ticket idles out.
  for (int i = 0; i < 120 && c.stream.TicketSet().StateOf(tk) == Tickets::State::Live; i++)
    tick();
  R.released = c.stream.TicketSet().StateOf(tk) != Tickets::State::Live;
  return true;
}

Status GateTicketDecay(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  DecayRun A, B;
  std::string why;
  const bool okA = RunDecay(c, A, why);
  const bool okB = okA && RunDecay(c, B, why);
  Regenerate(c);
  if (!okA || !okB) {
    detail = why;
    return Status::Fail;
  }
  const double mult = BaselineNumber("ticketDecay.lifetimeMult", 9);
  const int bound = (int)(A.meanLife * mult + 0.5);
  const bool burntOut = A.goneAt >= 0 && A.goneAt <= bound;
  const bool noSpread = A.woodKept == A.wood0 && A.fireSeen == 0;
  const bool someAsh = A.ash > 0;
  const bool same = A.goneAt == B.goneAt && A.hash == B.hash && A.woodKept == B.woodKept;
  RecordObserved("ticketDecay.goneAt", (double)A.goneAt);
  const bool ok = burntOut && noSpread && someAsh && A.released && same;
  detail = Format(
      "%u embers on a %u-cell wood slab 40 chunks past the window: all decayed by "
      "+%d ticks (authored mean life %.0f, allow %.0fx = %d); %u ash left; wood "
      "still wood %u / %u; fire cells seen %u (want 0: pair and emit rules do not "
      "run in a ticket); ticket %s; run twice: %s",
      A.embers, A.wood0, A.goneAt, A.meanLife, mult, bound, A.ash, A.woodKept, A.wood0,
      A.fireSeen, A.released ? "released by idle" : "STILL LIVE",
      same ? "IDENTICAL" : "DIVERGED");
  std::printf("ticket-decay: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& TicketGates() {
  // No deps: each gate builds its own world and regenerates on the way out.
  static const std::vector<Gate> g = {
      {"ticket-settle", "sim", {}, false, GateTicketSettle},
      {"ticket-land", "sim", {}, false, GateTicketLand},
      {"ticket-decay", "sim", {}, false, GateTicketDecay},
  };
  return g;
}

}  // namespace selftest
