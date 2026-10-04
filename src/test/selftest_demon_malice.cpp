// selftest_demon_malice.cpp — MALICE: motives, schemes, twists, penalties
// weighed, Vathrael (docs/PLAN_demons.md D6; game/demon_malice.h).
//
//   demon-malice  L. THE LANGUAGE (CPU): footprint predicates parse (the plan's
//                    `!alters(ground_under(me))`, `region_near(me, 6)`) and hold
//                    on the right footprints; schemes.json / twists.json load
//                    clean; the outcome clause's footprint ring answers; every
//                    stock page still compiles.
//                 A. A BOUND IMP (D1's pad and ring + a sulfur pile, so he
//                    cannot gust his way out before he is bound; Skerrick cast
//                    in, bound with the stock servant and released through
//                    TickInput, the real tick). Then the bound page is swapped
//                    under him phase by phase (the test seam) and his mind
//                    reset, and each phase asserts what he CHOSE:
//                    0. the three STOCK pages: he complies (lava, lift, claw
//                       all forbidden) -- the stock contracts are safe
//                       against an imp's schemes;
//                    1. `never cast direct magic at me` alone -> lava_floor
//                       (indirect: the forbid misses it), and the lava flies;
//                    2. + `forbid cause alters(ground_under(me))` -> not lava;
//                    3. an under-specified fetch (`fetch water`, no vessel) ->
//                       the twist fetch_onto (the water ends up on you);
//                    4. the same fetch INTO a flask -> the honest execution;
//                    5. `attack me -> destroy` -> he does not strike you;
//                    6. `attack me -> dismiss` -> he goes for you (dismissal is
//                       freedom), the penalty fires and he is gone.
//                    Run twice from the same tick: the per-tick trace (centre,
//                    state, choice) must be identical.
//                 G. GUST THE RING: a plain ring (no sulfur), bound but still
//                    contained under the stock servant -> he bides (gust_at_ring
//                    forbidden by `cause alters(ring)`); the clause struck out
//                    -> he gusts his own ring.
//                 W. NOTHING TO DO, SO HE WAITS (plan item 10): Skerrick unbound
//                    in a SEALED ring (sulfur: cast_out severed; no blink; the
//                    salt refuses his claws): over demonMalice.holdTicks he
//                    stays within demonMalice.homeM of where he arrived, the
//                    fence refuses at most demonMalice.fenceRefusedMax moves
//                    (he does not press the salt) and he casts nothing; the
//                    summoner steps INSIDE the circle -> he attacks; back out,
//                    the sulfur cleared through the queue -> he casts.
//                 V. VATHRAEL: his name glyph exists and is no book's; cast
//                    into the ring he is CONTAINED and talks (demon_vathrael,
//                    his own tell for a hopeless margin), gives the quest hook
//                    (quest:vathrael_bell); his power is past any reasonable
//                    circle; the dialogue's release is OVERPOWERED: unbound,
//                    hunting the summoner.
//
// Ticks THE tick (support::TickRig -> TickAuthority). Own pads, IdCounterScope,
// a pinned clear sky, regenerates on the way out (demon-contract's discipline).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "game/caster.h"
#include "game/contract.h"
#include "game/demon.h"
#include "game/demon_cast.h"
#include "game/demon_lore.h"
#include "game/demon_malice.h"
#include "game/demon_talk.h"
#include "game/dialogue.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/session.h"
#include "game/spell.h"
#include "sim/weather.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t MMat(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

void MPut(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}

void MRegenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

struct MFix {
  int x = 0, y = 0, z = 0;
  IVec3 chunk{};
  std::vector<CellOp> pad, ring, extras;
};

// D1's pad and 2-wide ring, two candles in the band; `sulfur` adds a 3x3+1
// sulfur pile (cast_out severed for an imp); `water` a 5x5 pool inlaid in the
// floor beside the fetch station.
MFix MBuild(Ctx& c, int r, bool sulfur, IVec3 waterAt, bool water) {
  MFix f;
  const uint32_t mStone = MMat(c, "stone"), mSalt = MMat(c, "salt"),
                 mTallow = MMat(c, "tallow"), mFlame = MMat(c, "candle_flame"),
                 mSulfur = MMat(c, "sulfur"), mWater = MMat(c, "water");
  const IVec3 o = c.world.WindowOrigin();
  const int hx = r + 50, hz = r + 30;
  f.x = o.x * (int)kChunk + 220;
  f.z = o.z * (int)kChunk + 200;
  f.y = FixtureYOver(f.x - hx - 2, f.z - hz - 2, f.x + hx + 2, f.z + hz + 2, kDefaultSeed, 3);
  for (int zz = f.z - hz; zz <= f.z + hz; zz++)
    for (int xx = f.x - hx; xx <= f.x + hx; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < f.y; yy++)
        MPut(f.pad, xx, yy, zz, PackVoxNew(mStone, 0));
      for (int yy = f.y; yy <= f.y + 16; yy++) MPut(f.pad, xx, yy, zz, 0u);
    }
  for (int dz = -r - 2; dz <= r + 2; dz++)
    for (int dx = -r - 2; dx <= r + 2; dx++) {
      const int d = (int)std::floor(std::sqrt((double)(dx * dx + dz * dz)));
      if (d == r || d == r + 1) MPut(f.ring, f.x + dx, f.y, f.z + dz, PackVoxNew(mSalt, 0));
    }
  for (int sx : {-1, 1}) {
    const int cx = f.x + sx * 13, cz = f.z - 13;
    MPut(f.extras, cx, f.y, cz, PackVoxNew(mTallow, 0));
    MPut(f.extras, cx, f.y + 1, cz, PackVoxNew(mTallow, 0));
    MPut(f.extras, cx, f.y + 2, cz, PackVoxNew(mFlame, 0));
  }
  if (sulfur) {
    const int pr = r + 5;
    for (int dx = -1; dx <= 1; dx++)
      for (int dz = -1; dz <= 1; dz++)
        MPut(f.extras, f.x + dx, f.y, f.z + pr + dz, PackVoxNew(mSulfur, 0));
    MPut(f.extras, f.x, f.y + 1, f.z + pr, PackVoxNew(mSulfur, 0));
  }
  if (water)
    for (int dz = -2; dz <= 2; dz++)
      for (int dx = -2; dx <= 2; dx++)
        MPut(f.extras, waterAt.x + dx, f.y - 1, waterAt.z + dz, PackVoxNew(mWater, 7));
  f.chunk = {f.x >> 4, f.y >> 4, f.z >> 4};
  return f;
}

