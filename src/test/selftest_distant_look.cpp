// =============================================================================
// distant-look -- a creature past the window, drawn as a baked flipbook statue
// (game/distant_draw.h; docs/PLAN_distant_npcs.md §2.9, package D4a, §4).
// =============================================================================
//
// Pure of the distant tier: everything here is a plain list of DistantDrawItem
// handed to DistantDraw::Produce, which is exactly the seam D1 will fill.
//
//   A. BAKE. A living human (the avatar's def) stood up through THE tick, and a
//      second one killed and left to settle; both baked from their LIVE rigs.
//      Every pose present; the corpse's Lying is its real ragdoll pose.
//   B. A STATUE AT 40 M COVERS > N PIXELS (distantLook.minPixels40m). Drawn in
//      open air 40 m down +Z from the camera -- no occluder is part of the
//      claim -- as a with/without differential, exactly like the micro-body
//      render probe in selftest_mob.cpp.
//   C. SLOTS. 5000 items through Produce with the budget the real bodies and
//      a handful of arrows leave: never more than that is drawn; the living
//      before the dead; nearest first; the near crowd's transforms, handles
//      and micro instances identical with and without the statues; the total
//      within kMaxBodySlots.
//   D. WORDS. A cache with a budget of a few poses cycled over more looks and
//      poses than fit: resident words never over its budget, the LRU evicts,
//      nothing is fatal; the default cache stays under kDistantPoseWordsMax.
//      Words and cells per pose are RECORDED (distantLook.words*, cells*).
//   E. EPOCH. The same cache handed a second MicroBodySet (a materials reload,
//      as far as it can tell): it drops its packed pose WITHOUT freeing
//      anything in either set.
//   F. THE OWNER'S LOOK CHECK (not asserted): build/distant_look.bmp -- a row
//      of statues 20..80 m out in every pose, on the ground, the live creature
//      6 m from the camera for scale -- and build/distant_look_close.bmp, the
//      poses side by side at 4..9 m, where the 5 cm cells can be judged.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/bodyreg.h"
#include "game/camera.h"
#include "game/distant_draw.h"
#include "game/dye.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "gpu/resources.h"
#include "sim/celestial.h"
#include "sim/microbody.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

