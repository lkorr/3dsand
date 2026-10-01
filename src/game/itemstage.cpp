#include "game/itemstage.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "game/container.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/itemcoat.h"
#include "game/mob.h"
#include "phys/bodystain.h"
#include "sim/materials.h"

namespace itemstage {

// ---- the grid ---------------------------------------------------------------

void LatticeGrid::Build(const std::vector<PrefabVoxel>& lat) {
  idx.clear();
  lo = dim = IVec3{};
  if (lat.empty()) return;
  IVec3 mn{lat[0].x, lat[0].y, lat[0].z}, mx = mn;
  for (const PrefabVoxel& v : lat) {
    mn.x = std::min(mn.x, (int)v.x); mn.y = std::min(mn.y, (int)v.y); mn.z = std::min(mn.z, (int)v.z);
    mx.x = std::max(mx.x, (int)v.x); mx.y = std::max(mx.y, (int)v.y); mx.z = std::max(mx.z, (int)v.z);
  }
  lo = mn;
  dim = IVec3{mx.x - mn.x + 1, mx.y - mn.y + 1, mx.z - mn.z + 1};
  // A lattice is a few hundred cells a side at most; anything past this is a
  // corrupt record, and a viewer that allocates gigabytes for it is worse
  // than one that shows nothing.
  const size_t n = (size_t)dim.x * dim.y * dim.z;
  if (n == 0 || n > (size_t)64 * 1024 * 1024) {
    dim = IVec3{};
    return;
  }
  idx.assign(n, -1);
  for (size_t i = 0; i < lat.size(); i++) {
    const PrefabVoxel& v = lat[i];
    if ((v.material & 0xFFFu) == 0) continue;
    const size_t k = ((size_t)(v.z - lo.z) * dim.y + (v.y - lo.y)) * dim.x + (v.x - lo.x);
    if (idx[k] < 0) idx[k] = (int32_t)i;   // first wins: a duplicate is a content bug
  }
}

Vec3 LatticeGrid::Centre() const {
  return Vec3{(float)lo.x + dim.x * 0.5f, (float)lo.y + dim.y * 0.5f,
              (float)lo.z + dim.z * 0.5f};
}

float LatticeGrid::Radius() const {
  return 0.5f * std::sqrt((float)dim.x * dim.x + (float)dim.y * dim.y +
                          (float)dim.z * dim.z);
}

Hit Raycast(const LatticeGrid& g, Vec3 ro, Vec3 rd) {
  Hit h;
  if (g.Empty()) return h;
  rd = rd.normalized();
  if (rd.len() < 0.5f) return h;
  // Into the grid's own frame (cell (0,0,0) = g.lo), clipped to its box.
  const float o[3] = {ro.x - g.lo.x, ro.y - g.lo.y, ro.z - g.lo.z};
  const float d[3] = {rd.x, rd.y, rd.z};
  const int n[3] = {g.dim.x, g.dim.y, g.dim.z};
  float t0 = 0.0f, t1 = 1e30f;
  int enterAxis = -1;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(d[a]) < 1e-12f) {
      if (o[a] < 0.0f || o[a] > (float)n[a]) return h;
      continue;
    }
    float ta = (0.0f - o[a]) / d[a], tb = ((float)n[a] - o[a]) / d[a];
    if (ta > tb) std::swap(ta, tb);
    if (ta > t0) { t0 = ta; enterAxis = a; }
    t1 = std::min(t1, tb);
  }
  if (t0 > t1) return h;
  // The cell the ray is in at t0 (nudged inside so a face hit is not read as
  // the cell outside it).
  int c[3], step[3];
  float tMaxA[3], tDelta[3];
  for (int a = 0; a < 3; a++) {
    const float p = o[a] + d[a] * (t0 + 1e-4f);
    c[a] = std::clamp((int)std::floor(p), 0, n[a] - 1);
    step[a] = d[a] > 0.0f ? 1 : -1;
    if (std::fabs(d[a]) < 1e-12f) {
      tMaxA[a] = 1e30f;
      tDelta[a] = 1e30f;
    } else {
      const float next = (float)(c[a] + (d[a] > 0.0f ? 1 : 0));
      tMaxA[a] = (next - o[a]) / d[a];
      tDelta[a] = std::fabs(1.0f / d[a]);
    }
  }
  int axis = enterAxis;
  float t = t0;
  for (int guard = 0; guard < n[0] + n[1] + n[2] + 3; guard++) {
    const int vi = g.idx[((size_t)c[2] * n[1] + c[1]) * n[0] + c[0]];
    if (vi >= 0) {
      h.hit = true;
      h.voxel = vi;
      h.cell = IVec3{c[0] + g.lo.x, c[1] + g.lo.y, c[2] + g.lo.z};
      h.axis = axis;
      h.sign = axis >= 0 ? step[axis] : 0;
      h.t = t;
      return h;
    }
    int a = 0;
    if (tMaxA[1] < tMaxA[a]) a = 1;
    if (tMaxA[2] < tMaxA[a]) a = 2;
    t = tMaxA[a];
    if (t > t1 + 1e-4f) break;
    c[a] += step[a];
    if (c[a] < 0 || c[a] >= n[a]) break;
    tMaxA[a] += tDelta[a];
    axis = a;
  }
  return h;
}