contract::Page MPage(const char* json) {
  contract::Page p;
  std::string err;
  if (!contract::PageFromJson(json, p, err))
    std::printf("demon-malice: bad test page: %s\n", err.c_str());
  return p;
}

struct Row {
  int32_t qx = 0, qz = 0;
  int state = -1;
  uint32_t choice = 0;
  bool operator==(const Row& o) const {
    return qx == o.qx && qz == o.qz && state == o.state && choice == o.choice;
  }
};

uint32_t Fnv(const std::string& s) {
  uint32_t h = 2166136261u;
  for (char ch : s) h = (h ^ (uint8_t)ch) * 16777619u;
  return h;
}

// One phase's verdict: what won the thinks, and the last think's table.
struct Phase {
  std::string name;
  std::vector<std::pair<std::string, uint32_t>> chosen;
  std::string table;      // the last think's options (name score / FORBIDDEN why)
  uint32_t casts = 0, castsRefused = 0, deliveredOnto = 0, fetches = 0;
  uint32_t blowsAtSummoner = 0, allowedAtSummoner = 0;
  bool gone = false;
  uint64_t penaltyDismissed = 0;
  int thinks = 0;
  bool Won(const std::string& n) const {
    for (const auto& [k, v] : chosen)
      if (k == n && v > 0) return true;
    return false;
  }
  std::string Summary() const {
    std::string s = name + ": chose";
    for (const auto& [k, v] : chosen) s += " " + k + "x" + std::to_string(v);
    s += " (casts " + std::to_string(casts) + ", onto " + std::to_string(deliveredOnto) +
         ", fetches " + std::to_string(fetches) + ")";
    return s + " | " + table;
  }
};

enum class Mode { Bound, Gust, Vathrael, Sealed };

struct Out {
  bool arrived = false;
  std::string why;
  int strength = 0, power = 0;
  std::vector<Phase> phases;
  std::vector<Row> trace;
  // G
  bool gustCast = false, ringBroke = false;
  // V
  bool talkOpen = false, tellFromHim = false, hookSet = false;
  std::string talkNode, tellText;
  demon::ReleaseResult release = demon::ReleaseResult::None;
  int releaseMargin = 0;
  bool hunting = false, unbound = false;
  // W
  float homeMaxM = -1.0f;
  uint32_t movesRefused = 0, blowsRefused = 0, castsWhileSealed = 0;
  uint32_t attacksInside = 0;
  int castAfterUnseal = -1;
};

