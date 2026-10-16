// quiver.cpp -- a quiver and the arrows in it (read game/quiver.h first).

#include "game/quiver.h"

#include <algorithm>

#include "game/anim.h"  // QuatFromTo / QuatRotate / QuatMul
#include "game/equipment.h"
#include "game/mob.h"

// ---- ONE QUIVER ---------------------------------------------------------------

int QuiverCount(const ItemInstance& q) {
  int n = 0;
  for (const ItemInstance& h : q.held) n += std::max(0, h.count);
  return n;
}

int QuiverRoom(const ItemInstance& q, const ItemDef& qd) {
  if (!qd.IsQuiver()) return 0;
  return std::max(0, qd.quiver.capacity - QuiverCount(q));
}

bool QuiverTakes(const ItemDef& qd, const ItemInstance& a) {
  return qd.IsQuiver() && !a.Empty() && a.held.empty() &&
         a.name == qd.quiver.ammo;
}

void QuiverPut(ItemInstance& q, ItemInstance one) {
  if (one.Empty()) return;
  one.held.clear();
  for (ItemInstance& h : q.held)
    if (h.StacksWith(one)) {
      h.count += one.count;
      return;
    }
  q.held.push_back(std::move(one));
}

int QuiverLoad(ItemInstance& q, const ItemDef& qd, ItemInstance& from, int n) {
  if (!QuiverTakes(qd, from)) return 0;
  int want = n < 0 ? from.count : std::min(n, from.count);
  want = std::min(want, QuiverRoom(q, qd));
  if (want <= 0) return 0;
  if (from.Plain()) {
    ItemInstance part = from;
    part.count = want;
    QuiverPut(q, std::move(part));
  } else {
    // A coated stack is that many separate arrows that happen to agree: each
    // goes in as its own entry, so one being shot never takes the others'
    // record with it.
    for (int i = 0; i < want; i++) {
      ItemInstance one = from;
      one.count = 1;
      q.held.push_back(std::move(one));
    }
  }
  from.count -= want;
  if (from.count <= 0) from = ItemInstance{};
  return want;
}

int QuiverPick(const ItemInstance& q, uint32_t key, const std::string& ammo) {
  uint32_t total = 0;
  for (const ItemInstance& h : q.held)
    if (!h.Empty() && (ammo.empty() || h.name == ammo)) total += (uint32_t)h.count;
  if (total == 0) return -1;
  uint32_t k = key % total;
  for (int i = 0; i < (int)q.held.size(); i++) {
    const ItemInstance& h = q.held[(size_t)i];
    if (h.Empty() || (!ammo.empty() && h.name != ammo)) continue;
    if (k < (uint32_t)h.count) return i;
    k -= (uint32_t)h.count;
  }
  return -1;
}

ItemInstance QuiverTakeOne(ItemInstance& q, int i) {
  if (i < 0 || i >= (int)q.held.size() || q.held[(size_t)i].Empty())
    return ItemInstance{};
  ItemInstance& h = q.held[(size_t)i];
  ItemInstance one = h;
  one.count = 1;
  if (--h.count <= 0) q.held.erase(q.held.begin() + i);
  return one;
}

// ---- THE QUIVER ON YOUR BACK ---------------------------------------------------

ItemInstance* KitWornQuiver(Kit& kit, const ItemLibrary& lib, const ItemDef** def) {
  ItemInstance& s = kit.equip.slots[(int)EquipSlotId::Back];
  const ItemDef* d = lib.Of(s);
  if (d == nullptr || !d->IsQuiver()) return nullptr;
  if (def) *def = d;
  return &s;
}

const ItemInstance* KitWornQuiver(const Kit& kit, const ItemLibrary& lib,
                                  const ItemDef** def) {
  return KitWornQuiver(const_cast<Kit&>(kit), lib, def);
}

bool KitReceiveArrow(Kit& kit, const ItemLibrary& lib, const ItemInstance& one,
                     bool* intoQuiver) {
  if (intoQuiver) *intoQuiver = false;
  if (one.Empty()) return false;
  const ItemDef* qd = nullptr;
  if (ItemInstance* q = KitWornQuiver(kit, lib, &qd)) {
    ItemInstance from = one;
    if (QuiverTakes(*qd, from) && QuiverRoom(*q, *qd) >= from.count) {
      QuiverLoad(*q, *qd, from);
      if (intoQuiver) *intoQuiver = true;
      return true;
    }
  }
  if (kit.bag.Add(one) >= 0) return true;
  return kit.hotbar.Add(one) >= 0;
}

// ---- THE QUIVER AS SEEN --------------------------------------------------------

