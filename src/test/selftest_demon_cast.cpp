// selftest_demon_cast.cpp — DEMONS THAT CAST (docs/PLAN_demons.md D4;
// game/demon_cast.h).
//
//   demon-cast  A. THE KIT (CPU): every assets/demons/spells entry loads, its
//                  words are all real glyphs, it compiles to a priced cast, it
//                  carries footprint tags; the imp's and the fiend's profiles
//                  name only kit entries; `cast` is an intent; the fiend's
//                  biggest single blast is under demonCast.maxBlastPower (no
//                  instant kill).
//               B. THE AI CASTS FROM INSIDE (the real tick, Skerrick contained
//                  in a salt ring, quicksilver severing his blink so he stays
//                  in, you standing outside the ring): his brain issues cast
//                  requests on its own cadence and at least one spell leaves
//                  him while he is contained. Run twice: identical event trace.
//               C. A FIREBOLT LANDS (a loose imp, a stone post where you would
//                  stand): fire is emitted at the post.
//               D. CAST_OUT AT THE RING (a contained imp, the post outside):
//                  with no sulfur the bolt crosses and fire lands outside;
//                  with D3's sulfur pile in the band the bolt is refused AT
//                  THE RING (demon::AllowCastOut) and nothing lands outside.
//               E. BLINK: a loose imp hops toward a far target onto the floor;
//                  a contained one with quicksilver severing `blink` is
//                  refused (demon::AllowBlink); unsevered, a hop inside the
//                  circle leaves it contained and a hop OUT of it unbinds it
//                  (the loophole).
//               F. LIFT ON ANOTHER BODY: `lift` from one imp at another raises
//                  it, the status is released after the kit's releaseAfterTicks,
//                  and it comes back down.
//               G. BOUNDED: a burst of requests in one tick leaves at most
//                  `maxLive` carriers in the air; the creature magic never
//                  pushes more than kOpsPerTick brush ops a tick.
//               H. THE SMALL BODY STANDS (the D1 float): on a flat pad and with
//                  a one-voxel step under its footprint, the imp's feet touch
//                  the ground under them (within demonCast.footClearMax); the
//                  human beside it for comparison.
//
// Ticks THE tick (support::TickRig -> TickAuthority): requests are issued in
// phase H's mobs.PreTick and served by MobCastTick right after it. Every
// fixture is its own stone pad on the harness map, a pinned clear sky, an
// IdCounterScope, and the world regenerated on the way out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/ai_behavior.h"
#include "game/demon.h"
#include "game/demon_cast.h"
#include "game/demon_seals.h"
#include "game/mob.h"
#include "game/session.h"
#include "game/spell.h"
#include "net/mobsync.h"
#include "sim/weather.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t Mat(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

void Put(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}

void Regen(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// ---- the fixture: a stone pad (optionally with a step), a salt ring, a post --
struct Fix {
  int x = 0, y = 0, z = 0, r = 0;
  IVec3 chunk{};
  std::vector<CellOp> pad, ring, post;
  int stepX = INT32_MAX;   // columns x >= stepX stand one higher
};

Fix Build(Ctx& c, int half, int ringR, int postDx, int stepDx = INT32_MAX) {
  Fix f;
  f.r = ringR;
  const uint32_t mStone = Mat(c, "stone"), mSalt = Mat(c, "salt");
  const IVec3 o = c.world.WindowOrigin();
  f.x = o.x * (int)kChunk + 200;
  f.z = o.z * (int)kChunk + 200;
  f.y = FixtureYOver(f.x - half - 2, f.z - half - 2, f.x + half + 2, f.z + half + 2, kDefaultSeed,
                     3);
  if (stepDx != INT32_MAX) f.stepX = f.x + stepDx;
  for (int zz = f.z - half; zz <= f.z + half; zz++)
    for (int xx = f.x - half; xx <= f.x + half; xx++) {
      const int top = xx >= f.stepX ? f.y + 1 : f.y;
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < top; yy++)
        Put(f.pad, xx, yy, zz, PackVoxNew(mStone, 0));
      for (int yy = top; yy <= f.y + 18; yy++) Put(f.pad, xx, yy, zz, 0u);
    }
  if (ringR > 0)
    for (int dz = -ringR - 2; dz <= ringR + 2; dz++)
      for (int dx = -ringR - 2; dx <= ringR + 2; dx++) {
        const int d = (int)std::floor(std::sqrt((double)(dx * dx + dz * dz)));
        if (d == ringR || d == ringR + 1) Put(f.ring, f.x + dx, f.y, f.z + dz, PackVoxNew(mSalt, 0));
      }
  // A STONE POST where a target stands: what a bolt aimed at "you" meets.
  if (postDx != 0)
    for (int yy = f.y; yy < f.y + 16; yy++)
      for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) Put(f.post, f.x + postDx + dx, yy, f.z + dz, PackVoxNew(mStone, 0));
  f.chunk = {f.x >> 4, f.y >> 4, f.z >> 4};
  return f;
}

void Lay(support::TickRig& rig, const Fix& f) {
  auto one = [&](const std::vector<CellOp>& cells) {
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = cells; });
  };
  for (size_t i = 0; i < f.pad.size(); i += kMaxCellOpsPerTick / 2)
    one(std::vector<CellOp>(f.pad.begin() + (ptrdiff_t)i,
                            f.pad.begin() + (ptrdiff_t)std::min(f.pad.size(),
                                                                i + kMaxCellOpsPerTick / 2)));
  if (!f.ring.empty()) one(f.ring);
  if (!f.post.empty()) one(f.post);
}

// A pile of `mat` in the band just outside the ring on -z (sulfur) or inlaid
// in the floor on -x (quicksilver, a liquid in a pit does not run).
std::vector<CellOp> Pile(Ctx& c, const Fix& f, const char* mat, bool inlay, uint32_t state) {
  std::vector<CellOp> ops;
  const uint32_t m = Mat(c, mat);
  const int pr = f.r + 5;
  for (int a = -1; a <= 1; a++)
    for (int b = -1; b <= 1; b++) {
      if (inlay) Put(ops, f.x - pr + b, f.y - 1, f.z + a, PackVoxNew(m, state));
      else Put(ops, f.x + a, f.y, f.z - pr + b, PackVoxNew(m, state));
    }
  if (!inlay) Put(ops, f.x, f.y + 1, f.z - pr, PackVoxNew(m, state));
  return ops;
}
std::vector<CellOp> Unpile(const std::vector<CellOp>& pile, Ctx& c, bool inlay) {
  std::vector<CellOp> ops = pile;
  for (CellOp& o : ops) o.word = inlay ? PackVoxNew(Mat(c, "stone"), 0) : 0u;
  return ops;
}