void BrushCells(const LatticeGrid& g, const std::vector<PrefabVoxel>& lat,
                Vec3 ro, Vec3 rd, float radius, std::vector<int32_t>& out) {
  out.clear();
  rd = rd.normalized();
  const Hit hit = Raycast(g, ro, rd);
  if (!hit.hit || radius <= 0.0f) return;
  // The disc is across the ray, centred on it: every cell is measured from
  // the line, so the disc is the same size on the item whatever the zoom.
  const Vec3 side = std::fabs(rd.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
  const Vec3 u = rd.cross(side).normalized();
  const Vec3 w = rd.cross(u);
  const float r2 = radius * radius;
  const float tMax = hit.t + radius * 2.0f + 2.0f;
  struct Cand {
    int32_t idx;
    float t;
    int64_t bin;
  };
  std::vector<Cand> cands;
  std::unordered_map<int64_t, float> nearest;
  for (size_t i = 0; i < lat.size(); i++) {
    const PrefabVoxel& v = lat[i];
    if ((v.material & 0xFFFu) == 0) continue;
    const Vec3 rel = Vec3{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f} - ro;
    const float t = rel.dot(rd);
    if (t <= 0.0f || t > tMax) continue;
    const float pu = rel.dot(u), pw = rel.dot(w);
    if (pu * pu + pw * pw > r2) continue;
    const int64_t bu = (int64_t)std::floor(pu), bw = (int64_t)std::floor(pw);
    const int64_t bin = (bu << 32) ^ (bw & 0xFFFFFFFFll);
    cands.push_back(Cand{(int32_t)i, t, bin});
    auto it = nearest.find(bin);
    if (it == nearest.end()) nearest.emplace(bin, t);
    else if (t < it->second) it->second = t;
  }
  // The face cell, and the one a slanted surface shows beside it.
  for (const Cand& c : cands)
    if (c.t <= nearest[c.bin] + 1.5f) out.push_back(c.idx);
  std::sort(out.begin(), out.end());
}

const std::vector<PrefabVoxel>* ViewLattice(const ItemInstance& it,
                                            const ItemDef& def, int shell,
                                            std::vector<PrefabVoxel>& scratch) {
  if (const std::vector<PrefabVoxel>* rec = ItemLatticeIfAny(it, shell)) return rec;
  if (shell < 0 || shell >= ItemLatticeCount(def)) return nullptr;
  // ItemLatticeMut on a throw-away instance: the same materialisation (the
  // held palette variant included), and the real instance is not touched.
  ItemInstance tmp;
  tmp.name = it.name;
  scratch = ItemLatticeMut(tmp, def, shell);
  return &scratch;
}

bool StageTakes(const ItemDef& def) {
  if (ItemLatticeCount(def) <= 0) return false;
  if (def.kind == ItemKind::Melee) return true;
  return ItemKindIsWorn(def.kind) && !def.cover.empty();
}

std::string ShellName(const ItemDef& def, int shell) {
  if (def.cover.empty() || shell < 0 || shell >= (int)def.cover.size()) return std::string();
  return def.cover[(size_t)shell].part;
}

namespace {

// One out of a stack, wherever it is (ContainerIsolateOne, the portrait
// brush's rule): the rest goes to a free slot of the same container, else of
// the next. False when there is nowhere to put it.
bool IsolateOne(Kit& kit, ItemStack* p) {
  if (!p || p->Empty() || p->count <= 1) return p != nullptr;
  ItemStack* hot = kit.hotbar.slots;
  ItemStack* bag = kit.bag.slots;
  ItemStack* eq = kit.equip.slots;
  if (p >= hot && p < hot + kItemSlots)
    return ContainerIsolateOne(hot, kItemSlots, (int)(p - hot), bag, Bag::kSlots);
  if (p >= bag && p < bag + Bag::kSlots)
    return ContainerIsolateOne(bag, Bag::kSlots, (int)(p - bag), hot, kItemSlots);
  if (p >= eq && p < eq + kEquipSlotCount)
    return ContainerIsolateOne(eq, kEquipSlotCount, (int)(p - eq), bag, Bag::kSlots);
  return false;
}

}  // namespace

StrokeResult ApplyStroke(Kit& kit, MobSystem* mobs, uint64_t wearerId,
                         const ItemLibrary& items,
                         const std::vector<MaterialDef>& mats, const Stroke& s,
                         int64_t& spendMilli) {
  StrokeResult r;
  if (!s.active) return r;
  ItemStack* st = kit.Resolve(s.item);
  const ItemDef* def = st ? items.Of(*st) : nullptr;
  if (!def || !StageTakes(*def) || s.shell < 0 || s.shell >= ItemLatticeCount(*def)) {
    r.refused = "nothing on the stage";
    return r;
  }
  ItemStack* vp = kit.Resolve(s.vessel);
  const ItemDef* vdef = vp ? items.Of(*vp) : nullptr;
  if (!vdef || !vdef->IsContainer() || !vp->Filled()) {
    r.refused = "choose a filled flask on the FLASKS row";
    return r;
  }
  if (vp->stoppered) {
    r.refused = "the flask is stoppered";
    return r;
  }
  // ONE flask's fill pays, and ONE item takes the coat: a stack of either is
  // split first (a coated sword is not a plain stack any more).
  if (!IsolateOne(kit, vp) || !IsolateOne(kit, st)) {
    r.refused = "no room to take one out of the stack";
    return r;
  }
  const uint16_t mat = ContainerTopMat(*vp, &mats);
  if (mat == 0 || mat >= mats.size()) {
    r.refused = "nothing pours from it";
    return r;
  }
  r.mat = mat;
  if (mats[mat].stainSlot == 0) {
    r.refused = "it will not coat anything";
    return r;
  }
  // On the rig, the rig is the truth (itemcoat.h): bring the instance up to
  // date first, so a coat that dried in the hand is not painted back.
  const bool onRig = s.item.space == KitSpace::Equip && mobs && wearerId;
  if (onRig) CaptureLimbToItem(*mobs, wearerId, s.item);
  std::vector<PrefabVoxel>& lat = ItemLatticeMut(*st, *def, s.shell);
  LatticeGrid g;
  g.Build(lat);
  std::vector<int32_t> cells;
  BrushCells(g, lat, s.ro, s.rd, s.radius, cells);
  if (cells.empty()) {
    ItemLatticeSettle(*st, *def);   // the materialised copy was not needed
    return r;
  }
  r.hit = true;
  // THE SPEND: the portrait brush's rate, of the brush's WORLD radius.
  const float worldR = s.radius / (float)std::max(1u, def->scale);
  spendMilli += (int64_t)std::llround(PourBrushCellsPerSec(*vdef, worldR) *
                                      kContainerUnitsPerCell * 1000.0f / 30.0f);
  const int units = (int)(spendMilli / 1000);
  spendMilli -= (int64_t)units * 1000;
  if (units > 0) r.spent = vp->contents.Take(mat, (uint32_t)units);
  const bool washes = (mats[mat].gpu.stainPack & kStainPackWashesBit) != 0;
  for (int32_t i : cells) {
    PrefabVoxel& v = lat[(size_t)i];
    const uint16_t next =
        washes ? WashBodyStain(v.stain, mat, kStrokeAmount, kStrokeAmount)
               : AddBodyStain(v.stain, mat, kStrokeAmount, kBodyStainAmtMax);
    if (next == v.stain) continue;
    v.stain = next;
    r.marked++;
  }
  // A blade washed spotless is a plain stack again (and the push below then
  // rinses the blade in the fist to match).
  ItemLatticeSettle(*st, *def);
  if (s.item.space == KitSpace::Equip && mobs && wearerId)
    r.pushed = PushItemLatticeToLimb(*mobs, wearerId, s.item);
  return r;
}

std::vector<CoatShare> CoatSummary(const std::vector<PrefabVoxel>& lat) {
  std::map<uint16_t, CoatShare> by;
  for (const PrefabVoxel& v : lat) {
    if (!v.stain) continue;
    const uint16_t m = (uint16_t)BodyStainMat(v.stain);
    CoatShare& c = by[m];
    c.mat = m;
    c.voxels++;
    c.amount += BodyStainAmt(v.stain);
  }
  std::vector<CoatShare> out;
  for (const auto& kv : by) out.push_back(kv.second);
  std::stable_sort(out.begin(), out.end(), [](const CoatShare& a, const CoatShare& b) {
    return a.voxels > b.voxels;
  });
  return out;
}

}  // namespace itemstage
