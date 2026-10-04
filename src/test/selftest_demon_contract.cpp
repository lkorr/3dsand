// selftest_demon_contract.cpp — CONVERSATION, CONTRACTS, WEIGHT, UPKEEP
// (docs/PLAN_demons.md D5; game/contract.h, game/demon_talk.h).
//
//   demon-contract  On a stone pad on the harness map, D1's 2-wide salt ring
//                   (radius demonContract.ringCells) with two candles in the
//                   band, Skerrick cast into it through the real queue and the
//                   real tick, the summoner standing outside it. Every player
//                   action goes through TickInput (talk, present, release by
//                   dialogue choice, dismiss, look-away).
//                   L. THE LANGUAGE: selectors / triggers / effects parse, a
//                      bad one names its fault, the stock pages load and weigh
//                      under the tariff.
//                   S. SERVANT ("Servant, one day"):
//                      1. T opens his conversation (demon_skerrick) and
//                         {tell} is filled from the margin;
//                      2. a contract heavier than the circle's margin is
//                         REFUSED (still contained, unbound, the dialogue
//                         reopens at `refused`);
//                      3. the stock servant BINDS (weight, upkeep reserved
//                         out of the summoner's mana max);
//                      4. the dialogue's `release` lets him out, the binding
//                         holding with the contract's weight;
//                      5. he FOLLOWS: the summoner walks off and he ends near;
//                      6. struck (so his neutral floor is provoked) he still
//                         never lands a blow on the summoner;
//                      7. Shift+Y (TB_DEMON_DISMISS) sends him home and frees
//                         the reservation.
//                      Run twice from the same tick: the per-tick trace must
//                      be identical.
//                   B. BODYGUARD (a page whose guard duty is the plan's
//                      `attacked_by(me) & hostile_to(me)`): the summoner
//                      strikes at a zombie; a villager stands by. He fights
//                      the zombie and only the zombie.
//                   P. PERSISTENCE + EXPIRY: a bound servant, the MOBS / DMNS /
//                      CNTR sections round-tripped through their bytes (the
//                      imp comes back under a fresh id): bound again, same
//                      page, same term left, same upkeep, the player's own page
//                      back. Then his term runs out: he departs and the
//                      reservation is freed.
//
// Ticks THE tick (support::TickRig -> TickAuthority). Own pads, IdCounterScope,
// a pinned clear sky, regenerates on the way out (demon-seals' discipline).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "game/caster.h"
#include "game/contract.h"
#include "game/demon.h"
#include "game/demon_talk.h"
#include "game/dialogue.h"
#include "game/mob.h"
#include "game/persist.h"
#include "game/player.h"
#include "game/session.h"
#include "game/spell.h"
#include "sim/weather.h"
#include "sim/worldgen_run.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t CMat(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

void CPut(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}

void CRegenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

struct CFix {
  int x = 0, y = 0, z = 0;
  IVec3 chunk{};
  std::vector<CellOp> pad, ring, candles;
};

// D1's pad and 2-wide ring, plus two candles in the band (D3's fixture).
CFix CBuild(Ctx& c, int r) {
  CFix f;
  const uint32_t mStone = CMat(c, "stone"), mSalt = CMat(c, "salt"),
                 mTallow = CMat(c, "tallow"), mFlame = CMat(c, "candle_flame");
  const IVec3 o = c.world.WindowOrigin();
  const int hx = r + 50, hz = r + 30;
  f.x = o.x * (int)kChunk + 220;
  f.z = o.z * (int)kChunk + 200;
  f.y = FixtureYOver(f.x - hx - 2, f.z - hz - 2, f.x + hx + 2, f.z + hz + 2, kDefaultSeed, 3);
  for (int zz = f.z - hz; zz <= f.z + hz; zz++)
    for (int xx = f.x - hx; xx <= f.x + hx; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < f.y; yy++)
        CPut(f.pad, xx, yy, zz, PackVoxNew(mStone, 0));
      for (int yy = f.y; yy <= f.y + 16; yy++) CPut(f.pad, xx, yy, zz, 0u);
    }
  for (int dz = -r - 2; dz <= r + 2; dz++)
    for (int dx = -r - 2; dx <= r + 2; dx++) {
      const int d = (int)std::floor(std::sqrt((double)(dx * dx + dz * dz)));
      if (d == r || d == r + 1) CPut(f.ring, f.x + dx, f.y, f.z + dz, PackVoxNew(mSalt, 0));
    }
  for (int sx : {-1, 1}) {
    const int cx = f.x + sx * 13, cz = f.z - 13;
    CPut(f.candles, cx, f.y, cz, PackVoxNew(mTallow, 0));
    CPut(f.candles, cx, f.y + 1, cz, PackVoxNew(mTallow, 0));
    CPut(f.candles, cx, f.y + 2, cz, PackVoxNew(mFlame, 0));
  }
  f.chunk = {f.x >> 4, f.y >> 4, f.z >> 4};
  return f;
}