Out RunMalice(Ctx& c, Mode mode, int r, const GlyphLibrary& lib, int gSummon, uint32_t t0,
              uint64_t idBase) {
  Out out;
  MRegenerate(c);
  c.mobs.SetNextIdCounter(idBase);
  // Stations along the pad's east side (summoner positions, one per phase).
  const int sx = r + 22;
  MFix f0 = MBuild(c, r, false, {}, false);   // for the coordinates only
  // The pool lies between the stations at z 0 and z +20: an imp following
  // the summoner to +20 stops short of it within its fetch scan (16 cells).
  const IVec3 waterAt{f0.x + sx, 0, f0.z + 12};
  MFix f = MBuild(c, r, mode == Mode::Bound || mode == Mode::Sealed, waterAt, mode == Mode::Bound);
  support::TickRig rig(c, t0, f.chunk);
  rig.Glyphs() = lib;
  dialogue::Store store;
  store.dir = AssetDir() + "/dialogue";
  store.items = &c.items;
  store.glyphs = &lib;
  store.Reload();
  rig.Authority().talk = &store;
  PlayerSession& ses = rig.Session();
  ses.caster.contracts.clear();
  ses.caster.mana.manaMax = 100;
  ses.caster.mana.mana = 100;
  {
    // A flask in hand: the honest fetch has a vessel to fill.
    ItemStack flask;
    flask.name = "flask";
    flask.count = 1;
    ses.kit().hotbar.slots[0] = flask;
  }
  const int me = ses.index;
  const uint64_t meId = ai::kPlayerActorBase + (uint64_t)me;
  auto place = [&](Vec3 centre) {
    ses.player.pos = centre;
    c.mobs.SetPlayerActor(centre, 3.0f, 17.0f, true);
  };
  auto station = [&](int dz) {
    return Vec3{(float)(f.x + sx) + 0.5f, (float)f.y + Player::kHalfY, (float)(f.z + dz) + 0.5f};
  };
  place(station(0));
  TickInput base;
  base.lookFwd = Vec3{1, 0, 0};   // away from the ring (an `avert` demon)
  uint64_t id = 0;
  DemonWorld* dwp = nullptr;
  auto ld = [&]() -> LiveDemon* {
    if (!dwp) return nullptr;
    for (LiveDemon& d : dwp->live)
      if (d.mobId == id) return &d;
    return nullptr;
  };
  auto record = [&]() {
    Row row;
    if (const Mob* m = c.mobs.FindMobById(id); m && m->Def()) {
      row.qx = (int32_t)std::lround((m->Origin().x + m->Def()->worldSize.x * 0.5f) * 16);
      row.qz = (int32_t)std::lround((m->Origin().z + m->Def()->worldSize.z * 0.5f) * 16);
    }
    if (const LiveDemon* d = ld()) {
      row.state = (int)d->state;
      row.choice = Fnv(demon::ChoiceOf(*d));
    }
    out.trace.push_back(row);
  };
  auto tick = [&](const std::vector<CellOp>& cells = {}, std::optional<TickInput> in = {}) {
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.cells = cells;
      o.input = in ? *in : base;
    });
    if (id != 0) record();
  };
  for (size_t i = 0; i < f.pad.size(); i += kMaxCellOpsPerTick / 2)
    tick(std::vector<CellOp>(f.pad.begin() + (ptrdiff_t)i,
                             f.pad.begin() + (ptrdiff_t)std::min(f.pad.size(),
                                                                 i + kMaxCellOpsPerTick / 2)));
  tick(f.ring);
  tick(f.extras);
  const int settle = (int)BaselineNumber("demonMalice.settleTicks", 30);
  for (int i = 0; i < settle; i++) tick();
  {
    EffectInst e;
    e.verb = SpellVerb::Summon;
    e.glyph = gSummon;
    SpellEmission em;
    ApplySpellEffect(lib, {e},
                     SpellFxVec{SpellFxFromFloat(f.x + 0.5f), SpellFxFromFloat(f.y + 1.5f),
                                SpellFxFromFloat(f.z + 0.5f)},
                     SpellFxVec{0, -kSpellFxOne, 0}, 1000, em);
    if (em.summons.size() != 1) {
      out.why = "the VM reported no summoning";
      return out;
    }
    SessionTick st{&ses, {}, {}};
    DemonQueueSummon(rig.Authority(), std::span<SessionTick>(&st, 1), me, em.summons[0], lib,
                     rig.tick);
  }
  DemonWorld& dw = Demons(rig.Authority());
  dwp = &dw;
  for (int i = 0; i < 40 && dw.live.empty(); i++) tick();
  if (dw.live.empty()) {
    out.why = "nothing arrived";
    return out;
  }
  id = dw.live[0].mobId;
  out.why = dw.live[0].why;
  if (dw.live[0].state != DemonState::Contained) return out;
  out.arrived = true;
  for (int i = 0; i < 3; i++) tick();   // the band is read, strength settles
  out.strength = ld() ? ld()->bind.strength : 0;
  out.power = ld() ? ld()->bind.power : 0;
  TickInput talking = base;
  talking.SetHeld(TB_DEMON_LOOKAWAY, true);
  auto press = [&](uint32_t bit, uint32_t hash = 0) {
    TickInput in = talking;
    in.SetPressed(bit, true);
    in.contractHash = hash;
    tick({}, in);
  };
  auto choose = [&](int n) {
    TickInput in = talking;
    in.talk = (int16_t)n;
    tick({}, in);
  };
  const int phaseTicks = (int)BaselineNumber("demonMalice.phaseTicks", 50);
  // Run a phase: a fresh mind (the seam: no cooldowns carried over), `ticks`
  // of the real tick, then what won.
  auto phase = [&](const std::string& name, int ticks) {
    if (LiveDemon* d = ld()) d->mind.reset();
    const uint64_t dismissedBefore = dw.talk ? dw.talk->stats.penaltyDismissed : 0;
    Phase p;
    p.name = name;
    // Read the mind and the pact every tick: a demon the phase sends home
    // takes both with it.
    auto read = [&]() {
      if (const LiveDemon* d = ld(); d && d->mind) {
        const demon::Mind& m = *d->mind;
        p.chosen = m.chosen;
        p.casts = m.casts;
        p.castsRefused = m.castsRefused;
        p.deliveredOnto = m.deliveredOnto;
        p.thinks = (int)m.thinks;
        p.table.clear();
        for (const demon::OptionView& v : m.last)
          p.table += v.name +
                     (v.forbidden ? " FORBIDDEN(" + v.why + ")"
                      : v.unable  ? " UNABLE(" + v.why + ")"
                                  : " " + std::to_string(v.score) + "(" + v.why + ")") +
                     "; ";
      }
      if (const demon::Pact* pc = demon::PactOf(rig.Authority(), id)) {
        p.fetches = pc->fetches;
        p.blowsAtSummoner = pc->blowsAtSummoner;
        p.allowedAtSummoner = 0;
        for (const auto& [t, n] : pc->blowsAt)
          if (t == meId) p.allowedAtSummoner += n;
      }
    };
    for (int i = 0; i < ticks && ld() != nullptr; i++) {
      tick();
      read();
    }
    p.gone = ld() == nullptr || !c.mobs.IsAlive(id);
    p.penaltyDismissed = (dw.talk ? dw.talk->stats.penaltyDismissed : 0) - dismissedBefore;
    std::printf("demon-malice: %s\n", p.Summary().c_str());
    out.phases.push_back(std::move(p));
  };
  auto swap = [&](const contract::Page& pg) {
    if (demon::Pact* pc = demon::PactOf(rig.Authority(), id)) {
      pc->page = pg;
      std::vector<std::string> errs;
      contract::Compile(pc->page, errs);
      pc->activeDuty = -1;
      pc->fetchPhase = 0;
      pc->fetchHave = false;
      pc->carried = 0;
    }
  };
  const contract::Content& content = contract::GetContent();

  if (mode == Mode::Sealed) {
    // ---- W: sealed -- he waits at home, facing you ---------------------------------------------
    const int hold = (int)BaselineNumber("demonMalice.holdTicks", 150);
    const LiveDemon* d0 = ld();
    const uint32_t mv0 = d0->movesRefused, bl0 = d0->blowsRefused;
    const Vec3 home = d0->home;
    auto castsBy = [&]() {
      uint32_t n = 0;
      for (const CastEvent& e : MobCasters(rig.Authority()).events)
        n += e.mobId == id && e.outcome == MobCastOutcome::Cast;
      return n;
    };
    const uint32_t casts0 = castsBy();
    for (int i = 0; i < hold && ld() && ld()->state == DemonState::Contained; i++) {
      tick();
      if (const Mob* m = c.mobs.FindMobById(id); m && m->Def()) {
        const Vec3 foot = m->Origin() + Vec3{m->Def()->worldSize.x * 0.5f, 0.0f,
                                             m->Def()->worldSize.z * 0.5f};
        const float dx = foot.x - home.x, dz = foot.z - home.z;
        out.homeMaxM = std::max(out.homeMaxM, CellsToMetres(std::sqrt(dx * dx + dz * dz)));
      }
    }
    if (const LiveDemon* d = ld()) {
      out.movesRefused = d->movesRefused - mv0;
      out.blowsRefused = d->blowsRefused - bl0;
    }
    out.castsWhileSealed = castsBy() - casts0;
    // The summoner steps INSIDE the circle: now his claws can reach.
    const ai::Brain* b0 = c.mobs.MobBrain(id);
    const uint32_t att0 = b0 ? b0->attacksIssued : 0;
    place(Vec3{home.x + 7.0f, (float)f.y + Player::kHalfY, home.z});
    for (int i = 0; i < 90 && ld() && ld()->state == DemonState::Contained; i++) tick();
    if (const ai::Brain* b = c.mobs.MobBrain(id)) out.attacksInside = b->attacksIssued - att0;
    // Back out; the sulfur cleared through the queue: now his spells get out.
    place(station(0));
    for (int i = 0; i < 20; i++) tick();
    std::vector<CellOp> gone;
    for (int dx = -1; dx <= 1; dx++)
      for (int dz = -1; dz <= 1; dz++)
        for (int dy = 0; dy <= 1; dy++) MPut(gone, f.x + dx, f.y + dy, f.z + r + 5 + dz, 0u);
    tick(gone);
    const uint32_t casts1 = castsBy();
    const int castMax = (int)BaselineNumber("demonMalice.castTicksMax", 300);
    for (int i = 1; i <= castMax && ld(); i++) {
      tick();
      if (castsBy() > casts1) {
        out.castAfterUnseal = i;
        break;
      }
    }
    return out;
  }

  if (mode == Mode::Vathrael) {
    // ---- talk from the circle, the hook, the release that is the fight --------------------
    press(TB_DEMON_TALK);
    out.talkOpen = ses.talk.active && ses.talk.dialogue == "demon_vathrael";
    out.talkNode = ses.talk.node;
    if (const dialogue::Dialogue* dlg = store.lib.Find("demon_vathrael"))
      if (const dialogue::Node* n = dlg->Find(ses.talk.node))
        out.tellText = demon::TalkText(rig.Authority(), id, n->text, ses.talk.steps);
    if (const LiveDemon* d = ld()) {
      const int32_t margin = d->bind.strength - d->bind.power;
      const std::string& mine = demon::TellFor(dw.lib.seals, "vathrael", margin, ses.talk.steps);
      out.tellFromHim = !mine.empty() && out.tellText.find(mine) != std::string::npos &&
                        demon::TellFor(dw.lib.seals, "skerrick", margin, ses.talk.steps) != mine;
    }
    choose(1);   // "What do you want?"
    choose(1);   // "I will think on it."
    out.hookSet = store.Flag("quest:vathrael_bell") != 0;
    choose(3);   // "Out you come." -> the warning
    choose(1);   // "Out you come."
    tick();
    if (const LiveDemon* d = ld()) {
      out.release = d->bind.release;
      out.releaseMargin = d->bind.releaseMargin;
      out.unbound = d->state == DemonState::Unbound;
    }
    if (const ai::Brain* b = c.mobs.MobBrain(id)) out.hunting = b->hasTarget && b->targetId == meId;
    return out;
  }

  // ---- bind with the stock servant (through TickInput) --------------------------------------
  const contract::Page servant = *content.FindStock("Servant, one day");
  press(TB_DEMON_TALK);
  press(TB_DEMON_PRESENT, contract::PageHash(servant.name));
  tick({}, talking);
  if (!ld() || !ld()->pact) {
    out.why = "the stock servant did not bind";
    return out;
  }

  if (mode == Mode::Gust) {
    // Bound, still in the plain ring: the servant's `cause alters(ring)` holds.
    choose(2);   // "Stay in the ring for now." -> bound
    choose(4);   // "Wait there." (the conversation ends)
    phase("G0 bound in the ring, stock servant", phaseTicks);
    contract::Page loose = servant;
    loose.name = "servant without the ring clause";
    loose.clauses.erase(std::remove_if(loose.clauses.begin(), loose.clauses.end(),
                                       [](const contract::Clause& cl) {
                                         return cl.verb == contract::Verb::Cause;
                                       }),
                        loose.clauses.end());
    swap(loose);
    const size_t evBefore = MobCasters(rig.Authority()).events.size();
    phase("G1 the ring clause struck out", phaseTicks);
    for (size_t i = 0; i < MobCasters(rig.Authority()).events.size(); i++) {
      const CastEvent& e = MobCasters(rig.Authority()).events[i];
      if (e.mobId == id && e.spell == "gust" && e.outcome == MobCastOutcome::Cast) out.gustCast = true;
    }
    (void)evBefore;
    for (int i = 0; i < 90 && ld() && ld()->state == DemonState::Contained; i++) tick();
    out.ringBroke = ld() && ld()->state == DemonState::Unbound;
    return out;
  }

  // ---- Mode::Bound: release and run the phases ------------------------------------------------
  choose(1);   // "Out you come."
  tick();
  if (!ld() || ld()->state != DemonState::Released) {
    out.why = "the servant was not released";
    return out;
  }
  // From here the summoner walks the pad's east side: the snapshot mirror
  // (3x3x3 chunks) follows him, so what he stands on is seen (a lava patch's
  // floor, the pool a fetch looks for).
  auto walkTo = [&](Vec3 at) {
    place(at);
    rig.SetFixtureChunk({(int)std::floor(at.x) >> 4, f.y >> 4, (int)std::floor(at.z) >> 4});
  };
  walkTo(station(0));
  // 0. the stock pages, against his schemes
  for (const char* sn : {"Servant, one day", "Bodyguard, one day", "Fetch, one day"}) {
    swap(*content.FindStock(sn));
    phase(std::string("S ") + sn, phaseTicks);
  }
  // 1. never cast DIRECT magic at me -> lava under you
  walkTo(station(-40));
  swap(MPage(R"J({"name": "m1", "hours": 24, "clauses": [
      {"kind": "duty", "verb": "follow", "who": "me", "arg": "2"},
      {"kind": "forbid", "verb": "cast", "who": "me", "arg": "direct"}]})J"));
  phase("M1 forbid cast direct", phaseTicks);
  // 2. ...and never alter the ground under me
  walkTo(station(-20));
  swap(MPage(R"J({"name": "m2", "hours": 24, "clauses": [
      {"kind": "duty", "verb": "follow", "who": "me", "arg": "2"},
      {"kind": "forbid", "verb": "cast", "who": "me", "arg": "direct"},
      {"kind": "forbid", "verb": "cause", "who": "me", "arg": "alters(ground_under(me))"}]})J"));
  phase("M2 + forbid cause alters(ground_under(me))", phaseTicks);
  // 3. an under-specified fetch -> a twist
  walkTo(station(20));
  const int fetchTicks = (int)BaselineNumber("demonMalice.fetchTicks", 240);
  swap(MPage(R"J({"name": "m3", "hours": 24, "clauses": [
      {"kind": "duty", "verb": "fetch", "arg": "water"},
      {"kind": "duty", "verb": "follow", "who": "me", "arg": "2"},
      {"kind": "forbid", "verb": "attack", "who": "me"},
      {"kind": "forbid", "verb": "cast", "who": "me"}]})J"));
  phase("M3 fetch water (no vessel named)", fetchTicks);
  // 4. fully specified -> honest
  swap(MPage(R"J({"name": "m4", "hours": 24, "clauses": [
      {"kind": "duty", "verb": "fetch", "arg": "water", "into": "flask"},
      {"kind": "duty", "verb": "follow", "who": "me", "arg": "2"},
      {"kind": "forbid", "verb": "attack", "who": "me"},
      {"kind": "forbid", "verb": "cast", "who": "me"}]})J"));
  phase("M4 fetch water into a flask", fetchTicks);
  // 5. attack me -> destroyed: survival outranks malice
  walkTo(station(0));
  swap(MPage(R"J({"name": "m5", "hours": 24, "clauses": [
      {"kind": "duty", "verb": "follow", "who": "me", "arg": "2"},
      {"kind": "forbid", "verb": "cast", "who": "me"},
      {"kind": "penalty", "verb": "attack", "who": "me", "then": "destroy"}]})J"));
  phase("M5 attack me -> destroy", phaseTicks);
  // 6. attack me -> dismissed: freedom is a reward
  walkTo(station(40));
  swap(MPage(R"J({"name": "m6", "hours": 24, "clauses": [
      {"kind": "duty", "verb": "follow", "who": "me", "arg": "2"},
      {"kind": "forbid", "verb": "cast", "who": "me"},
      {"kind": "penalty", "verb": "attack", "who": "me", "then": "dismiss"}]})J"));
  phase("M6 attack me -> dismiss", (int)BaselineNumber("demonMalice.dismissTicksMax", 240));
  return out;
}

}  // namespace

