// selftest_spell.cpp — spell selftest gates.
//
// Two gates:
//
//   spells        — the runtime invariants (trail budget, the overcast, linear
//                   amplification, the cast latch, THE NEST) and THE LAWS of
//                   the grammar (DESIGN.md §8), asserted over every sequence of
//                   length <= 3 drawn from a generated alphabet. A law that
//                   fails breaks a CLASS of spells, which is what the line
//                   says; a change that moves one spell's numbers is a
//                   rebaseline.
//                   L1 totality, L2 segment commutativity, L3 multiplicity,
//                   L4 root-lane additivity, L5 delivery invariance,
//                   L6 locality, L7 tariff monotonicity, L8 budgets,
//                   L9 nesting, L10 mod placement, L11 flattening,
//                   L12 lanes (rule 4).
//   spells-oracle — the C++ parser against scripts/magic_grammar.py, which is
//                   the executable reference: every sentence in the brief and
//                   every ordered pair over the 19-word alphabet, as canonical
//                   parse strings in assets/spells/grammar_oracle.json.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/brush.h"
#include "game/mob.h"
#include "game/spell.h"
#include "phys/debris.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// The test alphabet: the reference script's ALPHA2. Generated over, never
// pinned — the gate asserts algebraic properties, not sequences.
const char* const kAlphabet[] = {
    "fire", "gold",  "air",        "anything", "explosive", "gust",       "transmute",
    "mend", "trail", "null",       "aura",     "echo",      "shotgun",    "float",
    "lane", "projectile", "bomb",  "self",     "beam",      "twin",
};

struct FakeHealth {
  int32_t hp;
  CasterHealth cb;
  explicit FakeHealth(int32_t v) : hp(v) {
    cb.ctx = this;
    cb.get = [](void* c) { return ((FakeHealth*)c)->hp; };
    cb.spend = [](void* c, int32_t a) { ((FakeHealth*)c)->hp -= a; };
  }
};

// Every field of a lowered cast list, as one string, so two lowerings can be
// compared for IDENTITY rather than for a few hand-picked numbers.
void SigRecord(const GlyphLibrary& lib, const DeliveryRec& d, std::string& s);

void SigEffect(const GlyphLibrary& lib, const EffectInst& e, std::string& s) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "{%s g%d n%d c%d A%u/%d B%u/%d r%d f%d o%d a%d m%d",
                SpellVerbName(e.verb), e.glyph, e.n, e.complete ? 1 : 0, e.matA,
                e.anyA ? 1 : 0, e.matB, e.anyB ? 1 : 0, e.radius, (int)e.modField,
                (int)e.modOp, e.modAmount, e.modN);
  s += buf;
  // A Launch carries a whole delivery record of its own: without it two nests
  // that differ only in the carrier would compare equal and L9/L10 would be
  // asserting nothing.
  for (const DeliveryRec& d : e.launch) SigRecord(lib, d, s);
  for (const EffectInst& i : e.inner) SigEffect(lib, i, s);
  s += "}";
}

void SigRecord(const GlyphLibrary& lib, const DeliveryRec& d, std::string& s) {
  char buf[384];
  std::snprintf(buf, sizeof buf,
                "<d%d m%d w%d c%d sp%d lt%d ir%d g%d fu%d re%d n%d b%d p%d s%d rm%d "
                "body%d x%d tb%d te%d",
                d.glyph, (int)d.mech, d.weight, d.carryMille, d.speedFx, d.lifetimeTicks,
                d.impactRadius, d.gravityMille, d.fuseTicks, d.reach, d.count, d.bounces,
                d.pierce, d.seek, d.radiusMille, d.body ? 1 : 0, d.resolveOnExpiry ? 1 : 0,
                d.trailBudget, d.trailEvery);
  s += buf;
  for (const EffectInst& e : d.trail) SigEffect(lib, e, s);
  // RULE 4: the lanes are part of the record's identity. Without them two
  // casts that fire the same fan carrying different things would compare
  // equal and L2/L12 would be asserting nothing. A lane's own record is
  // laneless by construction, so this recursion is one level deep.
  for (const SpellLane& ln : d.lanes) {
    s += "/";
    SigRecord(lib, ln.rec, s);
    for (const EffectInst& e : ln.extra) SigEffect(lib, e, s);
  }
  s += ">";
}
std::string CastsSignature(const GlyphLibrary& lib, const CastList& l) {
  std::string s;
  char buf[256];
  for (const SpellCast& c : l.casts) {
    SigRecord(lib, c.delivery, s);
    std::snprintf(buf, sizeof buf,
                  "[inst%d word%d tar%d carry%d unk%d t%d v%d gen%d dep%d lv%d clamp%d]",
                  c.instances, c.wordCost, c.tariff, c.carryCost, c.priceUnknown ? 1 : 0,
                  c.ticks, c.voxels, c.generation, c.depth, c.leaves,
                  c.instancesClamped ? 1 : 0);
    s += buf;
    for (int m : c.wastedMods) s += "!" + std::to_string(m);
    s += "|";
    for (const EffectInst& e : c.payload) SigEffect(lib, e, s);
  }
  return s;
}
std::string CastSignature(const GlyphLibrary& lib, const CastList& l) {
  char buf[128];
  std::snprintf(buf, sizeof buf, " cost%d/%d/%d=%d", l.wordCost, l.tariff, l.carryCost,
                l.manaCost);
  return CastsSignature(lib, l) + buf;
}

// The canonical form of a parse under rule 1: every BOX is its delivery plus
// its pile SORTED, all the way down. Two sequences with the same canonical
// form are the same cast by definition; L2 asserts the lowering agrees.
std::string CanonNode(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return "";
  const SpellNode& n = t.nodes[node];
  if (!n.box) return NodeKey(lib, t, node);
  std::vector<std::string> items;
  for (int ii : n.items)
    items.push_back(CanonNode(lib, t, ii) + "#" + std::to_string(t.nodes[ii].n));
  std::sort(items.begin(), items.end());
  std::string s = "[";
  for (const std::string& i : items) s += i + ",";
  // The SEGMENT COUNT is part of the shape (rule 4): a box with an empty
  // trailing lane fires one more instance than one without it.
  return s + "|" + lib.Delivery(n.glyph).id + "|" +
         std::to_string(n.laneAt.size()) + "]";
}
std::string Canon(const GlyphLibrary& lib, const SpellTree& t) {
  std::string s;
  for (const SpellClause& c : t.clauses) s += "<" + CanonNode(lib, t, c.root) + ">";
  return s;
}

// The record of the FIRST carrier a cast fires. Under rule 2 a spoken
// delivery is a box inside the hand cast's payload, not the cast's own record
// — so a fixture that wants "the bomb's fuse" has to look one level in.
const DeliveryRec* BoxRecord(const SpellCast& c) {
  for (const EffectInst& e : c.payload)
    if (e.verb == SpellVerb::Launch && !e.launch.empty()) return &e.launch[0];
  return nullptr;
}

// How deep the launch boxes nest under a node, the hand box counted.
int BoxDepth(const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size() || !t.nodes[node].box) return 0;
  int d = 0;
  for (int ii : t.nodes[node].items) d = std::max(d, BoxDepth(t, ii));
  return d + 1;
}

int32_t RecordField(const DeliveryRec& d, ModField f) {
  switch (f) {
    case ModField::Count: return d.count;
    case ModField::Gravity: return d.gravityMille;
    case ModField::Speed: return d.speedFx;
    case ModField::Lifetime: return d.lifetimeTicks;
    case ModField::Radius: return d.radiusMille;
    case ModField::Bounces: return d.bounces;
    case ModField::Pierce: return d.pierce;
    case ModField::Seek: return d.seek;
    case ModField::Fuse: return d.fuseTicks;
    default: return 0;
  }
}

