// selftest_distant_calib.cpp -- DISTANT NPCs, PACKAGE D2a: the duel matrix
// that measures full-fidelity combat, and the unit arm of the pure resolver
// (docs/PLAN_distant_npcs.md §2.6, §3 D2a, §4 `distant-calib-matrix`).
//
//   distant-calib-matrix
//
//     A. THE MATRIX. Small full-fidelity fights through THE tick
//        (support::TickRig): weapon class x armour x creature kind, one bout
//        per arena, every arena of a round in ONE world at once (pitch 110
//        voxels: no creature's keep range reaches the next arena), a round per
//        seed (a fresh world, a different id base, start tick and arena
//        rotation). The stroke log (game/stroke_log.h) records every stroke
//        STARTED and its outcome; the archers' looses and their arrows' fates
//        are read off the authority's own event logs; a bout is won when the
//        other side is all dead (time-to-kill = ticks from the start to the
//        last death).
//     B. TWO PROBES in the ranged round: a passive dummy cut once by a direct
//        Mob::Damage (the bleed law: what an owed budget drains, and how
//        fast), and a passive dummy set alight (the burn: fraction per tick,
//        hp the flames eat, how long a body burns).
//     C. THE REDUCER turns all of it into a DistantCalib (strokelog::
//        ReduceStrokes + the probes + the shots + the matchups) and, under
//        SANDVOX_DISTANT_CALIB=<out.json>, WRITES it (the committed
//        assets/mobs/distant_combat.json is that file), reloads it and
//        requires the reload to agree.
//     D. THE SHIPPED TABLE loads clean (LoadDistantCombat: no warnings).
//     E. THE RESOLVER'S UNIT ARM: synthetic kits and a synthetic table, 10k
//        rolls each of ResolveStrike / ResolveArrow: hit rate, block rate,
//        role mix and mean hp match the table within tolerance; twice-run
//        bit-identical and order-independent (the rolls reversed give the same
//        answers); bleed pays out what it owes; the burn cap matches
//        Mob::BurnHealthCapFor; the death rules; the style draw's filters;
//        LimbRoleOf on the real rigs (human, zombie, snake, shark, piranha,
//        naga).
//     F. THE LOG PERTURBS NOTHING: one duel run twice from the same fresh
//        state, the log OFF then ON, every creature's hp and origin bits and
//        the whole op batch identical tick for tick -- and the ON arm logged.
//
// THE DEFAULT IS CHEAP: the bouts marked `inDefault`, `distantCalib.rounds`
// rounds of `distantCalib.ticks` (tests/baseline.json). SANDVOX_DISTANT_CALIB
// runs every bout for `distantCalib.calibRounds` rounds of
// `distantCalib.calibTicks` and writes the table.
//
// NO AXE OR SPEAR exists in the item library (items.json: sword, cleaver,
// shortsword, dagger, mace, bow): the cleaver (long, the heaviest edge) stands
// in for the axe and the dagger (short, the thrust-heavy form) for the spear.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "game/ai_behavior.h"
#include "game/arrows.h"
#include "game/distant_combat.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/mob_archery.h"
#include "game/session.h"
#include "game/shellresponse.h"
#include "game/stroke_log.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

// =============================================================================
// The matrix
// =============================================================================

struct Loadout {
  const char* def = "human";
  const char* profile = "duelist";
  const char* weapon = nullptr;   // right fist
  const char* shield = nullptr;   // left fist
  bool plate = false;             // the iron set
  bool archer = false;            // ArmArcher (bow + sidearm + quiver)
  const char* desc = "";
};

struct BoutSpec {
  const char* name;
  const char* matchup;            // non-null = an ASYMMETRIC matchup row
  Loadout a, b;
  int aCount = 1, bCount = 1;
  float gap = 14.0f;              // between the two lines, world voxels
  bool ranged = false;            // an archer bout: the ranged round
  bool inDefault = false;
};