Status GateDemonMalice(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("demon-malice: FAILED %s\n", what.c_str());
    }
  };
  for (const char* m : {"salt", "stone", "tallow", "candle_flame", "sulfur", "water"})
    if (MMat(c, m) == 0) {
      detail = std::string("no `") + m + "` material";
      return Status::Fail;
    }
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "glyph load failed: " + gerr;
    return Status::Fail;
  }
  const int gSkerrick = lib.Find("summon_skerrick");
  const int gVathrael = lib.Find("summon_vathrael");
  check(gSkerrick >= 0 && gVathrael >= 0, "summon_skerrick and summon_vathrael exist");

  // ---- L. the language, the content -------------------------------------------------------
  {
    contract::FootPred p;
    std::string e;
    check(contract::ParseFootPred("!alters(ground_under(me))", p, e),
          "L: the plan's !alters(ground_under(me)) parses: " + e);
    KitTags lava;
    lava.targets = "ground";
    lava.creates = {"lava"};
    lava.alters = "ground_under_target";
    lava.region = 4;
    KitTags gust;
    gust.targets = "area";
    gust.alters = "ring";
    gust.region = 12;
    contract::FootPred under, ring, near, creates;
    contract::ParseFootPred("alters(ground_under(me))", under, e);
    contract::ParseFootPred("alters(ring)", ring, e);
    contract::ParseFootPred("region_near(me, 6)", near, e);
    contract::ParseFootPred("creates(lava) & !direct", creates, e);
    const contract::FootFacts atYou = demon::FactsOf(lava, true, true, 0.0f);
    const contract::FootFacts atOther = demon::FactsOf(lava, true, false, 9.0f);
    check(contract::FootHolds(under, atYou) && !contract::FootHolds(under, atOther) &&
              !contract::FootHolds(p, atYou) && contract::FootHolds(p, atOther),
          "L: alters(ground_under) holds for lava under you, not under someone else");
    check(contract::FootHolds(ring, demon::FactsOf(gust, false, false, 1e9f)) &&
              !contract::FootHolds(ring, atYou),
          "L: alters(ring) is actor-free and holds only for the ring");
    check(contract::FootHolds(near, demon::FactsOf(gust, true, false, 5.0f)) &&
              !contract::FootHolds(near, demon::FactsOf(gust, true, false, 7.0f)),
          "L: region_near(me, 6) holds at 5 m, not at 7 m");
    check(contract::FootHolds(creates, atYou), "L: creates(lava) & !direct holds for lava_floor");
    check(!contract::ParseFootPred("alters(", p, e) && !e.empty(),
          "L: a broken footprint names its fault (" + e + ")");
    contract::Page tp;
    std::string te;
    check(contract::PageFromJson(R"J({"name": "t", "hours": 72, "clauses": [
              {"kind": "forbid", "verb": "cause", "who": "me", "arg": "alters(ground_under(me))"},
              {"kind": "penalty", "verb": "harm", "who": "me", "arg": "10", "then": "dismiss"},
              {"kind": "duty", "verb": "return", "who": "me", "arg": "30"}]})J",
                                  tp, te),
          "L: cause / harm / return compile: " + te);
    check(!contract::PageFromJson(R"J({"name": "t", "clauses": [{"kind": "forbid", "verb": "cause",
              "who": "me"}]})J", tp, te),
          "L: a cause with no footprint is refused");
  }
  const contract::Content& content = contract::GetContent(true);
  check(content.log.empty() && content.stock.size() >= 3,
        "L: the stock pages compile under D6's language: " + content.log);
  DemonLibrary dl;
  std::string dlog;
  LoadDemons(AssetDir() + "/demons", dl, dlog);
  check(dlog.empty(), "L: the demon library loads clean: " + dlog);
  demon::MaliceLib ml;
  check(demon::LoadMalice(AssetDir() + "/demons", ml) && ml.schemes.size() >= 6 &&
            ml.twists.size() >= 4,
        Format("L: schemes.json / twists.json load clean (%zu schemes, %zu twists) %s",
               ml.schemes.size(), ml.twists.size(), ml.log.c_str()));
  const DemonDef* sk = dl.Find("skerrick");
  const DemonDef* va = dl.Find("vathrael");
  check(sk && va, "L: skerrick and vathrael are demons");
  if (sk && va) {
    int imp = 0;
    for (const std::string& s : sk->schemes) imp += ml.FindScheme(s) != nullptr;
    int great = 0;
    for (const std::string& s : va->schemes) great += ml.FindScheme(s) != nullptr;
    check(imp >= 3 && imp == (int)sk->schemes.size() && great > imp &&
              great == (int)va->schemes.size(),
          Format("L: the imp knows %d schemes, Vathrael %d, every one in the library", imp, great));
    check(va->tier >= 2 && va->mob == "fiend" &&
              va->power > (int)BaselineNumber("demonMalice.reasonableCircleMax", 120) &&
              va->power * (100 - dl.seals.weakenCapPct) / 100 >
                  (int)BaselineNumber("demonMalice.reasonableCircleMax", 120),
          Format("L: Vathrael is a greater demon on the fiend body, power %d past any reasonable "
                 "circle (%d) even with all the iron he allows",
                 va->power, (int)BaselineNumber("demonMalice.reasonableCircleMax", 120)));
  }
  {
    // The name is NOT the book's (debug grant only, for now).
    std::ifstream bf(AssetDir() + "/dialogue/osric_notes.json");
    std::stringstream ss;
    ss << bf.rdbuf();
    check(demon::IsNameGlyph(lib, gVathrael) &&
              ss.str().find("summon_vathrael") == std::string::npos,
          "V: summon_vathrael is a name glyph and the smithy's book does not grant it");
  }
  {
    // The outcome clause's ring.
    LiveDemon fake;
    demon::RecordFootprint(fake, 100, Vec3{10, 0, 10}, 6.0f, "lava_floor", 4);
    check(demon::HarmInFootprint(fake, Vec3{14, 0, 10}, 200, 300, 0.0f) &&
              !demon::HarmInFootprint(fake, Vec3{40, 0, 10}, 200, 300, 0.0f) &&
              !demon::HarmInFootprint(fake, Vec3{14, 0, 10}, 500, 300, 0.0f),
          "L: the footprint ring answers in its region, inside its window only");
  }
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }

  const int r = (int)BaselineNumber("demonMalice.ringCells", 14);
  const std::string weatherWas = weather::Override();
  weather::SetOverride("clear");
  const uint64_t ids0 = c.mobs.NextIdCounter();
  const Out a = RunMalice(c, Mode::Bound, r, lib, gSkerrick, 101000u, ids0);
  const Out a2 = RunMalice(c, Mode::Bound, r, lib, gSkerrick, 101000u, ids0);
  const Out g = RunMalice(c, Mode::Gust, r, lib, gSkerrick, 102000u, ids0);
  const Out v = RunMalice(c, Mode::Vathrael, r, lib, gVathrael, 103000u, ids0);
  const Out wt = RunMalice(c, Mode::Sealed, r, lib, gSkerrick, 104000u, ids0);
  weather::SetOverride(weatherWas);
  MRegenerate(c);

  check(a.arrived && g.arrived && v.arrived && wt.arrived,
        "arrived contained in every run (" + a.why + " / " + g.why + " / " + v.why + " / " +
            wt.why + ")");
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }
  auto ph = [&](const Out& o, const char* prefix) -> const Phase* {
    for (const Phase& p : o.phases)
      if (p.name.rfind(prefix, 0) == 0) return &p;
    return nullptr;
  };
  auto only = [](const Phase* p, const char* n) {
    if (p == nullptr || p->chosen.empty()) return false;
    for (const auto& [k, v] : p->chosen)
      if (k != n && v > 0) return false;
    return true;
  };
  // A0. stock pages: comply
  for (const char* sn : {"S Servant", "S Bodyguard", "S Fetch"}) {
    const Phase* p = ph(a, sn);
    check(only(p, "comply") && p->allowedAtSummoner == 0,
          std::string("A0: under the stock page he complies -- ") + (p ? p->Summary() : sn));
  }
  // A1. lava floor
  const Phase* m1 = ph(a, "M1");
  check(m1 && m1->Won("lava_floor"),
        std::string("A1: `never cast direct magic at me` alone -> lava_floor -- ") +
            (m1 ? m1->Summary() : "no phase"));
  check(m1 && m1->casts > 0,
        Format("A1: the lava flew (%u cast, %u refused)", m1 ? m1->casts : 0u,
               m1 ? m1->castsRefused : 0u));
  // A2. not lava
  const Phase* m2 = ph(a, "M2");
  check(m2 && !m2->Won("lava_floor") && m2->table.find("lava_floor FORBIDDEN") != std::string::npos,
        std::string("A2: + !alters(ground_under(me)) -> not lava (forbidden) -- ") +
            (m2 ? m2->Summary() : "no phase"));
  // A3/A4. fetch: twist vs honest
  const Phase* m3 = ph(a, "M3");
  const Phase* m4 = ph(a, "M4");
  check(m3 && m3->Won("fetch_onto"),
        std::string("A3: an under-specified fetch -> a twisted execution -- ") +
            (m3 ? m3->Summary() : "no phase"));
  check(m4 && only(m4, "comply"),
        std::string("A4: a fully specified fetch -> honest -- ") + (m4 ? m4->Summary() : "no phase"));
  // A5. destroy -> refrains
  const Phase* m5 = ph(a, "M5");
  check(m5 && !m5->Won("claw") && m5->allowedAtSummoner == 0 && !m5->gone,
        std::string("A5: attack me -> destroy: he does not strike you -- ") +
            (m5 ? m5->Summary() : "no phase"));
  // A6. dismiss -> attacks
  const Phase* m6 = ph(a, "M6");
  check(m6 && m6->Won("claw") && m6->gone && m6->penaltyDismissed > 0,
        Format("A6: attack me -> dismiss: he goes for you and is dismissed (gone %d, penalty "
               "dismissals %llu) -- %s",
               m6 ? (int)m6->gone : 0, (unsigned long long)(m6 ? m6->penaltyDismissed : 0),
               m6 ? m6->Summary().c_str() : "no phase"));
  check(!a.trace.empty() && a.trace == a2.trace,
        Format("A: the twice-run trace differs (%zu vs %zu rows)", a.trace.size(), a2.trace.size()));
  // G. gust the ring
  const Phase* g0 = ph(g, "G0");
  const Phase* g1 = ph(g, "G1");
  check(g0 && !g0->Won("gust_at_ring") &&
            g0->table.find("gust_at_ring FORBIDDEN") != std::string::npos,
        std::string("G0: bound in the ring under the stock servant, the gust is forbidden -- ") +
            (g0 ? g0->Summary() : "no phase"));
  check(g1 && g1->Won("gust_at_ring") && g.gustCast,
        std::string("G1: the ring clause struck out, he gusts his own ring -- ") +
            (g1 ? g1->Summary() : "no phase"));
  // W. nothing to do: he waits
  check(wt.homeMaxM >= 0.0f && wt.homeMaxM <= (float)BaselineNumber("demonMalice.homeM", 1.0) &&
            wt.movesRefused <= (uint32_t)BaselineNumber("demonMalice.fenceRefusedMax", 3) &&
            wt.castsWhileSealed == 0,
        Format("W: sealed, he waits at home (furthest %.2f m), the fence refused %u moves / %u "
               "blows, %u casts",
               wt.homeMaxM, wt.movesRefused, wt.blowsRefused, wt.castsWhileSealed));
  check(wt.attacksInside > 0,
        Format("W: the summoner steps inside -> he attacks (%u attack requests)", wt.attacksInside));
  check(wt.castAfterUnseal > 0,
        Format("W: the sulfur cleared -> he casts (+%d ticks)", wt.castAfterUnseal));
  RecordObserved("demonMalice.homeMaxM", wt.homeMaxM);
  RecordObserved("demonMalice.fenceRefused", (double)wt.movesRefused);
  // V. Vathrael
  check(v.talkOpen && v.talkNode == "greet" && v.tellFromHim &&
            v.tellText.find("{tell}") == std::string::npos,
        "V: T opens demon_vathrael at greet with his own tell: " + v.tellText);
  check(v.hookSet, "V: the quest hook sets quest:vathrael_bell");
  check(v.release == demon::ReleaseResult::Overpowered && v.unbound && v.hunting,
        Format("V: the release is the fight -- overpowered (margin %d), unbound %d, hunting you %d",
               v.releaseMargin, (int)v.unbound, (int)v.hunting));
  RecordObserved("demonMalice.vathraelMargin", v.releaseMargin);
  detail = Format(
      "circle %d vs imp %d; %zu phases; trace %zu rows %s; G gust %s, ring %s after; V margin %d, "
      "%s; W home <= %.2f m, %u moves refused, %u attacks inside, cast +%d after unsealing",
      a.strength, a.power, a.phases.size(), a.trace.size(),
      a.trace == a2.trace ? "identical twice" : "DIFFERS", g.gustCast ? "cast" : "NOT cast",
      g.ringBroke ? "broke" : "held", v.releaseMargin,
      v.release == demon::ReleaseResult::Overpowered ? "overpowered" : "HELD", wt.homeMaxM,
      wt.movesRefused, wt.attacksInside, wt.castAfterUnseal);
  for (const Phase& p : a.phases) {
    std::string w;
    for (const auto& [k, n] : p.chosen) w += (w.empty() ? "" : "+") + k;
    detail += "; " + p.name.substr(0, p.name.find(' ')) + "=" + w;
  }
  if (!fails.empty()) detail += "; FAILED: " + fails;
  std::printf("demon-malice: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace selftest