SessionTick St(support::TickRig& rig) { return SessionTick{&rig.Session(), {}, {}}; }

// One scripted cast through the real path: served now, its immediate emission
// injected into the next tick (as MobCastTick's would have been). Each starts
// from a FULL pool and no blink cooldown: every scripted step tests one claim,
// not what the step before it spent.
MobCastOutcome Serve(support::TickRig& rig, uint64_t mobId, Vec3 target, const char* spell,
                     int32_t pool = 0) {
  if (rig.Authority().mobCast)
    if (MobCaster* mc = rig.Authority().mobCast->Find(mobId)) {
      if (pool > 0) mc->mana.manaMax = pool;   // a greater demon's pool, for the kit sweep
      mc->mana.mana = mc->mana.manaMax;
      mc->blinkReadyAt = 0;
    }
  ai::CastRequest req;
  req.mobId = mobId;
  req.targetPoint = target;
  req.tick = rig.tick + 1;
  Vec3 lo{}, hi{};
  rig.Engine().mobs.MobBodyBox(mobId, lo, hi);
  const Vec3 c = (lo + hi) * 0.5f;
  req.distance = std::hypot(target.x - c.x, target.z - c.z);
  SessionTick st = St(rig);
  OpBatch ob;
  const MobCastOutcome o = MobCastServe(rig.Authority(), std::span<SessionTick>(&st, 1), req,
                                        rig.tick + 1, spell, ob);
  support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& t) {
    t.ops = ob.ops;
    t.spawns = ob.spawns;
  });
  return o;
}

// Fire this tick, within `pred`: brush ops and sprayed particles (the two
// streams a spell's matter leaves by).
template <typename P>
int FireWhere(const Ctx& c, const OpBatch& b, P pred) {
  const uint32_t mFire = Mat(c, "fire");
  int n = 0;
  for (const BrushOp& o : b.ops)
    if ((o.material & 0xFFFu) == mFire && pred((float)o.x + 0.5f, (float)o.z + 0.5f)) n++;
  for (const ParticleSpawn& p : b.spawns)
    if ((p.payload & 0xFFFu) == mFire && pred(SpellFxToFloat(p.px), SpellFxToFloat(p.pz))) n++;
  return n;
}

uint64_t Summon(support::TickRig& rig, const GlyphLibrary& lib, int gSummon, Vec3 at) {
  EffectInst e;
  e.verb = SpellVerb::Summon;
  e.glyph = gSummon;
  SpellEmission em;
  ApplySpellEffect(lib, {e}, SpellFxVec{SpellFxFromFloat(at.x), SpellFxFromFloat(at.y),
                                        SpellFxFromFloat(at.z)},
                   SpellFxVec{0, -kSpellFxOne, 0}, 1000, em);
  if (em.summons.size() != 1) return 0;
  SessionTick st = St(rig);
  DemonQueueSummon(rig.Authority(), std::span<SessionTick>(&st, 1), rig.Session().index,
                   em.summons[0], lib, rig.tick);
  DemonWorld& dw = Demons(rig.Authority());
  const size_t before = dw.live.size();
  for (int i = 0; i < 40 && dw.live.size() == before; i++) support::RunTicks(rig, 1);
  return dw.live.size() > before ? dw.live.back().mobId : 0;
}

uint64_t SpawnAt(Ctx& c, const std::string& def, float cx, int y, float cz) {
  const int d = c.mobs.FindDef(def);
  if (d < 0) return 0;
  const Vec3 ws = c.mobs.Defs()[(size_t)d].worldSize;
  return c.mobs.Spawn(d, {(int)std::floor(cx - ws.x * 0.5f), y, (int)std::floor(cz - ws.z * 0.5f)});
}

Vec3 Centre(const Ctx& c, uint64_t id) {
  Vec3 lo{}, hi{};
  c.mobs.MobBodyBox(id, lo, hi);
  return (lo + hi) * 0.5f;
}

// The lowest voxel of each FOOT limb minus the floor under it: ~0 = standing.
// `floorAt(x)` is the fixture's floor top for column x.
template <typename F>
void FootClear(const Ctx& c, uint64_t id, F floorAt, float& worst, float& lowest) {
  const Mob* m = c.mobs.FindMobById(id);
  worst = -99.0f;
  lowest = 99.0f;
  if (!m || !m->Def()) return;
  for (size_t i = 0; i < m->Def()->limbs.size(); i++) {
    if (m->Def()->limbs[i].name.rfind("foot", 0) != 0 || !c.mobs.LimbBody(id, (int)i)) continue;
    float lo = 1e9f;
    Vec3 at{};
    const uint32_t n = c.mobs.LimbVoxelCount(id, (int)i);
    for (uint32_t v = 0; v < n; v++) {
      const Vec3 p = c.mobs.LimbVoxelPos(id, (int)i, v);
      if (p.y < lo) {
        lo = p.y;
        at = p;
      }
    }
    if (lo > 1e8f) continue;
    // LimbVoxelPos names the CENTRE of a collider voxel; the sole is half of
    // one below it.
    lo -= 0.5f / (float)std::max(1u, m->Def()->physScale);
    const float clr = lo - (float)floorAt((int)std::floor(at.x));
    worst = std::max(worst, clr);
    lowest = std::min(lowest, clr);
  }
}

// ---- B. the AI, contained, casting at you -----------------------------------------
struct AiOut {
  bool spawned = false, contained = false;
  int requests = 0, castsContained = 0, casts = 0;
  std::vector<std::string> trace;
  std::string why;
};