void PresentQuiver(const Mob& m, const ItemLibrary& items,
                   std::vector<RenderOnlyBody>& out) {
  const ItemInstance& st = m.GetKit().equip.At((int)EquipSlotId::Back);
  const ItemDef* qd = items.Of(st);
  if (qd == nullptr || !qd->IsQuiver() || qd->microModel < 0) return;
  Vec3 sockW{};
  Quat sockQ{};
  if (!m.SocketWorldXf("back", sockW, sockQ)) return;
  // SOCKET x GRIP, forward (Mob::EquipItem's composition): the quiver's own
  // `back` grip says how it lies on the socket, its hilt box which of its
  // points sits there (the strap's middle).
  const ItemGrip* grip = qd->Grip("back");
  const Quat gripRot = grip ? grip->rotation : Quat{};
  const Vec3 gripT = grip ? grip->translation : Vec3{};
  const Quat q = QuatNormalize(QuatMul(sockQ, gripRot));
  const Vec3 gripLocal = qd->hilt.has ? qd->hilt.center - gripT : gripT * -1.0f;
  const float inv = 1.0f / (float)(qd->scale ? qd->scale : 1u);
  const Vec3 sz = Vec3{(float)qd->size.x, (float)qd->size.y, (float)qd->size.z} * inv;
  // The socket is ON the back; the tube's axis lies half its own depth
  // behind it (the item's z is the socket's, `back` grips turn about z), so
  // it rests on the shoulder blades rather than inside them.
  sockW = sockW + QuatRotate(sockQ, Vec3{0.0f, 0.0f, -(sz.z * 0.5f + 0.05f)});
  const Vec3 corner = sockW - QuatRotate(q, gripLocal);
  RenderOnlyBody b;
  b.pos = corner;
  b.quat[0] = q.x;
  b.quat[1] = q.y;
  b.quat[2] = q.z;
  b.quat[3] = q.w;
  b.model = (uint32_t)qd->microModel;
  out.push_back(b);

  // THE ARROWS IN IT: the tube is authored along +x with its MOUTH at the
  // high end, so an arrow stands in it point-down (-x) with its nock a little
  // past the mouth. Fanned on a small grid across the mouth, each a little
  // prouder or deeper than its neighbour, so a full quiver reads as a sheaf.
  const ItemDef* ad = items.Named(qd->quiver.ammo);
  if (ad == nullptr || !ad->IsAmmo() || ad->microModel < 0) return;
  const int n = std::min(QuiverCount(st), kQuiverDrawnArrows);
  if (n <= 0) return;
  const Vec3 lo = Vec3{(float)qd->offset.x, (float)qd->offset.y, (float)qd->offset.z} * inv;
  const float mouthX = lo.x + sz.x;
  const float cy = lo.y + sz.y * 0.5f, cz = lo.z + sz.z * 0.5f;
  // The fan stays INSIDE the tube: an arrow's head is three cells across, so
  // its centre may stray at most (inner radius - 1.5 cells) off the axis or
  // the steel shows through the leather (seen 2026-10-15 at 0.28).
  const float ry = sz.y * 0.14f, rz = sz.z * 0.14f;
  const Vec3 dirW = QuatRotate(q, Vec3{-1.0f, 0.0f, 0.0f});
  const Quat qa = QuatNormalize(QuatFromTo(Vec3{1, 0, 0}, dirW));
  // A 4 x 3 grid of places across the mouth, filled in a fixed order.
  static const float kGy[kQuiverDrawnArrows] = {-1, 0, 1, -1, 0, 1, -0.5f, 0.5f, -0.5f, 0.5f, 0, 0};
  static const float kGz[kQuiverDrawnArrows] = {-1, -1, -1, 1, 1, 1, 0, 0, -0.5f, 0.5f, 0.3f, -0.3f};
  for (int i = 0; i < n; i++) {
    // How far the nock stands out of the mouth, world voxels: 1.2..2.2.
    const float proud = 1.2f + 0.25f * (float)((i * 7) % 5);
    const Vec3 nockL{mouthX + proud, cy + kGy[i] * ry, cz + kGz[i] * rz};
    const Vec3 nockW = corner + QuatRotate(q, nockL);
    const Vec3 tipW = nockW + dirW * ad->arrow.length;
    RenderOnlyBody a;
    a.pos = tipW - QuatRotate(qa, ad->arrow.tip);
    a.quat[0] = qa.x;
    a.quat[1] = qa.y;
    a.quat[2] = qa.z;
    a.quat[3] = qa.w;
    a.model = (uint32_t)ad->microModel;
    out.push_back(a);
  }
}
