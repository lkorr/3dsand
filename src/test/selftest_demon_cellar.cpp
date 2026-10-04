// selftest_demon_cellar.cpp — THE HARROWBY SMITHY CELLAR AND THE BOOK
// (docs/PLAN_demons.md D2).
//
//   harrowby-cellar  A CONTENT gate on the GAME's map, like village-harrowby
//                    (whose exit contract it shares). It switches to the map,
//                    regenerates the window round the smithy -- which queues
//                    the map's edit layer, where the cellar lives
//                    (scripts/paint_cellar.mjs) -- ticks THE tick until the
//                    layer has drained and the cellar's loose matter has had
//                    time to settle, reads the real voxels back, and asserts:
//                      A. the refs load clean and the book is a `readable`
//                         ref whose cell is matter, under the smithy floor;
//                      B. THERE IS A CELLAR: air under the smithy's footprint,
//                         at least cellar.minAirCells of it;
//                      C. IT IS REACHABLE: a walker flood (the player's box,
//                         7x7x17, resting on what is under it, up 6 / down 8 -- the
//                         player's step) from the bedroom's waynode reaches
//                         at least cellar.minReachableBelow standing cells
//                         2 m or more under the floor;
//                      D. THE RING IS CLOSED: on the cellar floor, a 2D flood
//                         from the salt's centroid over columns with no salt
//                         in a 3-cell slab does not get out (D1's detector
//                         rule, done locally: D1 had not landed). Its inner
//                         diameter is in [cellar.ringMinDiameter, ...Max],
//                         and the same flood with one band of the ring
//                         erased DOES get out (the check can fail);
//                      E. a candle flame at each cardinal point of the ring,
//                         and piles of sulfur, iron and quicksilver near it
//                         -- the quicksilver still in its dish after the
//                         settle ticks (it is a liquid);
//                      F. READING GRANTS THE NAME: the harness body stands at
//                         the lectern and presses USE on the book (TickInput
//                         useRef, through TickAuthority), pages through with
//                         [continue], and at the end owns the glyph
//                         `summon_skerrick` and the world knows the flag
//                         name:skerrick. If glyphs.json has no such glyph yet
//                         (D1 owns it), the gate first proves the FAIL-SOFT
//                         path -- a load warning naming it and a counted,
//                         harmless refused grant -- and then reads again
//                         against a library with a stand-in glyph of that
//                         name, so the grant itself is still asserted.
//                    SANDVOX_CELLAR_SHOT=<path.bmp> also writes a picture of
//                    the cellar from the foot of the stair.
//
// The voxel box, the start node and the book are read from the map's own refs
// and the voxels; the only coordinates written here are the box the readback
// covers, chunk-aligned round the smithy.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "game/bodyreg.h"
#include "game/dialogue.h"
#include "game/mob.h"
#include "game/schedule.h"
#include "game/session.h"
#include "phys/debris.h"
#include "sim/biomes.h"
#include "sim/chunkstore.h"
#include "sim/microbody.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/worldedit.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "world/refs.h"
#include "world/refs_game.h"
#include "world/refs_npc.h"
#include "world/structures.h"

using namespace sandvox;