// ---- spells ------------------------------------------------------------
Status GateSpells(Ctx& c, std::string& detail) {
  World& world = c.world;
  const std::vector<MaterialDef>& mats = c.mats;

  GlyphLibrary lib;
  std::string gerr;
  const std::string gpath = AssetDir() + "/spells/glyphs.json";
  if (!LoadGlyphs(gpath, mats, lib, gerr)) {
    std::printf("spells: FAIL (glyph load: %s)\n", gerr.c_str());
    detail = "glyph load failed";
    return Status::Fail;
  }
  SpellSystem sys;
  sys.SetLibrary(&lib);
  std::vector<uint32_t> classOf;
  for (const auto& m : mats) classOf.push_back(m.gpu.klass);

  auto speak = [&](std::initializer_list<const char*> words) {
    SpellStack st;
    for (const char* w : words) {
      const int gi = lib.Find(w);
      if (gi >= 0) st.spoken.push_back(gi);
    }
    return st;
  };

  // A column whose ground is INSIDE the window with `minAbove` voxels of
  // window above it, searched from the window's centre outward on a coarse
  // grid. The harness landform fades toward the sea at the window's edges
  // (environment truth P-C), so a corner column can have no ground in the
  // window at all -- a fixture placed there spawns nothing / lands on the
  // window floor, and reads as "the engine did nothing" (rule 7).
  auto groundColumn = [&](int minAbove, int& ox, int& oz, int& oh) {
    const IVec3 wo = world.WindowOrigin();
    const int yLo = wo.y * (int)kChunk, yHi = yLo + (int)kWorldN;
    const int cx = wo.x * (int)kChunk + (int)kWorldN / 2;
    const int cz = wo.z * (int)kChunk + (int)kWorldN / 2;
    for (int ring = 0; ring * 32 < (int)kWorldN / 2 - 16; ring++)
      for (int dz = -ring; dz <= ring; dz++)
        for (int dx = -ring; dx <= ring; dx++) {
          if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
          const int x = cx + dx * 32, z = cz + dz * 32;
          const int h = World::TerrainHeight(x, z, kDefaultSeed);
          if (h >= yLo + 8 && h + minAbove <= yHi - 8) {
            ox = x;
            oz = z;
            oh = h;
            return true;
          }
        }
    return false;
  };

  // ---- (1) the trail budget is respected EXACTLY -----------------------------
  // `fire trail projectile`: the trail is (fire ◂trail), a place(fire) mark at
  // every other voxel of the flight, under the trail glyph's voxel budget.
  // The total volume the trail ever authorizes must be <= the budget and the
  // projectile must be dead once it is spent — the rule-2 guarantee that
  // keeps a trail spell from becoming the fourth never-sleeping-chunk bug.
  int64_t trailVolume = 0;
  int32_t authoredBudget = 0;
  bool diedWithBudget = false;
  int flownTicks = 0;
  {
    const int gTrail = lib.Find("trail");
    if (gTrail >= 0) authoredBudget = lib.glyphs[gTrail].voxelBudget;
    CastList sp = CompileSpell(lib, speak({"fire", "trail", "projectile"}));
    CasterState cs;
    cs.mana = 10000;   // fund it fully: this gate is about the BUDGET
    cs.manaMax = 10000;
    FakeHealth hp(10000);
    // Fire horizontally through open air, INSIDE the current residency
    // window (out-of-window space is solid, DESIGN.md §3), anchored to the
    // live origin so test ordering cannot detonate it on tick 1.
    const IVec3 worg = world.WindowOrigin();
    const int sx = worg.x * (int)kChunk + 8;
    const int sz = worg.z * (int)kChunk + (int)kWorldN / 2;
    const int sy = worg.y * (int)kChunk + (int)kWorldN / 2;
    SpellEmission emit;
    SpellFxVec origin{SpellFxFromFloat((float)sx), SpellFxFromFloat((float)sy),
                      SpellFxFromFloat((float)sz)};
    sys.Cast(sp, cs, hp.cb, 1, origin, {kSpellFxOne, 0, 0}, 1, emit);
    // Count ONLY trail ops: paint-mode ops at the trail's radius. The impact
    // emits its own ops at other radii.
    const int32_t trailRadius = gTrail >= 0 ? lib.glyphs[gTrail].radius : 0;
    for (int t = 0; t < (int)lib.budgets.maxLifetimeTicks + 10; t++) {
      SpellEmission e;
      sys.Tick((uint32_t)(2 + t), world, classOf, e);
      for (const BrushOp& b : e.ops) {
        if (b.mode != 0u || b.radius != trailRadius) continue;
        int64_t d = 2 * (int64_t)b.radius + 1;
        trailVolume += d * d * d;
      }
      flownTicks++;
      if (sys.LiveCount() == 0) {
        diedWithBudget = true;
        break;
      }
    }
  }
  const bool budgetOk = authoredBudget > 0 && trailVolume > 0 &&
                        trailVolume <= authoredBudget && diedWithBudget;

  // ---- (1b) DELIVERY: a flight past the mirror still lands ------------------
  // `explosive projectile` fired straight down from well above the ground,
  // far from wherever the CPU mirror happens to be, must RESOLVE: one
  // explosion, at or above the ground contract's height for that column and
  // not under it, before its lifetime runs out. Before the chunk-cache /
  // TerrainHeight tiers every cell past the mirror read as passable, so a
  // bolt fizzled at the end of its life and "explosive projectile" did
  // nothing at all -- which no gate here noticed, because every flight in
  // this file was measured for what it did on the way, never for whether it
  // arrived.
  bool deliverOk = false, deliverColumn = false, deliverMirrored = false;
  int deliverTicks = 0, deliverGround = 0, deliverY = -1, deliverSx = 0, deliverSz = 0;
  {
    CastList sp = CompileSpell(lib, speak({"explosive", "projectile"}));
    CasterState cs;
    cs.mana = 10000;
    cs.manaMax = 10000;
    FakeHealth hp(10000);
    deliverColumn = groundColumn(70, deliverSx, deliverSz, deliverGround);
    {
      // Where the fixture is, for attribution when it is not where it should
      // be: the window and the contract height at its centre.
      const IVec3 wo = world.WindowOrigin();
      const int cx = wo.x * (int)kChunk + (int)kWorldN / 2, cz = wo.z * (int)kChunk + (int)kWorldN / 2;
      std::printf("spell delivery: window origin (%d,%d,%d) chunks, centre column (%d,%d) ground y%d, "
                  "column %s at (%d,%d) y%d\n",
                  wo.x, wo.y, wo.z, cx, cz, World::TerrainHeight(cx, cz, kDefaultSeed),
                  deliverColumn ? "found" : "NOT FOUND", deliverSx, deliverSz, deliverGround);
      const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
      const WorldSnapshot& sn = world.Snap();
      int topSolid = -9999;
      if (sn.valid) {
        // The highest solid the mirror holds at its centre column, as the
        // ground truth against the contract.
        const int mx = sn.mirrorBase.x * (int)kChunk + 24, mz = sn.mirrorBase.z * (int)kChunk + 24;
        for (int y = sn.mirrorBase.y * (int)kChunk + 47; y >= sn.mirrorBase.y * (int)kChunk; y--)
          if (world.KindAt({mx, y, mz}, classOf) == CellKind::Solid) { topSolid = y; break; }
        std::printf("spell delivery: mirror base (%d,%d,%d), top solid at its centre column (%d,%d) y%d, "
                    "contract there y%d\n", sn.mirrorBase.x, sn.mirrorBase.y, sn.mirrorBase.z, mx, mz,
                    topSolid, World::TerrainHeight(mx, mz, kDefaultSeed));
      }
      std::printf("spell delivery: map loaded=%d harness box (%d,%d)-(%d,%d) sea y%d spawn (%d,%d); "
                  "contract y at (100,100)=%d (150,150)=%d (72,72)=%d spawn=%d; mirror seed %u\n",
                  m.Loaded() ? 1 : 0, m.harnessX0, m.harnessZ0, m.harnessX1, m.harnessZ1, m.seaLevelY,
                  m.spawnX, m.spawnZ, World::TerrainHeight(100, 100, kDefaultSeed),
                  World::TerrainHeight(150, 150, kDefaultSeed), World::TerrainHeight(72, 72, kDefaultSeed),
                  World::TerrainHeight(m.spawnX, m.spawnZ, kDefaultSeed), world.WorldSeed());
    }
    const int sy = deliverGround + 60;
    const SpellFxVec origin{SpellFxFromFloat((float)deliverSx + 0.5f), SpellFxFromFloat((float)sy),
                            SpellFxFromFloat((float)deliverSz + 0.5f)};
    // Whether the launch cell is in the CPU mirror (then tier 1 answers) or
    // past it (tiers 2/3, the new path). Printed, not asserted: the claim is
    // that it lands either way.
    deliverMirrored =
        world.KindAt({deliverSx, sy, deliverSz}, classOf) != CellKind::Unknown;
    SpellEmission emit;
    sys.Cast(sp, cs, hp.cb, 3, origin, {0, -kSpellFxOne, 0}, 1, emit);
    const bool launched = sys.LiveCount() == 1 && emit.explosions.empty();
    bool resolved = false;
    for (int t = 0; t < (int)lib.budgets.maxLifetimeTicks + 10 && !resolved; t++) {
      SpellEmission e;
      sys.Tick((uint32_t)(600 + t), world, classOf, e);
      deliverTicks++;
      if (!e.explosions.empty()) {
        resolved = true;
        deliverY = (int)e.explosions[0].y;
      }
      if (sys.LiveCount() == 0) break;
    }
    // Landed on the ground (or on something standing on it: a tree, a rock),
    // never under it; not at the muzzle (60 voxels at <= ~6 vox/tick is more
    // than three ticks); and within the fall rather than at the lifetime
    // bound.
    deliverOk = deliverColumn && launched && resolved && deliverY >= deliverGround - 1 &&
                deliverTicks >= 3 &&
                deliverTicks < (BoxRecord(sp.casts[0]) ? BoxRecord(sp.casts[0])->lifetimeTicks
                                                       : sp.casts[0].delivery.lifetimeTicks);
  }

  // ---- (2) the overcast ------------------------------------------------------
  // Cost beyond mana + health must resolve Fatal, and a Fatal cast must run the
  // spell's own payload AT the caster: an explosive bolt goes off in the chest.
  bool fatalOk = false, carveAsked = false, fatalEmitted = false;
  {
    CastList sp = CompileSpell(lib, speak({"explosive", "projectile"}));
    CasterState cs;
    cs.mana = 1;
    cs.manaMax = 100;
    FakeHealth hp(2);   // mana + health = 3, well under the cost
    CastResult pre = ResolveCast(cs, hp.hp, sp.manaCost);
    SpellEmission emit;
    CastResult res = sys.Cast(sp, cs, hp.cb, 2, {0, 0, 0}, {kSpellFxOne, 0, 0}, 3, emit);
    fatalOk = pre.outcome == CastOutcome::Fatal && res.outcome == CastOutcome::Fatal;
    carveAsked = emit.carveCaster;
    fatalEmitted = !emit.explosions.empty();
    // A fatal cast must NOT also launch the projectile.
    fatalOk = fatalOk && sys.LiveCount() == 0;
  }

  // ---- (3) TOTALITY + LINEAR AMPLIFICATION ------------------------------------
  // Matter ADDS: `fire`×(k+1) throws exactly (k+1) times the matter of `fire`
  // at exactly (k+1) times the price. Asserted as a RELATION between casts
  // rather than as absolute counts, whatever the authored base is.
  bool sprayOk = false;
  int sprayN[3] = {0, 0, 0};
  int32_t sprayCost[3] = {0, 0, 0};
  {
    FakeHealth hp(1000000);
    for (int k = 0; k < 3; k++) {
      SpellStack st;
      const int gFire = lib.Find("fire");
      for (int j = 0; j <= k; j++) st.spoken.push_back(gFire);
      CastList sp = CompileSpell(lib, st);
      sprayCost[k] = sp.manaCost;
      CasterState cs;
      cs.mana = 1000000;
      cs.manaMax = 1000000;
      SpellEmission e;
      sys.Cast(sp, cs, hp.cb, 10 + k, {0, 0, 0}, {kSpellFxOne, 0, 0}, (uint32_t)(100 + k), e);
      sprayN[k] = (int)e.spawns.size();
    }
    sprayOk = sprayN[0] > 0 && sprayN[1] == 2 * sprayN[0] && sprayN[2] == 3 * sprayN[0] &&
              sprayCost[0] > 0 && sprayCost[1] == 2 * sprayCost[0] &&
              sprayCost[2] == 3 * sprayCost[0];
  }

  // ---- (4) THE CAST LATCH: a click must survive a frame that runs no ticks ----
  bool latchOk = false;
  {
    const double frameDt = 1.0 / 60.0;   // faster than the 30 Hz tick
    double acc = 0;
    bool queued = false;
    int clicks = 0, casts = 0, zeroTickFrames = 0;
    for (int frame = 0; frame < 120; frame++) {
      acc += frameDt;
      if (acc > 4 * kTickDt) acc = 4 * kTickDt;
      if (frame % 7 == 0) {
        queued = true;
        clicks++;
      }
      int ticksThis = 0;
      while (acc >= kTickDt && ticksThis < 4) {
        acc -= kTickDt;
        ticksThis++;
        const bool castNow = queued;
        queued = false;
        if (castNow) casts++;
      }
      if (ticksThis == 0) zeroTickFrames++;
    }
    const int accounted = casts + (queued ? 1 : 0);
    latchOk = (accounted == clicks) && zeroTickFrames > 0;
    std::printf(
        "spell cast latch: %s (%d clicks -> %d cast + %d pending, %d/%d frames ran no tick)\n",
        latchOk ? "PASS" : "FAIL", clicks, casts, queued ? 1 : 0, zeroTickFrames, 120);
  }

  // ---- THE LAWS (plan §8) over every sequence of length <= 3 -----------------
  std::vector<int> alpha;
  bool alphaOk = true;
  for (const char* w : kAlphabet) {
    const int gi = lib.Find(w);
    if (gi < 0) {
      std::printf("spells: alphabet word \"%s\" is not in glyphs.json\n", w);
      alphaOk = false;
    } else {
      alpha.push_back(gi);
    }
  }
  std::vector<std::vector<int>> seqs;
  for (int a : alpha) {
    seqs.push_back({a});
    for (int b : alpha) {
      seqs.push_back({a, b});
      for (int cc : alpha) seqs.push_back({a, b, cc});
    }
  }
  auto stackOf = [&](const std::vector<int>& ids) {
    SpellStack st;
    st.spoken = ids;
    return st;
  };
  auto spell = [&](const std::vector<int>& ids) {
    std::string s;
    for (int g : ids) s += (s.empty() ? "" : " ") + lib.glyphs[g].id;
    return s;
  };
  int lawFail = 0;
  auto fail = [&](const char* law, const std::string& what) {
    if (lawFail < 12) std::printf("spells: %s FAILED: %s\n", law, what.c_str());
    lawFail++;
  };

  // L1 totality — every sequence lowers to a non-empty CastList with a finite
  // cost and a non-empty description.
  int l1 = 0;
  for (const auto& s : seqs) {
    const CastList l = CompileSpell(lib, stackOf(s));
    const SpellReadout r = DescribeSpell(lib, l);
    if (l.casts.empty() || l.manaCost < 0 || l.manaCost > (1 << 28) || r.text.empty() ||
        r.verdict.empty() || !r.wellFormed)
      fail("L1", spell(s));
    else
      l1++;
  }

  // L2 bag commutativity — a permutation with the same canonical form (the
  // same binding, the same clause split) lowers to an IDENTICAL CastList.
  int l2 = 0, l2pairs = 0;
  for (const auto& s : seqs) {
    if (s.size() < 2) continue;
    const SpellTree t0 = ParseSpell(lib, stackOf(s));
    const std::string c0 = Canon(lib, t0);
    const std::string sig0 = CastSignature(lib, LowerSpell(lib, t0));
    std::vector<int> p = s;
    std::sort(p.begin(), p.end());
    do {
      if (p == s) continue;
      const SpellTree t1 = ParseSpell(lib, stackOf(p));
      if (Canon(lib, t1) != c0) continue;
      l2pairs++;
      if (CastSignature(lib, LowerSpell(lib, t1)) != sig0)
        fail("L2", spell(s) + "  vs  " + spell(p));
      else
        l2++;
    } while (std::next_permutation(p.begin(), p.end()));
  }

  // L3 multiplicity — `g g` ≡ `g×2`: the node's n is exactly N until the cap,
  // cost is monotone non-decreasing in N, and the scaled axis is exactly ×N
  // (matter: spray voxels; explode: power) or composed exactly once more per
  // word (mods: the record field).
  int l3 = 0;
  {
    const int32_t cap = lib.budgets.maxMultiplicity;
    FakeHealth hp(1 << 30);
    for (int g : alpha) {
      const GlyphDef& gd = lib.glyphs[g];
      int32_t prevCost = 0, prevField = 0, prevSpawn = 0, prevPower = 0;
      for (int32_t N = 1; N <= cap + 1; N++) {
        std::vector<int> s((size_t)N, g);
        const CastList l = CompileSpell(lib, stackOf(s));
        const int32_t want = std::min(N, cap);
        bool ok = !l.casts.empty() && l.manaCost >= prevCost;
        // Every item of the parse carries n == min(N, cap) - EXCEPT a
        // delivery, which does not merge at all: each one boxes what is in
        // front of it, so N of them are N nested boxes under the hand (rule
        // 2). And `lane` does not merge either: N of them open N segments.
        if (gd.sort == GlyphSort::Delivery) {
          ok = ok && l.tree.clauses.size() == 1 &&
               BoxDepth(l.tree, l.tree.clauses[0].root) == (int)N + 1 &&
               l.casts[0].depth == (int)N;
        } else if (gd.sort == GlyphSort::Separator) {
          // RULE 4: `lane` does not merge either, and N of them open N empty
          // segments on the hand box - so the sentence is still ONE cast, and
          // it fires one instance per segment.
          ok = ok && l.tree.clauses.size() == 1 &&
               (int)l.tree.nodes[l.tree.clauses[0].root].laneAt.size() == (int)N &&
               l.casts[0].instances == std::min((int32_t)N, lib.budgets.maxInstances);
        } else {
          ok = ok && l.tree.clauses.size() == 1 && l.tree.clauses[0].bag.size() == 1 &&
               l.tree.nodes[l.tree.clauses[0].bag[0]].n == want;
        }
        if (gd.sort == GlyphSort::Matter && !gd.wildcard && gd.material != 0) {
          CasterState cs;
          cs.mana = 1 << 30;
          cs.manaMax = 1 << 30;
          SpellEmission e;
          sys.Cast(l, cs, hp.cb, 77, {0, 0, 0}, {kSpellFxOne, 0, 0}, (uint32_t)N, e);
          const int32_t spawned = (int32_t)e.spawns.size();
          const int32_t expect = std::min(lib.budgets.sprayVoxels * want, lib.budgets.maxSprayVoxels);
          ok = ok && spawned == expect && spawned >= prevSpawn;
          prevSpawn = spawned;
        }
        if (gd.sort == GlyphSort::Effect && gd.verb == SpellVerb::Explode) {
          SpellEmission e;
          ApplySpellEffect(lib, l.casts[0].payload, {0, 0, 0}, {kSpellFxOne, 0, 0}, 1000, e);
          const int32_t power = e.explosions.empty() ? 0 : e.explosions[0].power;
          ok = ok && power == gd.power * want && power >= prevPower;
          prevPower = power;
        }
        if (gd.sort == GlyphSort::Mod) {
          // Compose: the field is the previous value edited ONCE more (or held
          // at its clamp).
          const int32_t v = RecordField(l.casts[0].delivery, gd.field);
          if (N > 1 && N <= cap) {
            int32_t once = prevField;
            switch (gd.op) {
              case ModOp::Mul: once = prevField * gd.amount; break;
              case ModOp::Div: once = prevField / gd.amount; break;
              case ModOp::Add: once = prevField + gd.amount; break;
            }
            // Either the exact composition, or the field's own clamp cut it
            // short — in which case the value lies between what it was and
            // what the edit asked for. (`twin` five times asks for 32
            // instances and gets the 27 the budget allows, which is neither
            // the old value nor the new one.)
            ok = ok && v >= std::min(prevField, once) && v <= std::max(prevField, once);
          }
          prevField = v;
        }
        if (!ok) fail("L3", spell(s));
        else l3++;
        prevCost = l.manaCost;
      }
    }
  }

  // L6 local binding — an operator's bound arguments do not change when a
  // word is inserted outside the immediate neighbourhood of EVERY operator
  // group: anywhere that does not land inside or adjacent to a group's spoken
  // span. "Every", not "its": binding is greedy left to right, so a word
  // dropped into ANOTHER operator's slot region can steal that operator's
  // argument and free a word for this one (`transmute fire transmute` + gold
  // at 1 hands `fire` to the second transmute). Locality is a property of
  // where the word lands relative to all bindings, and that is what is
  // asserted. Removal is the inverse of the same insertion and is covered by
  // the same comparison.
  int l6 = 0, l6cases = 0;
  for (const auto& s : seqs) {
    const SpellTree t0 = ParseSpell(lib, stackOf(s));
    std::vector<std::pair<int, int>> spans;
    for (const SpellNode& n : t0.nodes)
      if (n.group) spans.push_back({n.first, n.last});
    for (size_t ni = 0; ni < t0.nodes.size(); ni++) {
      const SpellNode& gnode = t0.nodes[ni];
      if (!gnode.group) continue;
      const std::string L0 = gnode.left >= 0 ? ShowNode(lib, t0, gnode.left) : "-";
      const std::string R0 = gnode.right >= 0 ? ShowNode(lib, t0, gnode.right) : "-";
      for (int w : alpha) {
        for (int p = 0; p <= (int)s.size(); p++) {
          bool near = false;
          for (const auto& sp : spans)
            if (p >= sp.first && p <= sp.second + 1) near = true;
          if (near) continue;   // inside, or adjacent to, some group's span
          std::vector<int> s1 = s;
          s1.insert(s1.begin() + p, w);
          const SpellTree t1 = ParseSpell(lib, stackOf(s1));
          const int at1 = gnode.at + (p <= gnode.at ? 1 : 0);
          const SpellNode* g1 = nullptr;
          for (const SpellNode& n : t1.nodes)
            if (n.group && n.at == at1 && n.glyph == gnode.glyph) g1 = &n;
          l6cases++;
          if (!g1) {
            fail("L6", spell(s) + " + " + lib.glyphs[w].id + " at " + std::to_string(p) +
                           ": the operator vanished");
            continue;
          }
          const std::string L1 = g1->left >= 0 ? ShowNode(lib, t1, g1->left) : "-";
          const std::string R1 = g1->right >= 0 ? ShowNode(lib, t1, g1->right) : "-";
          if (L1 != L0 || R1 != R0 || g1->complete != gnode.complete)
            fail("L6", spell(s) + " + " + lib.glyphs[w].id + " at " + std::to_string(p) +
                           ": (" + L0 + "," + R0 + ") became (" + L1 + "," + R1 + ")");
          else
            l6++;
        }
      }
    }
  }

  // L4 ROOT-LANE INDEPENDENCE - `lane A lane B` costs cost(A) + cost(B) and
  // fires two instances, the first carrying exactly what `A` alone lowers to
  // and the second exactly what `B` does. THE SEAM IS A ROOT LANE NOW: `also`
  // was dropped with rule 4, and two unrelated spells in one cast are two
  // columns of the hand box (PLAN_spell_graph section 0b). The guard is the
  // one place the additivity can fail on purpose: a DELIVERY in either arm
  // would box both columns into one (a delivery closes the whole pile,
  // lanes included), and a COUNT mod is record-wide by rule 4, so it would
  // fan the other arm too. Both are real rules, so both are excluded from the
  // law rather than papered over.
  int l4 = 0, l4cases = 0;
  {
    const int gLane = lib.Find("lane");
    auto laneClean = [&](const std::vector<int>& q) {
      for (int g : q) {
        const GlyphDef& gd = lib.glyphs[g];
        if (gd.sort == GlyphSort::Delivery || gd.sort == GlyphSort::Separator) return false;
        if (gd.sort == GlyphSort::Mod && gd.field == ModField::Count) return false;
      }
      return true;
    };
    std::vector<std::vector<int>> shortSeqs;
    std::vector<CastList> lowered;
    for (const auto& s : seqs) {
      if (s.size() > 2 || !laneClean(s)) continue;
      shortSeqs.push_back(s);
      lowered.push_back(CompileSpell(lib, stackOf(s)));
    }
    for (size_t ai = 0; gLane >= 0 && ai < shortSeqs.size(); ai++) {
      const CastList& la = lowered[ai];
      std::string pa;
      for (const EffectInst& e : la.casts[0].payload) SigEffect(lib, e, pa);
      for (size_t bi = 0; bi < shortSeqs.size(); bi++) {
        const CastList& lb = lowered[bi];
        std::vector<int> ab = {gLane};
        ab.insert(ab.end(), shortSeqs[ai].begin(), shortSeqs[ai].end());
        ab.push_back(gLane);
        ab.insert(ab.end(), shortSeqs[bi].begin(), shortSeqs[bi].end());
        const CastList lab = CompileSpell(lib, stackOf(ab));
        l4cases++;
        std::string pb, g0, g1;
        for (const EffectInst& e : lb.casts[0].payload) SigEffect(lib, e, pb);
        const SpellCast i0 = InstanceCast(lab.casts[0], 0);
        const SpellCast i1 = InstanceCast(lab.casts[0], 1);
        for (const EffectInst& e : i0.payload) SigEffect(lib, e, g0);
        for (const EffectInst& e : i1.payload) SigEffect(lib, e, g1);
        const bool ok = lab.casts.size() == 1 && lab.casts[0].instances == 2 &&
                        (int)lab.casts[0].delivery.lanes.size() == 2 &&
                        lab.manaCost == la.manaCost + lb.manaCost &&
                        lab.priceUnknown == (la.priceUnknown || lb.priceUnknown) &&
                        g0 == pa && g1 == pb;
        if (!ok)
          fail("L4", "lane " + spell(shortSeqs[ai]) + " lane " + spell(shortSeqs[bi]));
        else l4++;
      }
    }
  }

  // L12 LANES (rule 4). Three claims, over the same generated alphabet:
  //
  //  (a) a lane is a SEGMENT, not a second pile: `E lane F D` lowers to one
  //      instance carrying exactly what `E F D` carries, at exactly the same
  //      price - the compatibility claim that makes rule 4 free;
  //  (b) a lane BELONGS TO ONE INSTANCE: in `E lane F twin D` the first
  //      instance is `E F D`'s payload and the second is `E D`'s, and the
  //      tariff is their sum rather than either one doubled;
  //  (c) a sentence with NO lane word lowers with no lanes at all, which is
  //      why every pinned price in this repo is unmoved.
  int l12 = 0, l12cases = 0;
  {
    const int gLane = lib.Find("lane"), gTwin = lib.Find("twin");
    std::vector<int> nouns, flights;
    for (int g : alpha) {
      const GlyphDef& gd = lib.glyphs[g];
      if (gd.sort == GlyphSort::Effect || gd.sort == GlyphSort::Matter) nouns.push_back(g);
      if (gd.sort == GlyphSort::Delivery && gd.mech == DeliveryMech::Flight)
        flights.push_back(g);
    }
    // A payload as a SET of effect signatures. Rule 1 says a pile is a set,
    // and a lane's nouns are appended after the shared ones rather than
    // re-sorted with them — so the claim "instance 0 carries what `E F D`
    // carries" is about the multiset, not about the order it was built in.
    auto sigSet = [&](const std::vector<EffectInst>& es) {
      std::vector<std::string> v;
      for (const EffectInst& e : es) {
        std::string one;
        SigEffect(lib, e, one);
        v.push_back(one);
      }
      std::sort(v.begin(), v.end());
      std::string s2;
      for (const std::string& one : v) s2 += one;
      return s2;
    };
    // The payload of the ONE box a `... D` sentence lowers to.
    auto boxSig = [&](const CastList& l) {
      std::string s2;
      if (l.casts.size() != 1 || l.casts[0].payload.size() != 1) return s2;
      const EffectInst& bx = l.casts[0].payload[0];
      if (bx.verb != SpellVerb::Launch || bx.launch.empty()) return s2;
      return sigSet(bx.inner);
    };
    // The same, for instance `i` of that box (rule 4).
    auto instSig = [&](const CastList& l, int32_t i, int32_t& instances) {
      std::string s2;
      instances = 0;
      if (l.casts.size() != 1 || l.casts[0].payload.size() != 1) return s2;
      const EffectInst& bx = l.casts[0].payload[0];
      if (bx.verb != SpellVerb::Launch || bx.launch.empty()) return s2;
      SpellCast c;
      c.delivery = bx.launch[0];
      c.payload = bx.inner;
      instances = RecInstances(lib, c.delivery);
      const SpellCast ic = InstanceCast(c, i);
      return sigSet(ic.payload);
    };
    for (int e : nouns) {
      for (int f : nouns) {
        if (f == e) continue;
        for (int d : flights) {
          const CastList both = CompileSpell(lib, stackOf({e, f, d}));
          const CastList base = CompileSpell(lib, stackOf({e, d}));
          const std::string wantBoth = boxSig(both), wantBase = boxSig(base);
          if (wantBoth.empty() || wantBase.empty()) continue;
          // (a) one lane, one instance: the same cast as saying them together.
          const CastList one = CompileSpell(lib, stackOf({e, gLane, f, d}));
          int32_t n1 = 0;
          const std::string got1 = instSig(one, 0, n1);
          l12cases++;
          if (n1 != 1 || got1 != wantBoth || one.manaCost != both.manaCost)
            fail("L12a", spell({e, gLane, f, d}) + " is not " + spell({e, f, d}));
          else
            l12++;
          // (b) the lane belongs to instance 0; instance 1 is the shared copy.
          if (gTwin < 0) continue;
          const CastList two = CompileSpell(lib, stackOf({e, gLane, f, gTwin, d}));
          int32_t n2 = 0;
          const std::string g0 = instSig(two, 0, n2);
          int32_t n2b = 0;
          const std::string g1 = instSig(two, 1, n2b);
          l12cases++;
          const int32_t wantTariff = both.tariff + base.tariff;
          if (n2 != 2 || g0 != wantBoth || g1 != wantBase ||
              two.casts[0].tariff != wantTariff)
            fail("L12b", spell({e, gLane, f, gTwin, d}) + ": instances " +
                             std::to_string(n2) + ", tariff " +
                             std::to_string(two.casts[0].tariff) + " want " +
                             std::to_string(wantTariff));
          else
            l12++;
        }
      }
    }
    // (c) no `lane` word, no lanes anywhere in the lowering.
    std::function<bool(const std::vector<EffectInst>&)> anyLane =
        [&](const std::vector<EffectInst>& es) {
          for (const EffectInst& x : es) {
            for (const DeliveryRec& r : x.launch)
              if (!r.lanes.empty()) return true;
            if (anyLane(x.inner)) return true;
          }
          return false;
        };
    for (const auto& sq : seqs) {
      bool spoke = false;
      for (int g : sq) spoke = spoke || lib.glyphs[g].sort == GlyphSort::Separator;
      if (spoke) continue;
      const CastList l = CompileSpell(lib, stackOf(sq));
      l12cases++;
      bool clean = true;
      for (const SpellCast& c : l.casts)
        clean = clean && c.delivery.lanes.empty() && !anyLane(c.payload);
      if (!clean) fail("L12c", spell(sq) + " grew a lane nobody spoke");
      else l12++;
    }
  }

  // L7 tariff monotonicity — for `A transmute B` (A != B), cost is
  // non-decreasing in arcane(B) - arcane(A) and in the volume; for any spray,
  // in the voxel count.
  int l7 = 0;
  {
    std::vector<int> matter;
    for (int i = 0; i < (int)lib.glyphs.size(); i++)
      if (lib.glyphs[i].sort == GlyphSort::Matter && !lib.glyphs[i].wildcard) matter.push_back(i);
    std::stable_sort(matter.begin(), matter.end(), [&](int x, int y) {
      return lib.Arcane(lib.glyphs[x].material) < lib.Arcane(lib.glyphs[y].material);
    });
    const int gT = lib.Find("transmute"), gWide = lib.Find("wide");
    for (int a : matter) {
      int32_t prev = -1;
      for (int bm : matter) {
        if (bm == a) continue;
        const CastList l = CompileSpell(lib, stackOf({a, gT, bm}));
        if (prev >= 0 && l.manaCost < prev)
          fail("L7", spell({a, gT, bm}) + " costs less than the cheaper target before it");
        else
          l7++;
        prev = l.manaCost;
        // Volume: transmute×2 converts twice the volume; wide resolves wider.
        const int32_t base = l.manaCost;
        const int32_t twice = CompileSpell(lib, stackOf({a, gT, gT, bm})).manaCost;
        const int32_t wide = gWide >= 0 ? CompileSpell(lib, stackOf({gWide, a, gT, bm})).manaCost : base;
        if (twice < base || wide < base) fail("L7", spell({a, gT, bm}) + " volume");
        else l7++;
      }
      // A spray, in the voxel count.
      const int32_t one = CompileSpell(lib, stackOf({a})).manaCost;
      const int32_t two = CompileSpell(lib, stackOf({a, a})).manaCost;
      if (two < one) fail("L7", spell({a, a}) + " spray");
      else l7++;
    }
  }

  // L5 delivery invariance - the PAYLOAD INSIDE the box of `E... D` is
  // identical for every flight D; only the record differs. The box itself
  // differs, of course: that is the whole of rule 2.
  int l5 = 0;
  {
    const int ds[4] = {lib.Find("projectile"), lib.Find("bolt"), lib.Find("lob"),
                       lib.Find("orb")};
    for (const auto& s : seqs) {
      if (s.size() > 2) continue;
      bool hasDelivery = false;
      for (int g : s)
        hasDelivery = hasDelivery || lib.glyphs[g].sort == GlyphSort::Delivery ||
                      lib.glyphs[g].sort == GlyphSort::Separator;
      if (hasDelivery) continue;
      std::string sig[4];
      bool got[4] = {false, false, false, false};
      for (int k = 0; k < 4; k++) {
        if (ds[k] < 0) continue;
        std::vector<int> sd = s;
        sd.push_back(ds[k]);
        const CastList l = CompileSpell(lib, stackOf(sd));
        if (l.casts.size() != 1 || l.casts[0].payload.size() != 1) continue;
        const EffectInst& bx = l.casts[0].payload[0];
        if (bx.verb != SpellVerb::Launch) continue;
        for (const EffectInst& e : bx.inner) SigEffect(lib, e, sig[k]);
        got[k] = true;
      }
      bool ok = true;
      for (int k = 1; k < 4; k++)
        if (got[0] && got[k] && sig[k] != sig[0]) ok = false;
      if (!ok) fail("L5", spell(s) + " + delivery");
      else l5++;
    }
  }


  // L9 nesting - for any E and flight deliveries D1, D2, the payload of the
  // `E D1 D2` box is EXACTLY ONE Launch, and that Launch is bit-for-bit the
  // one `E D1` lowers to. This is rule 2 stated as an equation: a delivery
  // wraps what came before it and changes nothing about it.
  int l9 = 0, l9cases = 0;
  {
    std::vector<int> flights;
    for (int i = 0; i < (int)lib.glyphs.size(); i++)
      if (lib.glyphs[i].sort == GlyphSort::Delivery &&
          lib.glyphs[i].mech == DeliveryMech::Flight)
        flights.push_back(i);
    for (const auto& e : seqs) {
      if (e.size() > 1) continue;
      bool clean = true;
      for (int g : e)
        clean = clean && lib.glyphs[g].sort != GlyphSort::Delivery &&
                lib.glyphs[g].sort != GlyphSort::Separator;
      if (!clean) continue;
      for (int d1 : flights) {
        std::vector<int> s1 = e;
        s1.push_back(d1);
        const CastList l1 = CompileSpell(lib, stackOf(s1));
        if (l1.casts.size() != 1 || l1.casts[0].payload.size() != 1) continue;
        std::string want;
        SigEffect(lib, l1.casts[0].payload[0], want);
        for (int d2 : flights) {
          std::vector<int> s2 = s1;
          s2.push_back(d2);
          const CastList l2 = CompileSpell(lib, stackOf(s2));
          l9cases++;
          bool ok = l2.casts.size() == 1 && l2.casts[0].payload.size() == 1 &&
                    l2.casts[0].payload[0].verb == SpellVerb::Launch &&
                    l2.casts[0].payload[0].inner.size() == 1;
          if (ok) {
            std::string got;
            SigEffect(lib, l2.casts[0].payload[0].inner[0], got);
            ok = got == want;
          }
          if (!ok) fail("L9", spell(s2) + " is not [" + spell(s1) + "] boxed");
          else l9++;
        }
      }
    }
  }

  // L10 mod placement - `E mod D` and `mod E D` are the same cast (a pending
  // mod does not care when in the pile it was spoken), and in `E D mod` the
  // mod landed on the HAND instead: the box `E D` made is untouched by it.
  int l10 = 0, l10cases = 0;
  {
    std::vector<int> mods, effects, deliveries;
    for (int g : alpha) {
      if (lib.glyphs[g].sort == GlyphSort::Mod) mods.push_back(g);
      if (lib.glyphs[g].sort == GlyphSort::Effect || lib.glyphs[g].sort == GlyphSort::Matter)
        effects.push_back(g);
      if (lib.glyphs[g].sort == GlyphSort::Delivery) deliveries.push_back(g);
    }
    for (int e : effects)
      for (int m : mods)
        for (int d : deliveries) {
          const CastList a = CompileSpell(lib, stackOf({e, m, d}));
          const CastList b2 = CompileSpell(lib, stackOf({m, e, d}));
          l10cases++;
          if (CastSignature(lib, a) != CastSignature(lib, b2))
            fail("L10", spell({e, m, d}) + "  vs  " + spell({m, e, d}));
          else l10++;
          // `E D mod`: the box is the one `E D` made, unchanged; the mod is on
          // the hand record and shows up in the cast's wasted list or its own
          // fields, never inside the box.
          const CastList plain = CompileSpell(lib, stackOf({e, d}));
          const CastList after = CompileSpell(lib, stackOf({e, d, m}));
          l10cases++;
          bool ok = plain.casts.size() == 1 && after.casts.size() == 1 &&
                    plain.casts[0].payload.size() == 1 && after.casts[0].payload.size() == 1;
          if (ok) {
            std::string want, got;
            SigEffect(lib, plain.casts[0].payload[0], want);
            SigEffect(lib, after.casts[0].payload[0], got);
            ok = want == got;
          }
          if (!ok) fail("L10", spell({e, d, m}) + ": the mod reached inside the box");
          else l10++;
        }
  }

  // L11 flattening - a Fatal cast of ANY sequence emits no launches: nothing
  // leaves the caster's body, whatever the sentence nested.
  int l11 = 0;
  {
    SpellSystem fsys;
    fsys.SetLibrary(&lib);
    for (const auto& sq : seqs) {
      const CastList l = CompileSpell(lib, stackOf(sq));
      if (l.manaCost <= 0) continue;   // a free spell never resolves Fatal
      CasterState cs;
      cs.mana = 0;
      cs.manaMax = 100;
      FakeHealth hp(0);
      SpellEmission e;
      const CastResult res =
          fsys.Cast(l, cs, hp.cb, 4242, {0, 0, 0}, {kSpellFxOne, 0, 0}, 7, e);
      if (res.outcome != CastOutcome::Fatal || !e.launches.empty() ||
          fsys.LiveCount() != 0 || fsys.BombCount() != 0 || !e.carveCaster)
        fail("L11", spell(sq));
      else l11++;
      fsys.Clear();
    }
  }

  // ---- (5) THE BOMB IS A BODY: request, adopt, roll, fuse, resolve -----------
  // `explosive bomb` asks the owner for a rigid body; a fake owner adopts it,
  // reports it moving, and after the fuse the payload resolves WHERE THE BODY
  // IS (not where it was thrown) and the body is handed back.
  bool bombOk = false;
  {
    struct FakeBody {
      Vec3 at;
      bool alive = true;
    } fake{{0, 0, 0}};
    SpellBodyProbe bp;
    bp.ctx = &fake;
    bp.bodyAt = [](void* c, uint64_t h, Vec3& out) {
      FakeBody& f = *(FakeBody*)c;
      if (h != 77 || !f.alive) return false;
      out = f.at;
      return true;
    };
    SpellSystem bsys;
    bsys.SetLibrary(&lib);
    CastList sp = CompileSpell(lib, speak({"explosive", "bomb"}));
    CasterState cs;
    cs.mana = 100000;
    cs.manaMax = 100000;
    FakeHealth hp(100000);
    const IVec3 worg = world.WindowOrigin();
    const SpellFxVec origin{SpellFxFromFloat((float)(worg.x * (int)kChunk + 64)),
                            SpellFxFromFloat((float)(worg.y * (int)kChunk + (int)kWorldN / 2)),
                            SpellFxFromFloat((float)(worg.z * (int)kChunk + 64))};
    SpellEmission e0;
    bsys.Cast(sp, cs, hp.cb, 5, origin, {kSpellFxOne, 0, 0}, 1, e0);
    const bool requested = e0.bodyRequests.size() == 1 && e0.explosions.empty() &&
                           bsys.BombCount() == 1 && bsys.LiveCount() == 0;
    const DeliveryRec* bombRec = sp.casts.empty() ? nullptr : BoxRecord(sp.casts[0]);
    const int32_t fuse = bombRec ? bombRec->fuseTicks : 0;
    bool adopted = requested && bsys.AdoptBody(e0.bodyRequests[0].token, 77);
    // The owner reports the body rolling 20 voxels away over the fuse.
    fake.at = Vec3{SpellFxToFloat(origin.x) + 20.0f, SpellFxToFloat(origin.y),
                   SpellFxToFloat(origin.z)};
    int resolvedAtTick = -1, doneHandles = 0;
    ExplosionOp blast{};
    for (int t = 1; t <= fuse + 5 && resolvedAtTick < 0; t++) {
      SpellEmission e;
      bsys.Tick((uint32_t)(10 + t), world, classOf, e, &bp);
      if (!e.explosions.empty()) {
        resolvedAtTick = t;
        blast = e.explosions[0];
        doneHandles = (int)e.bodyDone.size();
      }
    }
    const bool atBody = resolvedAtTick > 0 && blast.x == SpellFxFloor(origin.x) + 20;
    bombOk = requested && adopted && fuse > 0 && resolvedAtTick == fuse && atBody &&
             doneHandles == 1 && bsys.BombCount() == 0;
    std::printf("spell bomb: %s (requested=%d adopted=%d fuse %d resolved at tick %d, at the "
                "body=%d, handed back=%d)\n",
                bombOk ? "PASS" : "FAIL", requested ? 1 : 0, adopted ? 1 : 0, fuse,
                resolvedAtTick, atBody ? 1 : 0, doneHandles);
  }

  // L8 budgets — every lowered cast declares finite ticks, voxels, instances
  // and generation; nothing lowers to an unbounded process (rule 2).
  int l8 = 0;
  {
    // A nest chains clocks: every level adds its own lifetime + fuse before
    // the next one starts, and a length-3 sentence can nest 3 deep.
    const int32_t tickBound =
        6 * lib.budgets.maxLifetimeTicks + lib.budgets.maxStatusTicks;
    for (const auto& s : seqs) {
      const CastList l = CompileSpell(lib, stackOf(s));
      bool ok = true;
      for (const SpellCast& c : l.casts)
        ok = ok && c.ticks >= 1 && c.ticks <= tickBound && c.voxels >= 0 &&
             c.voxels < (1 << 30) && c.instances >= 1 &&
             c.instances <= lib.budgets.maxInstances && c.generation >= 0 &&
             c.generation <= lib.budgets.maxGeneration &&
             // The nest is bounded twice over: the LEAF instance count (the
             // product of the fans down each path) never exceeds the instance
             // cap, and the depth never exceeds the words that could have made
             // it. What stops a deep nest from FIRING is the generation cap,
             // asserted at runtime in check (8) - a sentence may SAY more than
             // the engine will do, and saying so is not an unbounded process.
             c.leaves >= 1 && c.leaves <= lib.budgets.maxInstances &&
             c.depth >= 0 && c.depth <= kSpellStackMax;
      if (!ok) fail("L8", spell(s));
      else l8++;
    }
  }

  // ---- (6) SUSTAINED: aura attaches, bills, caps; a beam ends on release;
  // an echo repeats a bounded number of times; a ward refuses at the splice.
  bool sustainOk = false;
  {
    SpellSystem ssys;
    ssys.SetLibrary(&lib);
    CasterState cs;
    cs.mana = 100000;
    cs.manaMax = 100000;
    FakeHealth hp(100000);
    const IVec3 worg = world.WindowOrigin();
    const SpellFxVec origin{SpellFxFromFloat((float)(worg.x * (int)kChunk + 64)),
                            SpellFxFromFloat((float)(worg.y * (int)kChunk + (int)kWorldN / 2)),
                            SpellFxFromFloat((float)(worg.z * (int)kChunk + 64))};
    // A fake owner with ONE body at the origin (the caster's own).
    struct Bodies {
      Vec3 at;
    } bodies{Vec3{SpellFxToFloat(origin.x), SpellFxToFloat(origin.y), SpellFxToFloat(origin.z)}};
    SpellBodyProbe bp;
    bp.ctx = &bodies;
    bp.bodyIdAt = [](void* c, Vec3 from, float r, uint64_t& id, Vec3& out) {
      const Vec3 d = ((Bodies*)c)->at - from;
      if (d.x * d.x + d.y * d.y + d.z * d.z > r * r) return false;
      id = 42;
      out = ((Bodies*)c)->at;
      return true;
    };
    bp.bodyPos = [](void* c, uint64_t id, Vec3& out) {
      if (id != 42) return false;
      out = ((Bodies*)c)->at;
      return true;
    };
    // fire aura self: a status on body 42, billed every tick, capped.
    SpellEmission e0;
    ssys.Cast(CompileSpell(lib, speak({"fire", "aura", "self"})), cs, hp.cb, 9, origin,
              {kSpellFxOne, 0, 0}, 1, e0, nullptr, nullptr, &bp);
    const int after1 = ssys.StatusCountFor(9);
    int billed = 0, sprays = 0;
    for (int t = 0; t < 10; t++) {
      SpellEmission e;
      ssys.Tick((uint32_t)(20 + t), world, classOf, e, &bp);
      for (const SpellBill& sb : e.bills)
        if (sb.casterId == 9) billed += sb.amount;
      sprays += (int)e.spawns.size();
    }
    const int32_t reserve = ssys.ReservationFor(9);
    for (int k = 0; k < lib.budgets.maxStatusPerCaster + 3; k++) {
      SpellEmission e;
      ssys.Cast(CompileSpell(lib, speak({"fire", "aura", "self"})), cs, hp.cb, 9, origin,
                {kSpellFxOne, 0, 0}, (uint32_t)(40 + k), e, nullptr, nullptr, &bp);
    }
    const int capped = ssys.StatusCountFor(9);
    const bool dropped = ssys.DropNewestStatus(9);
    const int afterDrop = ssys.StatusCountFor(9);
    ssys.DropAll(9);
    const int afterAll = ssys.StatusCountFor(9);
    // A status runs out at its tick cap.
    SpellEmission e1;
    ssys.Cast(CompileSpell(lib, speak({"fire", "aura", "self"})), cs, hp.cb, 9, origin,
              {kSpellFxOne, 0, 0}, 60, e1, nullptr, nullptr, &bp);
    int capTicks = 0;
    for (int t = 0; t < lib.budgets.maxStatusTicks + 5 && ssys.StatusCountFor(9) > 0; t++) {
      SpellEmission e;
      ssys.Tick((uint32_t)(100 + t), world, classOf, e, &bp);
      capTicks++;
    }
    const bool attachedToBody = !ssys.Statuses().empty() || afterAll == 0;
    (void)attachedToBody;
    const bool statusOk = after1 == 1 && billed > 0 && sprays > 0 && reserve > 0 &&
                          capped == lib.budgets.maxStatusPerCaster && dropped &&
                          afterDrop == capped - 1 && afterAll == 0 &&
                          capTicks <= lib.budgets.maxStatusTicks;

    // A beam: held for 5 ticks, then released; it resolves on every held
    // tick (the cast itself resolves nothing) and is gone the tick after
    // release.
    SpellEmission e2;
    ssys.Cast(CompileSpell(lib, speak({"explosive", "beam"})), cs, hp.cb, 9, origin,
              {kSpellFxOne, 0, 0}, 200, e2, nullptr);
    int beamTicks = 0;
    for (int t = 0; t < 8; t++) {
      ssys.HoldBeam(9, origin, {kSpellFxOne, 0, 0}, t < 5);
      SpellEmission e;
      ssys.Tick((uint32_t)(200 + t), world, classOf, e, &bp);
      if (!e.explosions.empty()) beamTicks++;
    }
    const bool beamOk = (int)ssys.Beams().size() == 0 && beamTicks == 5;

    // An echo: explosive echo self repeats `repeats` times in all, then stops.
    const int gEcho = lib.Find("echo");
    const int32_t repeats = gEcho >= 0 ? lib.glyphs[gEcho].repeats : 0;
    SpellEmission e3;
    ssys.Cast(CompileSpell(lib, speak({"explosive", "echo", "self"})), cs, hp.cb, 9, origin,
              {kSpellFxOne, 0, 0}, 300, e3, nullptr);
    int echoBlasts = (int)e3.explosions.size();
    for (int t = 0; t < 200; t++) {
      SpellEmission e;
      ssys.Tick((uint32_t)(300 + t), world, classOf, e, &bp);
      echoBlasts += (int)e.explosions.size();
    }
    const bool echoOk = repeats > 1 && echoBlasts == repeats && ssys.Echoes().empty();

    // A ward: `transmute null self` refuses a convert op inside its radius at
    // the splice and lets a paint op through; the filter is gone next tick.
    SpellEmission e4;
    ssys.Cast(CompileSpell(lib, speak({"transmute", "null", "self"})), cs, hp.cb, 9, origin,
              {kSpellFxOne, 0, 0}, 400, e4, nullptr);
    std::vector<BrushOp> ops = {
        {SpellFxFloor(origin.x) + 1, SpellFxFloor(origin.y), SpellFxFloor(origin.z), 1, 5u, 1u, 0, 0},
        {SpellFxFloor(origin.x) + 1, SpellFxFloor(origin.y), SpellFxFloor(origin.z), 1, 5u, 0u, 0, 0},
        {SpellFxFloor(origin.x) + 500, SpellFxFloor(origin.y), SpellFxFloor(origin.z), 1, 5u, 1u, 0, 0},
    };
    std::vector<ExplosionOp> exps;
    std::vector<ParticleSpawn> spawns;
    std::vector<WindPrim> winds;
    const int refused = ssys.FilterStreams(ops, exps, spawns, winds);
    const bool wardOk = (int)ssys.Filters().size() == 1 && refused == 1 && ops.size() == 2 &&
                        ops[0].mode == 0u;
    {
      SpellEmission e;
      ssys.Tick(500, world, classOf, e, &bp);
    }
    const bool wardExpired = ssys.Filters().empty();

    sustainOk = statusOk && beamOk && echoOk && wardOk && wardExpired;
    std::printf("spell sustain: %s (aura attached=%d billed=%d sprayed=%d reserve=%d cap=%d/%d "
                "drop=%d capTicks=%d; beam ticks=%d; echo blasts=%d/%d; ward refused=%d "
                "expired=%d)\n",
                sustainOk ? "PASS" : "FAIL", after1, billed, sprays, reserve, capped,
                lib.budgets.maxStatusPerCaster, dropped ? 1 : 0, capTicks, beamTicks,
                echoBlasts, repeats, refused, wardExpired ? 1 : 0);
  }

  // ---- (7) MEND: the world half, the body half, and the cauterise rule -----
  // `blood mend` by hand over a patch of blood takes voxels out of the world
  // as filtered convert(cell->air) ops and posts a restore for exactly that
  // many; a carved creature's missing anatomy cells fill root-first with the
  // matter and the missing count falls by exactly what landed; and a stump
  // that has charred through stops bleeding while the creature lives.
  bool mendOk = false;
  {
    // The VM half, over a fake mirror: blood in a 3-voxel ball around reach.
    struct BloodPatch {
      int32_t cx, cy, cz;
      uint32_t blood;
    } patch{0, 0, 0, 0};
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == "blood") patch.blood = (uint32_t)i;
    SpellProbe bp;
    bp.ctx = &patch;
    bp.matAt = [](void* c, int32_t x, int32_t y, int32_t z, bool& known) -> uint32_t {
      const BloodPatch& p = *(BloodPatch*)c;
      known = true;
      const int32_t dx = x - p.cx, dy = y - p.cy, dz = z - p.cz;
      return dx * dx + dy * dy + dz * dz <= 9 ? p.blood : 0u;
    };
    const int gMend = lib.Find("mend");
    const int32_t perTick = gMend >= 0 ? lib.glyphs[gMend].perTick : 0;
    CastList sp = CompileSpell(lib, speak({"blood", "mend"}));
    patch.cx = 3;   // hand reach along +x from the origin
    CasterState cs;
    cs.mana = 100000;
    cs.manaMax = 100000;
    FakeHealth hp(100000);
    SpellSystem msys;
    msys.SetLibrary(&lib);
    SpellEmission e;
    msys.Cast(sp, cs, hp.cb, 9, {0, 0, 0}, {kSpellFxOne, 0, 0}, 1, e, &bp);
    int converts = 0;
    bool filtered = true;
    for (const BrushOp& o : e.ops) {
      if (o.mode != 1u || o.material != 0u) continue;
      converts++;
      filtered = filtered && o.pad0 == patch.blood && o.radius == 0;
    }
    const bool vmOk = patch.blood != 0 && perTick > 0 && converts == perTick && filtered &&
                      e.restores.size() == 1 && e.restores[0].material == patch.blood &&
                      e.restores[0].count == converts && e.restores[0].casterId == 9;

    // The body half, on a real creature: carve, then graft back.
    MobSystem& mobs = c.mobs;
    int dummyDef = -1;
    for (size_t i = 0; i < mobs.Defs().size(); i++)
      if (mobs.Defs()[i].name == kAvatarDefName) dummyDef = (int)i;
    bool bodyOk = false, cauteriseOk = false;
    int missingBefore = 0, restored = 0, missingAfter = 0, bleedTicks = 0, charTick = -1;
    bool died = false;
    int charredAtEnd = 0, burningAtEnd = 0, parentVox = 0, cookedAtEnd = 0;
    if (dummyDef >= 0) {
      // Inside the CURRENT residency window (streaming moved it): a creature
      // spawned outside it has no bodies and is culled on the first tick.
      // And on ground that is inside the window: 72 voxels in from the
      // corner sits on the landform's fade to the sea at some window
      // positions, where the ground is under the window floor and the
      // creature spawned there was culled the same way -- "missing 0 ->
      // grafted 0" with nothing wrong in mend.
      int sx = 0, sz = 0, h = 0;
      if (!groundColumn(40, sx, sz, h)) {
        const IVec3 wo = world.WindowOrigin();
        sx = wo.x * (int)kChunk + 72;
        sz = wo.z * (int)kChunk + 72;
        h = World::TerrainHeight(sx, sz, kDefaultSeed);
      }
      IVec3 pc{sx >> 4, (h + 1) >> 4, sz >> 4};
      const uint64_t id = mobs.Spawn(dummyDef, {sx, h + 1, sz});
      const MobDef& dd = mobs.Defs()[dummyDef];
      auto limbIndex = [&](const char* name) {
        for (size_t i = 0; i < dd.limbs.size(); i++)
          if (dd.limbs[i].name == name) return (int)i;
        return -1;
      };
      uint32_t t = 9000;
      auto mobTick = [&]() {
        std::vector<BrushOp> ops;
        std::vector<ParticleSpawn> spawns;
        std::vector<CellOp> cellOps;
        mobs.PreTick(t + 1, world, ops, cellOps, spawns);
        c.debris.QueueSupportEvents(world.Snap());
        c.debris.PreTick(t + 1, world, cellOps, spawns);
        ++t;
        SubmitTick(c.ctx, world, c.sim, t, kDefaultSeed, ops, {}, cellOps, false, pc,
                   true, false, spawns);
        c.ctx.WaitIdle();
        c.ctx.ProcessEvents();
        c.phys.Step(kTickDt);
        c.debris.PostStep();
        mobs.PostStep();
      };
      for (int i = 0; i < 6; i++) mobTick();
      // A small blast into the torso (the biggest limb) takes a bite out of it.
      const int torso = limbIndex("torso");
      std::vector<ParticleSpawn> spawns;
      if (torso >= 0 && mobs.LimbVoxelCount(id, torso) > 0)
        mobs.CarveLimbRadial(mobs.LimbBody(id, torso),
                             mobs.LimbVoxelPos(id, torso, mobs.LimbVoxelCount(id, torso) / 2),
                             1.5f, true, false, world, spawns);
      for (int i = 0; i < 2; i++) mobTick();
      int carvedLimb = -1;
      for (int li = 0; li < (int)dd.limbs.size(); li++)
        if (mobs.MissingVoxelCount(id, li) > 0 && carvedLimb < 0) carvedLimb = li;
      if (carvedLimb >= 0) {
        missingBefore = (int)mobs.MissingVoxelCount(id, carvedLimb);
        // RestoreBody walks root-first; the first limb with a hole is where
        // the graft lands, and the count must move by exactly what landed.
        restored = mobs.RestoreMob(id, patch.blood, 20);
        missingAfter = (int)mobs.MissingVoxelCount(id, carvedLimb);
        bodyOk = missingBefore > 0 && restored > 0 && restored <= 20 &&
                 missingAfter == missingBefore - restored;
      }
      // Cauterise: cut the forearm so the upper arm's stump bleeds, then put
      // REAL FIRE in the world around the wound — the path `fire self` takes,
      // through the CA and the body's own burn front (direct ignition is a
      // cloth-only entry point; bare skin catches from hot cells beside it) —
      // and wait for the flesh at the wound to char through while the
      // creature lives.
      const int arm = limbIndex("armL.L");
      uint32_t mFire = 0;
      for (size_t m = 0; m < mats.size(); m++)
        if (mats[m].name == "fire") mFire = (uint32_t)m;
      if (arm >= 0 && mFire != 0) {
        mobs.Sever(id, arm);
        Vec3 woundW{};
        bool haveWound = false;
        for (int i = 0; i < 8; i++) {
          mobTick();
          for (const MobSystem::BleedSource& bs : mobs.BleedSources())
            if (bs.key == ((id << 8) ^ (uint64_t)(limbIndex(dd.limbs[arm].parent.c_str()) + 1))) {
              bleedTicks++;
              woundW = bs.posVoxel;
              haveWound = true;
            }
        }
        const int parent = limbIndex(dd.limbs[arm].parent.c_str());
        // The STUMP's own wound, by the key the bleed list carries (the torso
        // carve above left a wound of its own that drips down on its own
        // clock and is nowhere near the fire).
        const uint64_t stumpKey = (id << 8) ^ (uint64_t)(parent + 1);
        auto soakTick = [&]() {
          std::vector<BrushOp> ops;
          std::vector<ParticleSpawn> spawns2;
          std::vector<CellOp> cellOps;
          mobs.PreTick(t + 1, world, ops, cellOps, spawns2);
          c.debris.QueueSupportEvents(world.Snap());
          c.debris.PreTick(t + 1, world, cellOps, spawns2);
          const IVec3 b{ifloor(woundW.x), ifloor(woundW.y), ifloor(woundW.z)};
          for (int dz = -2; dz <= 2; dz++)
            for (int dy = -2; dy <= 2; dy++)
              for (int dx = -2; dx <= 2; dx++) {
                const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
                if (!world.CellInWindow(cc)) continue;
                if (cellOps.size() >= kMaxCellOpsPerTick) break;
                cellOps.push_back({World::SlotCellIndex(cc),
                                   PackVoxNew(mFire, 7u) | kCellOpIfAir});
              }
          ++t;
          SubmitTick(c.ctx, world, c.sim, t, kDefaultSeed, ops, {}, cellOps, false, pc,
                     true, false, spawns2);
          c.ctx.WaitIdle();
          c.ctx.ProcessEvents();
          c.phys.Step(kTickDt);
          c.debris.PostStep();
          mobs.PostStep();
        };
        for (int i = 0; haveWound && i < 1500 && charTick < 0; i++) {
          // The creature WANDERS: the fire follows the wound, and the mirror
          // (which the burn front reads hot cells from) follows the creature.
          const Vec3 mo = mobs.MobOrigin(id);
          pc = IVec3{ifloor(mo.x) >> 4, ifloor(mo.y) >> 4, ifloor(mo.z) >> 4};
          soakTick();
          bool alive = false;
          for (uint32_t k = 0; k < mobs.MobCount(); k++) alive = alive || mobs.MobIdAt(k) == id;
          if (!alive) {
            died = true;
            break;
          }
          bool bleeding = false;
          for (const MobSystem::BleedSource& bs : mobs.BleedSources())
            if (bs.key == stumpKey) {
              bleeding = true;
              woundW = bs.posVoxel;
            }
          if (!bleeding && i > 4) charTick = i;
        }
        if (parent >= 0) {
          for (size_t m = 0; m < mats.size(); m++)
            if (Mob::BurnStageOfMaterialName(mats[m].name) == 2)
              charredAtEnd += (int)mobs.LimbMaterialCount(id, parent, (uint32_t)m);
            else if (Mob::BurnStageOfMaterialName(mats[m].name) == 1)
              cookedAtEnd += (int)mobs.LimbMaterialCount(id, parent, (uint32_t)m);
          burningAtEnd = (int)mobs.LimbBurningCount(id, parent);
          parentVox = (int)mobs.LimbVoxelCount(id, parent);
        }
        cauteriseOk = bleedTicks > 0 && charTick > 0 && !died;
      }
      mobs.Reset();
      c.debris.Reset();
    }
    mendOk = vmOk && bodyOk && cauteriseOk;
    std::printf("spell mend: %s (vm: %d convert ops filtered=%d, restore %d; body: missing %d "
                "-> grafted %d -> missing %d; cauterise: bled %d ticks, closed at tick %d, "
                "died=%d; stump %d voxels: %d charred, %d burning, %d cooked at the end)\n",
                mendOk ? "PASS" : "FAIL", converts, filtered ? 1 : 0,
                e.restores.empty() ? 0 : e.restores[0].count, missingBefore, restored,
                missingAfter, bleedTicks, charTick, died ? 1 : 0,
                parentVox, charredAtEnd, burningAtEnd, cookedAtEnd);
  }


  // ---- (8) THE NEST, AT RUNTIME (rule 2) -------------------------------------
  // A box that says "fire a bolt" must actually fire one, from the parent's
  // impact, one generation down, and the chain must STOP at the generation
  // cap. Four sentences, one fixture: bolts fired horizontally through open
  // air inside the window, ticked until nothing is left.
  bool nestOk = false;
  int nestChild = -1, nestFuseGap = -1, nestFan = 0, nestDeep = 0;
  {
    const IVec3 worg = world.WindowOrigin();
    const SpellFxVec origin{SpellFxFromFloat((float)(worg.x * (int)kChunk + 8)),
                            SpellFxFromFloat((float)(worg.y * (int)kChunk + (int)kWorldN / 2)),
                            SpellFxFromFloat((float)(worg.z * (int)kChunk + (int)kWorldN / 2))};
    // What a flight reports: how many CHILDREN (generation >= 1 carriers) were
    // ever born, the tick the first one appeared, the tick a parent settled on
    // its fuse, the deepest generation reached, and how many blasts landed.
    struct Flight {
      int children = 0, firstChildTick = -1, restTick = -1, maxGen = 0, blasts = 0;
    };
    auto fly = [&](std::initializer_list<const char*> words) {
      Flight f;
      SpellSystem nsys;
      nsys.SetLibrary(&lib);
      CastList sp = CompileSpell(lib, speak(words));
      CasterState cs;
      cs.mana = 1 << 28;
      cs.manaMax = 1 << 28;
      FakeHealth hp(1 << 28);
      SpellEmission e0;
      nsys.Cast(sp, cs, hp.cb, 31, origin, {kSpellFxOne, 0, 0}, 1, e0);
      std::vector<uint32_t> childSeqs;
      for (int t = 0; t < 3 * (int)lib.budgets.maxLifetimeTicks; t++) {
        SpellEmission e;
        nsys.Tick((uint32_t)(4000 + t), world, classOf, e, nullptr);
        f.blasts += (int)e.explosions.size();
        for (const SpellProjectile& pr : nsys.Live()) {
          f.maxGen = std::max(f.maxGen, (int)pr.gen);
          if (pr.resting && f.restTick < 0) f.restTick = t;
          if (pr.gen < 1) continue;
          bool known = false;
          for (uint32_t q : childSeqs) known = known || q == pr.seq;
          if (known) continue;
          childSeqs.push_back(pr.seq);
          f.children++;
          if (f.firstChildTick < 0) f.firstChildTick = t;
        }
        if (nsys.LiveCount() == 0 && nsys.BombCount() == 0 && t > 2) break;
      }
      return f;
    };

    // (a) ONE child, launched where the parent hit, and IT is what explodes.
    const Flight a2 = fly({"explosive", "projectile", "projectile"});
    const bool oneChild = a2.children == 1 && a2.blasts == 1 && a2.maxGen == 1;
    nestChild = a2.children;
    // (b) `fuse` on the OUTER box: the parent rests where it landed and the
    // child is born exactly `fuse` ticks later.
    const Flight b2 = fly({"explosive", "projectile", "fuse", "projectile"});
    const int gFuse = lib.Find("fuse");
    const int32_t fuseTicks = gFuse >= 0 ? lib.glyphs[gFuse].amount : 0;
    nestFuseGap = (b2.firstChildTick >= 0 && b2.restTick >= 0)
                      ? b2.firstChildTick - b2.restTick
                      : -1;
    const bool fused = b2.children == 1 && nestFuseGap == fuseTicks && b2.blasts == 1;
    // (c) `shotgun` between the two deliveries: three carriers fan, and each
    // fires one child, so three children explode.
    const Flight c2 = fly({"explosive", "projectile", "shotgun", "projectile"});
    nestFan = c2.children;
    const bool fanned = c2.children == 3 && c2.blasts == 3;
    // (d) THE DEPTH CAP. Five nested boxes is a legal, priced, total sentence;
    // the engine simply stops making carriers past budgets.maxGeneration
    // (rule 2), and nothing here runs away.
    const Flight d2 = fly({"explosive", "projectile", "projectile", "projectile",
                           "projectile", "projectile"});
    nestDeep = d2.maxGen;
    const bool capped = d2.maxGen <= lib.budgets.maxGeneration;
    nestOk = oneChild && fused && fanned && capped;
    std::printf("spell nest: %s (one child: %d born, gen %d, %d blast(s); fuse gap %d/%d "
                "(rest t%d, child t%d); shotgun children %d over %d blasts; five deliveries "
                "reach gen %d of %d)\n",
                nestOk ? "PASS" : "FAIL", a2.children, a2.maxGen, a2.blasts, nestFuseGap,
                fuseTicks, b2.restTick, b2.firstChildTick, nestFan, c2.blasts, nestDeep,
                lib.budgets.maxGeneration);
  }

  // ---- (9) LANES AT RUNTIME (rule 4) ---------------------------------------
  // The laws above are about the lowering; these two are about what leaves.
  //   (a) `explosive lane fire twin projectile` fires TWO bolts and exactly
  //       ONE of them carries the fire - the fan is not a copy machine any
  //       more.
  //   (b) `lane explosive projectile lane blood mend self` is two unrelated
  //       spells in one cast: a bolt leaves, and the mend resolves on the
  //       caster, from one utterance.
  bool laneRunOk = false;
  int laneBolts = 0, laneWithFire = 0, laneRestores = 0, laneLive = 0;
  {
    SpellSystem lsys;
    lsys.SetLibrary(&lib);
    CasterState cs;
    cs.mana = 1 << 28;
    cs.manaMax = 1 << 28;
    FakeHealth hp(1 << 28);
    const IVec3 worg = world.WindowOrigin();
    const SpellFxVec origin{SpellFxFromFloat((float)(worg.x * (int)kChunk + 8)),
                            SpellFxFromFloat((float)(worg.y * (int)kChunk + (int)kWorldN / 2)),
                            SpellFxFromFloat((float)(worg.z * (int)kChunk + (int)kWorldN / 2))};
    uint32_t mFire = 0;
    for (size_t m = 0; m < mats.size(); m++)
      if (mats[m].name == "fire") mFire = (uint32_t)m;
    SpellEmission e0;
    lsys.Cast(CompileSpell(lib, speak({"explosive", "lane", "fire", "twin", "projectile"})),
              cs, hp.cb, 88, origin, {kSpellFxOne, 0, 0}, 1, e0);
    laneBolts = lsys.LiveCount();
    for (const SpellProjectile& pr : lsys.Live()) {
      bool fire = false;
      for (const EffectInst& x : pr.cast.payload)
        fire = fire || (x.verb == SpellVerb::Spray && x.matA == mFire);
      if (fire) laneWithFire++;
    }
    lsys.Clear();
    // The mend needs something to draw on: a mirror that is blood everywhere
    // near the caster, so the world half of the graft has a source.
    struct Patch {
      uint32_t blood;
    } patch{0};
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == "blood") patch.blood = (uint32_t)i;
    SpellProbe probe2;
    probe2.ctx = &patch;
    probe2.matAt = [](void* c, int32_t, int32_t, int32_t, bool& known) -> uint32_t {
      known = true;
      return ((Patch*)c)->blood;
    };
    SpellEmission e1;
    lsys.Cast(CompileSpell(lib, speak({"lane", "explosive", "projectile", "lane", "blood",
                                       "mend", "self"})),
              cs, hp.cb, 89, origin, {kSpellFxOne, 0, 0}, 2, e1, &probe2);
    laneLive = lsys.LiveCount();
    laneRestores = (int)e1.restores.size();
    laneRunOk = laneBolts == 2 && laneWithFire == 1 && laneLive == 1 && laneRestores == 1;
    lsys.Clear();
    std::printf("spell lanes: %s (twin+lane: %d bolts, %d carrying fire; root lanes: %d bolt(s) "
                "and %d restore(s) from one cast)\n",
                laneRunOk ? "PASS" : "FAIL", laneBolts, laneWithFire, laneLive, laneRestores);
  }

  const bool lawsOk = alphaOk && lawFail == 0 && l1 > 0 && l2pairs > 0 && l3 > 0 &&
                      l6cases > 0 && l4cases > 0 && l7 > 0 && l5 > 0 && l8 > 0 &&
                      l9cases > 0 && l10cases > 0 && l11 > 0 && l12cases > 0 && laneRunOk;
  const bool spellOk = budgetOk && deliverOk && fatalOk && carveAsked && fatalEmitted &&
                       sprayOk && latchOk && lawsOk && bombOk && sustainOk && mendOk &&
                       nestOk;
  std::printf(
      "spells: %s (trail authorized %lld/%d voxels over %d ticks, died=%d; "
      "delivered=%d at y=%d over ground %d (%d,%d; column=%d mirrored=%d) after %d ticks; "
      "overcast fatal=%d carve=%d payload=%d; spray %d/%d/%d voxels for "
      "%d/%d/%d mana; bomb=%d sustain=%d mend=%d nest=%d; laws over %zu sequences: "
      "L1 %d, L2 %d/%d, L3 %d, L4 %d/%d, L5 %d, L6 %d/%d, L7 %d, L8 %d, L9 %d/%d, "
      "L10 %d/%d, L11 %d, L12 %d/%d, %d failures)\n",
      spellOk ? "PASS" : "FAIL", (long long)trailVolume, authoredBudget, flownTicks,
      diedWithBudget ? 1 : 0, deliverOk ? 1 : 0, deliverY, deliverGround, deliverSx, deliverSz,
      deliverColumn ? 1 : 0, deliverMirrored ? 1 : 0, deliverTicks,
      fatalOk ? 1 : 0, carveAsked ? 1 : 0, fatalEmitted ? 1 : 0,
      sprayN[0], sprayN[1], sprayN[2], sprayCost[0], sprayCost[1], sprayCost[2],
      bombOk ? 1 : 0, sustainOk ? 1 : 0, mendOk ? 1 : 0, nestOk ? 1 : 0, seqs.size(), l1,
      l2, l2pairs, l3, l4, l4cases, l5, l6, l6cases, l7, l8, l9, l9cases, l10, l10cases,
      l11, l12, l12cases, lawFail);
  detail = Format("laws %zu seq, %d failures", seqs.size(), lawFail);
  return spellOk ? Status::Pass : Status::Fail;
}