AiOut RunAi(Ctx& c, const GlyphLibrary& lib, int gSummon, uint32_t t0, uint64_t idBase,
            int ticks) {
  AiOut out;
  Regen(c);
  c.mobs.SetNextIdCounter(idBase);
  Fix f = Build(c, 34, 14, 0);
  support::TickRig rig(c, t0, f.chunk);
  rig.Glyphs() = lib;
  Lay(rig, f);
  // Quicksilver severs his blink (the imp profile names none today; this
  // keeps the claim about his CASTING if it ever does).
  const std::vector<CellOp> qs = Pile(c, f, "quicksilver", true, 7);
  support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = qs; });
  support::RunTicks(rig, 30);
  // YOU: outside the ring on +x, in his cast range, out of his reach.
  const Vec3 you{(float)(f.x + f.r + 8) + 0.5f, (float)f.y + 8.5f, (float)f.z + 0.5f};
  c.mobs.SetPlayerActor(you, 3.0f, 17.0f, true);
  const uint64_t id = Summon(rig, lib, gSummon, Vec3{f.x + 0.5f, f.y + 1.5f, f.z + 0.5f});
  if (id == 0) {
    out.why = "nothing arrived";
    return out;
  }
  out.spawned = true;
  DemonWorld& dw = Demons(rig.Authority());
  out.contained = dw.Find(id) && dw.Find(id)->state == DemonState::Contained;
  size_t seen = 0;
  for (int i = 0; i < ticks; i++) {
    support::RunTicks(rig, 1);
    const LiveDemon* ld = dw.Find(id);
    if (const ai::Brain* b = c.mobs.MobBrain(id)) out.requests = (int)b->castsIssued;
    if (rig.Authority().mobCast) {
      const MobCastWorld& m = *rig.Authority().mobCast;
      for (; seen < m.events.size(); seen++) {
        const CastEvent& e = m.events[seen];
        if (e.mobId != id) continue;
        out.trace.push_back(Format("%u %s %s", e.tick, e.spell.c_str(), CastOutcomeName(e.outcome)));
        if (e.outcome == MobCastOutcome::Cast) {
          out.casts++;
          if (ld && ld->state == DemonState::Contained) out.castsContained++;
        }
      }
    }
    const Vec3 ce = Centre(c, id);
    out.trace.push_back(Format("%d %d %d", (int)std::lround(ce.x * 16), (int)std::lround(ce.z * 16),
                               ld ? (int)ld->state : -1));
  }
  return out;
}

}  // namespace