namespace selftest {
namespace {

int FloorDiv16(int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }

std::string CellarMap() {
  if (const char* e = std::getenv("SANDVOX_VILLAGE_MAP"); e && *e) return e;
  return CurrentTuning().world.mapLayer.empty() ? std::string("default")
                                                : CurrentTuning().world.mapLayer;
}

// A box of real voxels (material ids), chunk-aligned, read back chunk by chunk.
struct VoxBox {
  IVec3 lo{};      // world cell of index 0
  IVec3 n{};       // size in cells (multiples of 16)
  std::vector<uint16_t> mat;
  bool In(int x, int y, int z) const {
    return x >= lo.x && y >= lo.y && z >= lo.z && x < lo.x + n.x && y < lo.y + n.y && z < lo.z + n.z;
  }
  size_t Idx(int x, int y, int z) const {
    return ((size_t)(z - lo.z) * n.y + (size_t)(y - lo.y)) * n.x + (size_t)(x - lo.x);
  }
  uint16_t At(int x, int y, int z) const { return In(x, y, z) ? mat[Idx(x, y, z)] : (uint16_t)0xFFFF; }
};

bool ReadBox(Ctx& c, IVec3 lo, IVec3 n, VoxBox& b, std::string& why) {
  b.lo = lo;
  b.n = n;
  b.mat.assign((size_t)n.x * n.y * n.z, 0);
  std::vector<uint32_t> w(kChunkVol);
  for (int cz = 0; cz < n.z / 16; cz++)
    for (int cy = 0; cy < n.y / 16; cy++)
      for (int cx = 0; cx < n.x / 16; cx++) {
        const IVec3 wc{(lo.x >> 4) + cx, (lo.y >> 4) + cy, (lo.z >> 4) + cz};
        if (!c.world.ChunkInWindow(wc)) {
          why = Format("chunk (%d,%d,%d) is outside the window", wc.x, wc.y, wc.z);
          return false;
        }
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(wc), 1, w.data(), "harrowby-cellar");
        for (int z = 0; z < 16; z++)
          for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
              b.mat[b.Idx(wc.x * 16 + x, wc.y * 16 + y, wc.z * 16 + z)] =
                  (uint16_t)(w[((uint32_t)z * kChunk + (uint32_t)y) * kChunk + (uint32_t)x] & 0xFFFu);
      }
  return true;
}

// The --shot-mob recipe (village-harrowby's Shot): bodies uploaded, warm
// frames so the shadow cache and the irradiance grid catch up, the last one
// grabbed.
void Shot(Ctx& c, Vec3 eye, Vec3 at, const std::string& path, int frames) {
  if (MicroBodySet* mbs = c.debris.MicroSet(); mbs != nullptr && mbs->dirty)
    c.sim.UploadMicroBodies(c.ctx.queue, *mbs);
  BodyRegistry reg(c.debris, c.mobs, nullptr);
  std::vector<BodyXformGpu> xf;
  reg.BuildXforms(xf);
  if (!xf.empty())
    c.ctx.queue.WriteBuffer(c.world.bodyXforms, 0, xf.data(), xf.size() * sizeof(BodyXformGpu));
  std::vector<MicroBodyInstGpu> microInsts;
  reg.BuildMicroInsts(microInsts);
  std::vector<BodyVoxInst> inst;
  reg.BuildInstances(inst);
  if (!inst.empty())
    c.ctx.queue.WriteBuffer(c.world.bodyInstances, 0, inst.data(), inst.size() * sizeof(BodyVoxInst));
  const uint32_t microCount = c.sim.UploadMicroBodyInsts(c.ctx.queue, microInsts);
  const Vec3 d = at - eye;
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  Camera cam;
  cam.yaw = std::atan2(d.z, d.x);
  cam.pitch = std::asin(std::clamp(d.y / std::max(len, 1e-3f), -1.0f, 1.0f));
  const uint32_t noon = (uint32_t)(0.5 * (double)TicksPerDay(CurrentTuning()));
  for (int f = 0; f < frames; f++) {
    WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)c.width / c.height, true, 11.7f,
                      kFarFogDensity, (float)c.height, noon);
    rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
    c.sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp =
        c.sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, c.width, c.height);
    c.sim.DrawWorld(rp);
    c.sim.DrawBodies(rp, (uint32_t)inst.size());
    c.sim.DrawMicroBodies(rp, microCount);
    rp.End();
    c.ctx.queue.Submit(enc.Finish());
  }
  c.ctx.WaitIdle();
  c.Grab(path.c_str());
}