Status GateDistantLook(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("distant-look: FAILED %s\n", what.c_str());
    }
  };
  auto cleanupWorld = [&] {
    c.mobs.Reset();
    c.debris.Reset();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  };
  MicroBodySet* setP = c.mobs.MicroSet();
  int defIndex = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == kAvatarDefName) defIndex = (int)i;
  if (setP == nullptr || defIndex < 0) {
    detail = setP == nullptr ? "no micro-body set" : "no human def";
    return Status::Skip;
  }
  MicroBodySet& set = *setP;
  const MobDef& def = c.mobs.Defs()[(size_t)defIndex];

  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const IVec3 o = c.world.WindowOrigin();
  const int cx = (o.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (o.z + (int)kNChunk / 2) * (int)kChunk;
  const int cy = World::TerrainHeight(cx, cz, kDefaultSeed);
  support::TickRig rig(c, 34000, IVec3{cx >> 4, cy >> 4, cz >> 4});

  // ---- A. two humans, one of them dead, baked from their live rigs ---------
  const uint64_t idA = c.mobs.Spawn(defIndex, IVec3{cx, cy + 1, cz});
  const int bx = cx + 30, bz = cz + 10;
  const uint64_t idB =
      c.mobs.Spawn(defIndex, IVec3{bx, World::TerrainHeight(bx, bz, kDefaultSeed) + 1, bz});
  if (idA == 0 || idB == 0) {
    cleanupWorld();
    detail = "Spawn refused";
    return Status::Skip;
  }
  // A is DRESSED AND ARMED: a dyed tunic (worn shells follow their host
  // limb, the dye is baked into the statue's colour) and a sword in the right
  // hand (a held item follows the hand). Whatever is missing from the library
  // is skipped; the gear is part of the claim only through the key below.
  bool dressed = false, armed = false;
  if (Mob* a = c.mobs.FindMobById(idA)) {
    const ItemDef* shirt = nullptr;
    const ItemDef* blade = nullptr;
    for (const ItemDef& it : c.items.items) {
      if (it.name == "tunic") shirt = &it;
      if (it.name == "sword") blade = &it;
    }
    if (shirt != nullptr) {
      int slot = -1;
      for (int s = 0; s < kEquipSlotCount && slot < 0; s++)
        if (EquipSlotIsWorn(s) && EquipSlotAccepts(s, shirt->kind)) slot = s;
      dressed = slot >= 0 &&
                a->WearItem(shirt, slot, nullptr, DyePack(0.79f, 0.21f, 0.13f));
    }
    if (blade != nullptr) armed = a->EquipItem(blade, "held_right");
  }
  support::RunTicks(rig, 30);
  if (Mob* b = c.mobs.FindMobById(idB)) b->Die();
  support::RunTicks(rig, 75);   // the corpse settles
  Mob* mA = c.mobs.FindMobById(idA);
  Mob* mB = c.mobs.FindMobById(idB);
  if (mA == nullptr || mB == nullptr || !mA->Alive() || mB->Alive()) {
    cleanupWorld();
    detail = "fixture: the living one died or the dead one is gone";
    return Status::Skip;
  }

  DistantDraw dd;
  const DistantBake bakeA = dd.Bake(*mA, set);
  const DistantBake bakeB = dd.Bake(*mB, set);
  check(bakeA.baked && bakeA.key.Valid(), "the living human bakes");
  check(bakeB.baked && bakeB.key.Valid(), "the corpse bakes");
  check(dd.HasLook(bakeA.key.bare), "Bake also bakes the def's bare look");
  // Gear is part of the look: a dressed or armed creature is not the bare def.
  if (dressed || armed)
    check(bakeA.key.full != bakeA.key.bare && bakeA.key.full != bakeB.key.full,
          "the dressed, armed human's look is its own (gear in the key)");
  DistantDraw::PoseInfo pi[kDistantPoseCount];
  for (int p = 0; p < kDistantPoseCount; p++) {
    dd.PoseOf(bakeA.key.full, (DistantPose)p, pi[p]);
    check(pi[p].present && pi[p].cells > 0,
          Format("pose %s of the living look is baked", DistantPoseName((DistantPose)p)));
  }
  DistantDraw::PoseInfo lieB;
  dd.PoseOf(bakeB.key.full, DistantPose::Lying, lieB);
  check(lieB.present && lieB.fromCorpse, "the corpse's Lying is its own ragdoll pose");
  check(!pi[(int)DistantPose::Lying].fromCorpse || bakeA.key.full == bakeB.key.full,
        "the living look's Lying is the tipped idle");
  // restY is where the corpse's lowest cell is: on the ground it lies on (its
  // origin.y follows the pelvis's REST corner and sits below that when flat).
  {
    const Vec3 ob = mB->Origin();
    const int gx = ifloor(ob.x + def.worldSize.x * 0.5f);
    const int gz = ifloor(ob.z + def.worldSize.z * 0.5f);
    const float groundTop = (float)(World::TerrainHeight(gx, gz, kDefaultSeed) + 1);
    check(std::fabs(bakeB.restY - groundTop) <= 6.0f,
          Format("the corpse rests on the ground: restY %.2f, ground top %.0f, "
                 "origin.y %.2f", bakeB.restY, groundTop, ob.y));
  }
  const double bakeMs = dd.Stats().bakeMsMax;

  // ---- render plumbing (the underwater-body / microbody probe shape) -------
  const uint32_t W = c.width, H = c.height;
  const Tuning base = CurrentTuning();
  uint32_t noonTick = 0;
  {
    float bestUp = -2.0f;
    for (uint32_t t = 0; t < 200000u; t += 64u) {
      const float up = ComputeSky(base, (double)t).sunDir[1];
      if (up > bestUp) { bestUp = up; noonTick = t; }
    }
  }
  auto camAt = [](Vec3 eye, Vec3 at) {
    Camera cam;
    const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
    cam.yaw = std::atan2(dz, dx);
    cam.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    return cam;
  };
  auto viewOf = [&](const Vec3& eye, const Camera& cam) {
    DistantView v;
    v.eye = eye;
    v.fwd = cam.Forward();
    v.right = cam.Right();
    v.up = cam.Up();
    v.tanHalfFovY = std::tan(CurrentTuning().camera.fovY * 0.5f);
    v.aspect = (float)W / (float)H;
    return v;
  };
  std::vector<BodyXformGpu> xf;
  std::vector<MicroBodyInstGpu> mi;
  // Upload the registry's walk (real bodies + `extras`) and render `frames`
  // frames, grabbing the last. `withMicro` false draws the world alone.
  auto render = [&](const Vec3& eye, const Camera& cam,
                    const std::vector<RenderOnlyBody>* extras, bool shadows,
                    std::vector<uint8_t>& out, uint32_t frames) -> bool {
    if (set.dirty) c.sim.UploadMicroBodies(c.ctx.queue, set);
    BodyRegistry reg(c.debris, c.mobs, nullptr, &set);
    if (extras) reg.SetRenderOnly({extras});
    reg.BuildXforms(xf);
    if (!xf.empty())
      c.ctx.queue.WriteBuffer(c.world.bodyXforms, 0, xf.data(),
                              xf.size() * sizeof(BodyXformGpu));
    reg.BuildMicroInsts(mi);
    const uint32_t microCount = c.sim.UploadMicroBodyInsts(c.ctx.queue, mi);
    rhi::Buffer shot = CreateBuffer(c.ctx.device, (uint64_t)W * H * 4,
                                    rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                    "distantLookShot");
    for (uint32_t f = 0; f < frames; f++) {
      WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)W / H, shadows,
                        11.7f, kFarFogDensity, (float)H, noonTick);
      rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
      c.sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp = c.sim.BeginRenderPass(
          enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
      c.sim.DrawWorld(rp);
      c.sim.DrawMicroBodies(rp, microCount);
      rp.End();
      if (f + 1 == frames) {
        rhi::TexelCopyTexture srcT{};
        srcT.texture = c.offscreen;
        rhi::TexelCopyBuffer dstB{};
        dstB.buffer = shot;
        dstB.bytesPerRow = W * 4;
        dstB.rowsPerImage = H;
        enc.CopyTextureToBuffer(srcT, dstB, rhi::Extent3D{W, H, 1});
      }
      c.ctx.queue.Submit(enc.Finish());
    }
    out.assign((size_t)W * H * 4, 0);
    return rhi::ReadBufferBlocking(c.ctx.device, shot, 0, out.data(), out.size());
  };
  auto diffPx = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    uint32_t n = 0;
    for (size_t p = 0; p + 3 < a.size() && p + 3 < b.size(); p += 4) {
      const int d = std::max({std::abs((int)a[p] - (int)b[p]),
                              std::abs((int)a[p + 1] - (int)b[p + 1]),
                              std::abs((int)a[p + 2] - (int)b[p + 2])});
      if (d > 10) n++;
    }
    return n;
  };

  // ---- B. one statue, 40 m out, in open air -------------------------------
  const float S40 = 40.0f * (float)kVoxelsPerMetre;
  const Vec3 eye40{(float)cx + 0.5f, (float)cy + 200.0f, (float)cz - S40 * 0.5f};
  const Vec3 at40{eye40.x, eye40.y, eye40.z + S40};
  const Camera cam40 = camAt(eye40, at40);
  std::vector<RenderOnlyBody> statues;
  std::vector<uint32_t> who;
  uint32_t px40 = 0;
  {
    std::vector<DistantDrawItem>& items = dd.Items();
    items.clear();
    DistantDrawItem it;
    it.look = bakeA.key;
    it.state = DistantDrawState::Idle;
    // Centred on the sight line, its feet ~0.9 m under it, facing the camera.
    it.origin = Vec3{at40.x - def.worldSize.x * 0.5f, at40.y - 9.0f,
                     at40.z - def.worldSize.z * 0.5f};
    it.heading = 3.14159265f;
    items.push_back(it);
    for (int f = 0; f < 3 && statues.empty(); f++)   // packs on the first
      dd.Produce(set, &c.mobs.Defs(), viewOf(eye40, cam40), 64, statues, &who);
    check(statues.size() == 1, Format("the 40 m statue is produced (%zu)", statues.size()));
    std::vector<uint8_t> with, without;
    const bool r1 = render(eye40, cam40, nullptr, false, without, 4);
    const bool r2 = render(eye40, cam40, &statues, false, with, 4);
    check(r1 && r2, "render/readback");
    px40 = diffPx(with, without);
    const double minPx = BaselineNumber("distantLook.minPixels40m", 120.0);
    check(px40 > (uint32_t)minPx,
          Format("a statue at 40 m covers %u px (> %.0f at %ux%u)", px40, minPx, W, H));
    RecordObserved("distantLook.pixels40m", (double)px40);
    if (std::getenv("SANDVOX_DISTANT_DEBUG")) WriteBmpFile("build/distant_40m.bmp", with, W, H);
  }

  // ---- C. slots --------------------------------------------------------------
  uint32_t freeSlots = 0, drawnC = 0, starvedC = 0, realSlots = 0;
  {
    std::vector<DistantDrawItem>& items = dd.Items();
    items.clear();
    const int kItems = 5000;
    const Vec3 ctr{(float)cx, (float)cy, (float)cz};
    for (int i = 0; i < kItems; i++) {
      DistantDrawItem it;
      it.look = (i & 1) ? bakeB.key : bakeA.key;
      it.dead = i % 3 == 0;
      it.state = (DistantDrawState)(i % 3);
      it.gaitPhase = (float)i * 0.31f;
      const float a = (float)i * 2.39996323f;
      const float r = (30.0f + 90.0f * (float)i / (float)kItems) * (float)kVoxelsPerMetre;
      it.origin = Vec3{ctr.x + std::cos(a) * r, ctr.y, ctr.z + std::sin(a) * r};
      it.heading = a;
      items.push_back(it);
    }
    BodyRegistry near(c.debris, c.mobs, nullptr, &set);
    std::vector<BodyXformGpu> xfNear;
    std::vector<uint64_t> hNear;
    std::vector<MicroBodyInstGpu> miNear;
    near.BuildXforms(xfNear);
    near.BuildHandles(hNear);
    near.BuildMicroInsts(miNear);
    realSlots = near.RealSlots();
    check(realSlots > 0 && realSlots == (uint32_t)xfNear.size(),
          Format("the near crowd takes %u slots (%zu transforms)", realSlots, xfNear.size()));
    // A few arrows in flight ahead of the statues, on any packed model.
    std::vector<RenderOnlyBody> arrows(7, statues.empty() ? RenderOnlyBody{} : statues[0]);
    freeSlots = kMaxBodySlots - realSlots - (uint32_t)arrows.size();
    DistantView nv;
    nv.eye = ctr;
    nv.cull = false;
    // A few frames: the per-frame pack cap spreads the first packs out, and
    // the claim is about the steady state.
    for (int f = 0; f < 4; f++)
      dd.Produce(set, &c.mobs.Defs(), nv, freeSlots, statues, &who);
    const DistantDrawStats& st = dd.Stats();
    drawnC = st.drawn;
    starvedC = st.starved;
    check(statues.size() <= freeSlots,
          Format("statues %zu within the %u slots left", statues.size(), freeSlots));
    check(st.drawn == freeSlots && st.starved == (uint32_t)kItems - freeSlots,
          Format("the slots fill and the rest starve (drawn %u, starved %u, deferred "
                 "%u, noFit %u, unbaked %u)",
                 st.drawn, st.starved, st.deferred, st.noFit, st.unbaked));
    // Ordering: the living before the dead, nearest first within each.
    bool sawDead = false, livingAfterDead = false, farther = false;
    float lastD2[2] = {-1.0f, -1.0f};
    for (uint32_t k : who) {
      const DistantDrawItem& it = items[k];
      if (it.dead) sawDead = true;
      else if (sawDead) livingAfterDead = true;
      const Vec3 dv = it.origin - nv.eye;
      const float d = std::sqrt(dv.dot(dv));
      // Placement is by the pose's centre, the order here by the origin: a
      // statue is ~2 m, so allow that much.
      if (lastD2[it.dead] >= 0.0f && d + 20.0f < lastD2[it.dead]) farther = true;
      lastD2[it.dead] = std::max(lastD2[it.dead], d);
    }
    uint32_t livingTotal = 0, livingDrawn = 0;
    for (const DistantDrawItem& it : items) livingTotal += it.dead ? 0u : 1u;
    for (uint32_t k : who) livingDrawn += items[k].dead ? 0u : 1u;
    check(!livingAfterDead, "the living are drawn before the dead");
    check(!farther, "nearest first");
    check(livingDrawn == std::min(livingTotal, (uint32_t)who.size()),
          Format("the dead only once every living one has a slot (%u of %u living "
                 "drawn, %zu drawn)", livingDrawn, livingTotal, who.size()));
    // The near crowd keeps every slot it had.
    BodyRegistry all(c.debris, c.mobs, nullptr, &set);
    all.SetRenderOnly({&arrows, &statues});
    std::vector<BodyXformGpu> xfAll;
    std::vector<uint64_t> hAll;
    std::vector<MicroBodyInstGpu> miAll;
    all.BuildXforms(xfAll);
    all.BuildHandles(hAll);
    all.BuildMicroInsts(miAll);
    check(xfAll.size() <= kMaxBodySlots && all.TotalSlots() <= kMaxBodySlots,
          Format("%zu transforms, %u slots, ceiling %u", xfAll.size(),
                 all.TotalSlots(), kMaxBodySlots));
    bool same = xfAll.size() >= xfNear.size() && hAll.size() >= hNear.size() &&
                miAll.size() >= miNear.size();
    for (size_t s = 0; same && s < xfNear.size(); s++)
      same = std::memcmp(&xfAll[s], &xfNear[s], sizeof(BodyXformGpu)) == 0 &&
             hAll[s] == hNear[s];
    for (size_t k = 0; same && k < miNear.size(); k++)
      same = std::memcmp(&miAll[k], &miNear[k], sizeof(MicroBodyInstGpu)) == 0;
    check(same, "the near crowd's transforms, handles and micro instances are "
                "untouched by the statues");
    // The arrows keep the slots straight after the real bodies, the statues
    // take the rest.
    bool arrowsFirst = miAll.size() >= miNear.size() + arrows.size();
    for (size_t k = 0; arrowsFirst && k < arrows.size(); k++)
      arrowsFirst = miAll[miNear.size() + k].slot == realSlots + (uint32_t)k;
    check(arrowsFirst, "the arrows keep the slots right after the real bodies");
    check(miAll.size() == miNear.size() + arrows.size() + statues.size(),
          Format("every statue drew (%zu micro instances, %zu near + %zu arrows + "
                 "%zu statues)", miAll.size(), miNear.size(), arrows.size(),
                 statues.size()));
    RecordObserved("distantLook.realSlots", (double)realSlots);
    RecordObserved("distantLook.slotsFree", (double)freeSlots);
    RecordObserved("distantLook.drawn", (double)drawnC);
    RecordObserved("distantLook.starved", (double)starvedC);
  }

  // ---- D. words ----------------------------------------------------------------
  // Every pose of the living look is packed by now (C drew all three states
  // of it): the words are exact.
  static const char* kPoseKey[kDistantPoseCount] = {"Idle", "WalkA", "WalkB",
                                                    "Strike", "Lying"};
  uint32_t wordsSum = 0;
  for (int p = 0; p < kDistantPoseCount; p++) {
    DistantDraw::PoseInfo info;
    dd.PoseOf(bakeA.key.full, (DistantPose)p, info);
    RecordObserved((std::string("distantLook.words") + kPoseKey[p]).c_str(),
                   (double)info.words);
    RecordObserved((std::string("distantLook.cells") + kPoseKey[p]).c_str(),
                   (double)info.cells);
    wordsSum += info.words;
    pi[p] = info;
  }
  const uint32_t residentDefault = dd.Stats().residentWords;
  check(residentDefault <= kDistantPoseWordsMax,
        Format("the default cache holds %u words (budget %u)", residentDefault,
               kDistantPoseWordsMax));
  uint64_t smallEvictions = 0;
  uint32_t smallPeak = 0, smallBudget = 0;
  {
    // Room for about two and a half poses.
    smallBudget = std::max(1u, pi[(int)DistantPose::Idle].words) * 5u / 2u;
    DistantDraw small(smallBudget);
    small.Bake(*mA, set);
    small.Bake(*mB, set);
    // A few more looks: the first defs that bake.
    std::vector<DistantLookKey> keys{bakeA.key, bakeB.key};
    for (size_t d = 0; d < c.mobs.Defs().size() && keys.size() < 6; d++) {
      const DistantBake b = small.BakeDef(c.mobs.Defs()[d], set);
      if (b.baked) keys.push_back(b.key);
    }
    DistantView nv;
    nv.eye = Vec3{(float)cx, (float)cy, (float)cz};
    nv.cull = false;
    bool over = false;
    for (int f = 0; f < 40; f++) {
      std::vector<DistantDrawItem>& items = small.Items();
      items.clear();
      for (int k = 0; k < 2; k++) {
        DistantDrawItem it;
        const int n = f * 2 + k;
        it.look = keys[(size_t)n % keys.size()];
        it.state = (DistantDrawState)(n % 3);
        it.dead = n % 7 == 0;
        it.gaitPhase = (float)n * 0.5f;
        it.origin = Vec3{(float)cx + 400.0f + 30.0f * (float)k, (float)cy, (float)cz};
        items.push_back(it);
      }
      small.Produce(set, nullptr, nv, 64, statues);
      smallPeak = std::max(smallPeak, small.Stats().residentWords);
      over = over || small.Stats().residentWords > small.WordBudget();
    }
    smallEvictions = small.Stats().evictions;
    check(!over, Format("a %u-word cache never held more (peak %u)", smallBudget, smallPeak));
    check(smallEvictions > 0, Format("...and its LRU evicted (%llu)",
                                     (unsigned long long)smallEvictions));
    const uint32_t freeBefore = MicroBodyFreeWords(set);
    const uint32_t held = small.Stats().residentWords;
    small.Reset(set);
    check(MicroBodyFreeWords(set) == freeBefore + held,
          Format("Reset gives its %u words back (%u -> %u free)", held, freeBefore,
                 MicroBodyFreeWords(set)));
  }
  RecordObserved("distantLook.residentWords", (double)residentDefault);
  RecordObserved("distantLook.smallEvictions", (double)smallEvictions);

  // ---- E. epoch ---------------------------------------------------------------
  {
    DistantDraw ep;
    ep.Bake(*mA, set);
    std::vector<DistantDrawItem>& items = ep.Items();
    items.clear();
    DistantDrawItem it;
    it.look = bakeA.key;
    items.push_back(it);
    DistantView nv;
    nv.cull = false;
    std::vector<RenderOnlyBody> one;
    ep.Produce(set, nullptr, nv, 4, one);
    const int packedModel = one.empty() ? -1 : (int)one[0].model;
    check(packedModel >= 0, "the epoch arm packed a pose");
    const uint32_t freeAfterPack = MicroBodyFreeWords(set);
    const size_t recAfterPack = set.freeModels.size();
    MicroBodySet other;   // a fresh epoch: a reload, as far as the cache can tell
    ep.Produce(other, nullptr, nv, 4, one);
    check(ep.Stats().epochDrops == 1 && ep.Stats().looks == 0 &&
              ep.Stats().packedPoses == 0,
          "a new set drops every look and packed pose");
    check(MicroBodyFreeWords(set) == freeAfterPack && set.freeModels.size() == recAfterPack,
          "...without freeing anything in the old set");
    check(other.models.empty() && one.empty(), "...and draws nothing from a dropped look");
    // The dropped pose is the harness's to give back (in a real reload its
    // table is gone with the set).
    if (packedModel >= 0) MicroBodyReleaseShared(set, (uint32_t)packedModel);
  }

  // ---- F. the owner's look check (written, not asserted) ----------------------
  {
    // A row on the ground: every pose, 20..80 m out, the live creature A a few
    // metres from the camera for scale. THE SIGHT LINE IS CHOSEN: of eight
    // compass directions from just behind the window centre, the one from
    // which the most statues' heads clear the terrain (World::TerrainHeight
    // sampled every 2 m along each line of sight) -- a dune crest between the
    // camera and the row hid most of it from a fixed +Z view.
    struct Spot { float m; float side; DistantDrawState st; bool dead; bool corpse; };
    const Spot spots[] = {
        {20, -40, DistantDrawState::Idle, false, false},
        {25, 30, DistantDrawState::Walk, false, false},
        {30, -20, DistantDrawState::Strike, false, false},
        {40, 40, DistantDrawState::Idle, false, false},
        {40, 0, DistantDrawState::Walk, false, false},
        {50, -50, DistantDrawState::Idle, true, false},
        {60, 30, DistantDrawState::Idle, true, true},
        {80, -10, DistantDrawState::Walk, false, false},
    };
    auto ground = [](float x, float z) {
      return (float)World::TerrainHeight(ifloor(x), ifloor(z), kDefaultSeed);
    };
    const float kBack = 60.0f, kLift = 40.0f;   // 6 m behind, 4 m up
    float bestA = 1.5707963f;
    int bestSeen = -1;
    for (int k = 0; k < 8; k++) {
      const float a = 1.5707963f + (float)k * 0.78539816f;
      const Vec3 u{std::cos(a), 0.0f, std::sin(a)}, v{-u.z, 0.0f, u.x};
      const float ex = (float)cx - u.x * kBack, ez = (float)cz - u.z * kBack;
      const float ey = ground(ex, ez) + kLift;
      int seen = 0;
      for (const Spot& s : spots) {
        const float px = ex + u.x * s.m * 10.0f + v.x * s.side;
        const float pz = ez + u.z * s.m * 10.0f + v.z * s.side;
        const float py = ground(px, pz) + 15.0f;
        bool clear = true;
        for (float t = 0.05f; t < 0.98f && clear; t += 20.0f / (s.m * 10.0f))
          clear = ground(ex + (px - ex) * t, ez + (pz - ez) * t) + 1.0f <
                  ey + (py - ey) * t;
        seen += clear ? 1 : 0;
      }
      if (seen > bestSeen) { bestSeen = seen; bestA = a; }
    }
    const Vec3 u{std::cos(bestA), 0.0f, std::sin(bestA)}, v{-u.z, 0.0f, u.x};
    const float ex = (float)cx - u.x * kBack, ez = (float)cz - u.z * kBack;
    const Vec3 eyeF{ex, ground(ex, ez) + kLift, ez};
    const float ax = ex + u.x * 400.0f, az = ez + u.z * 400.0f;
    const Vec3 atF{ax, ground(ax, az) + 9.0f, az};
    const Camera camF = camAt(eyeF, atF);
    std::vector<DistantDrawItem>& items = dd.Items();
    items.clear();
    int n = 0;
    for (const Spot& s : spots) {
      DistantDrawItem it;
      it.look = s.corpse ? bakeB.key : bakeA.key;
      it.state = s.st;
      it.dead = s.dead;
      it.gaitPhase = (float)n * 0.5f;
      const float x = ex + u.x * s.m * 10.0f + v.x * s.side;
      const float z = ez + u.z * s.m * 10.0f + v.z * s.side;
      // Every pose is grounded: its lowest cell sits at origin.y.
      it.origin = Vec3{x - def.worldSize.x * 0.5f, ground(x, z) + 1.0f,
                       z - def.worldSize.z * 0.5f};
      // Turned three-quarters to the camera, each a little differently.
      it.heading = bestA + 3.14159265f - 0.6f + 0.3f * (float)(n % 4);
      items.push_back(it);
      n++;
    }
    std::printf("distant-look: look-check row along %.0f deg, %d of %zu statues "
                "clear the terrain from the camera\n",
                bestA * 57.29578f, bestSeen, sizeof(spots) / sizeof(spots[0]));
    for (int f = 0; f < 3; f++)
      dd.Produce(set, &c.mobs.Defs(), viewOf(eyeF, camF), 64, statues);
    std::vector<uint8_t> img;
    if (render(eyeF, camF, &statues, true, img, 8))
      WriteBmpFile("build/distant_look.bmp", img, W, H);

    // Close: the five poses side by side 4..9 m out, in the air over the
    // window centre so nothing hides them, plus the corpse's own Lying.
    const Vec3 eyeC{(float)cx + 0.5f, (float)cy + 120.0f, (float)cz - 70.0f};
    const Vec3 atC{(float)cx + 0.5f, (float)cy + 112.0f, (float)cz};
    const Camera camC = camAt(eyeC, atC);
    items.clear();
    for (int p = 0; p < 6; p++) {
      DistantDrawItem it;
      it.look = p == 5 ? bakeB.key : bakeA.key;
      it.state = p == 1 || p == 2 ? DistantDrawState::Walk
                 : p == 3         ? DistantDrawState::Strike
                                  : DistantDrawState::Idle;
      it.gaitPhase = p == 2 ? 0.75f : 0.25f;
      it.dead = p >= 4;
      it.origin = Vec3{(float)cx - 50.0f + 20.0f * (float)p - def.worldSize.x * 0.5f,
                       (float)cy + 100.0f, (float)cz - def.worldSize.z * 0.5f};
      it.heading = 3.14159265f - 0.5f;
      items.push_back(it);
    }
    for (int f = 0; f < 3; f++)
      dd.Produce(set, &c.mobs.Defs(), viewOf(eyeC, camC), 64, statues);
    if (render(eyeC, camC, &statues, false, img, 4))
      WriteBmpFile("build/distant_look_close.bmp", img, W, H);
    std::printf("distant-look: wrote build/distant_look.bmp and "
                "build/distant_look_close.bmp (the owner's look check)\n");
  }

  const DistantDrawStats& fs = dd.Stats();
  std::printf("distant-look: words/pose idle %u walkA %u walkB %u strike %u lying %u "
              "(cells %u/%u/%u/%u/%u, dims idle %dx%dx%d); bake %.1f ms; art +%llu "
              "nearest %llu\n",
              pi[0].words, pi[1].words, pi[2].words, pi[3].words, pi[4].words,
              pi[0].cells, pi[1].cells, pi[2].cells, pi[3].cells, pi[4].cells,
              pi[0].dims.x, pi[0].dims.y, pi[0].dims.z, bakeMs,
              (unsigned long long)fs.artAdded, (unsigned long long)fs.artNearest);
  RecordObserved("distantLook.bakeMs", bakeMs);
  dd.Reset(set);
  cleanupWorld();

  detail = Format("40 m statue %u px; slots: %u real, %u free, %u drawn, %u starved; "
                  "words/pose %u..%u (sum %u, budget %u); small cache peak %u of %u, "
                  "%llu evictions; bake %.1f ms",
                  px40, realSlots, freeSlots, drawnC, starvedC,
                  std::min({pi[0].words, pi[1].words, pi[2].words, pi[3].words, pi[4].words}),
                  std::max({pi[0].words, pi[1].words, pi[2].words, pi[3].words, pi[4].words}),
                  wordsSum, kDistantPoseWordsMax, smallPeak, smallBudget,
                  (unsigned long long)smallEvictions, bakeMs);
  std::printf("distant-look: %s (%d checks, %s)\n", ok ? "PASS" : "FAIL", checks,
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DistantLookGates() {
  static const std::vector<Gate> g = {
      // A distant creature's baked statue: the bake, 40 m coverage, the slot
      // budget, the word budget and the set epoch (docs/PLAN_distant_npcs D4a).
      {"distant-look", "render", {}, false, GateDistantLook, true},
  };
  return g;
}

}  // namespace selftest
