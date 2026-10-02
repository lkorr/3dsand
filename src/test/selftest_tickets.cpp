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

}  // namespace

const std::vector<Gate>& TicketGates() {
  // No deps: each gate builds its own world and regenerates on the way out.
  static const std::vector<Gate> g = {
      {"ticket-settle", "sim", {}, false, GateTicketSettle},
      {"ticket-land", "sim", {}, false, GateTicketLand},
  };
  return g;
}

}  // namespace selftest