Status GateHarrowbyCellar(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& w) {
    if (!ok && why.size() < 12) why.push_back(w);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint64_t idWas = c.mobs.NextIdCounter();
  const std::string prevMap = worldmap::ActiveMapName(CurrentTuning().world.mapLayer);
  const std::string mapName = CellarMap();
  const std::string ad = AssetDir();
  worldmap::SetMapOverride(mapName);
  // The exit contract of the structure / village gates.
  auto restore = [&]() {
    refs::ResetResidents();
    c.debris.Reset();
    c.mobs.Reset();
    c.mobs.SetParkFn(nullptr);
    c.mobs.SetUnparkPlacer(nullptr);
    c.mobs.ClearPlayerActors();
    c.mobs.SetNextIdCounter(idWas);
    c.stream.Store().Clear();
    worldmap::SetMapOverride(prevMap);
    biomes::EnvironmentStamp s2;
    std::string l2;
    ReloadEnvironment(c.ctx, c.sim, c.mats, s2, l2);
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    structures::TakeReapply();
  };
  biomes::EnvironmentStamp stamp;
  std::string log;
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
    restore();
    detail = "map '" + mapName + "' did not load: " + log;
    return Status::Fail;
  }

  // ---- A. the refs ---------------------------------------------------------------
  refs::RefStore st;
  st.LoadMap(ad, mapName);
  st.BindChunkStore(&c.stream.Store());
  int refWarn = 0;
  for (const std::string& w : st.Warnings()) {
    refWarn++;
    check(false, "refs warning: " + w);
  }
  const refs::Ref* smithy = st.Find("harrowby/smithy");
  const refs::Ref* book = nullptr;
  for (const auto& [id, r] : st.All())
    if (r.kind == "readable" && r.props.value("dialogue", "") == "osric_notes") book = &r;
  const refs::Ref* room = st.Find("harrowby/smithy/waynode_room_1");
  if (!check(smithy != nullptr && book != nullptr && room != nullptr,
             "harrowby/smithy, its waynode_room_1 and a readable ref with dialogue osric_notes "
             "must all be in the map's refs")) {
    restore();
    detail = "missing refs | FAIL: " + why[0];
    return Status::Fail;
  }
  // The smithy's footprint, from the blueprint (structures::Frame is the one
  // place every placed-house coordinate comes from).
  IVec3 hlo{}, hhi{};
  {
    structures::Asset a;
    std::string err;
    std::vector<std::string> aw;
    if (!structures::LoadAsset(ad, smithy->base, true, a, err, aw)) {
      restore();
      detail = "the smithy's blueprint did not load: " + err;
      return Status::Fail;
    }
    structures::MakeFrame(a, smithy->pos, smithy->yaw).Box(hlo, hhi);
  }
  const int floorY = smithy->pos.y;   // the first air cell over the house floor

  // ---- the world round the smithy, with the layer applied and settled --------------
  const int cx = (hlo.x + hhi.x) / 2, cz = (hlo.z + hhi.z) / 2;
  const IVec3 origin{FloorDiv16(cx - (int)kWorldN / 2), 0, FloorDiv16(cz - (int)kWorldN / 2)};
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  refs::ResetResidents();
  c.stream.OnRegen();
  c.world.SetWindowOrigin(origin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const IVec3 bookChunk{FloorDiv16(book->pos.x), FloorDiv16(book->pos.y), FloorDiv16(book->pos.z)};
  uint32_t tick = 27000;
  auto rig = std::make_unique<support::TickRig>(c, tick, bookChunk);
  const int settle = (int)BaselineNumber("cellar.settleTicks", 60);
  support::RunTicks(*rig, settle);
  const bool layerDrained = !WorldEditLayer().HasPending();
  check(layerDrained, Format("the edit layer still has %zu chunks queued after %d ticks",
                             WorldEditLayer().PendingChunks(), settle));

  // ---- read the voxels -----------------------------------------------------------
  // Chunk-aligned round the house, from 3 m under its floor's chunk to 1 m over.
  const IVec3 blo{FloorDiv16(hlo.x - 8) * 16, FloorDiv16(floorY - 48) * 16, FloorDiv16(hlo.z - 8) * 16};
  const IVec3 bhi{(FloorDiv16(hhi.x + 8) + 1) * 16, (FloorDiv16(floorY + 16) + 1) * 16,
                  (FloorDiv16(hhi.z + 8) + 1) * 16};
  VoxBox vb;
  std::string rwhy;
  if (!ReadBox(c, blo, IVec3{bhi.x - blo.x, bhi.y - blo.y, bhi.z - blo.z}, vb, rwhy)) {
    rig.reset();
    restore();
    detail = "readback: " + rwhy;
    return Status::Fail;
  }
  auto klass = [&](uint16_t m) -> uint32_t {
    return m < c.mats.size() ? c.mats[m].gpu.klass : (uint32_t)CLASS_SOLID;
  };
  auto passable = [&](uint16_t m) { return m == 0 || (m != 0xFFFF && klass(m) == CLASS_GAS); };
  auto floorish = [&](uint16_t m) {
    return m != 0 && m != 0xFFFF && (klass(m) == CLASS_SOLID || klass(m) == CLASS_POWDER);
  };
  auto named = [&](const char* n) -> uint16_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint16_t)i;
    return 0xFFFF;
  };
  const uint16_t kSalt = named("salt"), kSulfur = named("sulfur"), kIron = named("iron"),
                 kQuick = named("quicksilver"), kFlame = named("candle_flame");
  check(kSalt != 0xFFFF && kSulfur != 0xFFFF && kIron != 0xFFFF && kQuick != 0xFFFF && kFlame != 0xFFFF,
        "salt, sulfur, iron, quicksilver and candle_flame are all materials");

  // A. the book's cell is matter, under the floor.
  const uint16_t bookMat = vb.At(book->pos.x, book->pos.y, book->pos.z);
  check(bookMat != 0 && bookMat != 0xFFFF,
        Format("%s at (%d,%d,%d) is air or outside the box: nothing there to read", book->id.c_str(),
               book->pos.x, book->pos.y, book->pos.z));
  check(book->pos.y < floorY - 10 && book->pos.x >= hlo.x && book->pos.x <= hhi.x && book->pos.z >= hlo.z &&
            book->pos.z <= hhi.z,
        Format("the book (%d,%d,%d) is not under the smithy (box x %d..%d z %d..%d, floor %d)", book->pos.x,
               book->pos.y, book->pos.z, hlo.x, hhi.x, hlo.z, hhi.z, floorY));

  // B. air under the house.
  long long air = 0;
  for (int z = hlo.z; z <= hhi.z; z++)
    for (int y = blo.y; y <= floorY - 4; y++)
      for (int x = hlo.x; x <= hhi.x; x++)
        if (vb.In(x, y, z) && passable(vb.At(x, y, z))) air++;
  const double minAir = BaselineNumber("cellar.minAirCells", 100000);
  check((double)air >= minAir, Format("%lld air cells under the smithy floor, want >= %.0f", air, minAir));

  // C. the walker flood from the bedroom.
  constexpr int kFoot = 3, kHead = 17, kUp = 6, kDown = 8;
  const int NX = vb.n.x, NY = vb.n.y, NZ = vb.n.z;
  std::vector<uint8_t> upRun((size_t)NX * NY * NZ, 0);   // passable cells from here up (capped)
  for (int z = 0; z < NZ; z++)
    for (int x = 0; x < NX; x++) {
      int run = 0;
      for (int y = NY - 1; y >= 0; y--) {
        run = passable(vb.mat[((size_t)z * NY + y) * NX + x]) ? std::min(run + 1, 255) : 0;
        upRun[((size_t)z * NY + y) * NX + x] = (uint8_t)run;
      }
    }
  auto standable = [&](int x, int y, int z) {   // box-local
    if (y < 1 || x < kFoot || z < kFoot || x >= NX - kFoot || z >= NZ - kFoot) return false;
    // THE PLAYER'S BOX (Player::kHalfXZ 3, 2 x kHalfY = 17): every column of
    // its footprint clear from its bottom to its top, and SOMETHING under the
    // footprint to stand on -- on a stair the box rests on the highest tread
    // it covers, which is what makes headroom at a ceiling's edge honest.
    bool supported = false;
    for (int dz = -kFoot; dz <= kFoot; dz++)
      for (int dx = -kFoot; dx <= kFoot; dx++) {
        if (upRun[((size_t)(z + dz) * NY + y) * NX + (x + dx)] < kHead) return false;
        supported = supported || floorish(vb.mat[((size_t)(z + dz) * NY + (y - 1)) * NX + (x + dx)]);
      }
    return supported;
  };
  IVec3 start{room->pos.x - blo.x, -1, room->pos.z - blo.z};
  for (int y = room->pos.y - blo.y - 2; y <= room->pos.y - blo.y + 2; y++)
    if (standable(start.x, y, start.z)) {
      start.y = y;
      break;
    }
  int reachable = 0, below = 0, deepest = 1 << 30;
  IVec3 deepAt{};
  if (check(start.y >= 0, Format("no standing room at the bedroom waynode (%d,%d,%d)", room->pos.x,
                                 room->pos.y, room->pos.z))) {
    std::vector<uint8_t> seen((size_t)NX * NY * NZ, 0);
    std::deque<IVec3> q;
    q.push_back(start);
    seen[((size_t)start.z * NY + start.y) * NX + start.x] = 1;
    const int belowY = floorY - 20 - blo.y;
    while (!q.empty()) {
      const IVec3 p = q.front();
      q.pop_front();
      reachable++;
      const int wx = p.x + blo.x, wz = p.z + blo.z;
      if (p.y <= belowY && wx >= hlo.x && wx <= hhi.x && wz >= hlo.z && wz <= hhi.z) {
        below++;
        if (p.y + blo.y < deepest) {
          deepest = p.y + blo.y;
          deepAt = IVec3{wx, deepest, wz};
        }
      }
      const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& d : dirs) {
        const int nx = p.x + d[0], nz = p.z + d[1];
        for (int ny = std::max(1, p.y - kDown); ny <= std::min(NY - 1, p.y + kUp); ny++) {
          const size_t k = ((size_t)nz * NY + ny) * NX + nx;
          if (nx < 0 || nz < 0 || nx >= NX || nz >= NZ || seen[k] || !standable(nx, ny, nz)) continue;
          seen[k] = 1;
          q.push_back(IVec3{nx, ny, nz});
        }
      }
    }
  }
  const double minBelow = BaselineNumber("cellar.minReachableBelow", 1000);
  check((double)below >= minBelow,
        Format("from the bedroom (%d,%d,%d) a walker reaches %d standing cells, %d of them 2 m or more "
               "under the floor (want >= %.0f): no way down",
               room->pos.x, room->pos.y, room->pos.z, reachable, below, minBelow));

  // D. the ring, on the cellar floor (the deepest standing level reached).
  // The level under the house with the most salt (the ring's own row; not
  // the walker's floor, so a broken stair still lets this part report).
  int ringY = floorY - 32, ringRow = 0;
  for (int y = blo.y; y <= floorY - 4; y++) {
    int n = 0;
    for (int z = hlo.z; z <= hhi.z; z++)
      for (int x = hlo.x; x <= hhi.x; x++) n += vb.At(x, y, z) == kSalt ? 1 : 0;
    if (n > ringRow) {
      ringRow = n;
      ringY = y;
    }
  }
  long long sx = 0, sz = 0, saltN = 0;
  std::vector<uint8_t> wall((size_t)NX * NZ, 0);
  for (int z = 0; z < NZ; z++)
    for (int x = 0; x < NX; x++)
      for (int y = ringY; y <= ringY + 2; y++)
        if (vb.At(x + blo.x, y, z + blo.z) == kSalt) {
          wall[(size_t)z * NX + x] = 1;
          sx += x;
          sz += z;
          saltN++;
        }
  int ringCx = -1, ringCz = -1;
  if (saltN > 0) {
    ringCx = (int)(sx / saltN);
    ringCz = (int)(sz / saltN);
  }
  const int escapeR = (int)BaselineNumber("cellar.ringEscapeRadius", 40);
  // 4-connected flood over non-salt columns from the centre: (closed, area).
  auto flood = [&](const std::vector<uint8_t>& w) -> std::pair<bool, int> {
    if (ringCx < 0 || w[(size_t)ringCz * NX + ringCx]) return {false, 0};
    std::vector<uint8_t> in((size_t)NX * NZ, 0);
    std::vector<std::pair<int, int>> todo{{ringCx, ringCz}};
    in[(size_t)ringCz * NX + ringCx] = 1;
    int area = 0;
    while (!todo.empty()) {
      const auto [x, z] = todo.back();
      todo.pop_back();
      area++;
      if ((x - ringCx) * (x - ringCx) + (z - ringCz) * (z - ringCz) > escapeR * escapeR || x == 0 ||
          z == 0 || x == NX - 1 || z == NZ - 1)
        return {false, area};
      const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& e : d) {
        const int nx = x + e[0], nz = z + e[1];
        const size_t k = (size_t)nz * NX + nx;
        if (w[k] || in[k]) continue;
        in[k] = 1;
        todo.push_back({nx, nz});
      }
    }
    return {true, area};
  };
  const auto [closed, area] = flood(wall);
  const double innerD = 2.0 * std::sqrt((double)area / 3.14159265358979);
  check(saltN > 0, Format("no salt on the cellar floor (y %d..%d)", ringY, ringY + 2));
  check(closed, Format("the salt ring (%lld cells, centre %d,%d) is NOT closed: a flood from its centre "
                       "gets out", saltN, ringCx + blo.x, ringCz + blo.z));
  const double dMin = BaselineNumber("cellar.ringMinDiameter", 20), dMax = BaselineNumber("cellar.ringMaxDiameter", 40);
  check(innerD >= dMin && innerD <= dMax,
        Format("the ring's inner diameter is %.1f voxels, want %.0f..%.0f", innerD, dMin, dMax));
  // The control: the same check on the same ring with one band erased (a
  // radial cut east of the centre) must call it open.
  bool gapOpens = false;
  if (closed) {
    std::vector<uint8_t> cut = wall;
    for (int x = ringCx; x < std::min(NX, ringCx + escapeR); x++) cut[(size_t)ringCz * NX + x] = 0;
    gapOpens = !flood(cut).first;
    check(gapOpens, "a ring with one band erased still reads as closed: the check cannot fail");
  }

  // E. candles at the cardinal points, the three piles, the dish.
  int quarter[4] = {0, 0, 0, 0};   // +x, -x, +z, -z
  int flames = 0;
  std::map<uint16_t, int> pileN;
  long long qx = 0, qz = 0, qN = 0;
  std::vector<IVec3> quick;
  for (int z = 0; z < NZ; z++)
    for (int x = 0; x < NX; x++)
      for (int y = ringY; y <= ringY + 8; y++) {
        const uint16_t m = vb.At(x + blo.x, y, z + blo.z);
        const int dx = x - ringCx, dz = z - ringCz;
        const double r = std::sqrt((double)(dx * dx + dz * dz));
        if (m == kFlame) {
          flames++;
          if (r >= 17 && r <= 26) {
            if (std::abs(dz) * 4 <= std::abs(dx)) quarter[dx > 0 ? 0 : 1]++;
            if (std::abs(dx) * 4 <= std::abs(dz)) quarter[dz > 0 ? 2 : 3]++;
          }
        }
        if ((m == kSulfur || m == kIron || m == kQuick) && r <= 28) pileN[m]++;
        if (m == kQuick) {
          qx += x;
          qz += z;
          qN++;
          quick.push_back(IVec3{x, y, z});
        }
      }
  const bool cardinal = quarter[0] && quarter[1] && quarter[2] && quarter[3];
  check(cardinal, Format("candle flames by the ring at +x %d, -x %d, +z %d, -z %d: want one at each "
                         "cardinal point (radius 17..26)", quarter[0], quarter[1], quarter[2], quarter[3]));
  const int minPile = (int)BaselineNumber("cellar.minPileCells", 5);
  check(pileN[kSulfur] >= minPile && pileN[kIron] >= minPile && pileN[kQuick] >= minPile,
        Format("piles near the ring: sulfur %d, iron %d, quicksilver %d (want >= %d each)", pileN[kSulfur],
               pileN[kIron], pileN[kQuick], minPile));
  int strayQuick = 0;
  if (qN > 0) {
    const int mx = (int)(qx / qN), mz = (int)(qz / qN);
    for (const IVec3& p : quick)
      if (std::abs(p.x - mx) > 2 || std::abs(p.z - mz) > 2 || p.y != ringY) strayQuick++;
  }
  check(strayQuick == 0, Format("%d quicksilver cells left the dish after %d ticks", strayQuick, settle));

  // ---- F. reading the book grants the name -------------------------------------------
  dialogue::Store store;
  store.dir = ad + "/dialogue";
  store.items = &c.items;
  GlyphLibrary& glyphs = rig->Glyphs();
  {
    std::string gerr;
    check(LoadGlyphs(ad + "/spells/glyphs.json", c.mats, glyphs, gerr), "glyphs.json loads: " + gerr);
  }
  store.glyphs = &glyphs;
  store.Reload();
  const bool realGlyph = glyphs.Find("summon_skerrick") >= 0;
  int notesErrors = 0;
  bool missingWarned = false;
  for (const dialogue::Problem& p : store.problems) {
    if (p.file.find("osric_notes") == std::string::npos) continue;
    if (p.error) {
      notesErrors++;
      check(false, "osric_notes: " + p.Line());
    }
    if (!p.error && p.field.find("grant") != std::string::npos &&
        p.msg.find("summon_skerrick") != std::string::npos)
      missingWarned = true;
  }
  int longest = 0;
  if (const dialogue::Dialogue* d = store.lib.Find("osric_notes"))
    for (const dialogue::Node& n : d->nodes) longest = std::max(longest, (int)n.text.size());
  const int maxChars = (int)BaselineNumber("cellar.maxPageChars", 260);
  check(longest > 0 && longest <= maxChars,
        Format("the book's longest page is %d characters (want 1..%d: short pages)", longest, maxChars));

  rig->Authority().refs = &st;
  rig->Authority().talk = &store;
  PlayerSession& s = rig->Session();
  // At the lectern, on the cellar floor, facing the book.
  s.player.pos = Vec3{(float)book->pos.x - 4.5f, (float)ringY, (float)book->pos.z + 0.5f};
  s.player.vel = Vec3{0, 0, 0};
  int pages = 0;
  std::string lastUse;
  // One read: USE on the book, then [continue] until it closes. Returns
  // whether the conversation opened on osric_notes.
  auto read = [&]() {
    st.ClearUses();
    support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) {
      o.input.SetPressed(TB_USE, true);
      o.input.useRef = book->hash;
    });
    for (const refs::UseRecord& u : st.Uses()) lastUse = u.message.empty() ? (u.used ? "used" : "?") : u.message;
    const bool opened = s.talk.active && s.talk.dialogue == "osric_notes";
    pages = opened ? 1 : 0;
    for (int k = 0; k < 12 && s.talk.active; k++) {
      support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.input.talk = (int16_t)kTalkContinue; });
      if (s.talk.active) pages++;
    }
    return opened;
  };
  std::string grantLine;
  if (!realGlyph) {
    // D1 has not shipped the glyph: the FAIL-SOFT half. A load warning, a
    // read that runs to the end, a grant counted as refused, nothing owned.
    check(missingWarned, "osric_notes grants summon_skerrick, glyphs.json has no such glyph, and the "
                         "load did not warn");
    const bool opened = read();
    check(opened, "USE on the book did not open osric_notes (" + lastUse + ")");
    check(!s.talk.active && store.stats.refusedGrants >= 1 && store.stats.grants == 0,
          Format("with no glyph the read should end with a refused grant (grants %llu, refused %llu, "
                 "still open %d)", (unsigned long long)store.stats.grants,
                 (unsigned long long)store.stats.refusedGrants, (int)s.talk.active));
    // ...then the grant itself, against a stand-in of that name.
    if (!glyphs.glyphs.empty()) {
      GlyphDef g = glyphs.glyphs[0];
      g.id = "summon_skerrick";
      glyphs.glyphs.push_back(g);
    }
    store.ResetState();
    grantLine = Format("glyph absent (D1): warned %s, refused %llu; then a stand-in: ", missingWarned ? "yes" : "NO",
                       (unsigned long long)store.stats.refusedGrants);
  }
  const int gi = glyphs.Find("summon_skerrick");
  const bool ownedBefore = gi >= 0 && s.caster.inventory.Owns(gi);
  const bool opened = read();
  check(opened, "USE on the book did not open osric_notes (" + lastUse + ")");
  const bool owns = gi >= 0 && s.caster.inventory.Owns(gi);
  check(!ownedBefore && owns, Format("after reading, summon_skerrick owned: before %d, after %d", (int)ownedBefore,
                                     (int)owns));
  check(store.Flag("name:skerrick") == 1, "after reading, the world flag name:skerrick is not set");
  check(!s.talk.active, "the book did not close at its last page");
  grantLine += Format("%d pages, summon_skerrick owned %s, name:skerrick %d", pages, owns ? "YES" : "no",
                      store.Flag("name:skerrick"));

  // ---- the picture ----------------------------------------------------------------
  if (const char* shot = std::getenv("SANDVOX_CELLAR_SHOT"); shot && *shot) {
    // From the stair's foot (north-west of the lectern), looking across the
    // ring to the far candles.
    const Vec3 eye{(float)book->pos.x - 8.0f, (float)ringY + 17.0f, (float)ringCz + blo.z + 26.0f};
    const Vec3 at{(float)ringCx + blo.x + 4.0f, (float)ringY + 1.0f, (float)ringCz + blo.z - 4.0f};
    Shot(c, eye, at, shot, (int)BaselineNumber("cellar.shotFrames", 12));
    const Vec3 eye2{(float)ringCx + blo.x + 26.0f, (float)ringY + 22.0f, (float)ringCz + blo.z - 26.0f};
    const Vec3 at2{(float)ringCx + blo.x - 6.0f, (float)ringY, (float)ringCz + blo.z + 6.0f};
    std::string p2 = shot;
    const size_t dot = p2.rfind('.');
    p2 = (dot == std::string::npos ? p2 : p2.substr(0, dot)) + "_ring.bmp";
    Shot(c, eye2, at2, p2, (int)BaselineNumber("cellar.shotFrames", 12));
  }

  RecordObserved("cellar.airCells", (double)air);
  RecordObserved("cellar.reachableBelow", (double)below);
  RecordObserved("cellar.ringDiameter", innerD);
  RecordObserved("cellar.ringSalt", (double)saltN);
  rig.reset();
  refs::RefCtx rc{&st, &c.mobs, &c.world, &c.stream.Store()};
  rc.phys = &c.phys;
  rc.debris = &c.debris;
  st.DeactivateAll(rc, refs::RefEvent::Reset);
  restore();

  detail = Format(
      "map '%s': %d refs warnings | layer drained %s after %d ticks | %lld air cells under the smithy | "
      "walker from %s: %d standing cells, %d under the floor, deepest y %d at (%d,%d) | ring: %lld salt, "
      "centre (%d,%d) y %d, %s, inner diameter %.1f vox, cut ring opens %s | flames %d (cardinal %d/%d/%d/%d) | "
      "piles sulfur %d iron %d quicksilver %d (stray %d) | book: errors %d, longest page %d | %s",
      mapName.c_str(), refWarn, layerDrained ? "yes" : "NO", settle, air, room->id.c_str(), reachable, below,
      below > 0 ? deepest : -1, deepAt.x, deepAt.z, saltN, ringCx + blo.x, ringCz + blo.z, ringY,
      closed ? "CLOSED" : "OPEN", innerD, gapOpens ? "yes" : "no", flames, quarter[0], quarter[1], quarter[2],
      quarter[3], pileN[kSulfur], pileN[kIron], pileN[kQuick], strayQuick, notesErrors, longest,
      grantLine.c_str());
  if (!why.empty()) {
    detail += " | FAIL: " + why[0];
    for (size_t i = 1; i < why.size() && i < 6; i++) detail += " | " + why[i];
  }
  return why.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& CellarGates() {
  static const std::vector<Gate> g = {
      {"harrowby-cellar", "world", {}, false, GateHarrowbyCellar, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