struct Row {
  int32_t qx = 0, qz = 0;
  int state = -1, pact = 0, duty = -1;
  bool operator==(const Row& o) const {
    return qx == o.qx && qz == o.qz && state == o.state && pact == o.pact && duty == o.duty;
  }
};

enum class Mode { Servant, Bodyguard, Persist };

struct Out {
  bool arrived = false;
  std::string why;
  int strength = 0, power = 0;
  // S
  bool talkOpen = false;
  std::string talkNode, tellText;
  demon::PresentInfo heavy, servant;
  bool heavyStillContained = false, heavyNoPact = false;
  std::string refusedNode, boundNode;
  int reservedBound = -1;
  bool released = false, releasedProfile = false;
  float distBefore = 0, distAfter = 0;
  uint32_t blowsAtSummoner = 0, allowedAtSummoner = 0, blowsUnsanctioned = 0;
  bool dismissedGone = false;
  int reservedAfterDismiss = -1;
  std::vector<Row> trace;
  // B
  uint64_t zombie = 0, villager = 0;
  int guardTicks = 0, wrongTargetTicks = 0;
  uint32_t blowsAtZombie = 0, blowsAtOthers = 0;
  // P
  bool restored = false, restoredReleased = false, pageBack = false;
  std::string restoredPage;
  int64_t leftBefore = -1, leftAfter = -1;
  int upkeepBefore = -1, upkeepAfter = -1, reservedAfterLoad = -1;
  uint64_t idBefore = 0, idAfter = 0;
  bool expiredGone = false;
  int reservedAfterExpiry = -1;
};

contract::Page TestPage(const char* json) {
  contract::Page p;
  std::string err;
  if (!contract::PageFromJson(json, p, err)) std::printf("demon-contract: bad test page: %s\n", err.c_str());
  return p;
}