// FACTIONS: side A is the `monster` faction (duelist, guardian, zombie, snake,
// archer), side B `quarry` (duelist_blue, training_dummy). Side A of an
// asymmetric matchup is the STRONGER side the plan's distant-calibrate gate
// compares the win rate of.
const BoutSpec kBouts[] = {
    {"sword_duel", nullptr,
     {"human", "duelist", "sword", nullptr, false, false, "duelist+sword"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 14.0f, false, true},
    {"cleaver_vs_sword", nullptr,
     {"human", "duelist", "cleaver", nullptr, false, false, "duelist+cleaver"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 14.0f, false, false},
    {"mace_vs_sword", nullptr,
     {"human", "duelist", "mace", nullptr, false, false, "duelist+mace"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 14.0f, false, false},
    {"dagger_vs_sword", nullptr,
     {"human", "duelist", "dagger", nullptr, false, false, "duelist+dagger"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 14.0f, false, false},
    {"unarmed_duel", nullptr,
     {"human", "duelist", nullptr, nullptr, false, false, "duelist, fists"},
     {"human", "duelist_blue", nullptr, nullptr, false, false, "duelist, fists"},
     1, 1, 12.0f, false, true},
    {"armoured_vs_unarmoured", "armoured_vs_unarmoured",
     {"human", "duelist", "sword", nullptr, true, false, "duelist+sword+iron plate"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 14.0f, false, true},
    {"mace_vs_plate", "mace_vs_plate",
     {"human", "duelist", "mace", nullptr, false, false, "duelist+mace"},
     {"human", "duelist_blue", "sword", nullptr, true, false, "duelist+sword+iron plate"},
     1, 1, 14.0f, false, false},
    {"sword_vs_zombie", "sword_vs_zombie",
     {"zombie", "zombie", nullptr, nullptr, false, false, "zombie"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 14.0f, false, true},
    {"zombie_vs_plate", nullptr,
     {"zombie", "zombie", nullptr, nullptr, false, false, "zombie"},
     {"human", "duelist_blue", "sword", nullptr, true, false, "duelist+sword+iron plate"},
     1, 1, 14.0f, false, false},
    {"snake_vs_sword", nullptr,
     {"snake", "snake", nullptr, nullptr, false, false, "snake"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 18.0f, false, true},
    {"shield_duel", nullptr,
     {"human", "guardian", "sword", "round_shield", false, false, "guardian+sword+round_shield"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 12.0f, false, true},
    {"brawl_4v3", "brawl_4v3",
     {"human", "duelist", "sword", nullptr, false, false, "duelist+sword"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     4, 3, 16.0f, false, true},
    {"archer_vs_melee", "archer_vs_melee",
     {"human", "archer", "sword", nullptr, false, true, "archer (bow, sword sidearm)"},
     {"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"},
     1, 1, 55.0f, true, true},
    {"archer_vs_dummy", nullptr,
     {"human", "archer", "sword", nullptr, false, true, "archer (bow, sword sidearm)"},
     {"human", "training_dummy", nullptr, nullptr, false, false, "standing dummy"},
     1, 1, 55.0f, true, false},
    {"archer_vs_plate_dummy", nullptr,
     {"human", "archer", "sword", nullptr, false, true, "archer (bow, sword sidearm)"},
     {"human", "training_dummy", nullptr, nullptr, true, false, "standing dummy in iron plate"},
     1, 1, 55.0f, true, false},
};
constexpr int kBoutCount = (int)(sizeof(kBouts) / sizeof(kBouts[0]));

// The window's centre in voxels (the anchor every arena is laid out from).
IVec3 WindowCentre(const World& world) {
  const IVec3 o = world.WindowOrigin();
  return IVec3{(o.x + (int)kNChunk / 2) * (int)kChunk, 0,
               (o.z + (int)kNChunk / 2) * (int)kChunk};
}

// The flattest dry 28x28 patch within 24 voxels of (cx, cz): two fighters
// need the same floor, and a fixture lake under one of them is a swim.
IVec3 Arena(int cx, int cz) {
  int bx = cx, bz = cz, best = INT32_MAX;
  for (int oz = -24; oz <= 24; oz += 8)
    for (int ox = -24; ox <= 24; ox += 8) {
      int lo = INT32_MAX, hi = INT32_MIN;
      bool wet = false;
      for (int dz = -14; dz <= 14; dz += 7)
        for (int dx = -14; dx <= 14; dx += 7) {
          const World::Column col =
              World::TerrainColumn(cx + ox + dx, cz + oz + dz, kDefaultSeed);
          lo = std::min(lo, col.h);
          hi = std::max(hi, col.h);
          wet = wet || col.water != INT32_MIN;
        }
      const int score = (hi - lo) + (wet ? 1000 : 0);
      if (score < best) {
        best = score;
        bx = cx + ox;
        bz = cz + oz;
      }
    }
  return IVec3{bx, World::TerrainHeight(bx, bz, kDefaultSeed), bz};
}

// A fresh process's state for one round: every body gone, Jolt's id history
// and the creature ids rewound to `idBase`, a clear dawn sky, worldgen again
// (selftest_archer.cpp Fresh, which mob-cap64-twice also relies on).
void Fresh(Ctx& c, uint64_t idBase) {
  c.mobs.Reset();
  c.debris.Reset();
  c.phys.ResetBirthOrdinals();
  c.phys.ResetBodyIdHistory();
  c.mobs.SetNextIdCounter(idBase);
  c.mobs.SetWeatherRain(0u);
  c.mobs.SetRainSlope(0, 0);
  c.mobs.SetDayPhase(0u);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

bool WearPlate(Ctx& c, Mob& m) {
  bool all = true;
  for (const char* n : {"iron_helm", "iron_cuirass", "iron_greaves", "iron_sabatons",
                        "iron_gauntlets"}) {
    const ItemDef* it = c.items.Named(n);
    bool worn = false;
    for (int sl = 0; it != nullptr && sl < kEquipSlotCount && !worn; sl++)
      worn = EquipSlotAccepts(sl, it->kind) && m.WearItem(it, sl);
    all = all && worn;
  }
  return all;
}

uint64_t SpawnOne(Ctx& c, const Loadout& L, int x, int z, std::string& why) {
  const int def = c.mobs.FindDef(L.def);
  if (def < 0) {
    why = std::string("no def \"") + L.def + "\"";
    return 0;
  }
  const int y = World::TerrainHeight(x, z, kDefaultSeed) + 1;
  const uint64_t id = c.mobs.Spawn(def, {x, y, z});
  if (id == 0) {
    why = std::string("Spawn refused ") + L.def;
    return 0;
  }
  if (!c.mobs.SetMobBehavior(id, L.profile)) {
    why = std::string("no profile \"") + L.profile + "\"";
    return 0;
  }
  if (L.weapon != nullptr) {
    const ItemDef* w = c.items.Named(L.weapon);
    if (w == nullptr || !c.mobs.EquipItem(id, w)) {
      why = std::string("could not arm with ") + L.weapon;
      return 0;
    }
  }
  if (L.shield != nullptr) {
    const ItemDef* s = c.items.Named(L.shield);
    if (s == nullptr || !c.mobs.EquipItem(id, s, "held_left")) {
      why = std::string("could not hold ") + L.shield;
      return 0;
    }
  }
  if (L.plate) {
    Mob* m = c.mobs.FindMobById(id);
    if (m == nullptr || !WearPlate(c, *m)) {
      why = "could not dress in iron plate";
      return 0;
    }
  }
  if (L.archer) {
    std::string note;
    if (ArmArcher(c.mobs, id, c.items, &note) < 0) {
      why = "ArmArcher refused: " + note;
      return 0;
    }
  }
  return id;
}

struct Bout {
  const BoutSpec* spec = nullptr;
  IVec3 at{};
  std::vector<uint64_t> a, b;
  int endTick = -1;     // fight tick of the last death, -1 = not over
  int winner = 0;       // 1 = A, 2 = B, 0 = draw / running
  // Each side's summed TotalHp at the start of the fight and when it ended
  // (the last death, or the timeout): the damage race (MatchupCalib::hpLeft*).
  float hp0A = 0, hp0B = 0, hpEndA = -1, hpEndB = -1;
};

float SideHp(MobSystem& mobs, const std::vector<uint64_t>& ids) {
  float s = 0.0f;
  for (uint64_t id : ids) s += std::max(0.0f, mobs.TotalHp(id));
  return s;
}

// One arrow an archer loosed. `hit` is CONTACT with the target's body -- an
// arrow that glanced off plate met the target and the plate stopped it, which
// is armour, not a miss -- and `hp` is the LIFE it cost (the target's TotalHp
// drop on the tick it struck, less that tick's blood loss: PierceResult's
// hpCharged is the struck SLOT's, which for a cuirass is the shell's).
struct Shot {
  uint32_t tick = 0, seed = 0;
  uint64_t shooter = 0, target = 0;
  float distance = 0, targetSpeed = 0, speed = 0;
  bool closed = false, hit = false, glanced = false, otherHit = false;
  float hp = 0;
  distant::LimbRole role = distant::LimbRole::Other;
  float hardness = 0;   // the struck limb's shell, at the loose
  std::vector<float> limbHardness;
};

struct MatchAcc {
  int n = 0, winA = 0, winB = 0, draw = 0;
  double hpA = 0, hpB = 0;   // summed end/start hp fractions per side
  double ttk = 0, ttk2 = 0;
  int ttkN = 0;
  int timeout = 0;
};

struct BleedProbe {
  uint64_t id = 0;
  float owedHp = 0;              // the budget the blow opened, in hp
  std::vector<float> loss;       // hp of blood per tick after the blow
  float remainingHp = 0;         // owed still in the wound at the end
  bool hitDone = false;
  float lost0 = 0;
};

struct BurnProbe {
  uint64_t id = 0;
  bool lit = false;
  int burningTicks = 0, deathTick = -1, lastBurning = -1;
  float frac0 = 0, frac1 = 0, burnHp0 = 0, burnHp1 = 0;
  uint32_t took = 0;
};

// One round of the matrix: one fresh world, the bouts laid out in arenas, run
// together through THE tick until every bout is over or `ticks` ran out.
struct RoundOut {
  bool ok = true;
  std::string why;
  int ticksRun = 0;
  std::vector<Bout> bouts;
  std::vector<Shot> shots;
};

// Track every creature's planar speed (world voxels/s, over the last tick).
struct Tracker {
  std::map<uint64_t, Vec3> last;
  std::map<uint64_t, float> speed;
  void Step(MobSystem& mobs, const std::vector<uint64_t>& ids) {
    for (uint64_t id : ids) {
      const Vec3 o = mobs.MobOrigin(id);
      auto it = last.find(id);
      if (it != last.end()) {
        const float dx = o.x - it->second.x, dz = o.z - it->second.z;
        const float v = std::sqrt(dx * dx + dz * dz) * 30.0f;
        float& s = speed[id];
        s = s * 0.6f + v * 0.4f;   // a few ticks of smoothing
      }
      last[id] = o;
    }
  }
};

Vec3 CentreOf(MobSystem& mobs, uint64_t id) {
  Vec3 lo{}, hi{};
  if (mobs.MobBodyBox(id, lo, hi)) return (lo + hi) * 0.5f;
  return mobs.MobOrigin(id);
}

// Face every creature of a bout at its nearest foe (spawn gives heading 0, so
// two lines along +Z would stand back to back: the duel gate's note).
void FaceFoes(Ctx& c, const Bout& b) {
  auto face = [&](const std::vector<uint64_t>& us, const std::vector<uint64_t>& them) {
    for (uint64_t u : us) {
      float best = 1e30f;
      Vec3 at{};
      const Vec3 pu = c.mobs.MobOrigin(u);
      for (uint64_t t : them) {
        const Vec3 pt = c.mobs.MobOrigin(t);
        const float d = (pu.x - pt.x) * (pu.x - pt.x) + (pu.z - pt.z) * (pu.z - pt.z);
        if (d < best) {
          best = d;
          at = pt;
        }
      }
      if (best < 1e30f) c.mobs.SetHeading(u, std::atan2(at.x - pu.x, at.z - pu.z));
    }
  };
  face(b.a, b.b);
  face(b.b, b.a);
}

int TorsoOf(const Mob& m) {
  int best = -1;
  float hp = -1.0f;
  for (int i = 0; i < m.AppendedBase(); i++) {
    const MobLimbDef& ld = m.LimbDefAt(i);
    if (distant::LimbRoleOf(ld.tag, ld.name, ld.bloodless) != distant::LimbRole::Torso)
      continue;
    if (ld.hp > hp) {
      hp = ld.hp;
      best = i;
    }
  }
  return best;
}

RoundOut RunRound(Ctx& c, const std::vector<const BoutSpec*>& specs, int round,
                  int ticks, bool rangedRound, BleedProbe* bleed, BurnProbe* burn,
                  strokelog::Sink& sink) {
  RoundOut out;
  const uint64_t idBase = 1 + 10000ull * (uint64_t)round + (rangedRound ? 5000u : 0u);
  Fresh(c, idBase);
  const IVec3 centre = WindowCentre(c.world);
  // ---- THE ARENAS --------------------------------------------------------------
  // Melee: a 4 x 4 grid at pitch 110 (no keep range -- the duelist's 46 x 1.45
  // = 67 is the longest of the melee profiles -- reaches the next arena),
  // rotated by the round so a bout meets different ground each seed. Ranged:
  // the archers' sight is 160 (keep 200), so the two archer bouts take
  // opposite corners with their TARGETS outermost (an archer backs off toward
  // the centre, never out of the window) and the probes the other two.
  std::vector<IVec3> grid;
  if (!rangedRound) {
    for (int gz = 0; gz < 4; gz++)
      for (int gx = 0; gx < 4; gx++)
        grid.push_back({centre.x - 165 + 110 * gx, 0, centre.z - 165 + 110 * gz});
    std::rotate(grid.begin(), grid.begin() + (round * 5) % (int)grid.size(), grid.end());
  } else {
    grid = {{centre.x - 150, 0, centre.z - 150}, {centre.x + 150, 0, centre.z + 150},
            {centre.x + 150, 0, centre.z - 150}, {centre.x - 150, 0, centre.z + 150}};
  }
  std::vector<uint64_t> everyone;
  size_t slot = 0;
  for (const BoutSpec* sp : specs) {
    if (slot >= grid.size()) break;
    Bout b;
    b.spec = sp;
    const IVec3 g = grid[slot++];
    b.at = Arena(g.x, g.z);
    // A seeded nudge so the same bout in two rounds does not start on the
    // same voxel (the ids already differ; this varies the geometry too).
    const uint32_t h = rng::Hash3(0xD157CA1u, (uint32_t)round, (uint32_t)slot);
    const int jx = (int)(h % 5u) - 2, jz = (int)((h >> 8) % 5u) - 2;
    // Lines along Z; a ranged bout's target line is the OUTER one (away from
    // the window centre), so the archer gives ground inward.
    int dirZ = 1;
    if (sp->ranged) dirZ = b.at.z < centre.z ? -1 : 1;
    const int half = (int)std::lround(sp->gap * 0.5f);
    for (int k = 0; k < sp->aCount; k++) {
      const int x = b.at.x + jx + (k - (sp->aCount - 1) / 2) * 6;
      const int z = b.at.z + jz - dirZ * half;
      const uint64_t id = SpawnOne(c, sp->a, x, z, out.why);
      if (id == 0) {
        out.ok = false;
        return out;
      }
      b.a.push_back(id);
    }
    for (int k = 0; k < sp->bCount; k++) {
      const int x = b.at.x + jx + (k - (sp->bCount - 1) / 2) * 6;
      const int z = b.at.z + jz + dirZ * half;
      const uint64_t id = SpawnOne(c, sp->b, x, z, out.why);
      if (id == 0) {
        out.ok = false;
        return out;
      }
      b.b.push_back(id);
    }
    for (uint64_t id : b.a) everyone.push_back(id);
    for (uint64_t id : b.b) everyone.push_back(id);
    out.bouts.push_back(std::move(b));
  }
  // ---- THE PROBES (ranged round only): passive `dummy`s of the archers' own
  // faction, so nothing in the stage targets them.
  auto probeAt = [&](size_t s) -> IVec3 {
    const IVec3 g = s < grid.size() ? grid[s] : IVec3{centre.x, 0, centre.z};
    return Arena(g.x, g.z);
  };
  const Loadout dummy{"human", "dummy", nullptr, nullptr, false, false, "probe"};
  if (bleed != nullptr) {
    const IVec3 p = probeAt(slot++);
    bleed->id = SpawnOne(c, dummy, p.x, p.z, out.why);
    if (bleed->id == 0) {
      out.ok = false;
      return out;
    }
  }
  if (burn != nullptr) {
    const IVec3 p = probeAt(slot++);
    burn->id = SpawnOne(c, dummy, p.x, p.z, out.why);
    if (burn->id == 0) {
      out.ok = false;
      return out;
    }
  }

  support::TickRig rig(c, 31000u + 977u * (uint32_t)round + (rangedRound ? 400u : 0u),
                       IVec3{centre.x >> 4, (centre.y >> 4) + 8, centre.z >> 4});
  // ...the mirror's centre: the window's middle, at ground height over it.
  rig.SetFixtureChunk(IVec3{centre.x >> 4,
                            World::TerrainHeight(centre.x, centre.z, kDefaultSeed) >> 4,
                            centre.z >> 4});
  for (int i = 0; i < 4; i++) support::RunTicks(rig, 1);   // rigs onto the ground
  for (Bout& b : out.bouts) {
    FaceFoes(c, b);
    b.hp0A = SideHp(c.mobs, b.a);
    b.hp0B = SideHp(c.mobs, b.b);
  }
  c.mobs.ClearAttackRequests();
  c.mobs.ClearBlockEvents();
  rig.Authority().archers.events.clear();
  rig.Authority().arrows.ClearEvents();

  Tracker track;
  track.Step(c.mobs, everyone);
  std::map<uint32_t, size_t> shotBySeed;
  std::map<uint64_t, std::pair<float, float>> prevHp;   // target -> (TotalHp, BloodLost)
  const float hpPerVox = CurrentTuning().gore.bleedHpPerVoxel;
  int t = 0;
  for (; t < ticks; t++) {
    // ---- THE PROBES' ONE ACT, on the first fight tick ----------------------
    // DIRECT CALLS ON PURPOSE (W2-O): a fixture injection, not a phase. The
    // bleed probe is a single Mob::Damage on the torso (the door every blade
    // goes through), so the budget it opens is known exactly and nothing else
    // ever hurts that body; the burn probe is MobSystem::IgniteLimb, the
    // direct-ignition entry a fire spell uses.
    if (t == 0 && bleed != nullptr && bleed->id != 0) {
      if (Mob* m = c.mobs.FindMobById(bleed->id)) {
        const int li = TorsoOf(*m);
        const uint64_t body = li >= 0 ? c.mobs.LimbBody(bleed->id, li) : 0;
        float before = 0.0f, after = 0.0f;
        for (int i = 0; i < m->AppendedBase(); i++)
          before += std::max(0.0f, c.mobs.LimbBleedBudget(bleed->id, i));
        if (body != 0)
          m->Damage(body, (float)BaselineNumber("distantCalib.bleedProbeHp", 30.0),
                    CentreOf(c.mobs, bleed->id), 0.0f, DamageCtx(DamageCause::Blade));
        for (int i = 0; i < m->AppendedBase(); i++)
          after += std::max(0.0f, c.mobs.LimbBleedBudget(bleed->id, i));
        bleed->owedHp = std::max(0.0f, after - before) * hpPerVox;
        bleed->lost0 = c.mobs.BloodLost(bleed->id);
        bleed->hitDone = body != 0;
      }
    }
    if (t == 0 && burn != nullptr && burn->id != 0) {
      if (Mob* m = c.mobs.FindMobById(burn->id)) {
        const int li = TorsoOf(*m);
        const uint32_t n = (uint32_t)BaselineNumber("distantCalib.burnProbeVoxels", 60.0);
        if (li >= 0) burn->took = c.mobs.IgniteLimb(burn->id, li, n);
        burn->lit = burn->took > 0;
        burn->frac0 = m->BurnFraction();
        burn->burnHp0 = m->HpLostBy(DamageCause::Burn);
      }
    }
    support::RunTicks(rig, 1);
    c.mobs.ClearAttackRequests();
    c.mobs.ClearBlockEvents();
    track.Step(c.mobs, everyone);
    // ---- THE ARCHERS: each loose, then each arrow's fate (by its seed) -----
    TickAuthorityCtx& A = rig.Authority();
    for (const ArcheryEvent& ev : A.archers.events) {
      if (ev.kind != ArcheryEvent::Kind::Loose) continue;
      Shot s;
      s.tick = ev.tick;
      s.seed = ev.seed;
      s.shooter = ev.mobId;
      s.target = ev.targetId;
      s.speed = ev.speed;
      s.distance = (CentreOf(c.mobs, ev.targetId) - ev.from).len();
      s.targetSpeed = track.speed.count(ev.targetId) ? track.speed[ev.targetId] : 0.0f;
      if (Mob* tm = c.mobs.FindMobById(ev.targetId))
        s.limbHardness = distant::ShellHardnessOfLimbs(*tm);
      shotBySeed[s.seed] = out.shots.size();
      if (!prevHp.count(s.target))
        prevHp[s.target] = {std::max(0.0f, c.mobs.TotalHp(s.target)),
                            c.mobs.BloodLost(s.target)};
      out.shots.push_back(std::move(s));
    }
    A.archers.events.clear();
    std::map<uint64_t, std::vector<size_t>> struckNow;   // target -> shots this tick
    for (const ArrowEvent& ev : A.arrows.Events()) {
      auto it = shotBySeed.find(ev.seed);
      if (it == shotBySeed.end()) continue;
      Shot& s = out.shots[it->second];
      if (ev.kind == ArrowEvent::Kind::Hit) {
        if (ev.mobId == s.target && ev.r.hit && !ev.r.missed) {
          // THE LIMB UNDER IT: a shell slot is placed on its host.
          if (!s.hit && ev.limb >= 0)
            if (Mob* tm = c.mobs.FindMobById(s.target)) {
              int li = ev.limb;
              bool shell = false;
              if (li >= tm->AppendedBase()) {
                li = tm->WornHostOf(li);
                shell = true;
              }
              if (li >= 0 && li < tm->AppendedBase()) {
                const MobLimbDef& ld = tm->LimbDefAt(li);
                s.role = distant::LimbRoleOf(ld.tag, ld.name, ld.bloodless);
                if ((size_t)li < s.limbHardness.size()) s.hardness = s.limbHardness[(size_t)li];
                if (shell && s.hardness <= 0.0f) s.hardness = 1.0f;   // a shell it met
              }
            }
          if (!s.hit) struckNow[s.target].push_back(it->second);
          s.hit = true;
          if (ev.r.outcome == PierceOutcome::Glanced) s.glanced = true;
        } else if (ev.mobId != 0 && ev.mobId != s.target) {
          s.otherHit = true;
        }
      } else {
        s.closed = true;
      }
    }
    A.arrows.ClearEvents();
    // THE LIFE IT COST: each struck target's TotalHp drop this tick, less its
    // blood loss this tick, shared by the arrows that met it this tick.
    for (const auto& kv : struckNow) {
      auto pv = prevHp.find(kv.first);
      if (pv == prevHp.end()) continue;
      const float hpNow = std::max(0.0f, c.mobs.TotalHp(kv.first));
      const float blood = (c.mobs.BloodLost(kv.first) - pv->second.second) * hpPerVox;
      const float drop = std::max(0.0f, pv->second.first - hpNow - std::max(0.0f, blood));
      for (size_t k : kv.second) out.shots[k].hp += drop / (float)kv.second.size();
    }
    for (auto& kv : prevHp)
      kv.second = {std::max(0.0f, c.mobs.TotalHp(kv.first)), c.mobs.BloodLost(kv.first)};
    // ---- THE PROBES, watched -------------------------------------------------
    if (bleed != nullptr && bleed->hitDone) {
      const float lost = c.mobs.BloodLost(bleed->id);
      bleed->loss.push_back(std::max(0.0f, lost - bleed->lost0) * hpPerVox);
      bleed->lost0 = lost;
    }
    if (burn != nullptr && burn->lit && burn->deathTick < 0) {
      if (Mob* m = c.mobs.FindMobById(burn->id)) {
        float hpFrac = 0, burning = 0;
        int lost = 0;
        m->BodyFacts(hpFrac, burning, lost);
        if (burning > 0.0f) {
          burn->burningTicks++;
          burn->lastBurning = t;
        }
        burn->frac1 = m->BurnFraction();
        burn->burnHp1 = m->HpLostBy(DamageCause::Burn);
        if (!m->Alive()) burn->deathTick = t;
      }
    }
    // ---- THE BOUTS --------------------------------------------------------------
    bool allOver = true;
    for (Bout& b : out.bouts) {
      if (b.endTick >= 0) continue;
      auto anyAlive = [&](const std::vector<uint64_t>& ids) {
        for (uint64_t id : ids)
          if (c.mobs.IsAlive(id)) return true;
        return false;
      };
      const bool aAlive = anyAlive(b.a), bAlive = anyAlive(b.b);
      if (!aAlive || !bAlive) {
        b.endTick = t + 1;
        b.winner = aAlive ? 1 : (bAlive ? 2 : 0);
        b.hpEndA = SideHp(c.mobs, b.a);
        b.hpEndB = SideHp(c.mobs, b.b);
        continue;
      }
      allOver = false;
    }
    // The probes want their whole window; a round with probes runs out its
    // ticks, one without stops when the last bout is decided.
    if (allOver && bleed == nullptr && burn == nullptr) {
      t++;
      break;
    }
  }
  out.ticksRun = t;
  for (Bout& b : out.bouts)
    if (b.hpEndA < 0.0f) {
      b.hpEndA = SideHp(c.mobs, b.a);
      b.hpEndB = SideHp(c.mobs, b.b);
    }
  if (bleed != nullptr && bleed->id != 0)
    if (Mob* m = c.mobs.FindMobById(bleed->id)) {
      float owed = 0.0f;
      for (int i = 0; i < m->AppendedBase(); i++)
        owed += std::max(0.0f, c.mobs.LimbBleedBudget(bleed->id, i));
      bleed->remainingHp = owed * hpPerVox;
    }
  sink.Flush(rig.tick);
  return out;
}

// =============================================================================
// E. The resolver's unit arm
// =============================================================================

distant::DistantKit SyntheticHuman(distant::GuardClass guard, float torsoShell) {
  distant::DistantKit k;
  k.def = "synthetic-human";
  k.guard = guard;
  k.slack = 2.0f;
  struct L {
    const char* n;
    distant::LimbRole r;
    float hp;
    bool vital;
  };
  const L limbs[] = {{"hips", distant::LimbRole::Torso, 180, true},
                     {"torso", distant::LimbRole::Torso, 180, true},
                     {"head", distant::LimbRole::Head, 135, true},
                     {"armU.L", distant::LimbRole::Arm, 48, false},
                     {"armL.L", distant::LimbRole::Arm, 39, false},
                     {"hand.L", distant::LimbRole::Arm, 27, false},
                     {"armU.R", distant::LimbRole::Arm, 48, false},
                     {"armL.R", distant::LimbRole::Arm, 39, false},
                     {"hand.R", distant::LimbRole::Arm, 27, false},
                     {"legU.L", distant::LimbRole::Leg, 66, false},
                     {"legL.L", distant::LimbRole::Leg, 54, false},
                     {"foot.L", distant::LimbRole::Leg, 72, false},
                     {"legU.R", distant::LimbRole::Leg, 66, false},
                     {"legL.R", distant::LimbRole::Leg, 54, false},
                     {"foot.R", distant::LimbRole::Leg, 72, false},
                     {"hair", distant::LimbRole::Other, 10, false}};
  for (const L& l : limbs) {
    distant::DistantLimb d;
    d.name = l.n;
    d.role = l.r;
    d.maxHp = l.hp;
    d.vital = l.vital;
    d.bloodless = std::string(l.n) == "hair";
    if (std::string(l.n) == "torso") d.shellHardness = torsoShell;
    k.limbs.push_back(d);
  }
  return k;
}

distant::DistantVitals FullVitals(const distant::DistantKit& k) {
  distant::DistantVitals v;
  for (const distant::DistantLimb& l : k.limbs) v.limbHp.push_back(l.maxHp);
  return v;
}

struct UnitResult {
  bool ok = true;
  int checks = 0;
  std::string fails;
};

UnitResult ResolverUnitArm(Ctx& c) {
  UnitResult u;
  auto check = [&](bool cond, const std::string& what) {
    u.checks++;
    if (!cond) {
      u.ok = false;
      u.fails += " [" + what + "]";
      std::printf("distant-calib-matrix: FAILED (unit) %s\n", what.c_str());
    }
  };
  const Tuning::Gear& gear = CurrentTuning().gear;
  const int rolls = (int)BaselineNumber("distantCalib.unitRolls", 10000);
  const double tol = BaselineNumber("distantCalib.unitTol", 0.03);
  const double hpTol = BaselineNumber("distantCalib.unitHpTol", 0.06);

  // ---- the synthetic table --------------------------------------------------
  distant::DistantCalib cal = distant::DistantCalibBuiltin();
  cal.reachSlack = 2.0f;
  cal.minSamples = 8;
  {
    distant::StyleCalib s;
    s.name = "synth_cut";
    s.hitRate = 0.6f;
    const float w[distant::kRoles] = {0.2f, 0.5f, 0.2f, 0.1f, 0, 0, 0};
    std::copy(w, w + distant::kRoles, s.roleWeights);
    s.meanPower = 1.5f;
    s.powerCv = 0.5f;
    distant::WeaponCalib wc;
    wc.weapon = "heavy";
    wc.n = 50;
    wc.hitRate = 0.3f;
    wc.meanPower = 3.0f;
    s.byWeapon.push_back(wc);
    cal.styles.push_back(s);
  }
  cal.blockChance[(int)distant::GuardClass::None] = 0.0f;
  cal.blockChance[(int)distant::GuardClass::Blade] = 0.1f;
  cal.blockChance[(int)distant::GuardClass::Shield] = 0.4f;
  cal.armour.exposure[(int)distant::StrikeKind::Cut] = 0.2f;
  cal.bleed.perHp[(int)distant::StrikeKind::Cut] = 0.8f;
  cal.bleed.decay = 0.95f;

  distant::DistantKit att;
  att.id = 77;
  att.slack = 2.0f;
  {
    distant::DistantStyle s;
    s.name = "synth_cut";
    s.weapon = "sword";
    s.reach = 10.0f;
    s.strike.cut = 14.0f;
    s.kind = distant::StrikeKind::Cut;
    att.styles.push_back(s);
    s.weapon = "heavy";   // the weapon row (n 50 >= minSamples) wins for it
    att.styles.push_back(s);
  }
  const distant::DistantKit bare = SyntheticHuman(distant::GuardClass::Blade, 0.0f);
  const distant::DistantKit plated = SyntheticHuman(distant::GuardClass::None, 80.0f);
  const distant::DistantVitals v0 = FullVitals(bare);

  // ---- 1. ResolveStrike over `rolls` ticks: rates, roles, hp ---------------
  auto runRolls = [&](const distant::DistantKit& def, int style, bool reverse,
                      std::vector<distant::StrikeOutcome>& outv) {
    outv.assign((size_t)rolls, {});
    for (int k = 0; k < rolls; k++) {
      const int i = reverse ? rolls - 1 - k : k;
      outv[(size_t)i] = distant::ResolveStrike(att, def, v0.limbHp, style, 8.0f,
                                               {att.id, 1000u + (uint32_t)i}, cal, gear);
    }
  };
  std::vector<distant::StrikeOutcome> A, B, R;
  runRolls(bare, 0, false, A);
  runRolls(bare, 0, false, B);
  runRolls(bare, 0, true, R);
  auto same = [](const distant::StrikeOutcome& x, const distant::StrikeOutcome& y) {
    return x.result == y.result && x.limb == y.limb && std::memcmp(&x.hp, &y.hp, 4) == 0 &&
           std::memcmp(&x.bleedAdded, &y.bleedAdded, 4) == 0;
  };
  bool twice = true, order = true;
  for (int i = 0; i < rolls; i++) {
    twice = twice && same(A[(size_t)i], B[(size_t)i]);
    order = order && same(A[(size_t)i], R[(size_t)i]);
  }
  check(twice, "ResolveStrike twice-run bit-identical");
  check(order, "ResolveStrike order-independent (reversed rolls agree)");
  int hits = 0, blocked = 0;
  double hpSum = 0;
  int roleN[distant::kRoles] = {};
  for (const distant::StrikeOutcome& o : A) {
    if (o.result == distant::StrikeResult::Blocked) blocked++;
    if (!o.hit) continue;
    hits++;
    hpSum += o.hp;
    roleN[(int)o.role]++;
  }
  const double hitP = (double)hits / rolls, blockP = (double)blocked / rolls;
  const double wantHit = 0.6 * (1.0 - 0.1), meanHp = hits ? hpSum / hits : 0.0;
  check(std::fabs(blockP - 0.1) <= tol, Format("block rate %.3f vs 0.100", blockP));
  check(std::fabs(hitP - wantHit) <= tol, Format("hit rate %.3f vs %.3f", hitP, wantHit));
  check(std::fabs(meanHp - 21.0) <= 21.0 * hpTol, Format("mean hp %.2f vs 21.00", meanHp));
  const float wantRole[4] = {0.2f, 0.5f, 0.2f, 0.1f};
  for (int r = 0; r < 4; r++) {
    const double f = hits ? (double)roleN[r] / hits : 0.0;
    check(std::fabs(f - wantRole[r]) <= tol,
          Format("role %s share %.3f vs %.3f", distant::LimbRoleName((distant::LimbRole)r), f,
                 wantRole[r]));
  }
  check(roleN[(int)distant::LimbRole::Other] == 0, "hair is never struck");
  // ...the weapon row (hitRate 0.3, meanPower 3.0) for the "heavy" style.
  {
    std::vector<distant::StrikeOutcome> H;
    runRolls(bare, 1, false, H);
    int h = 0;
    double s = 0;
    for (const auto& o : H)
      if (o.hit) {
        h++;
        s += o.hp;
      }
    const double p = (double)h / rolls, m = h ? s / h : 0.0;
    check(std::fabs(p - 0.3 * 0.9) <= tol, Format("weapon-row hit rate %.3f vs 0.270", p));
    check(std::fabs(m - 42.0) <= 42.0 * hpTol, Format("weapon-row mean hp %.2f vs 42.00", m));
  }
  // ...a plated torso: an edge on iron passes Blade's `passed` (0) plus the
  // exposure (0.2), so torso hits carry a fifth of the bare hp; a guard of
  // None blocks nothing.
  {
    std::vector<distant::StrikeOutcome> P;
    runRolls(plated, 0, false, P);
    int torsoHits = 0, blk = 0, other = 0;
    double torsoHp = 0, otherHp = 0;
    for (const auto& o : P) {
      if (o.result == distant::StrikeResult::Blocked) blk++;
      if (!o.hit) continue;
      if (plated.limbs[(size_t)o.limb].name == std::string("torso")) {
        torsoHits++;
        torsoHp += o.hp;
      } else {
        other++;
        otherHp += o.hp;
      }
    }
    check(blk == 0, "a defender with no guard blocks nothing");
    const double ratio = (torsoHits && other) ? (torsoHp / torsoHits) / (otherHp / other) : 0.0;
    check(std::fabs(ratio - 0.2) <= 0.2 * 0.15,
          Format("plated torso lets %.3f of a blow through (want 0.200)", ratio));
  }
  // ...out of reach, no target, a blocked rate for a shield.
  {
    const auto o = distant::ResolveStrike(att, bare, v0.limbHp, 0, 10.0f + 2.0f + 2.0f + 0.5f,
                                          {att.id, 5u}, cal, gear);
    check(o.result == distant::StrikeResult::OutOfReach, "past reach + slack is OutOfReach");
    distant::DistantVitals dead = v0;
    for (float& h : dead.limbHp) h = 0.0f;
    const auto n = distant::ResolveStrike(att, bare, dead.limbHp, 0, 5.0f, {att.id, 6u}, cal, gear);
    check(n.result == distant::StrikeResult::NoTarget, "a body with no live limb is NoTarget");
  }
  // ---- 2. a role the defender lacks folds into torso -------------------------
  {
    distant::DistantKit snake;
    for (int i = 0; i < 4; i++) {
      distant::DistantLimb l;
      l.role = i == 0 ? distant::LimbRole::Head
                      : (i == 3 ? distant::LimbRole::Tail : distant::LimbRole::Torso);
      l.maxHp = 16;
      l.vital = i == 0;
      snake.limbs.push_back(l);
    }
    const distant::DistantVitals sv = FullVitals(snake);
    int arms = 0, n = 0;
    for (int i = 0; i < 2000; i++) {
      const auto o = distant::ResolveStrike(att, snake, sv.limbHp, 0, 5.0f,
                                            {att.id, 20000u + (uint32_t)i}, cal, gear);
      if (!o.hit) continue;
      n++;
      arms += o.role == distant::LimbRole::Arm || o.role == distant::LimbRole::Leg;
    }
    check(n > 0 && arms == 0, "arm/leg draws fold into a snake's torso");
  }
  // ---- 3. the arrow ------------------------------------------------------------
  {
    distant::DistantKit shooter;
    shooter.id = 91;
    shooter.arrowDamage = 35.0f;
    shooter.arrowSpeed = 600.0f;
    cal.ranged.distKnots = {20.0f, 100.0f};
    cal.ranged.rateKnots = {0.8f, 0.4f};
    cal.ranged.speedPenalty = 0.5f;
    cal.ranged.speedRef = 40.0f;
    cal.ranged.damageFrac = 1.0f;
    int h = 0;
    double hp = 0;
    int flight = 0;
    for (int i = 0; i < rolls; i++) {
      const auto o = distant::ResolveArrow(shooter, bare, v0.limbHp, 60.0f, 20.0f,
                                           {shooter.id, 300u + (uint32_t)i}, cal, gear);
      flight = o.flightTicks;
      if (o.hit) {
        h++;
        hp += o.hp;
      }
    }
    const double want = 0.6 * (1.0 - 0.5 * 0.5);   // knot interp x speed penalty
    check(std::fabs((double)h / rolls - want) <= tol,
          Format("arrow hit rate %.3f vs %.3f", (double)h / rolls, want));
    check(flight == 3, Format("arrow flight %d ticks for 60 vox at 600 vox/s (want 3)", flight));
    check(h > 0 && std::fabs(hp / h - 35.0) < 0.01, "an arrow on bare flesh does its flat damage");
  }
  // ---- 4. bleed pays what it owes; the death rules -----------------------------
  {
    distant::DistantVitals v = v0;
    distant::StrikeOutcome o;
    o.hit = true;
    o.limb = 3;   // an arm
    o.hp = 20.0f;
    o.bleedAdded = 20.0f * 0.8f * (1.0f - cal.bleed.decay);
    distant::ApplyStrike(v, bare, o, cal);
    double paid = 0;
    for (int i = 0; i < 2000; i++) paid += distant::StepBleed(v, bare, cal);
    check(std::fabs(paid - 16.0) <= 16.0 * 0.01, Format("a 20 hp cut bleeds %.2f (owes 16)", paid));
    check(v.dead == distant::DistantDeath::None, "...and the body lives");
    distant::DistantVitals w = v0;
    w.limbHp[2] = 1.0f;   // the head
    o.limb = 2;
    o.hp = 5.0f;
    check(distant::ApplyStrike(w, bare, o, cal) == distant::DistantDeath::VitalLimb,
          "the head at zero is a death");
    distant::DistantVitals x = v0;
    x.bleedRate = 1e6f;
    distant::StepBleed(x, bare, cal);
    check(x.dead == distant::DistantDeath::BloodLoss, "a bill over the whole body bleeds it white");
  }
  // ---- 5. the burn cap curve is Mob::BurnHealthCapFor's ------------------------
  {
    const distant::DistantCalib b = distant::DistantCalibBuiltin();
    bool match = true;
    for (int i = 0; i <= 20; i++) {
      const float f = (float)i / 20.0f;
      match = match && std::fabs(distant::BurnCapFor(f, b.burn) - Mob::BurnHealthCapFor(f)) < 1e-5f;
    }
    check(match, "BurnCapFor == Mob::BurnHealthCapFor over 0..1");
    distant::DistantVitals v = v0;
    v.burnTicksLeft = 100000;
    distant::DistantCalib bc = b;
    bc.burn.fracPerTick = 0.01f;
    bc.burn.hpPerTick = 0.0f;
    int ticks = 0;
    while (v.dead == distant::DistantDeath::None && ticks < 1000) {
      distant::StepBurn(v, bare, bc);
      ticks++;
    }
    check(v.dead == distant::DistantDeath::Burns &&
              std::abs(ticks - (int)std::ceil(b.burn.deathFraction / 0.01f)) <= 1,
          Format("burning at 0.01/tick dies of burns at tick %d", ticks));
  }
  // ---- 6. the style draw's filters (PickAttackStyle's rules) -----------------
  {
    distant::DistantKit k;
    k.slack = 2.0f;
    distant::DistantStyle s;
    s.name = "long";
    s.reach = 20.0f;
    k.styles.push_back(s);
    s.name = "short";
    s.reach = 4.0f;
    k.styles.push_back(s);
    s.name = "punch";
    s.reach = 5.0f;
    s.fallback = true;
    k.styles.push_back(s);
    bool farOk = true, nearMix = false, noFallback = true;
    for (uint32_t t = 0; t < 200; t++) {
      farOk = farOk && distant::PickDistantStyle(k, 15.0f, 5, t) == 0;
      const int p = distant::PickDistantStyle(k, 3.0f, 5, t);
      nearMix = nearMix || p == 1;
      noFallback = noFallback && p != 2;
    }
    check(farOk, "at 15 only the 20-reach style is drawn");
    check(nearMix, "at 3 both real styles are drawn");
    check(noFallback, "a fallback is never drawn while a real style reaches");
    check(distant::PickDistantStyle(k, 40.0f, 5, 1) == -1, "nothing reaches 40");
  }
  // ---- 7. the table round-trips through its own writer and loader ------------
  {
    distant::DistantCalib t = cal;
    t.about = "unit-arm round trip";
    distant::MatchupCalib m;
    m.name = "x";
    m.winA = 0.75f;
    m.ttkTicks = 123.5f;
    t.matchups.push_back(m);
    const std::string path = "build/distant_calib_unit_roundtrip.json";
    {
      std::ofstream f(path, std::ios::binary);
      f << distant::DistantCalibToJson(t);
    }
    distant::DistantCalib r;
    std::string log;
    const bool loaded = distant::LoadDistantCombat(path, r, log);
    std::remove(path.c_str());
    check(loaded && log.empty(), "the written table loads clean" + (log.empty() ? "" : ": " + log));
    const distant::StyleCalib& rs = r.Style("synth_cut");
    check(std::fabs(rs.hitRate - 0.6f) < 1e-4f && std::fabs(rs.meanPower - 1.5f) < 1e-4f &&
              rs.byWeapon.size() == 1 && std::fabs(r.blockChance[2] - 0.4f) < 1e-4f &&
              r.Matchup("x") != nullptr && std::fabs(r.Matchup("x")->ttkTicks - 123.5f) < 1e-3f,
          "...and says what was written");
  }
  // ---- 8. roles on the real rigs ---------------------------------------------------
  {
    struct Want {
      const char* def;
      std::vector<distant::LimbRole> roles;
    };
    const Want wants[] = {
        {"human", {distant::LimbRole::Head, distant::LimbRole::Torso, distant::LimbRole::Arm,
                   distant::LimbRole::Leg}},
        {"zombie", {distant::LimbRole::Head, distant::LimbRole::Torso, distant::LimbRole::Arm,
                    distant::LimbRole::Leg}},
        {"snake", {distant::LimbRole::Head, distant::LimbRole::Torso, distant::LimbRole::Tail}},
        {"shark", {distant::LimbRole::Head, distant::LimbRole::Torso, distant::LimbRole::Tail,
                   distant::LimbRole::Fin}},
        {"piranha", {distant::LimbRole::Head, distant::LimbRole::Torso, distant::LimbRole::Tail}},
        {"naga", {distant::LimbRole::Head, distant::LimbRole::Torso, distant::LimbRole::Arm,
                  distant::LimbRole::Tail}},
    };
    for (const Want& w : wants) {
      const int d = c.mobs.FindDef(w.def);
      if (d < 0) {
        // The matrix's own creatures must exist; the swimmers and the naga
        // are checked when the content has them.
        const bool needed = std::string(w.def) == "human" || std::string(w.def) == "zombie" ||
                            std::string(w.def) == "snake";
        if (needed) check(false, Format("def %s loads (for the role check)", w.def));
        else std::printf("distant-calib-matrix: (no def %s: its role check skipped)\n", w.def);
        continue;
      }
      const MobDef& def = c.mobs.Defs()[(size_t)d];
      bool has[distant::kRoles] = {};
      bool vitalRole = false;
      for (const MobLimbDef& ld : def.limbs) {
        const distant::LimbRole r = distant::LimbRoleOf(ld.tag, ld.name, ld.bloodless);
        has[(int)r] = true;
        vitalRole = vitalRole || (ld.vital && r != distant::LimbRole::Other);
      }
      std::string missing;
      for (distant::LimbRole r : w.roles)
        if (!has[(int)r]) missing += std::string(" ") + distant::LimbRoleName(r);
      check(missing.empty(), Format("%s has roles%s", w.def,
                                    missing.empty() ? " (all)" : (" MISSING" + missing).c_str()));
      check(vitalRole, Format("%s has a vital limb with a role", w.def));
    }
  }
  return u;
}

// =============================================================================
// F. The log perturbs nothing
// =============================================================================

struct NeutralArm {
  std::vector<std::vector<uint32_t>> words;   // per tick: hp + origin bits, op batch
  size_t strokes = 0;
  std::string why;
};

NeutralArm RunNeutralArm(Ctx& c, bool logOn, int ticks) {
  NeutralArm arm;
  strokelog::Sink sink(&c.items);
  std::unique_ptr<strokelog::Scope> scope;
  if (logOn) scope = std::make_unique<strokelog::Scope>(&sink);
  Fresh(c, 900001u);
  const IVec3 centre = WindowCentre(c.world);
  const IVec3 at = Arena(centre.x - 55, centre.z - 55);
  const Loadout a{"human", "duelist", "sword", nullptr, false, false, ""};
  const Loadout b{"human", "duelist_blue", "sword", nullptr, false, false, ""};
  Bout bout;
  bout.a.push_back(SpawnOne(c, a, at.x, at.z - 6, arm.why));
  bout.b.push_back(SpawnOne(c, b, at.x, at.z + 6, arm.why));
  if (bout.a[0] == 0 || bout.b[0] == 0) return arm;
  support::TickRig rig(c, 47000u, IVec3{at.x >> 4, at.y >> 4, at.z >> 4});
  for (int i = 0; i < 4; i++) support::RunTicks(rig, 1);
  FaceFoes(c, bout);
  for (int t = 0; t < ticks; t++) {
    support::RunTicks(rig, 1);
    c.mobs.ClearAttackRequests();
    c.mobs.ClearBlockEvents();
    std::vector<uint32_t> w;
    for (uint64_t id : {bout.a[0], bout.b[0]}) {
      const float hp = c.mobs.TotalHp(id);
      const Vec3 o = c.mobs.MobOrigin(id);
      uint32_t u[4];
      std::memcpy(&u[0], &hp, 4);
      std::memcpy(&u[1], &o.x, 4);
      std::memcpy(&u[2], &o.y, 4);
      std::memcpy(&u[3], &o.z, 4);
      w.insert(w.end(), u, u + 4);
    }
    const OpBatch& ob = rig.LastBatch();
    auto put = [&w](const void* p, size_t n, size_t sz) {
      w.push_back((uint32_t)n);
      const uint32_t* q = (const uint32_t*)p;
      w.insert(w.end(), q, q + n * sz / 4);
    };
    put(ob.ops.data(), ob.ops.size(), sizeof(BrushOp));
    put(ob.cells.data(), ob.cells.size(), sizeof(CellOp));
    put(ob.spawns.data(), ob.spawns.size(), sizeof(ParticleSpawn));
    arm.words.push_back(std::move(w));
  }
  sink.Flush(rig.tick);
  arm.strokes = sink.Strokes().size();
  return arm;
}

// =============================================================================
// The gate
// =============================================================================

Status GateDistantCalibMatrix(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  // THE SHIPPED ITEM LIBRARY for the creatures (weapon forms, shields, the
  // bow), put back on the way out (npc-strike's and shield's reason).
  struct ItemsScope {
    MobSystem& m;
    const ItemLibrary* prev;
    ItemsScope(MobSystem& mm, const ItemLibrary* now) : m(mm), prev(mm.Items()) {
      m.SetItems(now);
    }
    ~ItemsScope() { m.SetItems(prev); }
  } itemsScope(c.mobs, &c.items);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("distant-calib-matrix: FAILED %s\n", what.c_str());
    }
  };
  const char* calibPath = std::getenv("SANDVOX_DISTANT_CALIB");
  const bool calib = calibPath != nullptr && calibPath[0] != '\0';
  const double t0 = NowSeconds();

  // ---- E. the unit arm (pure CPU, first: it needs no world) -----------------
  const UnitResult unit = ResolverUnitArm(c);
  check(unit.ok, "the resolver's unit arm" + unit.fails);
  checks += unit.checks;
  const double tUnit = NowSeconds();

  // ---- D. the shipped table loads clean ------------------------------------------
  distant::DistantCalib shipped;
  {
    std::string log;
    const std::string path = AssetDir() + "/mobs/distant_combat.json";
    const bool loaded = distant::LoadDistantCombat(path, shipped, log);
    if (!log.empty()) std::printf("distant-calib-matrix: shipped table: %s", log.c_str());
    // Under SANDVOX_DISTANT_CALIB the file is about to be (re)written, so its
    // absence is the bootstrap, not a failure.
    check(calib || (loaded && log.empty()),
          "assets/mobs/distant_combat.json loads with no warnings");
  }

  // ---- A + B. the matrix ------------------------------------------------------------
  strokelog::Sink sink(&c.items);
  const int rounds = calib ? (int)BaselineNumber("distantCalib.calibRounds", 6)
                           : (int)BaselineNumber("distantCalib.rounds", 1);
  const int ticks = calib ? (int)BaselineNumber("distantCalib.calibTicks", 600)
                          : (int)BaselineNumber("distantCalib.ticks", 300);
  const int rangedTicks = calib ? (int)BaselineNumber("distantCalib.calibRangedTicks", 600)
                                : (int)BaselineNumber("distantCalib.rangedTicks", 300);
  std::vector<const BoutSpec*> melee, ranged;
  for (const BoutSpec& b : kBouts)
    if (calib || b.inDefault) (b.ranged ? ranged : melee).push_back(&b);
  std::map<std::string, MatchAcc> acc;
  std::vector<Shot> shots;
  std::vector<BleedProbe> bleeds;
  std::vector<BurnProbe> burns;
  int boutsRun = 0, boutsDecided = 0, boutsHurt = 0, ticksRun = 0;
  std::string fixtureWhy;
  {
    strokelog::Scope logOn(&sink);
    for (int r = 0; r < rounds; r++) {
      // Melee round.
      RoundOut mo = RunRound(c, melee, r, ticks, false, nullptr, nullptr, sink);
      if (!mo.ok) {
        fixtureWhy = mo.why;
        break;
      }
      ticksRun += mo.ticksRun;
      // Ranged round: two archer bouts (cycled through the ranged list by
      // round) and the two probes.
      std::vector<const BoutSpec*> rs;
      for (size_t k = 0; k < ranged.size() && rs.size() < 2; k++)
        rs.push_back(ranged[((size_t)r + k) % ranged.size()]);
      BleedProbe bp;
      BurnProbe fp;
      RoundOut ro = RunRound(c, rs, r, rangedTicks, true, &bp, &fp, sink);
      if (!ro.ok) {
        fixtureWhy = ro.why;
        break;
      }
      ticksRun += ro.ticksRun;
      bleeds.push_back(bp);
      burns.push_back(fp);
      shots.insert(shots.end(), ro.shots.begin(), ro.shots.end());
      for (const RoundOut* o : {&mo, &ro})
        for (const Bout& b : o->bouts) {
          boutsRun++;
          MatchAcc& m = acc[b.spec->name];
          m.n++;
          const double fa = b.hp0A > 0.0f ? b.hpEndA / b.hp0A : 0.0;
          const double fb = b.hp0B > 0.0f ? b.hpEndB / b.hp0B : 0.0;
          m.hpA += fa;
          m.hpB += fb;
          if (fa < 0.99 || fb < 0.99) boutsHurt++;
          if (b.endTick >= 0) {
            boutsDecided++;
            if (b.winner == 1) m.winA++;
            else if (b.winner == 2) m.winB++;
            else m.draw++;
            m.ttk += b.endTick;
            m.ttk2 += (double)b.endTick * b.endTick;
            m.ttkN++;
          } else {
            m.draw++;
            m.timeout = std::max(m.timeout, o->ticksRun);
          }
          std::printf("distant-calib-matrix: round %d %-22s %s after %d ticks\n", r,
                      b.spec->name,
                      b.endTick < 0 ? "UNDECIDED"
                                    : (b.winner == 1 ? "A won" : (b.winner == 2 ? "B won" : "both died")),
                      b.endTick < 0 ? o->ticksRun : b.endTick);
        }
    }
  }
  if (!fixtureWhy.empty()) {
    c.mobs.Reset();
    c.debris.Reset();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    detail = "fixture: " + fixtureWhy;
    std::printf("distant-calib-matrix: FAILED the fixture: %s\n", fixtureWhy.c_str());
    return Status::Fail;
  }
  const double tMatrix = NowSeconds();

  // ---- C. reduce ------------------------------------------------------------------------
  // THE BLEED LAW from the probes: of the budget that LEFT the wound, the share
  // that drained as blood (the rest healed or cauterised), and the per-tick
  // decay of a geometric payout with the same mean drain time.
  double drained = 0, owed = 0, remaining = 0, tw = 0, w = 0;
  for (const BleedProbe& b : bleeds) {
    owed += b.owedHp;
    remaining += b.remainingHp;
    for (size_t t = 0; t < b.loss.size(); t++) {
      drained += b.loss[t];
      tw += (double)t * b.loss[t];
      w += b.loss[t];
    }
  }
  const double left = owed - remaining;
  const float drainShare =
      left > 1e-3 ? (float)std::clamp(drained / left, 0.05, 4.0) : 1.0f;
  const double meanT = w > 1e-6 ? tw / w : 0.0;
  const float decay = meanT > 0.0 ? (float)std::clamp(meanT / (1.0 + meanT), 0.5, 0.9995) : 0.985f;

  distant::DistantCalib cal = distant::DistantCalibBuiltin();
  strokelog::ReduceStats rst;
  std::string report;
  strokelog::ReduceStrokes(sink.Strokes(), (int)BaselineNumber("distantCalib.minSamples", 8),
                           drainShare, CurrentTuning().gear, cal, rst, report);
  cal.bleed.decay = decay;
  // THE BURN from its probes.
  {
    double frac = 0, hp = 0, burnT = 0;
    int n = 0, lifeT = 0, lifeN = 0;
    for (const BurnProbe& b : burns) {
      if (!b.lit || b.burningTicks <= 0) continue;
      n++;
      frac += (b.frac1 - b.frac0);
      hp += (b.burnHp1 - b.burnHp0);
      burnT += b.burningTicks;
      lifeT += b.deathTick >= 0 ? b.deathTick : b.lastBurning + 1;
      lifeN++;
    }
    if (n > 0 && burnT > 0) {
      cal.burn.fracPerTick = (float)(frac / burnT);
      cal.burn.hpPerTick = (float)(hp / burnT);
      cal.burn.meanTicks = (int)std::lround(burnT / n);
    }
    (void)lifeT;
    (void)lifeN;
  }
  // THE RANGED table from the shots.
  int shotsN = 0, shotHits = 0, glances = 0;
  {
    double speedSum = 0, dmgSum = 0;
    int dmgN = 0;
    float roles[distant::kRoles] = {};
    // Distance bins over STILL targets; the moving ones give the penalty.
    const float edges[] = {0.0f, 45.0f, 90.0f, 140.0f, 1e9f};
    double binD[4] = {}, binH[4] = {}, binN[4] = {};
    double movN = 0, movH = 0, movSv = 0, movD = 0;
    double expR = 0, expP = 0;
    int expN = 0;
    const float speedRef = cal.ranged.speedRef;
    for (const Shot& s : shots) {
      shotsN++;
      speedSum += s.speed;
      if (s.hit) shotHits++;
      if (s.glanced) glances++;
      if (s.hit && s.role != distant::LimbRole::Other) roles[(int)s.role] += 1.0f;
      if (s.hit && s.hardness <= 0.0f) {
        dmgSum += s.hp;
        dmgN++;
      }
      if (s.targetSpeed < 4.0f) {
        for (int b = 0; b < 4; b++)
          if (s.distance >= edges[b] && s.distance < edges[b + 1]) {
            binD[b] += s.distance;
            binH[b] += s.hit ? 1 : 0;
            binN[b] += 1;
          }
      } else {
        movN += 1;
        movH += s.hit ? 1 : 0;
        movSv += std::min(1.0f, s.targetSpeed / speedRef);
        movD += s.distance;
      }
    }
    if (shotsN > 0) {
      cal.ranged.n = shotsN;
      cal.ranged.hits = shotHits;
      cal.ranged.arrowSpeed = (float)(speedSum / shotsN);
      std::vector<float> dk, rk;
      for (int b = 0; b < 4; b++)
        if (binN[b] >= 3) {
          dk.push_back((float)(binD[b] / binN[b]));
          rk.push_back((float)((binH[b] + 1.0) / (binN[b] + 2.0)));   // Laplace
        }
      if (!dk.empty()) {
        cal.ranged.distKnots = dk;
        cal.ranged.rateKnots = rk;
      }
      // The moving shots against the still rate at their mean distance.
      if (movN >= 3 && !dk.empty()) {
        const float dMean = (float)(movD / movN);
        float still = rk.front();
        for (size_t i = 0; i < dk.size(); i++)
          if (dMean >= dk[i]) still = rk[i];
        const double movRate = (movH + 1.0) / (movN + 2.0);
        const double sv = movSv / movN;
        if (still > 1e-3f && sv > 1e-3)
          cal.ranged.speedPenalty = (float)std::clamp((1.0 - movRate / still) / sv, 0.0, 1.0);
      }
      if (dmgN > 0) cal.ranged.damageFrac = (float)(dmgSum / dmgN / 35.0);
      float rsum = 0;
      for (float x : roles) rsum += x;
      if (rsum > 0)
        for (int r = 0; r < distant::kRoles; r++) cal.ranged.roleWeights[r] = roles[r] / rsum;
      // A POINT INTO PLATE: what a covered limb's hit carried, against the bare
      // mean, less what the cut-hardness ratio already passes.
      for (const Shot& s : shots) {
        if (!s.hit || s.hardness <= 0.0f || dmgN == 0) continue;
        const double bareMean = dmgSum / dmgN;
        expR += std::min(2.0, s.hp / bareMean);
        expP += ShellResponseOf(s.hardness, DamageCause::Blade, CurrentTuning().gear).carve;
        expN++;
      }
      if (expN > 0) {
        const double r = expR / expN, p = expP / expN;
        const int k = (int)distant::StrikeKind::Pierce;
        cal.armour.n[k] = expN;
        cal.armour.exposure[k] = (float)std::clamp(p < 0.999 ? (r - p) / (1.0 - p) : 1.0, 0.0, 1.0);
      }
    }
  }
  // THE MATCHUPS.
  for (const BoutSpec& b : kBouts) {
    if (b.matchup == nullptr) continue;
    auto it = acc.find(b.name);
    if (it == acc.end() || it->second.n == 0) continue;
    const MatchAcc& m = it->second;
    distant::MatchupCalib mc;
    mc.name = b.matchup;
    mc.a = Format("%dx %s", b.aCount, b.a.desc);
    mc.b = Format("%dx %s", b.bCount, b.b.desc);
    mc.aCount = b.aCount;
    mc.bCount = b.bCount;
    mc.gapVox = b.gap;
    mc.n = m.n;
    mc.winA = (float)m.winA / (float)m.n;
    mc.winB = (float)m.winB / (float)m.n;
    mc.draw = (float)m.draw / (float)m.n;
    if (m.ttkN > 0) {
      const double mean = m.ttk / m.ttkN;
      mc.ttkTicks = (float)mean;
      mc.ttkSd = (float)std::sqrt(std::max(0.0, m.ttk2 / m.ttkN - mean * mean));
    }
    mc.timeoutTicks = b.ranged ? rangedTicks : ticks;
    mc.hpLeftA = (float)(m.hpA / m.n);
    mc.hpLeftB = (float)(m.hpB / m.n);
    cal.matchups.push_back(mc);
  }
  cal.about =
      "THE DISTANT TIER'S HIT MODEL, MEASURED (docs/PLAN_distant_npcs.md 2.6; "
      "game/distant_combat.h reads it). Produced by the duel-matrix gate "
      "distant-calib-matrix (src/test/selftest_distant_calib.cpp): full-fidelity "
      "fights through the real tick -- weapon class x armour x creature kind, "
      "archers, a 4v3 -- with every NPC stroke STARTED logged with its outcome "
      "(game/stroke_log.h), reduced to per-style hit rates, role weights and "
      "effective power, block chance per guard class, armour exposure, bleed and "
      "burn laws, ranged hit rate vs distance and target speed, and the "
      "asymmetric matchups' win rates and time-to-kill (ticks, 30 Hz). "
      "REGENERATE after any melee/gore/AI tuning change: bash scripts/run.sh "
      "./build/Release/sandvox.exe --verify distant-calib-matrix with "
      "SANDVOX_DISTANT_CALIB=assets/mobs/distant_combat.json in the environment "
      "(matrix size: tests/baseline.json distantCalib.calib*). " +
      Format("This file: %d rounds x %d melee bouts + %d ranged bouts, %d strokes "
             "(%d counted: %d hits, %d blocked/parried, %d misses), %d arrows.",
             rounds, (int)melee.size(), (int)std::min<size_t>(2, ranged.size()),
             rst.strokes, rst.counted, rst.hits, rst.blocked, rst.misses, shotsN);
  const double tReduce = NowSeconds();

  // ---- F. the log perturbs nothing ----------------------------------------------
  const int neutralTicks = (int)BaselineNumber("distantCalib.neutralTicks", 150);
  NeutralArm off = RunNeutralArm(c, false, neutralTicks);
  NeutralArm on = RunNeutralArm(c, true, neutralTicks);
  int firstDiff = -1;
  for (size_t t = 0; t < std::min(off.words.size(), on.words.size()) && firstDiff < 0; t++)
    if (off.words[t] != on.words[t]) firstDiff = (int)t;
  check(off.why.empty() && on.why.empty() && off.words.size() == (size_t)neutralTicks,
        "the log-neutral duel stood up" + off.why + on.why);
  check(firstDiff < 0 && off.words.size() == on.words.size(),
        Format("the log changes nothing (first differing tick %d of %zu)", firstDiff,
               off.words.size()));
  check(on.strokes > 0, Format("...and with it on, it logged (%zu strokes)", on.strokes));
  const double tNeutral = NowSeconds();

  // ---- what the matrix must have measured -------------------------------------------
  int open = 0;
  for (const strokelog::Stroke& s : sink.Strokes()) open += s.outcome == strokelog::Outcome::Open;
  check(open == 0, Format("every stroke started was closed with an outcome (%d open)", open));
  const int minStrokes = (int)BaselineNumber("distantCalib.minStrokes", 30);
  check(rst.counted >= minStrokes,
        Format("the matrix logged %d counted strokes (>= %d)", rst.counted, minStrokes));
  check(rst.misses > 0, Format("...misses among them (%d): a hit rate needs them", rst.misses));
  check(rst.hits > 0, Format("...and hits (%d)", rst.hits));
  // THE FIGHTS FOUGHT: a bout in which neither side lost a hundredth of its
  // hp is a fixture that never engaged (back to back, out of sight, a profile
  // that will not swing). DECIDED is asked only of the calibration's long
  // bouts: a full-fidelity duel takes 600+ ticks to kill (the table's TTKs),
  // so the default's 300 decides none, by design.
  const int minHurt = (int)BaselineNumber("distantCalib.minBoutsHurt", 6);
  check(boutsHurt >= std::min(minHurt, boutsRun),
        Format("bouts drew blood (%d of %d, need %d)", boutsHurt, boutsRun, minHurt));
  if (calib)
    check(boutsDecided > 0, Format("bouts were decided (%d of %d)", boutsDecided, boutsRun));
  RecordObserved("distantCalib.boutsHurt", (double)boutsHurt);
  check(shotsN > 0, Format("the archers loosed (%d arrows, %d hit)", shotsN, shotHits));
  bool bleedOk = false, burnOk = false;
  for (const BleedProbe& b : bleeds) bleedOk = bleedOk || (b.owedHp > 0.0f);
  for (const BurnProbe& b : burns) burnOk = burnOk || (b.lit && b.frac1 > b.frac0);
  check(bleedOk && drained > 0.0, Format("the bleed probe opened %.1f hp of blood and drained %.1f",
                                         owed, drained));
  check(burnOk, "the burn probe caught and burnt");
  // Every style seen has a sane row.
  for (const distant::StyleCalib& s : cal.styles)
    check(s.hitRate > 0.0f && s.hitRate <= 1.0f && s.meanPower >= 0.0f,
          Format("style %s row sane (hitRate %.3f meanPower %.3f)", s.name.c_str(), s.hitRate,
                 s.meanPower));

  std::printf("distant-calib-matrix: reduced %d strokes:\n%s", rst.strokes, report.c_str());
  std::printf("distant-calib-matrix: bleed: probes owed %.1f hp, %.1f left the wound, %.1f "
              "drained -> drainShare %.3f, mean drain %.1f ticks -> decay %.4f; perHp cut %.3f "
              "blunt %.3f bite %.3f\n",
              owed, left, drained, drainShare, meanT, decay, cal.bleed.perHp[0],
              cal.bleed.perHp[1], cal.bleed.perHp[2]);
  std::printf("distant-calib-matrix: burn: %.5f frac/tick, %.3f hp/tick, %d ticks alight\n",
              cal.burn.fracPerTick, cal.burn.hpPerTick, cal.burn.meanTicks);
  std::printf("distant-calib-matrix: ranged: %d arrows, %d hit (%d glanced), speed %.0f vox/s, "
              "damageFrac %.3f, speedPenalty %.3f, knots:",
              shotsN, shotHits, glances, cal.ranged.arrowSpeed, cal.ranged.damageFrac,
              cal.ranged.speedPenalty);
  for (size_t i = 0; i < cal.ranged.distKnots.size(); i++)
    std::printf(" %.0f->%.2f", cal.ranged.distKnots[i], cal.ranged.rateKnots[i]);
  std::printf("\n");
  for (const distant::MatchupCalib& m : cal.matchups) {
    std::printf("distant-calib-matrix: matchup %-24s n %d winA %.2f winB %.2f draw %.2f ttk %.0f "
                "(sd %.0f) ticks, hp left A %.2f B %.2f\n",
                m.name.c_str(), m.n, m.winA, m.winB, m.draw, m.ttkTicks, m.ttkSd, m.hpLeftA,
                m.hpLeftB);
    RecordObserved(("distantCalib." + m.name + ".hpLeftA").c_str(), m.hpLeftA);
    RecordObserved(("distantCalib." + m.name + ".hpLeftB").c_str(), m.hpLeftB);
    RecordObserved(("distantCalib." + m.name + ".winA").c_str(), m.winA);
    RecordObserved(("distantCalib." + m.name + ".ttkTicks").c_str(), m.ttkTicks);
  }
  // The measured pooled numbers beside the shipped table's (drift is printed,
  // not asserted: the default matrix is one seed and a coin's worth of noise).
  std::printf("distant-calib-matrix: pooled hitRate %.3f (shipped %.3f), meanPower %.3f "
              "(shipped %.3f), shield block %.3f (shipped %.3f)\n",
              cal.fallback.hitRate, shipped.fallback.hitRate, cal.fallback.meanPower,
              shipped.fallback.meanPower, cal.blockChance[2], shipped.blockChance[2]);
  RecordObserved("distantCalib.strokes", (double)rst.strokes);
  RecordObserved("distantCalib.counted", (double)rst.counted);
  RecordObserved("distantCalib.hits", (double)rst.hits);
  RecordObserved("distantCalib.misses", (double)rst.misses);
  RecordObserved("distantCalib.pooledHitRate", cal.fallback.hitRate);
  RecordObserved("distantCalib.pooledMeanPower", cal.fallback.meanPower);
  RecordObserved("distantCalib.blockShield", cal.blockChance[2]);
  RecordObserved("distantCalib.blockBlade", cal.blockChance[1]);
  RecordObserved("distantCalib.arrows", (double)shotsN);
  RecordObserved("distantCalib.arrowHits", (double)shotHits);
  RecordObserved("distantCalib.bleedDecay", decay);
  RecordObserved("distantCalib.drainShare", drainShare);
  RecordObserved("distantCalib.burnFracPerTick", cal.burn.fracPerTick);
  RecordObserved("distantCalib.ticksRun", (double)ticksRun);

  // ---- C'. write the table (SANDVOX_DISTANT_CALIB) ----------------------------------
  if (calib) {
    {
      std::ofstream f(calibPath, std::ios::binary);
      f << distant::DistantCalibToJson(cal);
      check((bool)f, Format("wrote %s", calibPath));
    }
    distant::DistantCalib back;
    std::string log;
    const bool loaded = distant::LoadDistantCombat(calibPath, back, log);
    check(loaded && log.empty(), Format("the written table reloads clean%s%s",
                                        log.empty() ? "" : ": ", log.c_str()));
    bool agree = back.styles.size() == cal.styles.size() &&
                 back.matchups.size() == cal.matchups.size() &&
                 std::fabs(back.fallback.hitRate - cal.fallback.hitRate) < 1e-3f;
    for (size_t i = 0; agree && i < cal.styles.size(); i++)
      agree = std::fabs(back.Style(cal.styles[i].name).hitRate - cal.styles[i].hitRate) < 1e-3f;
    check(agree, "...and says what was measured");
    std::printf("distant-calib-matrix: WROTE %s (%zu styles, %zu matchups)\n", calibPath,
                cal.styles.size(), cal.matchups.size());
  }

  // Leave the shared world as the next gate expects it.
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const double tEnd = NowSeconds();
  std::printf("distant-calib-matrix: %s mode, %d rounds, %d bouts (%d decided), %d ticks; "
              "seconds: unit %.1f, matrix %.1f, reduce %.2f, neutral %.1f, total %.1f\n",
              calib ? "CALIBRATION" : "default", rounds, boutsRun, boutsDecided, ticksRun,
              tUnit - t0, tMatrix - tUnit, tReduce - tMatrix, tNeutral - tReduce, tEnd - t0);
  detail = Format("%d checks; %d strokes (%d hits, %d misses), %d arrows, %d/%d bouts decided%s",
                  checks, rst.counted, rst.hits, rst.misses, shotsN, boutsDecided, boutsRun,
                  calib ? "; table WRITTEN" : "");
  std::printf("distant-calib-matrix: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DistantCalibGates() {
  static const std::vector<Gate> g = {
      // Spawns creatures in fresh worlds (its own id base, Jolt id history
      // rewound per round), sets the shipped item library for the duration,
      // and regenerates worldgen + restores the id counter on the way out.
      {"distant-calib-matrix", "mob", {}, false, GateDistantCalibMatrix},
  };
  return g;
}

}  // namespace selftest