// ---- spells-oracle ------------------------------------------------------------
// The C++ parser against the reference script, entry by entry. CPU-only: it
// loads the glyph library and the oracle and touches no world.
Status GateSpellsOracle(Ctx& c, std::string& detail) {
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    std::printf("spells-oracle: FAIL (glyph load: %s)\n", gerr.c_str());
    detail = "glyph load failed";
    return Status::Fail;
  }
  const std::string opath = AssetDir() + "/spells/grammar_oracle.json";
  std::ifstream f(opath);
  if (!f) {
    std::printf("spells-oracle: FAIL (cannot open %s; run `python scripts/magic_grammar.py --oracle`)\n",
                opath.c_str());
    detail = "no oracle";
    return Status::Fail;
  }
  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    std::printf("spells-oracle: FAIL (parse: %s)\n", e.what());
    detail = "oracle unreadable";
    return Status::Fail;
  }
  // Every word the reference knows must be a glyph here, or the two tables
  // have drifted.
  int missing = 0;
  for (const auto& g : j.value("glyphs", nlohmann::json::array())) {
    if (lib.Find(g.get<std::string>()) < 0) {
      if (missing < 8)
        std::printf("spells-oracle: glyph \"%s\" is in the oracle but not in glyphs.json\n",
                    g.get<std::string>().c_str());
      missing++;
    }
  }
  int entries = 0, mismatches = 0;
  for (const auto& e : j.value("entries", nlohmann::json::array())) {
    entries++;
    const std::string spoken = e.value("spoken", std::string());
    SpellStack st;
    bool unknown = false;
    size_t pos = 0;
    while (pos <= spoken.size()) {
      size_t sp = spoken.find(' ', pos);
      if (sp == std::string::npos) sp = spoken.size();
      const std::string w = spoken.substr(pos, sp - pos);
      if (!w.empty()) {
        const int gi = lib.Find(w);
        if (gi < 0) unknown = true;
        else st.spoken.push_back(gi);
      }
      pos = sp + 1;
    }
    const SpellTree t = ParseSpell(lib, st);
    const std::string got = BracketSpell(lib, t, BracketStyle::Oracle);
    const std::string want = e.value("parse", std::string());
    bool ok = !unknown && got == want;
    // And the clause structure: delivery, weight, payload keys, mod keys.
    const auto& cls = e.value("clauses", nlohmann::json::array());
    if (ok && cls.size() != t.clauses.size()) ok = false;
    for (size_t ci = 0; ok && ci < cls.size(); ci++) {
      const SpellClause& cl = t.clauses[ci];
      const std::string dname = cl.delivery < 0 ? "hand" : lib.glyphs[cl.delivery].id;
      if (cls[ci].value("delivery", std::string()) != dname) ok = false;
      if (cls[ci].value("weight", 1) != cl.weight) ok = false;
      std::vector<std::string> payload, mods;
      for (int ni : cl.bag) {
        const std::string k = NodeKey(lib, t, ni) +
                              (t.nodes[ni].n > 1 ? "\xC3\x97" + std::to_string(t.nodes[ni].n) : "");
        const GlyphSort s = NodeSort(lib, t, ni);
        if (s == GlyphSort::Effect || s == GlyphSort::Matter) payload.push_back(k);
        else if (s == GlyphSort::Mod) mods.push_back(k);
      }
      std::vector<std::string> wp, wm;
      for (const auto& x : cls[ci].value("payload", nlohmann::json::array()))
        wp.push_back(x.get<std::string>());
      for (const auto& x : cls[ci].value("mods", nlohmann::json::array()))
        wm.push_back(x.get<std::string>());
      if (wp != payload || wm != mods) ok = false;
    }
    if (!ok) {
      if (mismatches < 10)
        std::printf("spells-oracle: \"%s\"\n   want %s\n   got  %s%s\n", spoken.c_str(),
                    want.c_str(), got.c_str(), unknown ? "  (unknown glyph)" : "");
      mismatches++;
    }
  }
  const bool ok = entries > 0 && mismatches == 0 && missing == 0;
  std::printf("spells-oracle: %s (%d entries, %d mismatches, %d glyphs missing)\n",
              ok ? "PASS" : "FAIL", entries, mismatches, missing);
  detail = Format("%d entries, %d mismatches", entries, mismatches);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SpellGates() {
  static const std::vector<Gate> g = {
      // No deps. The VM gate anchors to world.WindowOrigin(), which exists
      // whether or not the streaming gate passed; it was declared to depend on
      // `streaming` at the original gate split with no reason recorded, and
      // when streaming went red (2026-09-04, tests/BASELINE.md) this gate was
      // SKIPPED in every scope for a day -- which is how "explosive
      // projectile does nothing" shipped with the gate nominally green.
      {"spells", "spell", {}, false, GateSpells},
      // No deps: CPU-only over the glyph library and the oracle file.
      {"spells-oracle", "spell", {}, false, GateSpellsOracle},
  };
  return g;
}

}  // namespace selftest