Status GateDemonCast(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("demon-cast: FAILED %s\n", what.c_str());
    }
  };
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "glyph load failed: " + gerr;
    return Status::Fail;
  }
  const int gSummon = lib.Find("summon_skerrick");

  // ---- A. the kit ---------------------------------------------------------------------
  CreatureKit kit;
  std::string klog;
  LoadCreatureKit(AssetDir() + "/demons/spells", kit, klog);
  check(klog.empty(), "A: the kit loads clean (" + klog + ")");
  int maxBlast = 0;
  std::string maxBlastBy;
  for (const KitSpell& s : kit.spells) {
    check(!s.tags.targets.empty(), "A: " + s.name + " has footprint tags");
    if (s.blink) continue;
    SpellStack st;
    std::string unknown;
    {
      size_t i = 0;
      const std::string& w = s.words;
      while (i < w.size()) {
        while (i < w.size() && w[i] == ' ') i++;
        size_t j = i;
        while (j < w.size() && w[j] != ' ') j++;
        if (j > i) {
          std::string id;
          int32_t mag = kMagUnset;
          SplitMagnitude(w.substr(i, j - i), id, mag);
          const int g = lib.Find(id);
          if (g >= 0) st.Push(g, mag);
          else unknown += " " + id;
        }
        i = j;
      }
    }
    check(unknown.empty(), "A: " + s.name + " names glyphs that do not exist:" + unknown);
    const CastList cl = CompileSpell(lib, st);
    check(!cl.Empty() && cl.manaCost > 0, "A: " + s.name + " compiles to a priced cast");
    std::printf("demon-cast: kit %-10s \"%s\" aim %s range %.0f..%.0f cost %d (word %d tariff %d "
                "carry %d) tags %s%s\n",
                s.name.c_str(), s.words.c_str(), KitAimName(s.aim), s.rangeMin, s.rangeMax,
                cl.manaCost, cl.wordCost, cl.tariff, cl.carryCost, s.tags.targets.c_str(),
                s.tags.direct ? " direct" : "");
    for (const SpellCast& sc : cl.casts) {
      std::vector<const EffectInst*> todo;
      for (const EffectInst& e : sc.payload) todo.push_back(&e);
      while (!todo.empty()) {
        const EffectInst* e = todo.back();
        todo.pop_back();
        if (e->verb == SpellVerb::Explode && e->glyph >= 0) {
          const int p = (int)((int64_t)lib.glyphs[(size_t)e->glyph].power * e->Scale() / 1000);
          if (p > maxBlast) {
            maxBlast = p;
            maxBlastBy = s.name;
          }
        }
        for (const EffectInst& in : e->inner) todo.push_back(&in);
      }
    }
  }
  const ai::Library& beh = c.mobs.Behaviors();
  for (const char* prof : {"imp", "fiend"}) {
    const ai::Profile* p = beh.At(beh.Find(prof));
    check(p != nullptr && !p->cast.spells.empty(), Format("A: profile %s casts", prof));
    if (p)
      for (const std::string& s : p->cast.spells)
        check(kit.Find(s) != nullptr, Format("A: %s's spell %s is in the kit", prof, s.c_str()));
  }
  check(ai::IntentFromName("cast") == ai::Intent::Cast, "A: `cast` is an intent");
  const int blastCap = (int)BaselineNumber("demonCast.maxBlastPower", 400);
  check(maxBlast > 0 && maxBlast <= blastCap,
        Format("A: the kit's biggest blast is %d (%s), cap %d", maxBlast, maxBlastBy.c_str(),
               blastCap));
  if (!fails.empty() || gSummon < 0) {
    detail = fails.empty() ? "no summon_skerrick glyph" : fails;
    return Status::Fail;
  }

  const std::string weatherWas = weather::Override();
  weather::SetOverride("clear");
  const uint64_t ids0 = c.mobs.NextIdCounter();

  // ---- B. the AI casts from inside, twice -----------------------------------------------
  const int aiTicks = (int)BaselineNumber("demonCast.aiTicks", 420);
  const AiOut b1 = RunAi(c, lib, gSummon, 95000u, ids0, aiTicks);
  const AiOut b2 = RunAi(c, lib, gSummon, 95000u, ids0, aiTicks);
  check(b1.spawned && b1.contained, "B: Skerrick arrived contained (" + b1.why + ")");
  check(b1.requests > 0, Format("B: his brain issued %d cast requests", b1.requests));
  check(b1.castsContained > 0,
        Format("B: %d spells left him while contained (%d in all)", b1.castsContained, b1.casts));
  check(!b1.trace.empty() && b1.trace == b2.trace,
        Format("B: the twice-run trace differs (%zu vs %zu rows)", b1.trace.size(),
               b2.trace.size()));

  // ---- C, E1, F, G, H-flat: the open pad ------------------------------------------------
  float flatWorst = 99, flatLowest = 99, humWorst = 99, humLowest = 99;
  int fireAtPost = 0, liftHeld = 0;
  float liftRise = 0, liftBack = 99;
  int burstCast = 0, burstRefused = 0, maxLive = 0;
  uint64_t released = 0;
  MobCastOutcome blinkOpen = MobCastOutcome::NoRoom;
  float blinkMoved = 0, blinkFloor = 99;
  int sweepCast = 0, lavaOps = 0, wardHeld = 0;
  std::string sweepFailed;
  {
    Regen(c);
    c.mobs.SetNextIdCounter(ids0);
    Fix f = Build(c, 40, 0, 20);
    support::TickRig rig(c, 96000u, f.chunk);
    rig.Glyphs() = lib;
    Lay(rig, f);
    support::RunTicks(rig, 10);
    // Nobody to fight: the casts below are the gate's.
    c.mobs.SetPlayerActor(Vec3{(float)f.x + 400.0f, (float)f.y, (float)f.z}, 3.0f, 17.0f, true);
    // Two imps (one faction: neither wants the other) and nobody else.
    const uint64_t imp = SpawnAt(c, "imp_skerrick", f.x + 0.5f, f.y, f.z + 0.5f);
    const uint64_t imp2 = SpawnAt(c, "imp_skerrick", f.x - 15.5f, f.y, f.z + 0.5f);
    support::RunTicks(rig, 30);
    // C. a firebolt at the post (+20 x).
    const Vec3 post{(float)f.x + 20.5f, (float)f.y + 5.0f, (float)f.z + 0.5f};
    const MobCastOutcome oc = Serve(rig, imp, post, "firebolt");
    check(oc == MobCastOutcome::Cast, Format("C: the firebolt was cast (%s)", CastOutcomeName(oc)));
    for (int i = 0; i < 20; i++) {
      support::RunTicks(rig, 1);
      fireAtPost += FireWhere(c, rig.LastBatch(), [&](float x, float z) {
        return std::abs(x - post.x) < 6.0f && std::abs(z - post.z) < 6.0f;
      });
    }
    // G. a burst: twelve requests in one tick, from a pool deep enough for all
    // twelve, so what refuses them is the in-flight cap and nothing else.
    if (rig.Authority().mobCast) maxLive = rig.Authority().mobCast->stats.maxLiveSeen;
    if (MobCaster* mc = rig.Authority().mobCast ? rig.Authority().mobCast->Find(imp) : nullptr)
      mc->mana.mana = mc->mana.manaMax = 100000;
    {
      SessionTick st = St(rig);
      OpBatch ob;
      for (int k = 0; k < 12; k++) {
        ai::CastRequest req;
        req.mobId = imp;
        req.targetPoint = post;
        req.distance = 20.0f;
        const MobCastOutcome o = MobCastServe(rig.Authority(), std::span<SessionTick>(&st, 1), req,
                                              rig.tick + 1, "firebolt", ob);
        (o == MobCastOutcome::Cast ? burstCast : burstRefused)++;
      }
      support::RunTicks(rig, 1);
      maxLive = std::max(maxLive, rig.Authority().mobCast->stats.maxLiveSeen);
    }
    support::RunTicks(rig, 30);
    // F. lift the other imp.
    const float y0 = c.mobs.MobOrigin(imp2).y;
    const MobCastOutcome ol = Serve(rig, imp, Centre(c, imp2), "lift");
    check(ol == MobCastOutcome::Cast, Format("F: the lift was cast (%s)", CastOutcomeName(ol)));
    for (int i = 0; i < 150; i++) {
      support::RunTicks(rig, 1);
      const float dy = c.mobs.MobOrigin(imp2).y - y0;
      liftRise = std::max(liftRise, dy);
      if (const MobCaster* mc = rig.Authority().mobCast->Find(imp))
        for (const SpellStatus& s : mc->spells.Statuses())
          if (s.target == imp2) liftHeld++;
    }
    liftBack = c.mobs.MobOrigin(imp2).y - y0;
    released = rig.Authority().mobCast->stats.released;
    // E1. a loose hop toward something far off.
    if (MobCaster* mc = rig.Authority().mobCast->Find(imp)) mc->blinkReadyAt = 0;
    const Vec3 before = Centre(c, imp);
    blinkOpen = Serve(rig, imp, Vec3{(float)f.x - 0.5f, (float)f.y + 5.0f, (float)f.z + 36.5f},
                      "blink");
    support::RunTicks(rig, 10);
    const Vec3 after = Centre(c, imp);
    blinkMoved = std::hypot(after.x - before.x, after.z - before.z);
    blinkFloor = c.mobs.MobOrigin(imp).y - (float)f.y;
    // A2. THE REST OF THE KIT leaves a caster through the same path (a
    // greater demon's pool): every entry casts, the feet-aimed transmute puts
    // lava at the post's foot, the ward holds a status on the caster itself.
    const uint32_t mLava = Mat(c, "lava");
    for (const char* sp : {"gust", "hellfire", "lava_floor", "ward"}) {
      const MobCastOutcome o = Serve(rig, imp, post, sp, 100000);
      sweepCast += o == MobCastOutcome::Cast;
      if (o != MobCastOutcome::Cast) sweepFailed += std::string(" ") + sp + "(" + CastOutcomeName(o) + ")";
      for (int i = 0; i < 15; i++) {
        support::RunTicks(rig, 1);
        for (const BrushOp& op : rig.LastBatch().ops)
          if ((op.material & 0xFFFu) == mLava && std::abs((float)op.x - post.x) < 8.0f &&
              std::abs((float)op.z - post.z) < 8.0f)
            lavaOps++;
      }
    }
    if (const MobCaster* mc = rig.Authority().mobCast->Find(imp))
      for (const SpellStatus& s : mc->spells.Statuses()) wardHeld += s.target == imp;
  }
  check(sweepCast == 4, "A2: the rest of the kit casts (failed:" + sweepFailed + ")");
  check(lavaOps > 0, Format("A2: lava_floor put lava at the post's foot (%d ops)", lavaOps));
  check(wardHeld > 0, Format("A2: the ward holds on its caster (%d statuses)", wardHeld));
  check(fireAtPost > 0, Format("C: fire landed at the post (%d fire ops/spawns)", fireAtPost));
  check(burstCast == 1 && maxLive <= 1,
        Format("G: a 12-request burst cast %d (refused %d), at most %d in the air (maxLive 1)",
               burstCast, burstRefused, maxLive));
  const float riseMin = (float)BaselineNumber("demonCast.liftRiseMin", 3.0);
  check(liftRise >= riseMin && liftHeld > 0,
        Format("F: the lifted imp rose %.1f voxels (min %.1f), held %d ticks", liftRise, riseMin,
               liftHeld));
  check(released > 0 && liftBack < 1.5f,
        Format("F: released (%llu) and back down (%.1f above where it stood)",
               (unsigned long long)released, liftBack));
  check(blinkOpen == MobCastOutcome::Blinked && blinkMoved >= 14.0f && std::abs(blinkFloor) < 1.5f,
        Format("E1: a loose blink %s, moved %.1f voxels, feet %.1f off the pad",
               CastOutcomeName(blinkOpen), blinkMoved, blinkFloor));

  // ---- D, E2..E4: the ring -------------------------------------------------------------
  int fireOutOpen = 0, fireOutSevered = 0;
  uint64_t ringRefused = 0;
  bool castOutSevered = false, blinkSevered = false;
  MobCastOutcome eSevered = MobCastOutcome::NoRoom, eInside = MobCastOutcome::NoRoom,
                 eOut = MobCastOutcome::NoRoom;
  DemonState afterInside = DemonState::Unbound, afterOut = DemonState::Contained;
  {
    Regen(c);
    c.mobs.SetNextIdCounter(ids0);
    Fix f = Build(c, 40, 14, 24);
    support::TickRig rig(c, 97000u, f.chunk);
    rig.Glyphs() = lib;
    Lay(rig, f);
    support::RunTicks(rig, 30);
    c.mobs.SetPlayerActor(Vec3{(float)f.x + 400.0f, (float)f.y, (float)f.z}, 3.0f, 17.0f, true);
    // Off-centre on -x, so a hop toward +x can land INSIDE the circle.
    const uint64_t imp = Summon(rig, lib, gSummon, Vec3{f.x - 9.5f, f.y + 1.5f, f.z + 0.5f});
    DemonWorld& dw = Demons(rig.Authority());
    check(imp != 0 && dw.Find(imp) && dw.Find(imp)->state == DemonState::Contained,
          "D: the ring fixture's imp arrived contained");
    if (imp != 0) {
      // D6: this arm measures D3/D4's ring hooks with scripted casts. D6's
      // malice would gust the plain ring open on its own (demon-malice's G),
      // so here Skerrick knows no schemes.
      for (DemonDef& d : dw.lib.defs)
        if (d.id == dw.Find(imp)->demon) d.schemes.clear();
      const LiveDemon ld0 = *dw.Find(imp);
      auto outside = [&](float x, float z) { return !ld0.circle.Inside(x, z); };
      const Vec3 post{(float)f.x + 24.5f, (float)f.y + 5.0f, (float)f.z + 0.5f};
      // D, OPEN: no sulfur -> the bolt crosses and lands outside.
      Serve(rig, imp, post, "firebolt");
      for (int i = 0; i < 20; i++) {
        support::RunTicks(rig, 1);
        fireOutOpen += FireWhere(c, rig.LastBatch(), outside);
      }
      // D, SEVERED: D3's sulfur in the band; the circle's re-read reads it.
      const std::vector<CellOp> sulfur = Pile(c, f, "sulfur", false, 0);
      support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = sulfur; });
      for (int i = 0; i < 40 && !demon::ChannelSevered(rig.Authority(), imp,
                                                         demon::Channel::CastOut);
           i++)
        support::RunTicks(rig, 1);
      castOutSevered = demon::ChannelSevered(rig.Authority(), imp, demon::Channel::CastOut);
      const uint64_t rr0 = rig.Authority().mobCast->stats.ringRefused;
      Serve(rig, imp, post, "firebolt");
      for (int i = 0; i < 20; i++) {
        support::RunTicks(rig, 1);
        fireOutSevered += FireWhere(c, rig.LastBatch(), outside);
      }
      ringRefused = rig.Authority().mobCast->stats.ringRefused - rr0;
      // E2: quicksilver severs `blink`.
      const std::vector<CellOp> qs = Pile(c, f, "quicksilver", true, 7);
      support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = qs; });
      for (int i = 0; i < 40 && !demon::ChannelSevered(rig.Authority(), imp, demon::Channel::Blink);
           i++)
        support::RunTicks(rig, 1);
      blinkSevered = demon::ChannelSevered(rig.Authority(), imp, demon::Channel::Blink);
      const Vec3 across{(float)f.x + 21.5f, (float)f.y + 5.0f, (float)f.z + 0.5f};
      if (MobCaster* mc = rig.Authority().mobCast->Find(imp)) mc->blinkReadyAt = 0;
      eSevered = Serve(rig, imp, across, "blink");
      // E3: the quicksilver taken up; a hop that lands INSIDE the circle.
      support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
        o.cells = Unpile(qs, c, true);
      });
      for (int i = 0; i < 40 && demon::ChannelSevered(rig.Authority(), imp, demon::Channel::Blink);
           i++)
        support::RunTicks(rig, 1);
      if (MobCaster* mc = rig.Authority().mobCast->Find(imp)) mc->blinkReadyAt = 0;
      eInside = Serve(rig, imp, across, "blink");
      support::RunTicks(rig, 2);
      afterInside = dw.Find(imp) ? dw.Find(imp)->state : DemonState::Unbound;
      // E4: a hop OUT: the loophole frees it.
      if (MobCaster* mc = rig.Authority().mobCast->Find(imp)) mc->blinkReadyAt = 0;
      // (Near enough that the landing, past the ring, is still in the CPU
    // mirror the hop reads its floor from.)
    const Vec3 far{(float)f.x + 27.5f, (float)f.y + 5.0f, (float)f.z + 0.5f};
      eOut = Serve(rig, imp, far, "blink");
      support::RunTicks(rig, 2);
      afterOut = dw.Find(imp) ? dw.Find(imp)->state : DemonState::Contained;
    }
  }
  check(fireOutOpen > 0,
        Format("D: with cast_out open the bolt crossed the ring (%d fire outside)", fireOutOpen));
  check(castOutSevered, "D: the sulfur pile severed cast_out");
  check(ringRefused > 0 && fireOutSevered == 0,
        Format("D: severed, the bolt was refused at the ring (%llu refused, %d fire outside)",
               (unsigned long long)ringRefused, fireOutSevered));
  check(blinkSevered && eSevered == MobCastOutcome::BlinkSevered,
        Format("E2: quicksilver severs the blink (%s)", CastOutcomeName(eSevered)));
  check(eInside == MobCastOutcome::Blinked && afterInside == DemonState::Contained,
        Format("E3: a hop inside the circle: %s, %s", CastOutcomeName(eInside),
               DemonStateName(afterInside)));
  check(eOut == MobCastOutcome::Blinked && afterOut == DemonState::Unbound,
        Format("E4: a hop out of the circle: %s, %s (the loophole)", CastOutcomeName(eOut),
               DemonStateName(afterOut)));

  // ---- H, the step ----------------------------------------------------------------------
  float stepWorst = 99, stepLowest = 99, hStepWorst = 99, hStepLowest = 99;
  {
    Regen(c);
    c.mobs.SetNextIdCounter(ids0);
    // Columns x >= f.x + 1 stand one higher: the step runs under the +x edge
    // of a body centred on f.x, while its feet stand on the lower floor.
    Fix f = Build(c, 34, 0, 0, 1);
    support::TickRig rig(c, 98000u, f.chunk);
    rig.Glyphs() = lib;
    Lay(rig, f);
    support::RunTicks(rig, 10);
    c.mobs.SetPlayerActor(Vec3{(float)f.x + 400.0f, (float)f.y, (float)f.z}, 3.0f, 17.0f, true);
    // STATUES (the `dummy` profile: blind, immobile), so what is measured is
    // the stance and nothing the brain did. On the flat (x << step) and with
    // the step under the +x edge of the footprint while the feet stand below.
    const uint64_t impFlat = SpawnAt(c, "imp_skerrick", f.x - 12.5f, f.y, f.z + 0.5f);
    const uint64_t imp = SpawnAt(c, "imp_skerrick", f.x + 0.2f, f.y, f.z + 0.5f);
    const uint64_t humFlat = SpawnAt(c, "human", f.x - 14.5f, f.y, f.z - 14.5f);
    const uint64_t hum = SpawnAt(c, "human", f.x - 0.5f, f.y, f.z - 14.5f);
    for (uint64_t id : {impFlat, imp, humFlat, hum}) c.mobs.SetMobBehavior(id, "dummy");
    support::RunTicks(rig, 60);
    auto step = [&](int x) { return x >= f.stepX ? f.y + 1 : f.y; };
    FootClear(c, impFlat, step, flatWorst, flatLowest);
    FootClear(c, humFlat, step, humWorst, humLowest);
    FootClear(c, imp, step, stepWorst, stepLowest);
    FootClear(c, hum, step, hStepWorst, hStepLowest);
    std::printf("demon-cast: H flat: imp feet %.2f..%.2f, human feet %.2f..%.2f voxels\n",
                flatLowest, flatWorst, humLowest, humWorst);
    std::printf("demon-cast: H step: imp feet %.2f..%.2f (origin %.2f, pad %d), human feet "
                "%.2f..%.2f (origin %.2f) voxels\n",
                stepLowest, stepWorst, c.mobs.MobOrigin(imp).y, f.y, hStepLowest, hStepWorst,
                c.mobs.MobOrigin(hum).y);
  }
  weather::SetOverride(weatherWas);
  Regen(c);
  const float clearMax = (float)BaselineNumber("demonCast.footClearMax", 0.6);
  check(flatWorst <= clearMax && flatLowest >= -clearMax * 2.0f,
        Format("H: on a flat pad the imp's feet are %.2f..%.2f off the floor (max %.2f)",
               flatLowest, flatWorst, clearMax));
  check(stepWorst <= clearMax && stepLowest >= -clearMax * 2.0f,
        Format("H: over a one-voxel step the imp's feet are %.2f..%.2f off the floor under "
               "them (max %.2f)",
               stepLowest, stepWorst, clearMax));

  RecordObserved("demonCast.impFootFlat", flatWorst);
  RecordObserved("demonCast.impFootStep", stepWorst);
  RecordObserved("demonCast.humanFootFlat", humWorst);
  RecordObserved("demonCast.humanFootStep", hStepWorst);
  RecordObserved("demonCast.liftRise", liftRise);
  RecordObserved("demonCast.aiCasts", b1.casts);
  detail = Format(
      "A kit %zu spells, biggest blast %d (%s); the rest of the kit %d/4 cast, %d lava ops, ward "
      "%d. B contained AI: %d requests, %d cast (%d while "
      "contained), trace %zu rows %s. C firebolt: %d fire at the post. D cast_out open: %d fire "
      "outside; severed: %llu refused at the ring, %d outside. E blink loose %s %.1f; severed %s; "
      "inside %s/%s; out %s/%s. F lift +%.1f, released %llu, back %.1f. G burst %d cast / %d "
      "refused, max live %d. H imp feet flat %.2f..%.2f, step %.2f..%.2f (human %.2f..%.2f / "
      "%.2f..%.2f)",
      kit.spells.size(), maxBlast, maxBlastBy.c_str(), sweepCast, lavaOps, wardHeld, b1.requests, b1.casts, b1.castsContained,
      b1.trace.size(), b1.trace == b2.trace ? "identical twice" : "DIFFERS", fireAtPost,
      fireOutOpen, (unsigned long long)ringRefused, fireOutSevered, CastOutcomeName(blinkOpen),
      blinkMoved, CastOutcomeName(eSevered), CastOutcomeName(eInside), DemonStateName(afterInside),
      CastOutcomeName(eOut), DemonStateName(afterOut), liftRise, (unsigned long long)released,
      liftBack, burstCast, burstRefused, maxLive, flatLowest, flatWorst, stepLowest, stepWorst,
      humLowest, humWorst, hStepLowest, hStepWorst);
  if (!fails.empty()) detail += "; FAILED: " + fails;
  std::printf("demon-cast: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ---- npc-hand-cast: a creature casts FROM ITS HAND, the player's way ---------------------
//
// game/demon_cast.h MobCastBegin + game/spellhand.h, through THE tick (TickRig).
// Two `battlemage` humans on an open pad, the player far out of their sight
// so their own AI never acts:
//   A  a sword in the RIGHT hand, the left empty: the firebolt goes into the
//      empty LEFT hand (no stow), the sword never leaves the right; the cast
//      leaves exactly raise + the flick's release ticks after the request,
//      from the fist (the event's origin within a few voxels of the hand), the
//      arm armed (`spell_ready`) and moving through the gesture, the body
//      CastBusy for the whole of it and free after.
//   B  a sword in EACH hand: the off (left) sword is PUT AWAY into the sheath
//      for the cast and DRAWN BACK into the left hand after it; the sheath is
//      empty again and the right sword never moved.
//   C  the arm reaches what the gesture asks (WeaponArmDiagnostics ikMiss at
//      the release, both bodies).
//   D  a request while one is mid-gesture is Busy, not a second cast.
Status GateNpcHandCast(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("npc-hand-cast: FAILED %s\n", what.c_str());
    }
  };
  auto regen = [&] {
    c.mobs.Reset();
    c.debris.Reset();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  };
  regen();
  const IVec3 o = c.world.WindowOrigin();
  const int fxX = o.x * (int)kChunk + 200, fxZ = o.z * (int)kChunk + 200;
  const int fy = FixtureYOver(fxX - 64, fxZ - 8, fxX + 64, fxZ + 8, kDefaultSeed, 0);
  uint32_t t0 = 97000;
  support::TickRig rig(c, t0, IVec3{fxX >> 4, fy >> 4, fxZ >> 4});
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, rig.Glyphs(), gerr)) {
    detail = "glyph load failed: " + gerr;
    std::printf("npc-hand-cast: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  PlayerSession& ses = rig.Session();
  ses.spells.SetLibrary(&rig.Glyphs());
  // Far outside a battlemage's 50-voxel sight: their AI has nobody to fight.
  ses.player.pos = Vec3{(float)fxX + 0.5f, (float)fy + 40.0f, (float)fxZ + 260.5f};
  const int hd = c.mobs.FindDef("human");
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  if (hd < 0 || sword == nullptr) {
    detail = "no human def or no sword item";
    std::printf("npc-hand-cast: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  auto spawn = [&](int dx, bool twoSwords) -> uint64_t {
    const uint64_t id = c.mobs.Spawn(hd, IVec3{fxX + dx, fy, fxZ});
    if (id == 0) return 0;
    c.mobs.SetMobBehavior(id, "battlemage");
    c.mobs.EquipItem(id, sword, "held_right");
    if (twoSwords) c.mobs.EquipItem(id, sword, "held_left");
    return id;
  };
  // 120 voxels apart: each is far outside the other's 50-voxel sight, so
  // neither one's own AI fights or casts at the other mid-gate.
  const uint64_t a = spawn(-60, false), b = spawn(60, true);
  check(a != 0 && b != 0, "two battlemages spawn");
  if (a == 0 || b == 0) {
    regen();
    detail = fails;
    return Status::Fail;
  }
  support::RunTicks(rig, 20);   // settle onto the pad
  const SpellGestureLib& gl = rig.Glyphs().gestures;
  const SpellGesture* flick = gl.At(gl.Find("flick"));
  check(flick != nullptr, "the bolt's gesture (flick) is loaded");
  const int expectAfter = MobHandCast::kRaiseTicks + (flick ? flick->releaseTicks : 0);

  struct Run {
    MobCastOutcome begun = MobCastOutcome::NothingInRange, second = MobCastOutcome::NothingInRange;
    int hand = -1;
    int castAfter = -1;          // ticks from the request to the cast event
    float fromToHand = 99.0f;    // the event's origin to the fist at that tick
    float handTravel = 0.0f;     // how far the fist moved during the gesture
    float ikMiss = 99.0f;        // at the release
    bool busyThroughout = true, armed = false, busyAfter = true;
    bool rightKept = true;       // the right hand's sword never left
    bool leftEmptyDuring = false, sheathedDuring = false;
    // The light in the fist (Mob::SpellLight): lit while the spell is in it,
    // carried through a MobPose encode/decode intact, out after.
    uint32_t light = 0, lightWire = 0, lightAfter = 1;
    std::string leftAfter, sheathAfter;
    uint32_t stows = 0, draws = 0;
  };
  auto runOne = [&](uint64_t id) {
    Run r;
    Vec3 lo{}, hi{};
    c.mobs.MobBodyBox(id, lo, hi);
    const Vec3 ctr = (lo + hi) * 0.5f;
    ai::CastRequest req;
    req.mobId = id;
    req.targetPoint = ctr + Vec3{0.0f, 0.0f, -25.0f};
    req.distance = 25.0f;
    SessionTick st{&ses, {}, {}};
    const uint32_t reqTick = rig.tick + 1;
    OpBatch ob;
    req.tick = reqTick;
    r.begun = MobCastBegin(rig.Authority(), std::span<SessionTick>(&st, 1), req, reqTick,
                           "firebolt", ob);
    {
      OpBatch ob2;
      r.second = MobCastBegin(rig.Authority(), std::span<SessionTick>(&st, 1), req, reqTick,
                              "firebolt", ob2);
    }
    const MobHandCast* hc = MobHandCastOf(rig.Authority(), id);
    if (hc) r.hand = hc->hand;
    const size_t ev0 = rig.Authority().mobCast ? rig.Authority().mobCast->events.size() : 0;
    Vec3 handStart{};
    uint32_t lightWire = 0;
    bool haveStart = false;
    const int total = expectAfter + 60;
    for (int i = 1; i <= total; i++) {
      support::RunTicks(rig, 1);
      Mob* m = c.mobs.FindMobById(id);
      if (m == nullptr) break;
      hc = MobHandCastOf(rig.Authority(), id);
      const bool active = hc && hc->phase != MobHandCast::Phase::Idle;
      if (m->HeldItem(Hand::Right) != "sword") r.rightKept = false;
      if (active) {
        if (!m->CastBusy()) r.busyThroughout = false;
        const Hand h = HandAt(hc->hand);
        if (m->ClipWeight(m->HandClip("spell_ready", h)) > 0.05f) r.armed = true;
        if (hc->phase == MobHandCast::Phase::Gesture && r.light == 0) {
          r.light = m->SpellLight(h);
          ::net::MobPose pose;
          if (c.mobs.BuildPose(id, rig.tick, pose)) {
            std::vector<uint8_t> bytes;
            ByteWriter bw{bytes};
            ::net::Encode(bw, pose);
            ::net::MobPose back;
            ByteReader br{bytes.data(), bytes.size()};
            if (::net::Decode(br, back)) lightWire = back.spellLight[hc->hand];
          }
        }
        if (hc->hand == 1 && m->HeldItem(Hand::Left).empty()) r.leftEmptyDuring = true;
        if (m->GetKit().equip.slots[(int)EquipSlotId::Sheath].name == "sword")
          r.sheathedDuring = true;
        Vec3 hp;
        if (hc->phase == MobHandCast::Phase::Gesture &&
            SpellHandWorld(c.mobs, c.phys, id, h, hp)) {
          if (!haveStart) handStart = hp, haveStart = true;
          r.handTravel = std::max(r.handTravel, (hp - handStart).len());
        }
      }
      // The cast event, the tick it lands.
      if (r.castAfter < 0 && rig.Authority().mobCast) {
        const auto& evs = rig.Authority().mobCast->events;
        for (size_t k = 0; k < evs.size(); k++) {
          const CastEvent& e = evs[k];
          if (e.mobId != id || e.outcome != MobCastOutcome::Cast || e.tick < reqTick) continue;
          r.castAfter = (int)(e.tick - reqTick);
          Vec3 hp;
          if (hc && SpellHandWorld(c.mobs, c.phys, id, HandAt(hc->hand), hp))
            r.fromToHand = (e.from - hp).len();
          // Only a solve that RAN is a measurement (99 = the arm was never posed).
          if (m->WeaponArmDiagnostics().ran) r.ikMiss = m->WeaponArmDiagnostics().ikMiss;
        }
      }
      (void)ev0;
    }
    Mob* m = c.mobs.FindMobById(id);
    hc = MobHandCastOf(rig.Authority(), id);
    r.lightWire = lightWire;
    if (m) {
      r.lightAfter = m->SpellLight(Hand::Left) | m->SpellLight(Hand::Right);
      r.busyAfter = m->CastBusy();
      r.leftAfter = m->HeldItem(Hand::Left);
      r.sheathAfter = m->GetKit().equip.slots[(int)EquipSlotId::Sheath].name;
    }
    if (hc) r.stows = hc->stows, r.draws = hc->draws;
    if (hc && hc->phase != MobHandCast::Phase::Idle) r.busyAfter = true;
    return r;
  };
  const Run ra = runOne(a);
  const Run rb = runOne(b);
  regen();

  const float fromTol = (float)BaselineNumber("npcHandCast.fromToHandMax", 6.0);
  const float ikTol = (float)BaselineNumber("npcHandCast.ikMissMax", 2.0);
  const float travelMin = (float)BaselineNumber("npcHandCast.handTravelMin", 2.0);
  // A
  check(ra.begun == MobCastOutcome::Gesturing,
        Format("A: the request puts the spell in a hand (%s)", CastOutcomeName(ra.begun)));
  check(ra.second == MobCastOutcome::Busy,
        Format("D: a second request mid-cast is busy (%s)", CastOutcomeName(ra.second)));
  check(ra.hand == 1 && ra.stows == 0,
        Format("A: the empty left hand takes it, nothing put away (hand %d, stows %u)", ra.hand,
               ra.stows));
  check(ra.rightKept, "A: the right hand's sword never leaves it");
  check(ra.castAfter == expectAfter,
        Format("A: the bolt leaves at raise + the flick's release, %d ticks on (%d)", expectAfter,
               ra.castAfter));
  check(ra.fromToHand <= fromTol,
        Format("A: it leaves from the fist (%.2f voxels off, max %.1f)", ra.fromToHand, fromTol));
  check(ra.armed && ra.busyThroughout && !ra.busyAfter,
        Format("A: armed (%d) and busy through the cast (%d), free after it (%d)", ra.armed,
               ra.busyThroughout, !ra.busyAfter));
  check(ra.handTravel >= travelMin,
        Format("A: the arm moves through the gesture (%.2f voxels, min %.1f)", ra.handTravel,
               travelMin));
  // B
  check(rb.begun == MobCastOutcome::Gesturing,
        Format("B: the request puts the spell in a hand (%s)", CastOutcomeName(rb.begun)));
  check(rb.hand == 1 && rb.stows == 1 && rb.leftEmptyDuring && rb.sheathedDuring,
        Format("B: the off-hand sword goes into the sheath for the cast (hand %d, stows %u, "
               "empty %d, sheathed %d)",
               rb.hand, rb.stows, rb.leftEmptyDuring, rb.sheathedDuring));
  check(rb.castAfter == expectAfter,
        Format("B: the bolt leaves %d ticks on (%d)", expectAfter, rb.castAfter));
  check(rb.draws == 1 && rb.leftAfter == "sword" && rb.sheathAfter.empty() && rb.rightKept,
        Format("B: the sword is drawn back into the left hand after (draws %u, left '%s', "
               "sheath '%s', right kept %d)",
               rb.draws, rb.leftAfter.c_str(), rb.sheathAfter.c_str(), rb.rightKept));
  check(ra.light != 0 && ra.lightWire == ra.light && ra.lightAfter == 0,
        Format("A: the fist is lit while the spell is in it (%08x), the pose carries it "
               "(%08x), dark after (%08x)",
               ra.light, ra.lightWire, ra.lightAfter));
  // C
  check(ra.ikMiss <= ikTol && rb.ikMiss <= ikTol,
        Format("C: the arm reaches the gesture's hand at the release (miss %.2f / %.2f, max %.1f)",
               ra.ikMiss, rb.ikMiss, ikTol));
  RecordObserved("npcHandCast.fromToHand", std::max(ra.fromToHand, rb.fromToHand));
  RecordObserved("npcHandCast.ikMiss", std::max(ra.ikMiss, rb.ikMiss));
  RecordObserved("npcHandCast.handTravel", std::min(ra.handTravel, rb.handTravel));

  detail = Format(
      "A: %s hand %d, cast +%d (want %d), from %.2f off the fist, arm travel %.1f, ik miss %.2f; "
      "B: stows %u draws %u, cast +%d, left after '%s', ik miss %.2f",
      CastOutcomeName(ra.begun), ra.hand, ra.castAfter, expectAfter, ra.fromToHand, ra.handTravel,
      ra.ikMiss, rb.stows, rb.draws, rb.castAfter, rb.leftAfter.c_str(), rb.ikMiss);
  if (!fails.empty()) detail += "; FAILED: " + fails;
  std::printf("npc-hand-cast: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace selftest