Out RunContract(Ctx& c, Mode mode, int r, const GlyphLibrary& lib, int gSummon, uint32_t t0,
                uint64_t idBase) {
  Out out;
  CRegenerate(c);
  c.mobs.SetNextIdCounter(idBase);
  CFix f = CBuild(c, r);
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
  const int me = ses.index;
  const uint64_t meId = ai::kPlayerActorBase + (uint64_t)me;
  // The summoner: outside the ring on +x, within talking reach. Its actor
  // (what the mobs see) and its body (what the tick reads) move together.
  auto place = [&](Vec3 centre) {
    ses.player.pos = centre;
    c.mobs.SetPlayerActor(centre, 3.0f, 17.0f, true);
  };
  const Vec3 home{(float)(f.x + r + 22) + 0.5f, (float)f.y + Player::kHalfY, (float)f.z + 0.5f};
  place(home);
  TickInput base;
  base.lookFwd = Vec3{1, 0, 0};   // away from the ring (he is an `avert` demon)
  auto tick = [&](const std::vector<CellOp>& cells = {}, std::optional<TickInput> in = {}) {
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.cells = cells;
      o.input = in ? *in : base;
    });
  };
  for (size_t i = 0; i < f.pad.size(); i += kMaxCellOpsPerTick / 2)
    tick(std::vector<CellOp>(f.pad.begin() + (ptrdiff_t)i,
                             f.pad.begin() + (ptrdiff_t)std::min(f.pad.size(),
                                                                 i + kMaxCellOpsPerTick / 2)));
  tick(f.ring);
  tick(f.candles);
  const int settle = (int)BaselineNumber("demonContract.settleTicks", 30);
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
  for (int i = 0; i < 40 && dw.live.empty(); i++) tick();
  if (dw.live.empty()) {
    out.why = "nothing arrived";
    return out;
  }
  uint64_t id = dw.live[0].mobId;
  out.why = dw.live[0].why;
  if (dw.live[0].state != DemonState::Contained) return out;
  out.arrived = true;
  for (int i = 0; i < 4; i++) tick();   // the band is read, strength settles
  auto ld = [&]() -> LiveDemon* {
    for (LiveDemon& d : dw.live)
      if (d.mobId == id) return &d;
    return nullptr;
  };
  out.strength = ld()->bind.strength;
  out.power = ld()->bind.power;
  auto demonCentre = [&]() -> Vec3 {
    const Mob* m = c.mobs.FindMobById(id);
    return m && m->Def() ? m->Origin() + m->Def()->worldSize * 0.5f : Vec3{};
  };
  auto record = [&]() {
    Row row;
    if (const Mob* m = c.mobs.FindMobById(id); m && m->Def()) {
      row.qx = (int32_t)std::lround((m->Origin().x + m->Def()->worldSize.x * 0.5f) * 16);
      row.qz = (int32_t)std::lround((m->Origin().z + m->Def()->worldSize.z * 0.5f) * 16);
    }
    if (const LiveDemon* d = ld()) {
      row.state = (int)d->state;
      row.pact = d->pact ? 1 : 0;
      row.duty = d->pact ? d->pact->activeDuty : -1;
    }
    out.trace.push_back(row);
  };
  // Talking: look away from him (he is `avert`), through the command.
  TickInput talking = base;
  talking.SetHeld(TB_DEMON_LOOKAWAY, true);
  auto press = [&](uint32_t bit, uint32_t hash = 0) {
    TickInput in = talking;
    in.SetPressed(bit, true);
    in.contractHash = hash;
    tick({}, in);
    record();
  };
  auto choose = [&](int n) {
    TickInput in = talking;
    in.talk = (int16_t)n;
    tick({}, in);
    record();
  };
  // ---- open the conversation and bind ---------------------------------------------
  press(TB_DEMON_TALK);
  out.talkOpen = ses.talk.active && ses.talk.dialogue == "demon_skerrick";
  out.talkNode = ses.talk.node;
  if (const dialogue::Dialogue* dlg = store.lib.Find("demon_skerrick"))
    if (const dialogue::Node* n = dlg->Find(ses.talk.node))
      out.tellText = demon::TalkText(rig.Authority(), id, n->text, ses.talk.steps);
  const contract::Page servant = *contract::GetContent().FindStock("Servant, one day");
  contract::Page bind = servant;
  if (mode == Mode::Servant) {
    // A page heavier than the margin: forever, and forbidding everything.
    ses.caster.contracts.push_back(TestPage(
        R"J({"name": "heavy", "hours": 0, "clauses": [
             {"kind": "duty", "verb": "follow", "who": "me"},
             {"kind": "forbid", "verb": "attack", "who": "all"},
             {"kind": "forbid", "verb": "cast", "who": "all"}]})J"));
    press(TB_DEMON_PRESENT, contract::PageHash("heavy"));
    for (const auto& [s, pi] : Demons(rig.Authority()).talk->lastPresent)
      if (s == me) out.heavy = pi;
    out.heavyStillContained = ld() && ld()->state == DemonState::Contained;
    out.heavyNoPact = ld() && !ld()->pact;
    out.refusedNode = ses.talk.node;
  }
  if (mode == Mode::Bodyguard) {
    bind = TestPage(R"J({"name": "my bodyguard", "hours": 24, "clauses": [
             {"kind": "duty", "verb": "guard", "who": "attacked_by(me) & hostile_to(me)"},
             {"kind": "duty", "verb": "follow", "who": "me"},
             {"kind": "forbid", "verb": "attack", "who": "me"}]})J");
    ses.caster.contracts.push_back(bind);
  }
  if (mode == Mode::Persist) {
    ses.caster.contracts.push_back(TestPage(R"J({"name": "kept page", "hours": 72,
        "clauses": [{"kind": "duty", "verb": "spin"}]})J"));
  }
  press(TB_DEMON_PRESENT, contract::PageHash(bind.name));
  for (const auto& [s, pi] : Demons(rig.Authority()).talk->lastPresent)
    if (s == me) out.servant = pi;
  out.boundNode = ses.talk.node;
  tick({}, talking);
  record();
  out.reservedBound = ses.caster.mana.reserved;
  // ---- release, by the dialogue's choice ("Out you come.") --------------------------
  choose(1);
  tick();
  record();
  out.released = ld() && ld()->state == DemonState::Released && ld()->pact;
  if (const ai::Brain* b = c.mobs.MobBrain(id))
    out.releasedProfile = b->profile >= 0 && b->profile == c.mobs.Behaviors().Find("imp_bound");
  if (!out.released) return out;

  if (mode == Mode::Servant) {
    // ---- follow: the summoner walks off west across the pad -----------------------
    const Vec3 away{(float)(f.x - 40) + 0.5f, home.y, (float)f.z + 0.5f};
    place(away);
    out.distBefore = (demonCentre() - away).len();
    const int followTicks = (int)BaselineNumber("demonContract.followTicks", 240);
    for (int i = 0; i < followTicks; i++) {
      tick();
      record();
    }
    const Vec3 dc = demonCentre();
    out.distAfter = std::sqrt((dc.x - away.x) * (dc.x - away.x) + (dc.z - away.z) * (dc.z - away.z));
    // ---- struck: his neutral floor is provoked, and still he never strikes you ----
    if (Mob* m = c.mobs.FindMobById(id)) {
      std::vector<uint64_t> hs;
      m->AppendBodyHandles(hs);
      if (!hs.empty()) c.mobs.Damage(hs[0], m->TotalHp() * 0.05f, demonCentre());
    }
    for (int i = 0; i < 90; i++) {
      tick();
      record();
    }
    if (const demon::Pact* p = demon::PactOf(rig.Authority(), id)) {
      out.blowsAtSummoner = p->blowsAtSummoner;
      out.blowsUnsanctioned = p->blowsUnsanctioned;
      for (const auto& [t, n] : p->blowsAt)
        if (t == meId) out.allowedAtSummoner += n;
    }
    // ---- dismiss, from anywhere --------------------------------------------------------
    TickInput in = base;
    in.SetPressed(TB_DEMON_DISMISS, true);
    tick({}, in);
    tick();
    out.dismissedGone = !c.mobs.IsAlive(id) && ld() == nullptr;
    out.reservedAfterDismiss = ses.caster.mana.reserved;
    return out;
  }

  if (mode == Mode::Bodyguard) {
    // ---- a zombie at the summoner, a villager standing by -------------------------------
    const int hd = c.mobs.FindDef("human");
    if (hd < 0) {
      out.why = "no `human` mob def";
      return out;
    }
    const MobDef& md = c.mobs.Defs()[(size_t)hd];
    auto spawn = [&](Vec3 foot, const char* profile) -> uint64_t {
      const uint64_t m = c.mobs.Spawn(hd, IVec3{(int)(foot.x - md.worldSize.x * 0.5f), f.y,
                                                (int)(foot.z - md.worldSize.z * 0.5f)});
      if (m != 0) c.mobs.SetMobBehavior(m, profile);
      return m;
    };
    out.zombie = spawn(Vec3{home.x + 14.0f, 0, home.z + 4.0f}, "zombie");
    out.villager = spawn(Vec3{home.x - 4.0f, 0, home.z - 34.0f}, "villager");
    const int guardTicks = (int)BaselineNumber("demonContract.guardTicks", 300);
    for (int i = 0; i < guardTicks; i++) {
      // The summoner strikes at the zombie every second (attacked_by(me)).
      TickInput in = base;
      if (const Mob* z = c.mobs.FindMobById(out.zombie); z && z->Def() && i % 30 == 0) {
        const Vec3 zc = z->Origin() + z->Def()->worldSize * 0.5f;
        in.lookFwd = (zc - ses.player.EyePos()).normalized();
        in.SetPressed(TB_ATTACK, true);
      }
      tick({}, in);
      record();
      if (const demon::Pact* p = demon::PactOf(rig.Authority(), id)) {
        if (p->guardTarget == out.zombie) out.guardTicks++;
        else if (p->guardTarget != 0) out.wrongTargetTicks++;
      }
      if (!c.mobs.IsAlive(out.zombie)) break;
    }
    if (const demon::Pact* p = demon::PactOf(rig.Authority(), id))
      for (const auto& [t, n] : p->blowsAt)
        (t == out.zombie ? out.blowsAtZombie : out.blowsAtOthers) += n;
    return out;
  }

  // ---- Mode::Persist ---------------------------------------------------------------------
  for (int i = 0; i < 10; i++) tick();
  {
    const demon::Pact* p = demon::PactOf(rig.Authority(), id);
    out.leftBefore = p && p->expireTick ? (int64_t)p->expireTick - (int64_t)rig.tick : -1;
    out.upkeepBefore = p ? p->upkeep : -1;
  }
  out.idBefore = id;
  EntityIO io = MakeEntityIO(c.debris, c.mobs, nullptr);
  demon::AppendSaveSections(io, rig.Authority(), ses);
  std::vector<std::pair<const EntitySection*, std::vector<uint8_t>>> bytes;
  for (const EntitySection& sec : io.sections) {
    std::vector<uint8_t> b;
    sec.save(b);
    bytes.push_back({&sec, std::move(b)});
  }
  for (const EntitySection& sec : io.sections) sec.reset();
  ses.caster.contracts.clear();
  for (auto& [sec, b] : bytes)
    if (!sec->load(b.data(), b.size(), sec->version))
      std::printf("demon-contract: section %08x refused its own bytes\n", sec->id);
  for (int i = 0; i < 3; i++) tick();
  for (const LiveDemon& d : dw.live)
    if (d.pact) {
      out.restored = true;
      out.idAfter = d.mobId;
      out.restoredReleased = d.state == DemonState::Released;
      out.restoredPage = d.pact->page.name;
      out.leftAfter = d.pact->expireTick ? (int64_t)d.pact->expireTick - (int64_t)rig.tick : -1;
      out.upkeepAfter = d.pact->upkeep;
      id = d.mobId;
    }
  out.leftAfter = out.leftAfter < 0 ? -1 : out.leftAfter + 3;   // the three ticks since
  out.reservedAfterLoad = ses.caster.mana.reserved;
  for (const contract::Page& p : ses.caster.contracts) out.pageBack = out.pageBack || p.name == "kept page";
  // ---- the term runs out (test seam: the expiry tick brought near) ----------------------
  if (demon::Pact* p = demon::PactOf(rig.Authority(), id)) p->expireTick = rig.tick + 20;
  for (int i = 0; i < 26; i++) tick();
  out.expiredGone = !c.mobs.IsAlive(id);
  out.reservedAfterExpiry = ses.caster.mana.reserved;
  return out;
}

}  // namespace

Status GateDemonContract(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("demon-contract: FAILED %s\n", what.c_str());
    }
  };
  for (const char* m : {"salt", "stone", "tallow", "candle_flame"})
    if (CMat(c, m) == 0) {
      detail = std::string("no `") + m + "` material";
      return Status::Fail;
    }
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "glyph load failed: " + gerr;
    return Status::Fail;
  }
  const int gSummon = lib.Find("summon_skerrick");
  check(gSummon >= 0, "summon_skerrick exists");

  // ---- L. the language and the tariff ----------------------------------------------------
  const contract::Content& content = contract::GetContent(true);
  check(content.log.empty(), "the contract content loads clean: " + content.log);
  check(content.stock.size() >= 3, Format("three stock pages (%zu)", content.stock.size()));
  const contract::Page* sv = content.FindStock("Servant, one day");
  const contract::Page* bg = content.FindStock("Bodyguard, one day");
  const contract::Page* fe = content.FindStock("Fetch, one day");
  check(sv && bg && fe, "Servant, Bodyguard and Fetch ship");
  {
    contract::Selector s;
    std::string e;
    check(contract::ParseSelector("attacked_by(me) & hostile_to(me) & !faction(villager)", s, e) &&
              s.nodes.size() == 8,
          Format("the plan's selector parses (%zu nodes) %s", s.nodes.size(), e.c_str()));
    e.clear();
    check(!contract::ParseSelector("attacked_by(me", s, e) && !e.empty(),
          "a broken selector names its fault (" + e + ")");
    std::vector<contract::Cond> cs;
    e.clear();
    check(contract::ParseTrigger("count(hostile_to(me)) > 0 & dist < 6", {}, cs, e) && cs.size() == 2,
          "a trigger with a selector parses: " + e);
    contract::Effect ef;
    e.clear();
    check(contract::ParseEffect("fetched+1", {"fetched"}, ef, e) && ef.counter == 0 && ef.value == 1,
          "an effect names a counter");
  }
  const int wServant = sv ? contract::Weigh(*sv, content.tariff).total : -1;
  const int wBody = bg ? contract::Weigh(*bg, content.tariff).total : -1;
  const int wFetch = fe ? contract::Weigh(*fe, content.tariff).total : -1;
  std::printf("demon-contract: stock weights: servant %d, bodyguard %d, fetch %d\n", wServant,
              wBody, wFetch);
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }

  const int r = (int)BaselineNumber("demonContract.ringCells", 14);
  const float followNearM = (float)BaselineNumber("demonContract.followNearM", 3.5);
  const std::string weatherWas = weather::Override();
  weather::SetOverride("clear");
  const uint64_t ids0 = c.mobs.NextIdCounter();
  const Out s1 = RunContract(c, Mode::Servant, r, lib, gSummon, 97000u, ids0);
  const Out s2 = RunContract(c, Mode::Servant, r, lib, gSummon, 97000u, ids0);
  const Out b = RunContract(c, Mode::Bodyguard, r, lib, gSummon, 98000u, ids0);
  const Out p = RunContract(c, Mode::Persist, r, lib, gSummon, 99000u, ids0);
  weather::SetOverride(weatherWas);
  CRegenerate(c);

  check(s1.arrived && b.arrived && p.arrived,
        "Skerrick arrived contained in every run (" + s1.why + " / " + b.why + " / " + p.why + ")");
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }
  const std::string circle = Format("circle %d vs power %d", s1.strength, s1.power);
  std::printf("demon-contract: %s\n", circle.c_str());
  // S1. talk
  check(s1.talkOpen && s1.talkNode == "greet",
        "S1: T opens demon_skerrick at greet (node '" + s1.talkNode + "')");
  check(!s1.tellText.empty() && s1.tellText.find("{tell}") == std::string::npos,
        "S1: {tell} is filled: " + s1.tellText);
  // S2. refused
  check(s1.heavy.result == demon::PresentResult::Refused && s1.heavyStillContained &&
            s1.heavyNoPact && s1.refusedNode == "refused",
        Format("S2: a page of weight %d over the margin (%d) is refused, still contained, the "
               "dialogue at '%s' (result %s)",
               s1.heavy.weight, s1.heavy.margin, s1.refusedNode.c_str(),
               demon::PresentResultName(s1.heavy.result)));
  // S3. bound
  check(s1.servant.result == demon::PresentResult::Bound && s1.boundNode == "bound_now" &&
            s1.servant.weight == wServant,
        Format("S3: the stock servant (weight %d, margin %d) binds, dialogue at '%s' (result %s)",
               s1.servant.weight, s1.servant.margin, s1.boundNode.c_str(),
               demon::PresentResultName(s1.servant.result)));
  check(s1.reservedBound == s1.servant.upkeep && s1.servant.upkeep > 0,
        Format("S3: the upkeep is reserved out of the mana max (%d reserved, upkeep %d)",
               s1.reservedBound, s1.servant.upkeep));
  // S4. release
  check(s1.released && s1.releasedProfile,
        "S4: the dialogue's release lets him out, bound, on the imp_bound floor");
  // S5. follow
  check(s1.distAfter <= MetresToCells(followNearM) && s1.distAfter < s1.distBefore,
        Format("S5: he follows (%.1f -> %.1f m, near is %.1f m)", CellsToMetres(s1.distBefore),
               CellsToMetres(s1.distAfter), followNearM));
  // S6. never strikes the summoner
  check(s1.allowedAtSummoner == 0,
        Format("S6: no blow at the summoner got through (%u asked, all refused; %u unsanctioned)",
               s1.blowsAtSummoner, s1.blowsUnsanctioned));
  // S7. dismiss
  check(s1.dismissedGone && s1.reservedAfterDismiss == 0,
        Format("S7: dismissed: gone, reservation freed (%d reserved)", s1.reservedAfterDismiss));
  check(!s1.trace.empty() && s1.trace == s2.trace,
        Format("S: the twice-run trace differs (%zu vs %zu rows)", s1.trace.size(),
               s2.trace.size()));
  // B. bodyguard
  check(b.released && b.servant.result == demon::PresentResult::Bound,
        Format("B: the bodyguard page (weight %d, margin %d) binds and he is released",
               b.servant.weight, b.servant.margin));
  check(b.guardTicks > 0 && b.wrongTargetTicks == 0,
        Format("B: he guards against the zombie (%d ticks) and never targets anyone else (%d)",
               b.guardTicks, b.wrongTargetTicks));
  check(b.blowsAtZombie >= (uint32_t)BaselineNumber("demonContract.guardBlowsMin", 1) &&
            b.blowsAtOthers == 0,
        Format("B: blows land on the zombie only (%u at it, %u at anyone else)", b.blowsAtZombie,
               b.blowsAtOthers));
  // P. persistence + expiry
  check(p.restored && p.restoredReleased && p.restoredPage == "Servant, one day" &&
            p.idAfter != p.idBefore,
        Format("P: a bound servant round-trips (restored %d, page '%s', id %llu -> %llu)",
               (int)p.restored, p.restoredPage.c_str(), (unsigned long long)p.idBefore,
               (unsigned long long)p.idAfter));
  check(p.leftAfter >= 0 && std::llabs(p.leftAfter - p.leftBefore) <= 2 &&
            p.upkeepAfter == p.upkeepBefore && p.reservedAfterLoad == p.upkeepBefore,
        Format("P: the term left (%lld -> %lld ticks) and the upkeep (%d -> %d, %d reserved) "
               "survive",
               (long long)p.leftBefore, (long long)p.leftAfter, p.upkeepBefore, p.upkeepAfter,
               p.reservedAfterLoad));
  check(p.pageBack, "P: the player's own contract page round-trips");
  check(p.expiredGone && p.reservedAfterExpiry == 0,
        Format("P: the term runs out: he departs and the reservation is freed (%d reserved)",
               p.reservedAfterExpiry));
  RecordObserved("demonContract.strength", s1.strength);
  RecordObserved("demonContract.followEndM", CellsToMetres(s1.distAfter));
  detail = Format(
      "%s; stock weights servant %d / bodyguard %d / fetch %d; heavy page %d refused (margin %d); "
      "servant bound (margin %d, upkeep %d reserved), released, followed %.1f -> %.1f m, %u blows "
      "at you all refused, dismissed (reserved %d); trace %zu rows %s; bodyguard %d ticks on the "
      "zombie, %u blows at it, %u elsewhere; save/load: page '%s', term %lld -> %lld, upkeep %d; "
      "expiry: gone %d, reserved %d",
      circle.c_str(), wServant, wBody, wFetch, s1.heavy.weight, s1.heavy.margin,
      s1.servant.margin, s1.reservedBound, CellsToMetres(s1.distBefore),
      CellsToMetres(s1.distAfter), s1.blowsAtSummoner, s1.reservedAfterDismiss, s1.trace.size(),
      s1.trace == s2.trace ? "identical twice" : "DIFFERS", b.guardTicks, b.blowsAtZombie,
      b.blowsAtOthers, p.restoredPage.c_str(), (long long)p.leftBefore, (long long)p.leftAfter,
      p.upkeepAfter, (int)p.expiredGone, p.reservedAfterExpiry);
  if (!fails.empty()) detail += "; FAILED: " + fails;
  std::printf("demon-contract: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace selftest
