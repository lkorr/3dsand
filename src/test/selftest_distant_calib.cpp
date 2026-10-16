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
#include "game/persist.h"   // EntityParking: distant-fight's promotion arm
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
  // D2b ATTRIBUTION: who died of what, when, with how many limbs off; and
  // the limbs off the survivors at the end.
  std::vector<uint8_t> seenDead;
  std::string deaths;
};

// Base limbs with no body (severed), on a near creature.
int SeveredOf(MobSystem& mobs, uint64_t id) {
  const Mob* m = mobs.FindMobById(id);
  if (m == nullptr) return -1;
  int n = 0;
  for (int i = 0; i < m->AppendedBase(); i++) n += mobs.LimbBody(id, i) == 0 ? 1 : 0;
  return n;
}

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

// D2b: THE STUMP PROBE. A passive dummy of `def` loses its left forearm to a
// direct Mob::Sever on the first fight tick; its blood loss per tick after
// that is the stump's law as the near tier actually pays it (the gout, then
// the open stump's top-up, through the drip budget and DrainBlood).
struct StumpProbe {
  const char* def = "human";
  uint64_t id = 0;
  bool cut = false;
  float lost0 = 0;
  std::vector<float> loss;   // hp of blood per tick after the sever
  float hp0 = 0, hp1 = 0;    // TotalHp right after the sever, and at the end
  int deathTick = -1;
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
                  strokelog::Sink& sink, std::vector<StumpProbe>* stumps = nullptr) {
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
  if (stumps != nullptr)
    for (size_t k = 0; k < stumps->size(); k++) {
      const IVec3 p = probeAt(slot++);
      const Loadout L{(*stumps)[k].def, "dummy", nullptr, nullptr, false, false, "stump probe"};
      // Off the grid's corners (a fifth and sixth spot): beside the centre.
      (*stumps)[k].id = SpawnOne(c, L, p.x + 30 * (int)k, p.z + 40, out.why);
      if ((*stumps)[k].id == 0) {
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
    if (t == 0 && stumps != nullptr)
      for (StumpProbe& sp : *stumps)
        if (Mob* m = c.mobs.FindMobById(sp.id)) {
          int li = -1;
          for (int i = 0; i < m->AppendedBase() && li < 0; i++)
            if (m->LimbDefAt(i).name == "armL.L") li = i;
          if (li < 0)
            for (int i = 0; i < m->AppendedBase() && li < 0; i++)
              if (m->LimbDefAt(i).severable && !m->LimbDefAt(i).vital &&
                  distant::LimbRoleOf(m->LimbDefAt(i).tag, m->LimbDefAt(i).name) ==
                      distant::LimbRole::Arm)
                li = i;
          // DIRECT CALL ON PURPOSE (W2-O): a fixture injection, as the bleed
          // probe's Damage: the one sever, and nothing else ever hurts it.
          if (li >= 0) m->Sever(li, DamageCtx(DamageCause::Blade));
          sp.cut = li >= 0 && c.mobs.LimbBody(sp.id, li) == 0;
          sp.lost0 = c.mobs.BloodLost(sp.id);
          sp.hp0 = c.mobs.TotalHp(sp.id);
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
    if (stumps != nullptr)
      for (StumpProbe& sp : *stumps) {
        if (!sp.cut || sp.deathTick >= 0) continue;
        const float lost = c.mobs.BloodLost(sp.id);
        sp.loss.push_back(std::max(0.0f, lost - sp.lost0) * hpPerVox);
        sp.lost0 = lost;
        sp.hp1 = c.mobs.TotalHp(sp.id);
        if (!c.mobs.IsAlive(sp.id)) sp.deathTick = t;
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
      if (b.seenDead.empty()) b.seenDead.assign(b.a.size() + b.b.size(), 0);
      for (size_t q = 0; q < b.a.size() + b.b.size(); q++) {
        const uint64_t id = q < b.a.size() ? b.a[q] : b.b[q - b.a.size()];
        if (b.seenDead[q] || c.mobs.IsAlive(id)) continue;
        b.seenDead[q] = 1;
        const Mob* m = c.mobs.FindMobById(id);
        b.deaths += Format("%s%zu died '%s' at %d with %d limbs off; ", q < b.a.size() ? "A" : "B",
                           q < b.a.size() ? q : q - b.a.size(),
                           m != nullptr && m->DeathCause() != nullptr ? m->DeathCause() : "?",
                           t + 1, SeveredOf(c.mobs, id));
      }
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
  // ...and who is still standing, with how much and how many limbs off --
  // read HERE, while the round's world still holds them.
  for (Bout& b : out.bouts)
    for (size_t q = 0; q < b.a.size() + b.b.size(); q++) {
      const uint64_t id = q < b.a.size() ? b.a[q] : b.b[q - b.a.size()];
      if (!c.mobs.IsAlive(id)) continue;
      const float h0 = 1.0f;
      (void)h0;
      b.deaths += Format("%s%zu alive hp %.0f with %d limbs off; ", q < b.a.size() ? "A" : "B",
                         q < b.a.size() ? q : q - b.a.size(), c.mobs.TotalHp(id),
                         SeveredOf(c.mobs, id));
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
  // LONE-FIGHT BLEEDING (D2b, measured 2026-10-15). The matrix packs every
  // bout of a round into ONE MobSystem for speed, and the near tier's drips
  // go through a SHARED POT (gore.bleedOpsPerTick, 6 a tick, taken in turns
  // by every creature that wants one: Mob::BleedTick) -- a per-system op
  // budget (rule 2) that a lone fight never meets. Thirty-odd bleeding
  // creatures took turns at it, so every wound and every open stump in the
  // matrix bled at about a sixth of its rate: near survivors carried 5-11
  // severed limbs for 1800 ticks, which a lone fight's stumps would not have
  // allowed. The distant tier's bleeding is a creature's own (it emits no
  // ops), so its reference must be the lone fight: the pot is lifted to its
  // ceiling for the gate's duration and put back on the way out.
  struct PotScope {
    Tuning was;
    PotScope() : was(CurrentTuning()) {
      Tuning t = was;
      t.gore.bleedOpsPerTick = 64;
      SetCurrentTuning(t);
    }
    ~PotScope() {
      Tuning t = CurrentTuning();
      t.gore.bleedOpsPerTick = was.gore.bleedOpsPerTick;
      SetCurrentTuning(t);
    }
  } potScope;
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
  std::vector<StumpProbe> stumpProbes;
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
      std::vector<StumpProbe> sps(2);
      sps[0].def = "human";
      sps[1].def = "zombie";
      RoundOut ro = RunRound(c, rs, r, rangedTicks, true, &bp, &fp, sink, &sps);
      for (const StumpProbe& sp : sps) {
        const int gout = std::min((int)sp.loss.size(), CurrentTuning().gore.severDecayTicks);
        double g = 0, st = 0;
        int stN = 0;
        for (int k = 0; k < (int)sp.loss.size(); k++) {
          if (k < gout) g += sp.loss[(size_t)k];
          else {
            st += sp.loss[(size_t)k];
            stN++;
          }
        }
        std::printf("distant-calib-matrix: stump probe %s round %d: cut %d, gout %.1f hp over "
                    "%d ticks, then %.4f hp/tick over %d ticks; TotalHp %.0f -> %.0f; died at %d\n",
                    sp.def, r, sp.cut ? 1 : 0, g, gout, stN > 0 ? st / stN : 0.0, stN, sp.hp0,
                    sp.hp1, sp.deathTick);
        stumpProbes.push_back(sp);
      }
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
          std::printf("distant-calib-matrix: round %d %-22s %s after %d ticks | %s\n", r,
                      b.spec->name,
                      b.endTick < 0 ? "UNDECIDED"
                                    : (b.winner == 1 ? "A won" : (b.winner == 2 ? "B won" : "both died")),
                      b.endTick < 0 ? o->ticksRun : b.endTick, b.deaths.c_str());
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


// =============================================================================
// D2b: THE FIGHT AT DISTANCE (docs/PLAN_distant_npcs.md §3 D2b, §4)
// =============================================================================
//
//   distant-fight
//     A. TWO SQUADS FIGHT DISTANT. Four plated swordsmen and an archer against
//        five unarmoured swordsmen, built near (spawned, armed and dressed at the
//        window centre) and demoted through the park rule's own door
//        (MobSystem::DemoteForTest), stood 160 voxels past the window's +X
//        face facing each other. The tier is stepped ALONE -- DIRECT PHASE
//        CALLS ON PURPOSE (W2-O): the fight is on ground no window holds, and
//        the twice-run claim needs both runs on the SAME tick numbers (every
//        brain clock and every roll is keyed on them), which THE tick cannot
//        rewind to. Run twice from one saved tier: the digests over every
//        creature's limb hp, origin and heading bits, death, cause and death
//        tick are equal; casualties > 0; swings hit AND miss; the archer
//        loosed; and after the fight the awake count reaches 0 (the survivors'
//        bleeds run out and they sleep).
//     B. THE DEAD COME BACK AS CORPSES ON THE GROUND (THE tick). The dead of
//        A are walked into the window and promotion is switched on (phase H's
//        Unpark, as the game's): each comes back DEAD under its own id with
//        its cause, through the record's dead block (LoadOne's spawnDead_ +
//        EnterLoadedDead: no cry), snapped to the real ground; 60 ticks on its
//        body lies within distantCorpseGroundMaxVox of the ground, with no
//        voice (Hurt or Death) from any of them and no fall billed.
//     C. THE COST (section 2.11): 64 against 64 duelists in two lines, 300
//        ticks; TickDistant's wall time per tick and where it goes (the
//        `distant` block's ns* fields), recorded as distantFight.engaged128*.
//        Asserted only against distantEngaged128MaxUs, a regression ceiling;
//        the plan's target (1.5 ms) is the number reported beside it.
//
//   distant-calibrate
//     The ASYMMETRIC matchups D2a measured at full fidelity (the table's
//     `matchups`, set up exactly as distant-calib-matrix's kBouts sets them),
//     fought AT DISTANCE for distantCalibrate.seeds seeds each (an id base and
//     a start tick per seed), one bout at a time in one arena outside the
//     window, to the table's own timeout. Compared with the full-fidelity row:
//     each side's WIN RATE within distantWinRateTol; the mean TIME-TO-KILL
//     ratio within distantTtkRatioMin..Max wherever both decided bouts; and
//     each side's HP LEFT (TotalHp at the end over the start, decided or not --
//     D2a found most full-fidelity bouts end UNDECIDED with both sides maimed,
//     so the distant tier must reproduce that, not force decisive fights)
//     within distantHpLeftTol. Every number is printed and recorded.

// FNV-1a 64 over raw bytes (the twice-run digests).
struct FnvDigest {
  uint64_t h = 1469598103934665603ull;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void F(float v) { Bytes(&v, sizeof v); }
  void U(uint64_t v) { Bytes(&v, sizeof v); }
};

// The tier on with the gates' own numbers, whatever tuning.json says
// (selftest_distant.cpp SetTier's).
void DistantTierOn() {
  Tuning t = CurrentTuning();
  t.distant.enabled = true;
  t.distant.radius = 1280.0f;
  t.distant.hysteresis = 256.0f;
  t.distant.idleStride = 4;
  t.distant.surfaceSnapMaxVox = 24.0f;
  t.distant.promoteMaxDropVox = 48.0f;
  SetCurrentTuning(t);
}

// ONE ORDER of a distant muster: a creature of loadout `L` to stand at column
// (x, z) facing `faceTo`.
struct DistantOrder {
  Loadout L;
  float x = 0, z = 0;
  Vec3 faceTo{};
};

// THE MUSTER. Every order is BUILT NEAR -- spawned, armed and dressed on a
// grid round the window centre, 9 voxels apart so no two bodies start inside
// each other -- and ANIMATED by THE tick for kPoseTicks before it leaves: the
// kit reads its reaches off the LIVE pose (MobSystem::StyleReachOn measures
// the arm's bones and the weapon's edge), and a rig that has never been posed
// answers a fist's reach as 0 and a mace's as 1.2 (measured, the first run of
// this gate). Then each is DEMOTED through the park rule's own door
// (DemoteForTest: SaveOne -> DemoteToDistant, which builds the kit -> Release-
// Rig) and stood at its column on DistantGround, facing its mark. In batches
// of 32 (the living cap is 64). The ids in order; empty on any refusal (`why`).
constexpr int kPoseTicks = 4;
std::vector<uint64_t> MusterDistant(Ctx& c, const std::vector<DistantOrder>& orders,
                                    uint32_t& t, std::string& why) {
  std::vector<uint64_t> out;
  const IVec3 wc = WindowCentre(c.world);
  const IVec3 pc{wc.x >> 4, World::TerrainHeight(wc.x, wc.z, kDefaultSeed) >> 4, wc.z >> 4};
  for (size_t b0 = 0; b0 < orders.size(); b0 += 32) {
    const size_t b1 = std::min(orders.size(), b0 + 32);
    std::vector<uint64_t> batch;
    for (size_t k = b0; k < b1; k++) {
      const int j = (int)(k - b0);
      const uint64_t id =
          SpawnOne(c, orders[k].L, wc.x - 32 + (j % 8) * 9, wc.z - 14 + (j / 8) * 9, why);
      if (id == 0) return {};
      batch.push_back(id);
    }
    {
      support::TickCursor rt{c, t, pc};
      for (int k = 0; k < kPoseTicks; k++) rt();
    }
    DistantMobs& tier = c.mobs.DistantMut();
    for (size_t k = b0; k < b1; k++) {
      const uint64_t id = batch[k - b0];
      if (!c.mobs.DemoteForTest(id)) {
        why = std::string("DemoteForTest refused ") + orders[k].L.desc;
        return {};
      }
      DistantMob* d = tier.Find(id);
      if (d == nullptr) {
        why = "demoted creature not in the tier";
        return {};
      }
      const Vec3 ws = d->worldSize;
      const int cx = ifloor(orders[k].x), cz = ifloor(orders[k].z);
      const float gy = (float)(c.mobs.DistantGroundMut().TerrainTop(cx, cz) + 1);
      tier.Relocate(id, Vec3{(float)cx + 0.5f - ws.x * 0.5f, gy, (float)cz + 0.5f - ws.z * 0.5f});
      d = tier.Find(id);
      const Vec3 cc = d->Centre();
      d->steer.heading = d->steer.desiredHeading =
          std::atan2(orders[k].faceTo.x - cc.x, orders[k].faceTo.z - cc.z);
      d->steer.turnVel = 0.0f;
      d->steer.speedNow = 0.0f;
      out.push_back(id);
    }
  }
  return out;
}

// How a body lies: over its attached base limbs, the lowest and the highest
// limb CENTRE above TerrainHeight + 1 under each. False if it has none.
bool CorpseLie(const Mob& m, float& lowGap, float& highGap) {
  bool any = false;
  lowGap = 1e9f;
  highGap = -1e9f;
  for (int i = 0; i < m.AppendedBase(); i++) {
    Vec3 cc{};
    if (!m.LimbCentreWorld(i, cc)) continue;
    const float g = (float)(World::TerrainHeight(ifloor(cc.x), ifloor(cc.z), kDefaultSeed) + 1);
    lowGap = std::min(lowGap, cc.y - g);
    highGap = std::max(highGap, cc.y - g);
    any = true;
  }
  return any;
}

// TotalHp of a distant creature (Mob::TotalHp's set: attached flesh with hp
// left; a corpse has none).
float DistantHp(const MobSystem& mobs, uint64_t id) {
  const DistantMob* d = mobs.Distant().Find(id);
  if (d == nullptr || d->dead || !d->hasKit) return 0.0f;
  float s = 0.0f;
  for (size_t i = 0; i < d->kit.limbs.size() && i < d->limbHp.size(); i++)
    if (d->kit.limbs[i].alive && !d->kit.limbs[i].bloodless && d->limbHp[i] > 0.0f)
      s += d->limbHp[i];
  return s;
}

bool DistantAlive(const MobSystem& mobs, uint64_t id) {
  const DistantMob* d = mobs.Distant().Find(id);
  return d != nullptr && !d->dead;
}

// Everything a twice-run must reproduce, creature by creature in id order.
uint64_t TierDigest(const DistantMobs& tier) {
  FnvDigest dg;
  for (const DistantMob& d : tier.All()) {
    dg.U(d.id);
    for (const float h : d.limbHp) dg.F(h);
    dg.F(d.origin.x);
    dg.F(d.origin.y);
    dg.F(d.origin.z);
    dg.F(d.steer.heading);
    dg.F(d.bloodLossPerTick);
    dg.U(d.dead ? 1u : 0u);
    dg.U(d.deathTick);
    dg.U(d.deathKind);
    dg.Bytes(d.cause.data(), d.cause.size());
    dg.U((uint64_t)d.ammo);
  }
  return dg.h;
}

// ---- distant-fight -------------------------------------------------------------
Status GateDistantFight(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  struct ItemsScope {
    MobSystem& m;
    const ItemLibrary* prev;
    ItemsScope(MobSystem& mm, const ItemLibrary* now) : m(mm), prev(mm.Items()) {
      m.SetItems(now);
    }
    ~ItemsScope() { m.SetItems(prev); }
  } itemsScope(c.mobs, &c.items);
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& why) {
    if (!ok) fails.push_back(why);
    return ok;
  };
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetParkFn(nullptr);
  c.mobs.ClearVoiceEvents();
  // THE WORLD the near half stands on (the muster's spawns, arm B's
  // promotions): this gate may run first (`--gate distant-fight`), and a
  // window nobody generated has no ground -- every body falls through it.
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  DistantTierOn();
  c.mobs.SetDistantStream(&c.stream);
  c.mobs.SetNextIdCounter(70001);
  const IVec3 home = c.world.WindowOrigin();
  const Vec3 wlo{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
                 (float)(home.z * (int)kChunk)};
  const IVec3 wc = WindowCentre(c.world);
  const DistantMobs& tier = c.mobs.Distant();
  std::string why;

  // ======== A: two squads, twice ===============================================
  const Loadout plate{"human", "duelist", "sword", nullptr, true, false, "duelist+sword+plate"};
  const Loadout archer{"human", "archer", "sword", nullptr, false, true, "archer"};
  const Loadout blue{"human", "duelist_blue", "sword", nullptr, false, false, "duelist+sword"};
  const IVec3 at = Arena((int)(wlo.x + (float)kWorldN) + 160, wc.z);
  std::vector<uint64_t> sideA, sideB;
  uint32_t tPose = 68000;
  {
    std::vector<DistantOrder> orders;
    for (int k = 0; k < 5; k++) {
      const float x = (float)at.x + (float)(k - 2) * 6.0f;
      orders.push_back({k == 4 ? archer : plate, x, (float)at.z - (k == 4 ? 30.0f : 8.0f),
                        Vec3{x, 0.0f, (float)at.z + 8.0f}});
    }
    for (int k = 0; k < 5; k++) {
      const float x = (float)at.x + (float)(k - 2) * 6.0f;
      orders.push_back({blue, x, (float)at.z + 8.0f, Vec3{x, 0.0f, (float)at.z - 8.0f}});
    }
    const std::vector<uint64_t> got = MusterDistant(c, orders, tPose, why);
    for (size_t k = 0; k < got.size(); k++) (k < 5 ? sideA : sideB).push_back(got[k]);
  }
  const bool built = why.empty() && sideA.size() == 5 && sideB.size() == 5;
  check(built, "A: squads built (" + why + ")");
  const std::vector<DistantMob> start = tier.All();   // the saved tier
  const DistantMobs::Stats s0 = tier.GetStats();
  const uint32_t tBase = 70000;
  const int maxTicks = (int)BaselineNumber("distantFight.maxTicks", 6000);
  uint64_t digest[2] = {0, 0};
  int decidedAt[2] = {-1, -1}, quietAt[2] = {-1, -1}, dead[2] = {0, 0};
  for (int run = 0; run < 2 && built; run++) {
    if (run == 1) {
      // THE SAME TIER AGAIN, whole (the arrows in the air go with Clear).
      c.mobs.DistantMut().Clear();
      for (const DistantMob& d : start) {
        DistantMob copy = d;
        c.mobs.DistantMut().Insert(std::move(copy));
      }
    }
    FnvDigest trace;
    for (int k = 0; k < maxTicks; k++) {
      c.mobs.TickDistant(tBase + (uint32_t)k, c.world, nullptr);
      if (k % 30 == 29) trace.U(TierDigest(tier));
      bool aAlive = false, bAlive = false;
      for (uint64_t id : sideA) aAlive = aAlive || DistantAlive(c.mobs, id);
      for (uint64_t id : sideB) bAlive = bAlive || DistantAlive(c.mobs, id);
      if (decidedAt[run] < 0 && (!aAlive || !bAlive)) decidedAt[run] = k + 1;
      if (decidedAt[run] >= 0 && tier.AwakeCount() == 0 && tier.Arrows().empty()) {
        quietAt[run] = k + 1;
        break;
      }
    }
    trace.U(TierDigest(tier));
    digest[run] = trace.h;
    for (const DistantMob& d : tier.All()) dead[run] += d.dead ? 1 : 0;
    // ATTRIBUTION (rule 6): whoever is still awake, and every reason it has.
    if (quietAt[run] < 0)
      for (const DistantMob& d : tier.All()) {
        if (d.dead || d.asleep) continue;
        const Vec3 cc = d.Centre();
        std::printf("distant-fight: A run %d still awake: %llu at (%.0f,%.0f) intent %s target %d "
                    "(%llu) threat %d bleed %.4f stumps %u burn %u swing %u draw %u routine %d hold "
                    "%d wanderOwned %d pause %u wake %u steps %u hp %.0f\n",
                    run, (unsigned long long)d.id, (double)cc.x, (double)cc.z,
                    ai::IntentName(d.brain.intent), d.brain.hasTarget ? 1 : 0,
                    (unsigned long long)d.brain.targetId, d.brain.hasThreat ? 1 : 0,
                    d.bloodLossPerTick, (unsigned)d.stumps, (unsigned)d.burnTicksLeft,
                    d.strokeEndTick, d.drawLooseTick, d.brain.routine.active ? 1 : 0,
                    d.brain.routine.hold ? 1 : 0, d.brain.wanderOwned ? 1 : 0,
                    d.brain.wanderPauseUntil, d.wakeTick, d.steps, DistantHp(c.mobs, d.id));
      }
  }
  const DistantMobs::Stats s1 = tier.GetStats();
  // Counted over both runs (they are the same fight twice).
  const uint64_t hits = s1.strokeHits - s0.strokeHits, misses = s1.strokeMisses - s0.strokeMisses;
  const uint64_t looses = s1.looses - s0.looses, arrowHits = s1.arrowHits - s0.arrowHits;
  const uint64_t strokes = s1.strokes - s0.strokes, blocked = s1.strokeBlocked - s0.strokeBlocked;
  const uint64_t bleedTicks = s1.bleedTicks - s0.bleedTicks, grips = s1.gripFails - s0.gripFails;
  const bool okDigest = check(built && digest[0] == digest[1] && digest[0] != 0,
                              Format("A: twice-run digests %016llx / %016llx",
                                     (unsigned long long)digest[0], (unsigned long long)digest[1]));
  const bool okFight = check(dead[0] > 0 && dead[0] == dead[1] && hits > 0 && misses > 0 &&
                                 looses > 0 && decidedAt[0] > 0,
                             Format("A: casualties %d / %d, decided at tick %d, %llu swings (%llu "
                                    "hit, %llu missed), %llu arrows loosed",
                                    dead[0], dead[1], decidedAt[0], (unsigned long long)strokes,
                                    (unsigned long long)hits, (unsigned long long)misses,
                                    (unsigned long long)looses));
  const bool okQuiet = check(quietAt[0] > 0 && quietAt[1] == quietAt[0],
                             Format("A: awake count reached 0 at tick %d / %d (of %d)",
                                    quietAt[0], quietAt[1], maxTicks));

  // ======== B: the dead come back as corpses on the ground =====================
  int bDead = 0, bBack = 0, bOnGround = 0, bVoices = 0, bWrongCause = 0, bFalls = 0;
  float bWorstGap = 0.0f, bWorstHigh = 0.0f;
  std::string bWhy;
  {
    // Only the dead stay; the living leave the tier (they would walk in).
    std::vector<uint64_t> corpses, living;
    for (const DistantMob& d : tier.All()) (d.dead ? corpses : living).push_back(d.id);
    for (const uint64_t id : living) c.mobs.DistantMut().Erase(id);
    c.mobs.DistantMut().ArrowsMut().clear();
    bDead = (int)corpses.size();
    std::vector<std::string> causes;
    // Walked into the window: a row across its centre, inside the promotion
    // line, each on the ground TerrainHeight gives (the snap puts it on the
    // real one).
    // Inside the CPU mirror (3 x 3 x 3 chunks round the rig's fixture chunk,
    // the only ground the promotion's wait can see): 6 voxels apart.
    for (size_t k = 0; k < corpses.size(); k++) {
      const DistantMob* d = tier.Find(corpses[k]);
      causes.push_back(d != nullptr ? d->cause : std::string());
      const float x = (float)wc.x - 12.0f + 6.0f * (float)k, z = (float)wc.z + 40.0f;
      const int cx = ifloor(x), cz = ifloor(z);
      const Vec3 ws = d != nullptr ? d->worldSize : Vec3{};
      c.mobs.DistantMut().Relocate(
          corpses[k], Vec3{(float)cx + 0.5f - ws.x * 0.5f,
                           (float)(World::TerrainHeight(cx, cz, kDefaultSeed) + 1),
                           (float)cz + 0.5f - ws.z * 0.5f});
    }
    uint32_t t = 80000;   // past every tick arm A stepped the tier on
    const IVec3 pc{wc.x >> 4, World::TerrainHeight(wc.x, wc.z + 40, kDefaultSeed) >> 4,
                   (wc.z + 40) >> 4};
    support::TickCursor realTick{c, t, pc};
    EntityParking parking;
    parking.Bind(c.mobs, c.stream.Store(), true);
    realTick.Rig().Authority().parking = &parking;
    c.mobs.ClearVoiceEvents();
    auto countVoices = [&]() {
      for (const MobSystem::VoiceEvent& v : c.mobs.VoiceEvents())
        if (std::find(corpses.begin(), corpses.end(), v.mobId) != corpses.end()) bVoices++;
      c.mobs.ClearVoiceEvents();
    };
    // ATTRIBUTION (CLAUDE.md rule 6): each corpse's body box and the ground
    // under it, the tick it came back and every 10 ticks after, so a corpse
    // that is not on the ground says whether it never landed, sank, or came
    // back somewhere else.
    std::vector<int> backAt(corpses.size(), -1);
    auto trace = [&](int tk) {
      for (size_t k = 0; k < corpses.size(); k++) {
        const Mob* m = c.mobs.FindMobById(corpses[k]);
        if (m == nullptr) continue;
        if (backAt[k] < 0) backAt[k] = tk;
        if ((tk - backAt[k]) % 30 != 0) continue;
        float lowGap = 0.0f, highGap = 0.0f;
        CorpseLie(*m, lowGap, highGap);
        std::printf("distant-fight: B corpse %llu t+%d origin (%.1f,%.1f,%.1f): limb centres "
                    "%.1f..%.1f over their ground, alive %d\n",
                    (unsigned long long)corpses[k], tk - backAt[k], (double)m->Origin().x,
                    (double)m->Origin().y, (double)m->Origin().z, (double)lowGap,
                    (double)highGap, m->Alive() ? 1 : 0);
      }
    };
    int waited = 0;
    for (; waited < 120; waited++) {
      realTick();
      countVoices();
      trace(waited);
      bool all = true;
      for (uint64_t id : corpses) all = all && c.mobs.FindMobById(id) != nullptr;
      if (all) break;
    }
    for (int k = 0; k < 60; k++) {
      realTick();
      countVoices();
      trace(waited + 1 + k);
    }
    for (size_t k = 0; k < corpses.size(); k++) {
      const Mob* m = c.mobs.FindMobById(corpses[k]);
      if (m == nullptr) {
        if (bWhy.empty()) bWhy = Format("corpse %llu never came back", (unsigned long long)corpses[k]);
        continue;
      }
      bBack += m->Alive() ? 0 : 1;
      const std::string cause = m->DeathCause() != nullptr ? m->DeathCause() : "";
      if (cause != causes[k]) {
        bWrongCause++;
        if (bWhy.empty())
          bWhy = Format("corpse %llu came back of '%s', died distant of '%s'",
                        (unsigned long long)corpses[k], cause.c_str(), causes[k].c_str());
      }
      bFalls += (int)m->FallsBilled();
      // ON THE GROUND, LYING: the lowest limb centre near the ground under
      // it (MobBodyBox is no use on a corpse: its origin follows the pelvis's
      // REST corner, under the ground when it lies flat), and no limb centre
      // a standing body's height up.
      float lowGap = 0.0f, highGap = 0.0f;
      if (CorpseLie(*m, lowGap, highGap)) {
        bWorstGap = std::max(bWorstGap, std::fabs(lowGap));
        bWorstHigh = std::max(bWorstHigh, highGap);
        if (std::fabs(lowGap) <= (float)BaselineNumber("distantCorpseGroundMaxVox", 4) &&
            highGap <= 0.6f * m->Def()->worldSize.y)
          bOnGround++;
      }
    }
    realTick.Rig().Authority().parking = nullptr;
    c.mobs.SetParkFn(nullptr);
  }
  const bool okB = check(bDead > 0 && bBack == bDead && bOnGround == bDead && bVoices == 0 &&
                             bWrongCause == 0 && bFalls == 0,
                         Format("B: %d dead, %d back as corpses, %d lying on the ground (worst "
                                "gap %.2f, max %.0f; highest limb %.1f up), %d voices, %d wrong "
                                "causes, %d falls billed%s%s",
                                bDead, bBack, bOnGround, (double)bWorstGap,
                                BaselineNumber("distantCorpseGroundMaxVox", 4), (double)bWorstHigh,
                                bVoices,
                                bWrongCause, bFalls, bWhy.empty() ? "" : ": ", bWhy.c_str()));
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.SetDistantStream(&c.stream);

  // ======== C: the cost of 128 engaged ===========================================
  double usPerTick = -1.0, usPerStep = -1.0, engagedPerTick = 0.0;
  double phase[8] = {};
  int built128 = 0;
  uint64_t caps = 0;
  {
    c.mobs.SetNextIdCounter(90001);
    const IVec3 at2 = Arena((int)(wlo.x + (float)kWorldN) + 420, wc.z);
    std::string why2;
    // Two blocks of 16 x 4, pitch 5, 12 apart: every rank within sight (46)
    // of the other side's front, so all 128 can be engaged at once. Mustered
    // a side at a time (a batch is one faction: nobody fights while posing).
    std::vector<DistantOrder> orders;
    for (int side = 0; side < 2; side++)
      for (int r = 0; r < 4; r++)
        for (int q = 0; q < 16; q++) {
          const float x = (float)at2.x + (float)(q - 8) * 5.0f;
          const float za = (float)at2.z - 6.0f - 5.0f * (float)r;
          const float zb = (float)at2.z + 6.0f + 5.0f * (float)r;
          if (side == 0)
            orders.push_back({Loadout{"human", "duelist", "sword", nullptr, false, false, "a"}, x,
                              za, Vec3{x, 0.0f, zb}});
          else
            orders.push_back({Loadout{"human", "duelist_blue", "sword", nullptr, false, false, "b"},
                              x, zb, Vec3{x, 0.0f, za}});
        }
    uint32_t tPose2 = 86000;
    built128 = (int)MusterDistant(c, orders, tPose2, why2).size();
    const uint32_t tc0 = 90000;
    for (int k = 0; k < 60; k++) c.mobs.TickDistant(tc0 + (uint32_t)k, c.world, nullptr);
    const DistantMobs::Stats a = tier.GetStats();
    const int measure = 240;
    for (int k = 60; k < 60 + measure; k++) c.mobs.TickDistant(tc0 + (uint32_t)k, c.world, nullptr);
    const DistantMobs::Stats b = tier.GetStats();
    const uint64_t calls = b.tickCallsAll - a.tickCallsAll;
    if (calls > 0) {
      usPerTick = (double)(b.tickNs - a.tickNs) / 1000.0 / (double)calls;
      engagedPerTick = (double)(b.stepsEngaged - a.stepsEngaged) / (double)calls;
      const uint64_t steps = b.steps - a.steps;
      usPerStep = steps > 0 ? (double)(b.tickNs - a.tickNs) / 1000.0 / (double)steps : -1.0;
      const uint64_t ns[8] = {b.nsSetup - a.nsSetup, b.nsSelf - a.nsSelf,     b.nsWorld - a.nsWorld,
                              b.nsThink - a.nsThink, b.nsMove - a.nsMove,     b.nsCombat - a.nsCombat,
                              b.nsSleep - a.nsSleep, b.nsFile - a.nsFile};
      for (int k = 0; k < 8; k++) phase[k] = (double)ns[k] / 1000.0 / (double)calls;
    }
    caps = b.actorCaps - a.actorCaps;
    std::printf("distant-fight: C %d built, %.1f engaged/tick: %.1f us/tick (%.2f us/step); "
                "setup %.1f self %.1f world %.1f think %.1f move %.1f combat %.1f sleep %.1f "
                "file %.1f us/tick; %llu capped actor lists\n",
                built128, engagedPerTick, usPerTick, usPerStep, phase[0], phase[1], phase[2],
                phase[3], phase[4], phase[5], phase[6], phase[7], (unsigned long long)caps);
  }
  const double maxUs = BaselineNumber("distantEngaged128MaxUs", 3000);
  const bool okC = check(built128 == 128 && usPerTick > 0.0 && usPerTick <= maxUs && engagedPerTick >= 100.0,
                         Format("C: %d built, %.1f engaged a tick, %.1f us/tick (ceiling %.0f, "
                                "target 1500)",
                                built128, engagedPerTick, usPerTick, maxUs));

  RecordObserved("distantFight.casualties", (double)dead[0]);
  RecordObserved("distantFight.decidedAtTick", (double)decidedAt[0]);
  RecordObserved("distantFight.quietAtTick", (double)quietAt[0]);
  RecordObserved("distantFight.strokes", (double)strokes / 2.0);
  RecordObserved("distantFight.strokeHits", (double)hits / 2.0);
  RecordObserved("distantFight.strokeBlocked", (double)blocked / 2.0);
  RecordObserved("distantFight.looses", (double)looses / 2.0);
  RecordObserved("distantFight.arrowHits", (double)arrowHits / 2.0);
  RecordObserved("distantFight.bleedTicks", (double)bleedTicks / 2.0);
  RecordObserved("distantFight.gripFails", (double)grips / 2.0);
  RecordObserved("distantFight.corpseWorstGapVox", (double)bWorstGap);
  RecordObserved("distantFight.corpseHighestLimbVox", (double)bWorstHigh);
  RecordObserved("distantFight.engaged128UsPerTick", usPerTick);
  RecordObserved("distantFight.engaged128UsPerStep", usPerStep);
  RecordObserved("distantFight.engaged128PerTick", engagedPerTick);
  const char* names[8] = {"Setup", "Self", "World", "Think", "Move", "Combat", "Sleep", "File"};
  for (int k = 0; k < 8; k++)
    RecordObserved(Format("distantFight.engaged128Us%s", names[k]).c_str(), phase[k]);
  RecordObserved("distantFight.engaged128ActorCaps", (double)caps);

  const bool ok = okDigest && okFight && okQuiet && okB && okC;
  detail = Format("A %s: %d dead (decided at %d, quiet at %d), %llu swings %llu hit %llu blocked, "
                  "%llu arrows %llu hit, digest %016llx | B %s: %d/%d corpses back on the ground "
                  "(worst %.2f) | C %s: 128 engaged %.0f us/tick (think %.0f, world %.0f, self "
                  "%.0f, move %.0f, combat %.0f)",
                  okDigest && okFight && okQuiet ? "ok" : "FAIL", dead[0], decidedAt[0], quietAt[0],
                  (unsigned long long)(strokes / 2), (unsigned long long)(hits / 2),
                  (unsigned long long)(blocked / 2), (unsigned long long)(looses / 2),
                  (unsigned long long)(arrowHits / 2), (unsigned long long)digest[0],
                  okB ? "ok" : "FAIL", bOnGround, bDead, (double)bWorstGap, okC ? "ok" : "FAIL",
                  usPerTick, phase[3], phase[2], phase[1], phase[4], phase[5]);
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];

  // ---- LEAVES NOTHING ----
  // (Arm B's corpses bled on the window's ground: worldgen again, as the
  // world-touching distant gates before it leave it.)
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.ClearVoiceEvents();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---- distant-calibrate ---------------------------------------------------------
Status GateDistantCalibrate(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  struct ItemsScope {
    MobSystem& m;
    const ItemLibrary* prev;
    ItemsScope(MobSystem& mm, const ItemLibrary* now) : m(mm), prev(mm.Items()) {
      m.SetItems(now);
    }
    ~ItemsScope() { m.SetItems(prev); }
  } itemsScope(c.mobs, &c.items);
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetParkFn(nullptr);
  // The ground the muster stands its creatures on while they are posed (the
  // gate may run first: an ungenerated window has none).
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  DistantTierOn();
  c.mobs.SetDistantStream(&c.stream);
  const distant::DistantCalib& cal = c.mobs.DistantCalibration();
  const int seeds = (int)BaselineNumber("distantCalibrate.seeds", 16);
  const double tolWin = BaselineNumber("distantWinRateTol", 0.3);
  const double ttkMin = BaselineNumber("distantTtkRatioMin", 0.75);
  const double ttkMax = BaselineNumber("distantTtkRatioMax", 1.25);
  const double tolHp = BaselineNumber("distantHpLeftTol", 0.2);
  const IVec3 home = c.world.WindowOrigin();
  const Vec3 wlo{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
                 (float)(home.z * (int)kChunk)};
  const IVec3 wc = WindowCentre(c.world);
  // ONE ARENA, outside the window (past the promotion line, so nobody is held
  // there), the flattest dry patch near it.
  const IVec3 arena = Arena((int)(wlo.x + (float)kWorldN) + 220, wc.z);
  std::vector<std::string> fails;
  std::string table;
  int matchupsRun = 0, knownN = 0;
  const double t0 = NowSeconds();
  for (size_t mi = 0; mi < cal.matchups.size(); mi++) {
    const distant::MatchupCalib& row = cal.matchups[mi];
    const BoutSpec* spec = nullptr;
    for (const BoutSpec& b : kBouts)
      if (b.matchup != nullptr && row.name == b.matchup) spec = &b;
    if (spec == nullptr) {
      std::printf("distant-calibrate: matchup %s has no bout spec here; skipped\n",
                  row.name.c_str());
      continue;
    }
    matchupsRun++;
    const int timeout = row.timeoutTicks > 0 ? row.timeoutTicks : 1800;
    int winA = 0, winB = 0, draw = 0, ttkN = 0, built = 0;
    double ttk = 0.0, hpA = 0.0, hpB = 0.0;
    std::string why;
    struct Diag {
      uint64_t swingsA = 0, swingsB = 0, ticks = 0, hits = 0, misses = 0, blocked = 0, far = 0;
      uint64_t looses = 0, arrowHits = 0, grips = 0;
      double blowHp = 0, lostHp = 0;
      std::string deathsA, deathsB;
    } diag;
    for (int s = 0; s < seeds && why.empty(); s++) {
      c.mobs.DistantMut().Clear();
      c.mobs.SetNextIdCounter(200001 + 1000ull * (uint64_t)(mi * (size_t)seeds + (size_t)s));
      // A seeded nudge of the whole bout (D2a's), so two seeds do not start
      // on the same voxel either.
      const uint32_t h = rng::Hash3(0xD157CA1u ^ 0x2Bu, (uint32_t)s, (uint32_t)mi);
      const int jx = (int)(h % 5u) - 2, jz = (int)((h >> 8) % 5u) - 2;
      const int half = (int)std::lround(spec->gap * 0.5f);
      std::vector<uint64_t> a, b;
      {
        std::vector<DistantOrder> orders;
        for (int k = 0; k < spec->aCount; k++) {
          const float x = (float)(arena.x + jx + (k - (spec->aCount - 1) / 2) * 6);
          const float z = (float)(arena.z + jz - half);
          orders.push_back({spec->a, x, z, Vec3{x, 0.0f, z + 2.0f * (float)half}});
        }
        for (int k = 0; k < spec->bCount; k++) {
          const float x = (float)(arena.x + jx + (k - (spec->bCount - 1) / 2) * 6);
          const float z = (float)(arena.z + jz + half);
          orders.push_back({spec->b, x, z, Vec3{x, 0.0f, z - 2.0f * (float)half}});
        }
        // Posed on ticks well before the bout's own (every brain clock the
        // pose ticks set is long past when the bout starts).
        uint32_t tPose = 40000u + 64u * (uint32_t)(mi * (size_t)seeds + (size_t)s);
        const std::vector<uint64_t> got = MusterDistant(c, orders, tPose, why);
        for (size_t k = 0; k < got.size(); k++)
          ((int)k < spec->aCount ? a : b).push_back(got[k]);
      }
      if (!why.empty()) break;
      built++;
      // ATTRIBUTION (rule 6): what each side fights with, once per matchup.
      if (s == 0) {
        auto kitLine = [&](uint64_t id) {
          const DistantMob* d = c.mobs.Distant().Find(id);
          if (d == nullptr) return std::string("?");
          std::string l = Format("%s guard %s ammo %d%s%s:", d->defName.c_str(),
                                 distant::GuardClassName(d->kit.guard), d->ammo,
                                 d->haveSidearm ? " sidearm " : "",
                                 d->haveSidearm ? d->sidearm.c_str() : "");
          for (const distant::DistantStyle& st : d->kit.styles)
            l += Format(" %s[%s %s total %.1f reach %.1f impact %d/%d ticks, table hit %.2f "
                        "power %.2f]",
                        st.name.c_str(), st.weapon.c_str(), distant::StrikeKindName(st.kind),
                        st.strike.Total(), st.reach, st.impactTicks, st.totalTicks,
                        cal.Style(st.name).hitRate, cal.Style(st.name).meanPower);
          int shelled = 0;
          for (const distant::DistantLimb& L : d->kit.limbs) shelled += L.shellHardness > 0.0f;
          l += Format(" | %d limbs shelled", shelled);
          return l;
        };
        std::printf("distant-calibrate: %s A: %s\n", row.name.c_str(), kitLine(a[0]).c_str());
        std::printf("distant-calibrate: %s B: %s\n", row.name.c_str(), kitLine(b[0]).c_str());
      }
      const DistantMobs::Stats sb0 = c.mobs.Distant().GetStats();
      float hp0A = 0, hp0B = 0;
      for (uint64_t id : a) hp0A += DistantHp(c.mobs, id);
      for (uint64_t id : b) hp0B += DistantHp(c.mobs, id);
      const uint32_t tBase = 60000u + 10007u * (uint32_t)s;
      int end = -1, winner = 0;
      // Swings by side (a new strokeStartTick), deaths by side and cause.
      std::vector<uint32_t> lastStart(a.size() + b.size(), 0);
      for (int k = 0; k < timeout; k++) {
        c.mobs.TickDistant(tBase + (uint32_t)k, c.world, nullptr);
        for (size_t q = 0; q < a.size() + b.size(); q++) {
          const uint64_t id = q < a.size() ? a[q] : b[q - a.size()];
          const DistantMob* d = c.mobs.Distant().Find(id);
          if (d == nullptr) continue;
          if (d->strokeStartTick != lastStart[q] && d->strokeEndTick != 0) {
            lastStart[q] = d->strokeStartTick;
            (q < a.size() ? diag.swingsA : diag.swingsB)++;
          }
        }
        // ATTRIBUTION: the first two seeds of each matchup, every 60 ticks.
        if (s < 2 && k % 60 == 0 && !a.empty() && !b.empty()) {
          const DistantMob* da = c.mobs.Distant().Find(a[0]);
          const DistantMob* db = c.mobs.Distant().Find(b[0]);
          if (da != nullptr && db != nullptr) {
            const Vec3 ca = da->Centre(), cb = db->Centre();
            std::printf("distant-calibrate: %s s%d t%4d dist %5.1f | A %s hp %.0f ammo %d "
                        "swing %d draw %d armed %d | B %s hp %.0f swing %d armed %d bleed %.2f\n",
                        row.name.c_str(), s, k,
                        std::sqrt((ca.x - cb.x) * (ca.x - cb.x) + (ca.z - cb.z) * (ca.z - cb.z)),
                        ai::IntentName(da->brain.intent), DistantHp(c.mobs, a[0]), da->ammo,
                        da->strokeEndTick != 0, da->drawLooseTick != 0, da->armed,
                        ai::IntentName(db->brain.intent), DistantHp(c.mobs, b[0]),
                        db->strokeEndTick != 0, db->armed, db->bloodLossPerTick);
          }
        }
        bool aAlive = false, bAlive = false;
        for (uint64_t id : a) aAlive = aAlive || DistantAlive(c.mobs, id);
        for (uint64_t id : b) bAlive = bAlive || DistantAlive(c.mobs, id);
        if (!aAlive || !bAlive) {
          end = k + 1;
          winner = aAlive ? 1 : (bAlive ? 2 : 0);
          break;
        }
      }
      float hpEndA = 0, hpEndB = 0;
      for (uint64_t id : a) hpEndA += DistantHp(c.mobs, id);
      for (uint64_t id : b) hpEndB += DistantHp(c.mobs, id);
      {
        const DistantMobs::Stats sb1 = c.mobs.Distant().GetStats();
        diag.ticks += (uint64_t)(end > 0 ? end : timeout);
        diag.blowHp += (double)(sb1.hpDealtMilli - sb0.hpDealtMilli) / 1000.0;
        diag.lostHp += (double)(hp0A - hpEndA) + (double)(hp0B - hpEndB);
        diag.hits += sb1.strokeHits - sb0.strokeHits;
        diag.misses += sb1.strokeMisses - sb0.strokeMisses;
        diag.blocked += sb1.strokeBlocked - sb0.strokeBlocked;
        diag.far += sb1.strokeOutOfReach - sb0.strokeOutOfReach;
        diag.looses += sb1.looses - sb0.looses;
        diag.arrowHits += sb1.arrowHits - sb0.arrowHits;
        diag.grips += sb1.gripFails - sb0.gripFails;
        for (uint64_t id : a)
          if (const DistantMob* d = c.mobs.Distant().Find(id); d != nullptr && d->dead)
            diag.deathsA += d->cause + "@" + std::to_string(d->deathTick - tBase) + " ";
        for (uint64_t id : b)
          if (const DistantMob* d = c.mobs.Distant().Find(id); d != nullptr && d->dead)
            diag.deathsB += d->cause + "@" + std::to_string(d->deathTick - tBase) + " ";
      }
      hpA += hp0A > 0.0f ? hpEndA / hp0A : 0.0;
      hpB += hp0B > 0.0f ? hpEndB / hp0B : 0.0;
      if (end < 0 || winner == 0) {
        draw++;
      } else {
        (winner == 1 ? winA : winB)++;
        ttk += end;
        ttkN++;
      }
    }
    c.mobs.DistantMut().Clear();
    if (!why.empty() || built == 0) {
      fails.push_back(Format("%s: could not build (%s)", row.name.c_str(), why.c_str()));
      continue;
    }
    const double n = (double)built;
    const double dWinA = winA / n, dWinB = winB / n, dHpA = hpA / n, dHpB = hpB / n;
    const double dTtk = ttkN > 0 ? ttk / ttkN : 0.0;
    const bool fDecided = (row.winA + row.winB) > 0.0f && row.ttkTicks > 0.0f;
    const double ratio = fDecided && ttkN > 0 ? dTtk / row.ttkTicks : -1.0;
    // WIN RATE: within the owner's tolerance, or within two standard errors
    // of the DIFFERENCE of the two measured proportions (full fidelity's
    // row.n bouts, this tier's `built`): the full-fidelity reference is a
    // handful of bouts, and a band tighter than its own resolution would
    // fail a faithful model by chance alone. The effective band is printed.
    auto winBand = [&](float pFull, double pDist) {
      const double pool = ((double)pFull * row.n + pDist * n) / ((double)row.n + n);
      const double se =
          row.n > 0 ? std::sqrt(std::max(0.0, pool * (1.0 - pool)) * (1.0 / row.n + 1.0 / n)) : 0.0;
      return std::max(tolWin, 2.0 * se);
    };
    const double bandA = winBand(row.winA, dWinA), bandB = winBand(row.winB, dWinB);
    const bool okWin = std::fabs(dWinA - row.winA) <= bandA && std::fabs(dWinB - row.winB) <= bandB;
    // TTK where full fidelity decided: the distant tier must decide too, in
    // the owner's band (+/-25%) -- OR within twice the full-fidelity mean's
    // own standard error, which with 3-6 decided bouts of sd ~500 ticks is
    // itself 30-40% of the mean: a band tighter than the reference's own
    // noise would fail a perfect model half the time. Both are printed.
    const int fDecidedN = (int)std::lround((row.winA + row.winB) * (float)row.n);
    const double fSe = fDecidedN > 0 ? row.ttkSd / std::sqrt((double)fDecidedN) : 0.0;
    const bool inBand = ratio >= ttkMin && ratio <= ttkMax;
    const bool inNoise = ttkN > 0 && std::fabs(dTtk - row.ttkTicks) <= 2.0 * fSe;
    const bool okTtk = !fDecided || inBand || inNoise;
    const bool okHp = std::fabs(dHpA - row.hpLeftA) <= tolHp && std::fabs(dHpB - row.hpLeftB) <= tolHp;
    // A KNOWN DIVERGENCE (tests/baseline.json distantCalibrate.knownDivergent,
    // with its reason there): measured and printed like the rest, and not
    // asserted -- a model gap stated as data, not a band widened to hide it.
    const std::string* dvKey = BaselineValue("distantCalibrate.knownDivergent");
    const std::string divergent = dvKey != nullptr ? *dvKey : std::string();
    const bool known = !divergent.empty() && ("," + divergent + ",").find("," + row.name + ",") !=
                                                 std::string::npos;
    const std::string line = Format(
        "%-24s n %2d | winA %.2f (full %.2f, band %.2f) winB %.2f (full %.2f, band %.2f) draw "
        "%.2f (full %.2f) | ttk "
        "%.0f (full %.0f +/- %.0f se, ratio %.2f) | hp left A %.2f (full %.2f) B %.2f (full "
        "%.2f) | %s%s%s%s",
        row.name.c_str(), built, dWinA, row.winA, bandA, dWinB, row.winB, bandB, draw / n,
        row.draw, dTtk,
        row.ttkTicks, fSe, ratio, dHpA, row.hpLeftA, dHpB, row.hpLeftB,
        okWin ? "win ok" : "WIN FAIL",
        okTtk ? (inBand || !fDecided ? ", ttk ok" : ", ttk ok (in the reference's 2 se)")
              : ", TTK FAIL",
        okHp ? ", hp ok" : ", HP FAIL", known ? " [KNOWN DIVERGENCE, not asserted]" : "");
    std::printf("distant-calibrate: %s\n", line.c_str());
    std::printf("distant-calibrate: %s attribution: swings per 1000 ticks A %.1f B %.1f; %llu "
                "hit %llu missed %llu blocked %llu out of reach; %llu arrows %llu hit; %llu grip "
                "fails; hp lost %.0f of which blows %.0f (the rest bleeding); deaths A [%s] B "
                "[%s]\n",
                row.name.c_str(),
                diag.ticks ? 1000.0 * (double)diag.swingsA / (double)diag.ticks : 0.0,
                diag.ticks ? 1000.0 * (double)diag.swingsB / (double)diag.ticks : 0.0,
                (unsigned long long)diag.hits, (unsigned long long)diag.misses,
                (unsigned long long)diag.blocked, (unsigned long long)diag.far,
                (unsigned long long)diag.looses, (unsigned long long)diag.arrowHits,
                (unsigned long long)diag.grips, diag.lostHp, diag.blowHp, diag.deathsA.c_str(),
                diag.deathsB.c_str());
    table += (table.empty() ? "" : " || ") + line;
    if (!(okWin && okTtk && okHp) && !known) fails.push_back(row.name);
    if (known) knownN++;
    const std::string k = "distantCalibrate." + row.name;
    RecordObserved((k + ".winA").c_str(), dWinA);
    RecordObserved((k + ".winB").c_str(), dWinB);
    RecordObserved((k + ".draw").c_str(), draw / n);
    RecordObserved((k + ".ttkTicks").c_str(), dTtk);
    RecordObserved((k + ".ttkRatio").c_str(), ratio);
    RecordObserved((k + ".hpLeftA").c_str(), dHpA);
    RecordObserved((k + ".hpLeftB").c_str(), dHpB);
  }
  const double secs = NowSeconds() - t0;
  const DistantMobs::Stats& st = c.mobs.Distant().GetStats();
  std::printf("distant-calibrate: %d matchups x %d seeds in %.1f s; tier totals: %llu swings, "
              "%llu hits, %llu missed, %llu blocked, %llu out of reach, %llu arrows (%llu hit), "
              "%llu deaths (%llu vital, %llu blood), %llu grip fails, %llu sidearms\n",
              matchupsRun, seeds, secs, (unsigned long long)st.strokes,
              (unsigned long long)st.strokeHits, (unsigned long long)st.strokeMisses,
              (unsigned long long)st.strokeBlocked, (unsigned long long)st.strokeOutOfReach,
              (unsigned long long)st.looses, (unsigned long long)st.arrowHits,
              (unsigned long long)st.deaths, (unsigned long long)st.deathsVital,
              (unsigned long long)st.deathsBlood, (unsigned long long)st.gripFails,
              (unsigned long long)st.sidearms);
  const bool ok = fails.empty() && matchupsRun > knownN;
  detail = Format("%d matchups x %d seeds (%.1f s), %d known divergent: ", matchupsRun, seeds,
                  secs, knownN) +
           table;
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : ", ") + fails[i];
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  return ok ? Status::Pass : Status::Fail;
}

// ---- distant-edge --------------------------------------------------------------
//
// THE EDGE (docs/PLAN_distant_npcs.md 2.7, package D3; game/distant.h). One
// world, THE tick (support::TickCursor, EntityParking bound and in the tick, so
// the park rule DEMOTES and Unpark PROMOTES inside it, as in the game), five
// arms at five places round the window's faces, all at once, run TWICE from a
// fresh world (Fresh: Jolt's ids rewound, the same id base, the same ticks):
//   M. NEAR HITS DISTANT, DISTANT HITS NEAR. A near swordsman 12 voxels inside
//      the +X face and a distant one 3 outside it (anchored: it turns and
//      strikes, never walks), hostile. The near one's strokes queue at their
//      cut and land on the distant body by the roll (its hp drops). The distant
//      one's land through the near body's own door: its lattice is carved (art
//      voxels gone), Blade hp is charged, and blood left it.
//   A. A DISTANT ARCHER SHOOTS IN. An anchored distant archer 50 outside the -X
//      face at a near dummy 10 inside: the arrow is flown for real up to the
//      boundary, injected as an ArrowFlight, and HITS the dummy (an ArrowEvent
//      Hit on it, from ArrowFlights' own PierceAlongRay).
//   B. A NEAR ARCHER SHOOTS OUT. A near archer 30 inside the +Z face at a
//      distant dummy 20 outside: its flights leave the window and are handed
//      to the tier (ArrowEvent PastEdge), resolved by the ranged roll at the
//      flight time left, and at least one lands (the dummy's hp drops).
//   C. AN ABSTRACT ARROW WHOSE TARGET IS NEAR (the impact pass's old
//      `arrowsLost` branch): a distant arrow already in the air at a creature
//      that is not distant (pushed by the gate, as if loosed 60 voxels outside
//      the -X face two ticks before, at the bow's speed) is converted at its
//      arc's state into a
//      real flight, flown to the boundary, injected -- and hits the creature.
//   T. THRASH. A duelist paced across the -Z face, 40 voxels each side: it
//      walks IN as a distant creature (held at the promotion line while it
//      may not come back, promoted in THE tick), and once near at the inner
//      end the gate sends it OUT through the park rule's own demotion
//      (DemoteForTest; a near creature does not walk off known ground, and in
//      the game the outward crossing is the window moving). Its tier changes
//      (observed tick by tick) number at most distantMaxFlips in any 600
//      ticks, it did pace (>= distantEdge.minThrashFlips) and the bound
//      engaged (Stats::flipWaits > 0).
//   P. NEVER A PLAYER. A player actor 12 inside the -X face beside a near
//      dummy, an anchored distant zombie ~30 voxels from both (inside its
//      sight) that targets the dummy: no distant actor list ever held a player id (Stats::
//      playerActorsSeen stays 0) and no distant brain ever targets one --
//      while the zombie was awake and on the dummy (the claim is not vacuous).
//   D. TWICE-RUN: the per-30-tick digest (every near creature's hp and origin
//      bits, the tier's digest, the arrows' counters) is equal.
// Thresholds: tests/baseline.json distantEdge.*, distantMaxFlips.
// LEAVES NOTHING: mobs / debris / tier reset, store cleared, park function and
// player actors removed, tuning / items / id counter restored, ground caches
// dropped, worldgen regenerated (the near fight bled on the window's ground).
Status GateDistantEdge(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  struct ItemsScope {
    MobSystem& m;
    const ItemLibrary* prev;
    ItemsScope(MobSystem& mm, const ItemLibrary* now) : m(mm), prev(mm.Items()) {
      m.SetItems(now);
    }
    ~ItemsScope() { m.SetItems(prev); }
  } itemsScope(c.mobs, &c.items);
  std::vector<std::string> fails, lines;
  auto check = [&](bool ok, const std::string& why) {
    lines.push_back(why);
    if (!ok) fails.push_back(why);
    return ok;
  };
  const int ticks = std::max(60, (int)BaselineNumber("distantEdge.ticks", 900));
  const int maxFlips = (int)BaselineNumber("distantMaxFlips", 4);
  const int minThrash = (int)BaselineNumber("distantEdge.minThrashFlips", 3);
  const DistantMobs& tier = c.mobs.Distant();

  struct Run {
    std::string why;            // the fixture refused (empty = built)
    uint64_t nM = 0, dM = 0, tA = 0, dA = 0, nB = 0, dB = 0, tP = 0, zP = 0, pT = 0, tC = 0;
    int hitsOnTC = 0;           // C: hits the converted arrow made
    std::string convTrace;      // C: the flight, tick by tick, and every event near it
    // M
    float dmHp0 = 0, dmHp1 = 0, nmHp0 = 0, nmHp1 = 0, nmBlood = 0, nmBlade = 0;
    int nmCarvedLimbs = 0;
    // A / B
    int hitsOnTA = 0, pastEdge = 0;
    float dbHp0 = 0, dbHp1 = 0;
    // T
    std::vector<uint32_t> flipTicks;
    int maxWindow = 0;
    bool pacerLost = false;
    std::string pacerTrace;     // every 60 ticks: what the pacer was doing (rule 6)
    // P
    int playerTargets = 0, zAwake = 0, zOnDummy = 0;
    // every counter that moved
    DistantMobs::Stats s0, s1;
    uint64_t arrowsInjected = 0, arrowsHandedOut = 0;
    uint64_t digest = 0;
  };

  auto runOnce = [&](Run& r) {
    Fresh(c, 120001);
    c.stream.Store().Clear();
    c.mobs.SetParkFn(nullptr);
    c.mobs.ClearPlayerActors();
    c.mobs.ClearVoiceEvents();
    DistantTierOn();
    c.mobs.SetDistantStream(&c.stream);
    const IVec3 home = c.world.WindowOrigin();
    const Vec3 wlo{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
                   (float)(home.z * (int)kChunk)};
    const IVec3 wc = WindowCentre(c.world);
    const float N = (float)kWorldN;
    const float faceX = wlo.x + N, faceZ = wlo.z + N;
    const float wcx = (float)wc.x, wcz = (float)wc.z;
    uint32_t t = 120000;
    std::string why;

    // ---- THE DISTANT HALF: built near, posed, demoted (MusterDistant) ------
    const Loadout blueSword{"human", "duelist_blue", "sword", nullptr, false, false,
                            "duelist_blue+sword"};
    const Loadout archerL{"human", "archer", "sword", nullptr, false, true, "archer"};
    const Loadout dummyL{"human", "training_dummy", nullptr, nullptr, false, false, "dummy"};
    const Loadout zombieL{"zombie", "zombie", nullptr, nullptr, false, false, "zombie"};
    const Loadout swordL{"human", "duelist", "sword", nullptr, false, false, "duelist+sword"};
    const Loadout pacerL{"human", "duelist", nullptr, nullptr, false, false, "pacer"};
    std::vector<DistantOrder> orders;
    orders.push_back({blueSword, faceX + 3.0f, wcz, Vec3{faceX - 12.0f, 0.0f, wcz}});
    orders.push_back({archerL, wlo.x - 50.0f, wcz, Vec3{wlo.x + 10.0f, 0.0f, wcz}});
    orders.push_back({dummyL, wcx, faceZ + 20.0f, Vec3{wcx, 0.0f, faceZ - 30.0f}});
    orders.push_back({zombieL, wlo.x - 16.0f, wlo.z + 470.0f, Vec3{wlo.x + 12.0f, 0.0f, wlo.z + 470.0f}});
    const std::vector<uint64_t> d = MusterDistant(c, orders, t, why);
    if (d.size() != 4) {
      r.why = "muster: " + why;
      return;
    }
    r.dM = d[0];
    r.dA = d[1];
    r.dB = d[2];
    r.zP = d[3];
    // ANCHORED (a gate's seam, as the gates write `asleep`): the swordsman,
    // the archer and the zombie turn and strike where they stand and never
    // walk -- so the pair stays across the edge, the archer outside, and the
    // zombie 37 voxels from the player it must not see.
    for (uint64_t id : {r.dM, r.dA, r.zP})
      if (DistantMob* m = c.mobs.DistantMut().Find(id)) m->groundMode = DistantGroundMode::Anchored;

    // ---- THE NEAR HALF ------------------------------------------------------
    r.nM = SpawnOne(c, swordL, ifloor(faceX - 12.0f), wc.z, why);
    r.tA = r.nM ? SpawnOne(c, dummyL, ifloor(wlo.x + 10.0f), wc.z, why) : 0;
    r.nB = r.tA ? SpawnOne(c, archerL, wc.x, ifloor(faceZ - 30.0f), why) : 0;
    r.tP = r.nB ? SpawnOne(c, dummyL, ifloor(wlo.x + 12.0f), ifloor(wlo.z + 470.0f), why) : 0;
    const float pacerX = wcx + 80.0f;
    r.pT = r.tP ? SpawnOne(c, pacerL, ifloor(pacerX), ifloor(wlo.z + 40.0f), why) : 0;
    // C's target: a passive creature of the archer's own faction, so nothing
    // in the fixture shoots at it but the arrow C converts.
    const Loadout monsterDummyL{"human", "dummy", nullptr, nullptr, false, false, "dummy (monster)"};
    r.tC = r.pT ? SpawnOne(c, monsterDummyL, ifloor(wlo.x + 10.0f), wc.z - 120, why) : 0;
    if (r.tC == 0) {
      r.why = "near spawns: " + why;
      return;
    }
    // THE PLAYER, as the NPC targeting layer sees one: an actor in the player
    // band, beside the near dummy, 37 voxels from the distant zombie.
    {
      const int px = ifloor(wlo.x + 12.0f), pz = ifloor(wlo.z + 480.0f);
      const MobSystem::PlayerActorDesc pa{
          Vec3{(float)px, (float)World::TerrainHeight(px, pz, kDefaultSeed) + 10.0f, (float)pz},
          4.0f, 18.0f, true};
      c.mobs.SetPlayerActors(std::span<const MobSystem::PlayerActorDesc>(&pa, 1));
    }

    // ---- THE TICK, with the park rule's tier changes in it ----------------
    EntityParking parking;
    parking.Bind(c.mobs, c.stream.Store(), true);
    const IVec3 pc{wc.x >> 4, World::TerrainHeight(wc.x, wc.z, kDefaultSeed) >> 4, wc.z >> 4};
    support::TickCursor rt{c, t, pc};
    rt.Rig().Authority().parking = &parking;
    ArrowFlights& arrows = rt.Rig().Authority().arrows;
    arrows.ClearEvents();
    r.s0 = tier.GetStats();
    const ArrowFlights::Stats as0 = arrows.stats;
    r.dmHp0 = DistantHp(c.mobs, r.dM);
    r.dbHp0 = DistantHp(c.mobs, r.dB);
    r.nmHp0 = c.mobs.TotalHp(r.nM);
    std::vector<uint32_t> art0;
    if (const Mob* m = c.mobs.FindMobById(r.nM))
      for (int li = 0; li < m->AppendedBase(); li++) art0.push_back(c.mobs.LimbArtVoxelCount(r.nM, li));
    // THE PACER. Its INWARD crossing is the tier's own: a distant walker on a
    // Goto to the inner end walks in over DistantGround, is held at the
    // promotion line while it may not come back, and is PROMOTED in THE tick
    // (Unpark, phase H). Its OUTWARD crossing is driven by the gate: a near
    // creature does not walk off known ground into the unknown past the face
    // (the near drive refuses the step -- measured, it stood at the face), and
    // in the game the outward crossing is the WINDOW moving with the player.
    // So once it is near and has reached the inner end, the gate demotes it
    // through the park rule's own door (DemoteForTest: SaveOne ->
    // DemoteToDistant -> ReleaseRig, the flip noted as PreTick's would be)
    // and stands it at the outer end, to walk in again. Every round trip
    // wants two flips; the bound lets four through per 600 ticks.
    const Vec3 inner{pacerX, 0.0f, wlo.z + 40.0f}, outer{pacerX, 0.0f, wlo.z - 40.0f};
    auto gotoInner = [&]() {
      ai::Brain* b = c.mobs.BrainAnyTierMut(r.pT);
      if (b == nullptr) return;
      b->routine = ai::Routine{};
      b->routine.active = true;
      b->routine.verb = ai::Intent::Goto;
      b->routine.goal = inner;
      b->routine.final = true;
      b->routine.arriveRadius = 4.0f;
      b->routine.speed = 1.0f;
    };
    auto sendOut = [&]() {
      if (!c.mobs.DemoteForTest(r.pT)) return false;
      const DistantMob* d = tier.Find(r.pT);
      if (d == nullptr) return false;
      const Vec3 ws = d->worldSize;
      const int cx = ifloor(outer.x), cz = ifloor(outer.z);
      const float gy = (float)(c.mobs.DistantGroundMut().TerrainTop(cx, cz) + 1);
      c.mobs.DistantMut().Relocate(
          r.pT, Vec3{(float)cx + 0.5f - ws.x * 0.5f, gy, (float)cz + 0.5f - ws.z * 0.5f});
      gotoInner();
      return true;
    };
    int lastTier = 1;
    const std::vector<uint64_t> nearIds = {r.nM, r.tA, r.nB, r.tP, r.pT, r.tC};
    FnvDigest trace;
    for (int k = 0; k < ticks; k++) {
      // ---- C: AN ABSTRACT ARROW WHOSE TARGET IS NEAR (the arrowsLost branch
      // D3 turned into a real flight). A distant arrow in the air, loosed two
      // ticks ago from 60 voxels outside the -X face, at the bow's own speed,
      // at a creature that is NOT distant: the impact pass must convert it, at
      // its arc's state this tick, into an inbound flight, fly it to the
      // boundary and hand it to ArrowFlights -- which must then hit the
      // creature physically. (At the speed a bow looses: an arrow the fixture
      // slowed to 7 m/s for a high arc went through the target's legs without
      // a PierceHit, measured -- a fixture question, not this tier's.)
      if (k == ticks / 3) {
        // AIMED AT THE TRUNK ITSELF (the root limb's centre), not the body
        // box's middle: measured, this standing dummy's pelvis sits 1.3
        // voxels off its box centre, and a ray through the box middle along
        // the flight's line met no collider at all.
        Vec3 to{};
        const Mob* tcm = c.mobs.FindMobById(r.tC);
        if (tcm != nullptr && tcm->Def() != nullptr &&
            tcm->LimbCentreWorld(tcm->Def()->rootLimb, to)) {
          const int fx = ifloor(wlo.x - 60.0f), fz = ifloor(to.z);
          const DistantMob* sh = tier.Find(r.dA);
          DistantArrowFlight f;
          f.shooter = r.dA;
          f.target = r.tC;
          f.from = Vec3{(float)fx + 0.5f, (float)(World::TerrainHeight(fx, fz, kDefaultSeed) + 10),
                        (float)fz + 0.5f};
          f.to = to;
          const float speed =
              sh != nullptr && sh->kit.arrowSpeed > 0.0f ? sh->kit.arrowSpeed : 500.0f;
          const int flight = std::max(3, (int)std::ceil((to - f.from).len() / (speed / 30.0f)));
          f.looseTick = t + 1 - 2;
          f.impactTick = f.looseTick + (uint32_t)flight;
          f.ammo = sh != nullptr && !sh->kit.ammo.empty() ? sh->kit.ammo : std::string("arrow");
          f.out.hit = true;
          c.mobs.DistantMut().ArrowsMut().push_back(f);
        }
      }
      rt();
      // ---- A / B: what the arrows did --------------------------------------
      for (const ArrowEvent& e : arrows.Events()) {
        if (e.kind == ArrowEvent::Kind::Hit && e.mobId == r.tA) r.hitsOnTA++;
        if (e.kind == ArrowEvent::Kind::Hit && e.mobId == r.tC) r.hitsOnTC++;
        // C's attribution (rule 6): every arrow event in the -X face's
        // quarter round the target, after the push.
        if (k >= ticks / 3 && k < ticks / 3 + 90 && e.at.x < wlo.x + 60.0f &&
            std::fabs(e.at.z - (float)(wc.z - 120)) < 40.0f)
          r.convTrace += Format(" ev%d:k%d mob%llu at(%.1f,%.1f,%.1f)", (int)e.kind, k,
                                (unsigned long long)e.mobId, (double)(e.at.x - wlo.x), (double)e.at.y,
                                (double)(e.at.z - wlo.z));
        if (e.kind == ArrowEvent::Kind::PastEdge) r.pastEdge++;
      }
      arrows.ClearEvents();
      if (k >= ticks / 3 && k < ticks / 3 + 90) {
        for (const DistantArrowFlight& f : tier.Arrows())
          if (f.target == r.tC)
            r.convTrace += Format(" k%d:%s(%.1f,%.1f,%.1f)", k, f.inbound ? "in" : "abs",
                                  (double)(SpellFxToFloat(f.pos[0]) - wlo.x),
                                  (double)SpellFxToFloat(f.pos[1]),
                                  (double)(SpellFxToFloat(f.pos[2]) - wlo.z));
        for (const ArrowFlight& f : arrows.Flights())
          if (f.shot.target == r.tC)
            r.convTrace += Format(" k%d:real(%.1f,%.1f,%.1f)", k,
                                  (double)(SpellFxToFloat(f.pos.x) - wlo.x),
                                  (double)SpellFxToFloat(f.pos.y),
                                  (double)(SpellFxToFloat(f.pos.z) - wlo.z));
        if (k == ticks / 3) {
          Vec3 lo{}, hi{};
          if (c.mobs.MobBodyBox(r.tC, lo, hi)) {
            r.convTrace += Format(" target box (%.1f..%.1f, %.1f..%.1f, %.1f..%.1f)",
                                  (double)(lo.x - wlo.x), (double)(hi.x - wlo.x), (double)lo.y,
                                  (double)hi.y, (double)(lo.z - wlo.z), (double)(hi.z - wlo.z));
            // ...and what a ray through its middle along the flight's line
            // meets (the collider the flight's PierceAlongRay would cast for).
            const Vec3 mid = (lo + hi) * 0.5f;
            float frac = 1.0f;
            const uint64_t hb = c.phys.CastRayBody(Vec3{wlo.x - 2.0f, mid.y, mid.z}, Vec3{1, 0, 0},
                                                   40.0f, frac);
            int li = -1;
            const Mob* owner = hb != 0 ? c.mobs.FindOwner(hb, &li) : nullptr;
            int bodies = 0;
            for (int i = 0; i < 40; i++) bodies += c.mobs.LimbBody(r.tC, i) != 0 ? 1 : 0;
            Vec3 lc0{};
            const Mob* tm = c.mobs.FindMobById(r.tC);
            const bool haveLc = tm != nullptr && tm->LimbCentreWorld(tm->Def()->rootLimb, lc0);
            r.convTrace += Format(" ray body %llu owner %llu limb %d at %.1f; target limb bodies "
                                  "%d alive %d, root limb centre %s(%.1f,%.1f,%.1f)",
                                  (unsigned long long)hb,
                                  (unsigned long long)(owner ? owner->Id() : 0), li,
                                  (double)(frac * 40.0f - 2.0f), bodies,
                                  tm != nullptr && tm->Alive() ? 1 : 0, haveLc ? "" : "NONE ",
                                  (double)(lc0.x - wlo.x), (double)lc0.y, (double)(lc0.z - wlo.z));
          }
        }
      }
      // ---- P: never a player, and the zombie on the dummy ------------------
      for (const DistantMob& dm : tier.All())
        if (dm.brain.hasTarget && ai::IsPlayerActorId(dm.brain.targetId)) r.playerTargets++;
      if (const DistantMob* z = tier.Find(r.zP)) {
        if (!z->asleep && !z->dead) r.zAwake++;
        if (z->brain.hasTarget && z->brain.targetId == r.tP) r.zOnDummy++;
      }
      // ---- T: the pacer's tier, tick by tick, and its next crossing --------
      // (Read after the tick and again after the gate's own demotion, so a
      // flip is stamped with the tick it happened on.)
      auto observe = [&]() {
        const int tierNow = c.mobs.FindMobById(r.pT) != nullptr ? 1
                            : tier.Find(r.pT) != nullptr    ? 2
                                                            : 0;
        if (tierNow == 0) {
          r.pacerLost = true;
          return 0;
        }
        if (tierNow != lastTier) r.flipTicks.push_back(t);
        lastTier = tierNow;
        return tierNow;
      };
      const int tierNow = observe();
      if (tierNow != 0) {
        Vec3 pcen{};
        if (tierNow == 1) {
          Vec3 lo{}, hi{};
          pcen = c.mobs.MobBodyBox(r.pT, lo, hi) ? (lo + hi) * 0.5f : c.mobs.MobOrigin(r.pT);
        } else {
          pcen = tier.Find(r.pT)->Centre();
        }
        const ai::Brain* b = c.mobs.BrainAnyTierMut(r.pT);
        if (k % 60 == 0 && b != nullptr)
          r.pacerTrace += Format(" t%d:%s(%.0f,%.0f,%.0f) %s%s%s", k, tierNow == 1 ? "near" : "far",
                                 (double)(pcen.x - wlo.x), (double)pcen.y, (double)(pcen.z - wlo.z),
                                 ai::IntentName(b->intent), b->routine.active ? "" : " !routine",
                                 b->hasTarget ? " target" : "");
        const float dz = pcen.z - inner.z, dx = pcen.x - inner.x;
        if (tierNow == 1 && (k == 0 || (b != nullptr && b->routine.arrived) || dx * dx + dz * dz <= 36.0f)) {
          if (sendOut()) observe();
        }
      }
      // ---- D: the trace ------------------------------------------------------
      if (k % 30 == 29 || k + 1 == ticks) {
        for (const uint64_t id : nearIds) {
          const Mob* m = c.mobs.FindMobById(id);
          trace.U(id);
          trace.U(m != nullptr ? (m->Alive() ? 2u : 1u) : 0u);
          if (m == nullptr) continue;
          trace.F(m->Origin().x);
          trace.F(m->Origin().y);
          trace.F(m->Origin().z);
          trace.F(c.mobs.TotalHp(id));
        }
        trace.U(TierDigest(tier));
        trace.U(arrows.stats.launched);
        trace.U(arrows.stats.hits);
        trace.U(arrows.stats.injected);
        trace.U(arrows.stats.handedOut);
        trace.U((uint64_t)arrows.Live());
      }
    }
    r.digest = trace.h;
    r.s1 = tier.GetStats();
    r.arrowsInjected = arrows.stats.injected - as0.injected;
    r.arrowsHandedOut = arrows.stats.handedOut - as0.handedOut;
    r.dmHp1 = DistantHp(c.mobs, r.dM);
    r.dbHp1 = DistantHp(c.mobs, r.dB);
    // The near swordsman: wherever it is now (a demotion would have moved it).
    if (const Mob* m = c.mobs.FindMobById(r.nM)) {
      r.nmHp1 = c.mobs.TotalHp(r.nM);
      r.nmBlood = c.mobs.BloodLost(r.nM);
      r.nmBlade = m->HpLostBy(DamageCause::Blade);
      for (int li = 0; li < m->AppendedBase() && li < (int)art0.size(); li++)
        r.nmCarvedLimbs += c.mobs.LimbArtVoxelCount(r.nM, li) < art0[(size_t)li] ? 1 : 0;
    }
    // THE BOUND, as observed: the most flips inside any 600 ticks.
    for (size_t a = 0; a < r.flipTicks.size(); a++) {
      int n = 0;
      for (size_t b = a; b < r.flipTicks.size(); b++)
        n += r.flipTicks[b] - r.flipTicks[a] < kDistantFlipWindowTicks ? 1 : 0;
      r.maxWindow = std::max(r.maxWindow, n);
    }
    rt.Rig().Authority().parking = nullptr;
    c.mobs.SetParkFn(nullptr);
    c.mobs.ClearPlayerActors();
  };

  Run runs[2];
  for (int k = 0; k < 2; k++) {
    runOnce(runs[k]);
    if (!runs[k].why.empty()) break;
  }
  const Run& r = runs[0];
  const bool built = runs[0].why.empty() && runs[1].why.empty();
  check(built, built ? std::string("fixture: built twice")
                     : "fixture: " + (runs[0].why.empty() ? runs[1].why : runs[0].why));
  auto delta = [&](uint64_t DistantMobs::Stats::*f) { return r.s1.*f - r.s0.*f; };

  // ---- M ----
  const bool okM = check(
      built && delta(&DistantMobs::Stats::edgeNearHits) > 0 && r.dmHp1 < r.dmHp0 &&
          delta(&DistantMobs::Stats::edgeFarApplied) > 0 && r.nmHp1 < r.nmHp0 &&
          r.nmCarvedLimbs > 0 && r.nmBlade > 0.0f && r.nmBlood > 0.0f,
      Format("M: near->distant blows %llu (hit %llu, miss %llu, blocked %llu, out of reach "
             "%llu, no target %llu, unmatched %llu), distant hp %.0f -> %.0f | distant->near "
             "swings %llu (hit %llu, applied %llu, unapplied %llu, ray misses %llu, covered "
             "%llu; hp roll %.1f, door %.1f), near hp %.0f -> %.0f, %d limbs carved, Blade hp "
             "%.1f, blood lost %.2f",
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearBlows),
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearHits),
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearMisses),
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearBlocked),
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearOutOfReach),
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearNoTarget),
             (unsigned long long)delta(&DistantMobs::Stats::edgeNearUnmatched), (double)r.dmHp0,
             (double)r.dmHp1, (unsigned long long)delta(&DistantMobs::Stats::edgeFarSwings),
             (unsigned long long)delta(&DistantMobs::Stats::edgeFarHits),
             (unsigned long long)delta(&DistantMobs::Stats::edgeFarApplied),
             (unsigned long long)delta(&DistantMobs::Stats::edgeFarUnapplied),
             (unsigned long long)delta(&DistantMobs::Stats::edgeFarRayMisses),
             (unsigned long long)delta(&DistantMobs::Stats::edgeFarCovered),
             (double)delta(&DistantMobs::Stats::edgeFarHpRollMilli) / 1000.0,
             (double)delta(&DistantMobs::Stats::edgeFarHpDoorMilli) / 1000.0, (double)r.nmHp0,
             (double)r.nmHp1, r.nmCarvedLimbs, (double)r.nmBlade, (double)r.nmBlood));
  // ---- A ----
  const bool okA = check(
      built && delta(&DistantMobs::Stats::edgeArrowsIn) > 0 && r.arrowsInjected > 0 &&
          r.hitsOnTA > 0,
      Format("A: distant archer loosed %llu inbound, %llu injected (tier %llu), %llu grounded, "
             "%llu expired, %d hits on the near dummy",
             (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsIn),
             (unsigned long long)r.arrowsInjected,
             (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsInjected),
             (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsGrounded),
             (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsExpired), r.hitsOnTA));
  // ---- C ----
  const bool okC = check(built && delta(&DistantMobs::Stats::edgeArrowsConverted) > 0 &&
                             r.hitsOnTC > 0,
                         Format("C: abstract arrows converted to real flights %llu, hits on "
                                "their near target %d",
                                (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsConverted),
                                r.hitsOnTC));
  if (!okC) std::printf("distant-edge: C trace:%s\n", r.convTrace.c_str());
  // ---- B ----
  const bool okB = check(
      built && r.arrowsHandedOut > 0 && r.pastEdge > 0 &&
          delta(&DistantMobs::Stats::edgeArrowsOutResolved) > 0 && r.dbHp1 < r.dbHp0,
      Format("B: near archer's flights handed out %llu (events %d), resolved %llu, lost %llu, "
             "distant dummy hp %.0f -> %.0f",
             (unsigned long long)r.arrowsHandedOut, r.pastEdge,
             (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsOutResolved),
             (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsOutLost), (double)r.dbHp0,
             (double)r.dbHp1));
  // ---- T ----
  std::string flips;
  for (const uint32_t ft : r.flipTicks) flips += Format("%s%u", flips.empty() ? "" : ",", ft - 120000u);
  const bool okT = check(built && !r.pacerLost && (int)r.flipTicks.size() >= minThrash &&
                             r.maxWindow <= maxFlips && delta(&DistantMobs::Stats::flipWaits) > 0,
                         Format("T: %zu tier flips (at %s), at most %d in 600 ticks (bound %d), "
                                "flip waits %llu, lost %d",
                                r.flipTicks.size(), flips.c_str(), r.maxWindow, maxFlips,
                                (unsigned long long)delta(&DistantMobs::Stats::flipWaits),
                                r.pacerLost ? 1 : 0));
  if (!okT && !r.pacerTrace.empty())
    std::printf("distant-edge: T pacer (window-relative x,z):%s\n", r.pacerTrace.c_str());
  // ---- P ----
  const bool okP = check(built && delta(&DistantMobs::Stats::playerActorsSeen) == 0 &&
                             r.playerTargets == 0 && r.zAwake >= ticks / 2 && r.zOnDummy > 0,
                         Format("P: player ids in distant actor lists %llu, distant brains on a "
                                "player %d; the zombie awake %d of %d ticks, on the dummy %d",
                                (unsigned long long)delta(&DistantMobs::Stats::playerActorsSeen),
                                r.playerTargets, r.zAwake, ticks, r.zOnDummy));
  // ---- D ----
  const bool okD = check(built && runs[0].digest == runs[1].digest && runs[0].digest != 0,
                         Format("D: twice-run digests %016llx / %016llx",
                                (unsigned long long)runs[0].digest,
                                (unsigned long long)runs[1].digest));

  RecordObserved("distantEdge.nearBlows", (double)delta(&DistantMobs::Stats::edgeNearBlows));
  RecordObserved("distantEdge.nearHits", (double)delta(&DistantMobs::Stats::edgeNearHits));
  RecordObserved("distantEdge.farSwings", (double)delta(&DistantMobs::Stats::edgeFarSwings));
  RecordObserved("distantEdge.farApplied", (double)delta(&DistantMobs::Stats::edgeFarApplied));
  RecordObserved("distantEdge.farHpRoll",
                 (double)delta(&DistantMobs::Stats::edgeFarHpRollMilli) / 1000.0);
  RecordObserved("distantEdge.farHpDoor",
                 (double)delta(&DistantMobs::Stats::edgeFarHpDoorMilli) / 1000.0);
  RecordObserved("distantEdge.nearCarvedLimbs", (double)r.nmCarvedLimbs);
  RecordObserved("distantEdge.arrowsIn", (double)delta(&DistantMobs::Stats::edgeArrowsIn));
  RecordObserved("distantEdge.arrowsInjected", (double)r.arrowsInjected);
  RecordObserved("distantEdge.arrowHitsIn", (double)r.hitsOnTA);
  RecordObserved("distantEdge.arrowsOut", (double)r.arrowsHandedOut);
  RecordObserved("distantEdge.arrowsOutResolved",
                 (double)delta(&DistantMobs::Stats::edgeArrowsOutResolved));
  RecordObserved("distantEdge.thrashFlips", (double)r.flipTicks.size());
  RecordObserved("distantEdge.thrashMaxPer600", (double)r.maxWindow);
  RecordObserved("distantEdge.flipWaits", (double)delta(&DistantMobs::Stats::flipWaits));
  RecordObserved("distantEdge.bandPeak", (double)r.s1.bandPeak);

  const bool ok = okM && okA && okB && okC && okT && okP && okD;
  detail = Format("M %s: near->distant %llu hit, distant->near %llu applied (near %d limbs "
                  "carved) | A %s: %d arrows in hit | B %s: %llu out resolved | T %s: %zu flips, "
                  "<= %d/600 | P %s | D %s: %016llx",
                  okM ? "ok" : "FAIL", (unsigned long long)delta(&DistantMobs::Stats::edgeNearHits),
                  (unsigned long long)delta(&DistantMobs::Stats::edgeFarApplied), r.nmCarvedLimbs,
                  okA ? "ok" : "FAIL", r.hitsOnTA, okB ? "ok" : "FAIL",
                  (unsigned long long)delta(&DistantMobs::Stats::edgeArrowsOutResolved),
                  okT ? "ok" : "FAIL", r.flipTicks.size(), r.maxWindow, okP ? "ok" : "FAIL",
                  okD ? "ok" : "FAIL", (unsigned long long)runs[0].digest);
  // EVERY claim's numbers, pass or fail: one line each, so a run says why.
  for (const std::string& l : lines) std::printf("distant-edge: %s\n", l.c_str());
  std::printf("distant-edge: %s\n", ok ? "PASS" : "FAIL");
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];

  // ---- LEAVES NOTHING ----
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.SetParkFn(nullptr);
  c.mobs.ClearPlayerActors();
  c.mobs.ClearVoiceEvents();
  c.stream.Store().Clear();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  c.mobs.SetDistantStream(&c.stream);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// The near crowd, in id order, as a twice-run must reproduce it: every living
// or dead creature's id, life, origin bits and TotalHp.
void NearDigest(MobSystem& mobs, FnvDigest& dg) {
  std::vector<uint64_t> ids;
  for (uint32_t i = 0; i < mobs.MobCount(); i++)
    if (const Mob* m = mobs.MobAt(i)) ids.push_back(m->Id());
  std::sort(ids.begin(), ids.end());
  for (const uint64_t id : ids) {
    const Mob* m = mobs.FindMobById(id);
    if (m == nullptr) continue;
    dg.U(id);
    dg.U(m->Alive() ? 2u : 1u);
    dg.F(m->Origin().x);
    dg.F(m->Origin().y);
    dg.F(m->Origin().z);
    dg.F(mobs.TotalHp(id));
  }
}

// THE ITEM LIBRARY for a gate's duration (the D2b gates' own scope).
struct GateItemsScope {
  MobSystem& m;
  const ItemLibrary* prev;
  GateItemsScope(MobSystem& mm, const ItemLibrary* now) : m(mm), prev(mm.Items()) {
    m.SetItems(now);
  }
  ~GateItemsScope() { m.SetItems(prev); }
};

// ---- distant-overflow (D8) ------------------------------------------------------
//
// FIGHTS LARGER THAN 64 (docs/PLAN_distant_npcs.md D8; game/distant.h, MobSystem::
// OverflowSwaps). One world, THE tick with parking bound (Unpark -- and the
// overflow pass in it -- in phase H, as in the game), a player actor at the
// window centre, run TWICE from a fresh world (Fresh: the same id base, the
// same ticks):
//   S. THE STATIC SWAP. 64 near creatures -- the living cap, full -- in a ring
//      round the player: 60 training dummies 170..215 voxels out, leaving a gap
//      of +/-40 degrees round +X where 4 blue duelists stand at 225; and 8
//      distant dummies INSIDE the window, 65..100 from the player (built near,
//      demoted, stood there by the gate). The cap binds: each distant dummy
//      is swapped in for the farthest near creature, one a tick. After
//      distantOverflow.ticksStatic ticks: >= distantOverflow.minSwaps swaps,
//      and THE NEAR SIDE IS THE SIDE NEAREST THE PLAYER -- no living distant
//      creature waiting over the promotion line stands nearer the player than
//      the farthest near creature less the swap margin.
//   F. THE FIGHT ACROSS IT. The blues S demoted are stood 12 voxels nearer
//      the player than the farthest near dummy (still distant: nothing near is
//      farther than them by the margin), and 8 red duelists (hostile to them),
//      held anchored 80 voxels outside the -X face through S, two to a blue
//      ~26 voxels in front of it, 36 nearer than that dummy: the reds are
//      swapped in (far dummies out), and the reds -- near -- and the blues --
//      distant, in the window, held at the player's ring -- fight ACROSS THE
//      TIERS (near strokes at distant ones queued at their cut, distant swings
//      through the near door) for distantOverflow.ticksFight ticks: >= 1 swap,
//      cross-tier blows >= distantOverflow.minCrossTierBlows.
//   Throughout: never more than 64 living near; no overflow creature ever
//      stands nearer the player than distant.overflowMinDistVox less a voxel;
//      every swap traded a near creature at least that far for a waiting one
//      nearer by the margin; no player id in any distant actor list and no
//      distant brain on a player; no creature changes tier more than
//      distantMaxFlips times in any 600 ticks.
//   D. TWICE-RUN: the per-30-tick digest (every near creature's id, life,
//      origin bits and hp; the tier's digest; the overflow counters) is equal.
// Thresholds: tests/baseline.json distantOverflow.*, distantMaxFlips.
// LEAVES NOTHING: mobs / debris / tier reset, the store cleared, the park
// function, the player actors and the overflow log removed, tuning / items /
// the id counter restored, ground caches dropped, worldgen regenerated at home.
Status GateDistantOverflow(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  GateItemsScope itemsScope(c.mobs, &c.items);
  std::vector<std::string> fails, lines;
  auto check = [&](bool ok, const std::string& why) {
    lines.push_back(why);
    if (!ok) fails.push_back(why);
    return ok;
  };
  const int ticksS = std::max(30, (int)BaselineNumber("distantOverflow.ticksStatic", 120));
  const int ticksF = std::max(30, (int)BaselineNumber("distantOverflow.ticksFight", 360));
  const int minSwaps = (int)BaselineNumber("distantOverflow.minSwaps", 8);
  const int minCross = (int)BaselineNumber("distantOverflow.minCrossTierBlows", 1);
  const int maxFlips = (int)BaselineNumber("distantMaxFlips", 4);
  const float ringVox = (float)BaselineNumber("distantOverflow.ringVox", 150.0);
  const DistantMobs& tier = c.mobs.Distant();
  constexpr float kPi = 3.14159265f;

  struct Run {
    std::string why;
    std::vector<uint64_t> distS, reds, blues, ring;
    uint32_t maxLive = 0;
    uint64_t swapsS = 0, swapsF = 0, filled = 0;
    float maxNearS = 0.0f, minWaitS = 0.0f;
    uint64_t maxNearId = 0, minWaitId = 0;
    bool waitAny = false;
    uint64_t crossNear = 0, crossNearHits = 0, crossFar = 0, crossFarApplied = 0;
    int ringViol = 0;
    std::string ringWhy;
    int badSwaps = 0;
    std::string badWhy;
    int playerTargets = 0;
    int maxWindow = 0;
    uint64_t maxWindowId = 0;
    uint64_t digest = 0;
    DistantMobs::Stats s0, sMid, s1;
    std::vector<DistantOverflowSwap> log;
  };

  auto runOnce = [&](Run& r) {
    Fresh(c, 140001);
    c.stream.Store().Clear();
    c.mobs.SetParkFn(nullptr);
    c.mobs.ClearPlayerActors();
    c.mobs.ClearVoiceEvents();
    c.mobs.ClearOverflowLog();
    DistantTierOn();
    {
      Tuning tu = CurrentTuning();
      tu.distant.overflowMinDistVox = ringVox;
      SetCurrentTuning(tu);
    }
    c.mobs.SetDistantStream(&c.stream);
    const IVec3 home = c.world.WindowOrigin();
    const Vec3 wlo{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
                   (float)(home.z * (int)kChunk)};
    const IVec3 wc = WindowCentre(c.world);
    const float wcx = (float)wc.x, wcz = (float)wc.z;
    uint32_t t = 140000;
    std::string why;
    auto at = [&](float rad, float deg) {
      const float a = deg * kPi / 180.0f;
      return Vec3{wcx + rad * std::cos(a), 0.0f, wcz + rad * std::sin(a)};
    };
    const Loadout dummyL{"human", "training_dummy", nullptr, nullptr, false, false, "dummy"};
    const Loadout redL{"human", "duelist", "sword", nullptr, false, false, "red duelist"};
    const Loadout blueL{"human", "duelist_blue", "sword", nullptr, false, false, "blue duelist"};

    // ---- THE DISTANT HALF, mustered FIRST (the living cap is free now) ------
    std::vector<DistantOrder> orders;
    for (int k = 0; k < 8; k++) {
      const Vec3 p = at(65.0f + 5.0f * (float)k, 22.5f + 45.0f * (float)k);
      orders.push_back({dummyL, p.x, p.z, Vec3{wcx, 0.0f, wcz}});
    }
    for (int k = 0; k < 8; k++)
      orders.push_back({redL, wlo.x - 80.0f, wcz - 28.0f + 8.0f * (float)k,
                        Vec3{wlo.x + 40.0f, 0.0f, wcz}});
    const std::vector<uint64_t> d = MusterDistant(c, orders, t, why);
    if (d.size() != 16) {
      r.why = "muster: " + why;
      return;
    }
    r.distS.assign(d.begin(), d.begin() + 8);
    r.reds.assign(d.begin() + 8, d.end());
    // HELD OUT OF IT through S (a gate's seam, as distant-edge's): the reds
    // think and turn 80 voxels past the -X face, and never walk.
    for (const uint64_t id : r.reds)
      if (DistantMob* m = c.mobs.DistantMut().Find(id)) m->groundMode = DistantGroundMode::Anchored;

    // ---- THE NEAR HALF: 64, the cap ----------------------------------------
    for (int k = 0; k < 4 && why.empty(); k++) {
      const Vec3 p = at(225.0f, -15.0f + 10.0f * (float)k);
      const uint64_t id = SpawnOne(c, blueL, ifloor(p.x), ifloor(p.z), why);
      if (id != 0) r.blues.push_back(id);
    }
    for (int k = 0; k < 60 && why.empty(); k++) {
      const float deg = 40.0f + 280.0f * (float)k / 59.0f;
      const float rad = 170.0f + 45.0f * (float)((k * 7) % 10) / 9.0f;
      const Vec3 p = at(rad, deg);
      const uint64_t id = SpawnOne(c, dummyL, ifloor(p.x), ifloor(p.z), why);
      if (id != 0) r.ring.push_back(id);
    }
    if (!why.empty() || c.mobs.LiveMobCount() != MobSystem::MaxLiveMobs()) {
      r.why = Format("near spawns: %s (%u living of %u)", why.c_str(), c.mobs.LiveMobCount(),
                     MobSystem::MaxLiveMobs());
      return;
    }
    // THE PLAYER, as the NPC layer sees one, at the window centre -- set after
    // the muster, so no creature carried a player target into the tier.
    const MobSystem::PlayerActorDesc pa{
        Vec3{wcx, (float)World::TerrainHeight(wc.x, wc.z, kDefaultSeed) + 9.0f, wcz}, 4.0f,
        18.0f, true};
    c.mobs.SetPlayerActors(std::span<const MobSystem::PlayerActorDesc>(&pa, 1));

    // ---- THE TICK, parking bound: Unpark + the overflow in phase H --------
    EntityParking parking;
    parking.Bind(c.mobs, c.stream.Store(), true);
    const IVec3 pc{wc.x >> 4, World::TerrainHeight(wc.x, wc.z, kDefaultSeed) >> 4, wc.z >> 4};
    support::TickCursor rt{c, t, pc};
    rt.Rig().Authority().parking = &parking;
    r.s0 = tier.GetStats();
    std::vector<uint64_t> all;
    for (const auto* v : {&r.distS, &r.reds, &r.blues, &r.ring}) all.insert(all.end(), v->begin(), v->end());
    std::sort(all.begin(), all.end());
    std::vector<int> lastTier(all.size(), 0);
    std::vector<std::vector<uint32_t>> flipAt(all.size());
    for (size_t k = 0; k < all.size(); k++)
      lastTier[k] = c.mobs.FindMobById(all[k]) != nullptr ? 1 : tier.Find(all[k]) != nullptr ? 2 : 0;
    FnvDigest trace;
    auto observe = [&](int k) {
      r.maxLive = std::max(r.maxLive, c.mobs.LiveMobCount());
      for (size_t i = 0; i < all.size(); i++) {
        const int now = c.mobs.FindMobById(all[i]) != nullptr ? 1
                        : tier.Find(all[i]) != nullptr      ? 2
                                                            : 0;
        if (now != lastTier[i] && now != 0 && lastTier[i] != 0) flipAt[i].push_back(t);
        lastTier[i] = now;
      }
      for (const DistantMob& dm : tier.All()) {
        if (dm.dead) continue;
        if (dm.brain.hasTarget && ai::IsPlayerActorId(dm.brain.targetId)) r.playerTargets++;
        if (!dm.overflow) continue;
        const Vec3 cc = dm.Centre();
        if (!c.world.CellInWindow(IVec3{ifloor(cc.x), ifloor(cc.y), ifloor(cc.z)})) continue;
        const float pd = c.mobs.PlayerDistance(cc);
        if (pd < ringVox - 1.0f) {
          if (r.ringViol++ == 0)
            r.ringWhy = Format("%llu at %.1f from the player, tick %d", (unsigned long long)dm.id,
                               (double)pd, k);
        }
      }
      if (k % 30 == 29) {
        NearDigest(c.mobs, trace);
        trace.U(TierDigest(tier));
        trace.U(tier.GetStats().overflowSwaps - r.s0.overflowSwaps);
        trace.U(tier.GetStats().overflowFilled - r.s0.overflowFilled);
      }
    };

    // ======== S ===================================================================
    for (int k = 0; k < ticksS; k++) {
      rt();
      observe(k);
    }
    r.sMid = tier.GetStats();
    r.swapsS = r.sMid.overflowSwaps - r.s0.overflowSwaps;
    // THE PARTITION: the farthest near creature against the nearest living
    // distant one waiting over the promotion line (one the thrash bound holds
    // back excepted -- none is expected here).
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++) {
      const Mob* m = c.mobs.MobAt(i);
      if (m == nullptr || !m->Alive()) continue;
      Vec3 lo{}, hi{};
      if (!c.mobs.MobBodyBox(m->Id(), lo, hi)) continue;
      const float pd = c.mobs.PlayerDistance((lo + hi) * 0.5f);
      if (pd > r.maxNearS) {
        r.maxNearS = pd;
        r.maxNearId = m->Id();
      }
    }
    {
      std::vector<uint64_t> cands;
      c.mobs.Distant().PromotionCandidates(c.world.WindowOrigin(), EntityParking::kUnparkMarginChunks,
                                           cands);
      for (const uint64_t id : cands) {
        const DistantMob* dm = tier.Find(id);
        if (dm == nullptr || dm->dead || !c.mobs.DistantFlipBudgetOk(id, t)) continue;
        const float pd = c.mobs.PlayerDistance(dm->Centre());
        if (!r.waitAny || pd < r.minWaitS) {
          r.minWaitS = pd;
          r.minWaitId = id;
          r.waitAny = true;
        }
      }
    }

    // ======== F ===================================================================
    // THE TWO SIDES, stood by DISTANCE FROM THE PLAYER (3D, PlayerDistance's
    // own measure -- the terrain round the ring is not level) relative to the
    // farthest near creature S left, `farNear`:
    //   * the blues (overflow, distant, in the window) at farNear - 12: no
    //     near creature is farther than them by the margin, so they stay
    //     distant;
    //   * the reds two to a blue, 3 degrees either side of it, at farNear -
    //     36 -- ~26 voxels from it, inside a duelist's sight (46) -- and
    //     ANCHORED while they wait (a gate's seam: a red that walked out to
    //     meet a blue would stop being nearer the player than the near
    //     dummies, and never be swapped in). Each is nearer than the far
    //     dummies by more than the margin, so they come in one a tick, and
    //     near reds fight distant blues across the tiers.
    // `place` solves the column's planar radius for the wanted distance.
    const float farNear = r.maxNearS;
    auto place = [&](uint64_t id, float deg, float want) {
      DistantMob* m = c.mobs.DistantMut().Find(id);
      if (m == nullptr) return;
      const Vec3 ws = m->worldSize;
      float rad = want;
      Vec3 o{};
      for (int it = 0; it < 4; it++) {
        const Vec3 p = at(rad, deg);
        const float gy =
            (float)(c.mobs.DistantGroundMut().TerrainTop(ifloor(p.x), ifloor(p.z)) + 1);
        o = Vec3{p.x - ws.x * 0.5f, gy, p.z - ws.z * 0.5f};
        rad += want - c.mobs.PlayerDistance(o + ws * 0.5f);
      }
      c.mobs.DistantMut().Relocate(id, o);
      if (DistantMob* mm = c.mobs.DistantMut().Find(id)) {
        mm->asleep = false;
        mm->wakeTick = 0;
        mm->steer.heading = mm->steer.desiredHeading = kPi * 0.5f - deg * kPi / 180.0f;
      }
    };
    for (size_t k = 0; k < r.blues.size(); k++)
      place(r.blues[k], -15.0f + 10.0f * (float)k, farNear - 12.0f);
    for (size_t k = 0; k < r.reds.size(); k++)
      place(r.reds[k], -15.0f + 10.0f * (float)(k / 2) + ((k % 2) != 0 ? 3.0f : -3.0f),
            farNear - 36.0f);
    c.mobs.DistantMut().MarkAwakeHint();
    c.mobs.DistantMut().MarkMoved();
    for (int k = 0; k < ticksF; k++) {
      rt();
      observe(ticksS + k);
    }
    r.s1 = tier.GetStats();
    r.swapsF = r.s1.overflowSwaps - r.sMid.overflowSwaps;
    r.filled = r.s1.overflowFilled - r.s0.overflowFilled;
    r.crossNear = r.s1.edgeNearBlows - r.sMid.edgeNearBlows;
    r.crossNearHits = r.s1.edgeNearHits - r.sMid.edgeNearHits;
    r.crossFar = r.s1.edgeFarSwings - r.sMid.edgeFarSwings;
    r.crossFarApplied = r.s1.edgeFarApplied - r.sMid.edgeFarApplied;
    trace.U(TierDigest(tier));
    NearDigest(c.mobs, trace);
    r.digest = trace.h;
    r.log = c.mobs.OverflowLog();
    for (const DistantOverflowSwap& s : r.log)
      if (!(s.demotedDist >= ringVox - 1e-3f) ||
          !(s.demotedDist > s.promotedDist + kDistantOverflowMarginVox - 1e-3f)) {
        if (r.badSwaps++ == 0)
          r.badWhy = Format("tick %u: %llu out at %.1f for %llu in at %.1f", s.tick - 140000u,
                            (unsigned long long)s.demoted, (double)s.demotedDist,
                            (unsigned long long)s.promoted, (double)s.promotedDist);
      }
    // THE BOUND, as observed: the most flips of any one creature in 600 ticks.
    for (size_t i = 0; i < all.size(); i++) {
      const std::vector<uint32_t>& f = flipAt[i];
      for (size_t a = 0; a < f.size(); a++) {
        int n = 0;
        for (size_t b = a; b < f.size(); b++) n += f[b] - f[a] < kDistantFlipWindowTicks ? 1 : 0;
        if (n > r.maxWindow) {
          r.maxWindow = n;
          r.maxWindowId = all[i];
        }
      }
    }
    rt.Rig().Authority().parking = nullptr;
    c.mobs.SetParkFn(nullptr);
    c.mobs.ClearPlayerActors();
  };

  Run runs[2];
  for (int k = 0; k < 2; k++) {
    runOnce(runs[k]);
    if (!runs[k].why.empty()) break;
  }
  const Run& r = runs[0];
  const bool built = runs[0].why.empty() && runs[1].why.empty();
  check(built, built ? std::string("fixture: built twice (64 near, 16 distant)")
                     : "fixture: " + (runs[0].why.empty() ? runs[1].why : runs[0].why));
  auto delta = [&](uint64_t DistantMobs::Stats::*f) { return r.s1.*f - r.s0.*f; };
  // ---- S ----
  const bool okS = check(
      built && (int)r.swapsS >= minSwaps && r.waitAny &&
          r.maxNearS <= r.minWaitS + kDistantOverflowMarginVox + 1e-3f,
      Format("S: %llu swaps (need %d), farthest near %llu at %.1f, nearest waiting %llu at %.1f "
             "(margin %.0f)",
             (unsigned long long)r.swapsS, minSwaps, (unsigned long long)r.maxNearId,
             (double)r.maxNearS, (unsigned long long)r.minWaitId, (double)r.minWaitS,
             (double)kDistantOverflowMarginVox));
  // ---- F ----
  const uint64_t cross = r.crossNear + r.crossFar;
  const bool okF = check(built && r.swapsF >= 1 && cross >= (uint64_t)minCross,
                         Format("F: %llu swaps; across the tiers: %llu near strokes at distant "
                                "ones (%llu hit), %llu distant swings at near ones (%llu through "
                                "the door)",
                                (unsigned long long)r.swapsF, (unsigned long long)r.crossNear,
                                (unsigned long long)r.crossNearHits, (unsigned long long)r.crossFar,
                                (unsigned long long)r.crossFarApplied));
  // ---- THROUGHOUT ----
  const bool okCap = check(built && r.maxLive <= MobSystem::MaxLiveMobs(),
                           Format("cap: at most %u living near (cap %u); %llu slots filled "
                                  "nearest-first, %llu no-victim waits, %llu ground waits, %llu "
                                  "refused, %llu last resorts",
                                  r.maxLive, MobSystem::MaxLiveMobs(), (unsigned long long)r.filled,
                                  (unsigned long long)delta(&DistantMobs::Stats::overflowNoVictim),
                                  (unsigned long long)delta(&DistantMobs::Stats::overflowGroundWaits),
                                  (unsigned long long)delta(&DistantMobs::Stats::overflowRefused),
                                  (unsigned long long)delta(&DistantMobs::Stats::overflowLastResort)));
  const bool okRing = check(built && r.ringViol == 0 && r.badSwaps == 0,
                            Format("ring: %d overflow creatures nearer than %.0f%s%s; %llu steps "
                                   "held at it; %d swaps against the rule%s%s",
                                   r.ringViol, (double)ringVox, r.ringViol ? " -- first " : "",
                                   r.ringWhy.c_str(),
                                   (unsigned long long)delta(&DistantMobs::Stats::overflowRingHolds),
                                   r.badSwaps, r.badSwaps ? " -- first " : "", r.badWhy.c_str()));
  const bool okP = check(built && delta(&DistantMobs::Stats::playerActorsSeen) == 0 &&
                             r.playerTargets == 0,
                         Format("P: player ids in distant actor lists %llu, distant brains on a "
                                "player %d",
                                (unsigned long long)delta(&DistantMobs::Stats::playerActorsSeen),
                                r.playerTargets));
  const bool okT = check(built && r.maxWindow <= maxFlips,
                         Format("T: at most %d tier flips in 600 ticks (creature %llu; bound %d), "
                                "%llu flip waits",
                                r.maxWindow, (unsigned long long)r.maxWindowId, maxFlips,
                                (unsigned long long)delta(&DistantMobs::Stats::flipWaits)));
  const bool okD = check(built && runs[0].digest == runs[1].digest && runs[0].digest != 0,
                         Format("D: twice-run digests %016llx / %016llx",
                                (unsigned long long)runs[0].digest,
                                (unsigned long long)runs[1].digest));
  RecordObserved("distantOverflow.swapsStatic", (double)r.swapsS);
  RecordObserved("distantOverflow.swapsFight", (double)r.swapsF);
  RecordObserved("distantOverflow.filled", (double)r.filled);
  RecordObserved("distantOverflow.maxLive", (double)r.maxLive);
  RecordObserved("distantOverflow.crossNearBlows", (double)r.crossNear);
  RecordObserved("distantOverflow.crossFarSwings", (double)r.crossFar);
  RecordObserved("distantOverflow.crossFarApplied", (double)r.crossFarApplied);
  RecordObserved("distantOverflow.ringHolds",
                 (double)delta(&DistantMobs::Stats::overflowRingHolds));
  RecordObserved("distantOverflow.inWindowPeak", (double)r.s1.overflowInWindowPeak);
  RecordObserved("distantOverflow.maxFlipsPer600", (double)r.maxWindow);
  RecordObserved("distantOverflow.partitionNear", (double)r.maxNearS);
  RecordObserved("distantOverflow.partitionWaiting", (double)r.minWaitS);
  // RULE 6: every swap of the first run, so a red says who traded for whom.
  for (const DistantOverflowSwap& s : r.log)
    std::printf("distant-overflow: swap t+%u: %llu out (%.1f from the player%s) for %llu in "
                "(%.1f)\n",
                s.tick - 140000u, (unsigned long long)s.demoted, (double)s.demotedDist,
                s.lastResort ? ", on a player" : "", (unsigned long long)s.promoted,
                (double)s.promotedDist);

  const bool ok = okS && okF && okCap && okRing && okP && okT && okD;
  detail = Format("S %s: %llu swaps | F %s: %llu swaps, %llu cross-tier blows | cap %s: <= %u "
                  "| ring %s | P %s | T %s: <= %d/600 | D %s: %016llx",
                  okS ? "ok" : "FAIL", (unsigned long long)r.swapsS, okF ? "ok" : "FAIL",
                  (unsigned long long)r.swapsF, (unsigned long long)cross, okCap ? "ok" : "FAIL",
                  r.maxLive, okRing ? "ok" : "FAIL", okP ? "ok" : "FAIL", okT ? "ok" : "FAIL",
                  r.maxWindow, okD ? "ok" : "FAIL", (unsigned long long)runs[0].digest);
  for (const std::string& l : lines) std::printf("distant-overflow: %s\n", l.c_str());
  std::printf("distant-overflow: %s\n", ok ? "PASS" : "FAIL");
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];

  // ---- LEAVES NOTHING ----
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.SetParkFn(nullptr);
  c.mobs.ClearPlayerActors();
  c.mobs.ClearVoiceEvents();
  c.mobs.ClearOverflowLog();
  c.stream.Store().Clear();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  c.mobs.SetDistantStream(&c.stream);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---- distant-arrow (D9) ----------------------------------------------------------
//
// THE PLAYER'S REACH (docs/PLAN_distant_npcs.md D9; game/arrows.h). One world,
// THE tick (TickCursor; no parking bound, so nothing is promoted), run TWICE
// from a fresh world:
//   O. PAST THE EDGE, A HIT. A distant training dummy (anchored) ~40 voxels
//      outside the +X face; a player-style flight (shooter in the player band,
//      no ArrowShot target) loosed from just inside the face at the bow's full
//      draw, held over for the drop, at its body centre. It leaves the window
//      (ArrowEvent Continued), is flown on by the tier, and HITS the dummy
//      (DistantShotEvent Hit) within distantArrow.tickSlack ticks of the tick
//      the same integrator, flown in the open, first enters its body box --
//      and the dummy's hp is lower on that very tick.
//   M. A MISS LANDS. A second flight from beside the first, 30 voxels to the
//      side and pitched at the ground ~75 past the face: it continues, and
//      meets DistantGround OUTSIDE the window (DistantShotEvent Grounded)
//      within distantArrow.groundGapMax of TerrainHeight + 1 under it,
//      hitting nobody.
//   I. IN THE WINDOW. A distant dummy demoted where it stands INSIDE the
//      window (a statue: no collider); a flight 16 voxels off at its centre
//      meets its BODY BOX (ArrowEvent DistantHit), and the next distant tick
//      applies it (DistantShotEvent BoxHit): its hp is lower.
//   D. TWICE-RUN: the per-tick digest (the tier's; every arrow and shot
//      event's tick, kind, target, limb and hp bits) is equal.
// Thresholds: tests/baseline.json distantArrow.*.
// LEAVES NOTHING: mobs / debris / tier reset, tuning / items / the id counter
// restored, ground caches dropped, worldgen regenerated at home.
Status GateDistantArrow(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  GateItemsScope itemsScope(c.mobs, &c.items);
  std::vector<std::string> fails, lines;
  auto check = [&](bool ok, const std::string& why) {
    lines.push_back(why);
    if (!ok) fails.push_back(why);
    return ok;
  };
  const int ticks = std::max(60, (int)BaselineNumber("distantArrow.ticks", 90));
  const int slack = (int)BaselineNumber("distantArrow.tickSlack", 2);
  const float gapMax = (float)BaselineNumber("distantArrow.groundGapMax", 2.0);
  const DistantMobs& tier = c.mobs.Distant();
  const ItemDef* bow = c.items.Named("bow");
  const ItemDef* arrowDef = bow != nullptr ? c.items.Named(bow->bow.ammo) : nullptr;
  if (bow == nullptr || arrowDef == nullptr || !arrowDef->IsAmmo()) {
    detail = "no bow or no arrow for it in the item library";
    return Status::Fail;
  }
  const float speed = BowArrowSpeed(*bow, BowFullDrawTicks(*bow));   // world voxels/s
  const uint32_t kSeedO = 0xD9000001u, kSeedM = 0xD9000002u, kSeedI = 0xD9000003u;

  struct Run {
    std::string why;
    uint64_t dO = 0, dI = 0;
    float oHp0 = 0, oHp1 = 0, iHp0 = 0, iHp1 = 0;
    uint32_t tLaunch = 0, tLaunchI = 0;
    int expectO = -1;           // the open flight's tick into the box (after the loose)
    int oHit = -1, oDrop = -1;  // ticks after the loose: the Hit event; the hp first lower
    int oLimb = -1;
    float oHp = 0.0f;
    int continued = 0;
    int mGround = -1;
    Vec3 mAt{};
    float mGap = 1e9f;
    bool mOutside = false;
    int shotHitsOther = 0;      // Hit / BoxHit events on anyone but the expected dummy
    int iDistantHit = -1, iBoxHit = -1;   // ticks after I's loose
    int nearHits = 0;           // ArrowEvent Hit (a NEAR body): there is none to hit
    uint64_t digest = 0;
    DistantMobs::Stats s0, s1;
  };

  auto runOnce = [&](Run& r) {
    Fresh(c, 150001);
    c.stream.Store().Clear();
    c.mobs.SetParkFn(nullptr);
    c.mobs.ClearPlayerActors();
    c.mobs.ClearDistantShotEvents();
    DistantTierOn();
    c.mobs.SetDistantStream(&c.stream);
    const IVec3 home = c.world.WindowOrigin();
    const Vec3 wlo{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
                   (float)(home.z * (int)kChunk)};
    const IVec3 wc = WindowCentre(c.world);
    const float faceX = wlo.x + (float)kWorldN;
    uint32_t t = 150000;
    std::string why;
    const Loadout dummyL{"human", "training_dummy", nullptr, nullptr, false, false, "dummy"};
    const IVec3 aO = Arena((int)faceX + 40, wc.z);
    const IVec3 aI = Arena(wc.x - 60, wc.z + 60);
    std::vector<DistantOrder> orders;
    orders.push_back({dummyL, (float)aO.x, (float)aO.z, Vec3{faceX - 40.0f, 0.0f, (float)aO.z}});
    orders.push_back({dummyL, (float)aI.x, (float)aI.z, Vec3{(float)aI.x - 40.0f, 0.0f, (float)aI.z}});
    const std::vector<uint64_t> d = MusterDistant(c, orders, t, why);
    if (d.size() != 2) {
      r.why = "muster: " + why;
      return;
    }
    r.dO = d[0];
    r.dI = d[1];
    // Both stand where they are put (a gate's seam): a shot at a statue that
    // wandered is not the claim.
    for (const uint64_t id : d)
      if (DistantMob* m = c.mobs.DistantMut().Find(id)) m->groundMode = DistantGroundMode::Anchored;
    const DistantMob* o = tier.Find(r.dO);
    const DistantMob* in = tier.Find(r.dI);
    if (o == nullptr || in == nullptr || !c.world.CellInWindow(IVec3{ifloor(in->Centre().x),
                                                                    ifloor(in->Centre().y),
                                                                    ifloor(in->Centre().z)})) {
      r.why = "the dummies are not where the arms need them (O outside, I inside the window)";
      return;
    }
    const Vec3 oLo = o->origin, oHi = o->origin + o->worldSize;
    const Vec3 oC = o->Centre();
    const Vec3 iC = in->Centre();

    support::TickCursor rt{c, t, IVec3{wc.x >> 4,
                                       World::TerrainHeight(wc.x, wc.z, kDefaultSeed) >> 4,
                                       wc.z >> 4}};
    ArrowFlights& arrows = rt.Rig().Authority().arrows;
    arrows.ClearEvents();
    r.s0 = tier.GetStats();
    r.oHp0 = DistantHp(c.mobs, r.dO);
    r.iHp0 = DistantHp(c.mobs, r.dI);
    const uint64_t shooter = ai::kPlayerActorBase;

    // O: from just inside the face, at the centre, held over for the drop.
    const float yO = std::max(oC.y, (float)World::TerrainHeight(ifloor(faceX - 3.0f), ifloor(oC.z),
                                                                kDefaultSeed) + 10.0f);
    const Vec3 fromO{faceX - 3.0f, yO, oC.z};
    const Vec3 dirO = ArrowHoldover(fromO, oC, speed) - fromO;
    // THE OPEN FLIGHT the same integrator flies (velocity loses g, then the
    // tick's move), in float: the tick it first enters the box.
    {
      const float g = ArrowGravityPerTick2();
      Vec3 p = fromO, v = dirO * (speed / 30.0f / std::max(1e-6f, dirO.len()));
      for (int k = 1; k <= ArrowFlights::kLifeTicks && r.expectO < 0; k++) {
        v.y -= g;
        const Vec3 q = p + v;
        float t0 = 0.0f, t1 = 1.0f;
        bool hit = true;
        const float A[3] = {p.x, p.y, p.z}, D[3] = {v.x, v.y, v.z};
        const float L[3] = {oLo.x, oLo.y, oLo.z}, H[3] = {oHi.x, oHi.y, oHi.z};
        for (int a = 0; a < 3 && hit; a++) {
          if (std::fabs(D[a]) < 1e-9f) {
            hit = A[a] >= L[a] && A[a] <= H[a];
            continue;
          }
          float ta = (L[a] - A[a]) / D[a], tb = (H[a] - A[a]) / D[a];
          if (ta > tb) std::swap(ta, tb);
          t0 = std::max(t0, ta);
          t1 = std::min(t1, tb);
          hit = t0 <= t1;
        }
        if (hit) r.expectO = k;
        p = q;
      }
    }
    // M: 30 to the side, pitched at the ground ~75 past the face.
    const float zM = oC.z + 30.0f;
    const Vec3 fromM{faceX - 3.0f, yO, zM};
    const Vec3 aimM{faceX + 75.0f,
                    (float)World::TerrainHeight(ifloor(faceX + 75.0f), ifloor(zM), kDefaultSeed) + 1.0f,
                    zM};
    FnvDigest trace;
    for (int k = 0; k < ticks; k++) {
      if (k == 0) {
        r.tLaunch = t;
        arrows.Launch(*arrowDef, fromO, dirO, speed, Vec3{}, kSeedO, {}, shooter);
        arrows.Launch(*arrowDef, fromM, aimM - fromM, speed, Vec3{}, kSeedM, {}, shooter);
      }
      if (k == ticks / 3) {
        // I: 16 voxels off on -X, level with its centre.
        r.tLaunchI = t;
        arrows.Launch(*arrowDef, iC - Vec3{16.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, speed, Vec3{},
                      kSeedI, {}, shooter);
      }
      rt();
      for (const ArrowEvent& e : arrows.Events()) {
        trace.U((uint64_t)e.kind);
        trace.U(e.seed);
        trace.U(e.mobId);
        if (e.kind == ArrowEvent::Kind::Continued) r.continued++;
        if (e.kind == ArrowEvent::Kind::Hit) r.nearHits++;
        if (e.kind == ArrowEvent::Kind::DistantHit && e.seed == kSeedI && e.mobId == r.dI &&
            r.iDistantHit < 0)
          r.iDistantHit = (int)(t - r.tLaunchI);
      }
      arrows.ClearEvents();
      for (const DistantShotEvent& e : c.mobs.DistantShotEvents()) {
        trace.U((uint64_t)e.kind);
        trace.U(e.target);
        trace.U(e.tick);
        trace.U((uint64_t)(int64_t)e.limb);
        trace.F(e.hp);
        if (e.kind == DistantShotEvent::Kind::Hit) {
          if (e.target == r.dO && r.oHit < 0) {
            r.oHit = (int)(e.tick - r.tLaunch);
            r.oLimb = e.limb;
            r.oHp = e.hp;
          } else {
            r.shotHitsOther++;
          }
        }
        if (e.kind == DistantShotEvent::Kind::BoxHit) {
          if (e.target == r.dI && r.iBoxHit < 0) r.iBoxHit = (int)(e.tick - r.tLaunchI);
          else if (e.target != r.dI) r.shotHitsOther++;
        }
        if (e.kind == DistantShotEvent::Kind::Grounded && r.mGround < 0) {
          r.mGround = (int)(e.tick - r.tLaunch);
          r.mAt = e.at;
          const float gy = (float)(World::TerrainHeight(ifloor(e.at.x), ifloor(e.at.z),
                                                       kDefaultSeed) + 1);
          r.mGap = std::fabs(e.at.y - gy);
          r.mOutside = !c.world.CellInWindow(IVec3{ifloor(e.at.x), ifloor(e.at.y), ifloor(e.at.z)});
        }
      }
      c.mobs.ClearDistantShotEvents();
      if (r.oDrop < 0 && DistantHp(c.mobs, r.dO) < r.oHp0) r.oDrop = (int)(t - r.tLaunch);
      trace.U(TierDigest(tier));
    }
    r.s1 = tier.GetStats();
    r.oHp1 = DistantHp(c.mobs, r.dO);
    r.iHp1 = DistantHp(c.mobs, r.dI);
    r.digest = trace.h;
  };

  Run runs[2];
  for (int k = 0; k < 2; k++) {
    runOnce(runs[k]);
    if (!runs[k].why.empty()) break;
  }
  const Run& r = runs[0];
  const bool built = runs[0].why.empty() && runs[1].why.empty();
  check(built, built ? std::string("fixture: built twice")
                     : "fixture: " + (runs[0].why.empty() ? runs[1].why : runs[0].why));
  auto delta = [&](uint64_t DistantMobs::Stats::*f) { return r.s1.*f - r.s0.*f; };
  const bool okO = check(
      built && r.continued >= 2 && r.oHit >= 0 && r.expectO > 0 && r.oHit >= r.expectO &&
          r.oHit <= r.expectO + slack && r.oDrop == r.oHit && r.oHp1 < r.oHp0,
      Format("O: %d flights continued past the face; the hit on the distant dummy %d ticks after "
             "the loose (the open flight enters its box at %d, slack %d), limb %d, %.1f hp; its "
             "hp %.0f -> %.0f (first lower at %d)",
             r.continued, r.oHit, r.expectO, slack, r.oLimb, (double)r.oHp, (double)r.oHp0,
             (double)r.oHp1, r.oDrop));
  const bool okM = check(built && r.mGround > 0 && r.mOutside && r.mGap <= gapMax,
                         Format("M: the miss landed %d ticks after the loose at (%.1f, %.1f, %.1f) "
                                "-- %s the window, %.2f off the ground (max %.1f)",
                                r.mGround, (double)r.mAt.x, (double)r.mAt.y, (double)r.mAt.z,
                                r.mOutside ? "outside" : "INSIDE", (double)r.mGap, (double)gapMax));
  const bool okI = check(built && r.iDistantHit > 0 && r.iBoxHit == r.iDistantHit + 1 &&
                             r.iHp1 < r.iHp0,
                         Format("I: the in-window statue's box met %d ticks after the loose, "
                                "applied at %d; its hp %.0f -> %.0f",
                                r.iDistantHit, r.iBoxHit, (double)r.iHp0, (double)r.iHp1));
  const bool okX = check(built && r.shotHitsOther == 0 && r.nearHits == 0,
                         Format("X: hits on anyone else %d, near-body hits %d (none to hit)",
                                r.shotHitsOther, r.nearHits));
  const bool okD = check(built && runs[0].digest == runs[1].digest && runs[0].digest != 0,
                         Format("D: twice-run digests %016llx / %016llx",
                                (unsigned long long)runs[0].digest,
                                (unsigned long long)runs[1].digest));
  RecordObserved("distantArrow.hitTicks", (double)r.oHit);
  RecordObserved("distantArrow.expectTicks", (double)r.expectO);
  RecordObserved("distantArrow.hitHp", (double)r.oHp);
  RecordObserved("distantArrow.groundGap", (double)r.mGap);
  RecordObserved("distantArrow.shotsOut", (double)delta(&DistantMobs::Stats::shotsOut));
  RecordObserved("distantArrow.boxHitsApplied", (double)delta(&DistantMobs::Stats::boxHitsApplied));

  const bool ok = okO && okM && okI && okX && okD;
  detail = Format("O %s: hit at +%d (open +%d) | M %s: landed outside, %.2f off | I %s: box +%d, "
                  "applied +%d | X %s | D %s: %016llx",
                  okO ? "ok" : "FAIL", r.oHit, r.expectO, okM ? "ok" : "FAIL", (double)r.mGap,
                  okI ? "ok" : "FAIL", r.iDistantHit, r.iBoxHit, okX ? "ok" : "FAIL",
                  okD ? "ok" : "FAIL", (unsigned long long)runs[0].digest);
  for (const std::string& l : lines) std::printf("distant-arrow: %s\n", l.c_str());
  std::printf("distant-arrow: %s\n", ok ? "PASS" : "FAIL");
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];

  // ---- LEAVES NOTHING ----
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.ClearDistantShotEvents();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  c.mobs.SetDistantStream(&c.stream);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DistantCalibGates() {
  static const std::vector<Gate> g = {
      // Spawns creatures in fresh worlds (its own id base, Jolt id history
      // rewound per round), sets the shipped item library for the duration,
      // and regenerates worldgen + restores the id counter on the way out.
      {"distant-calib-matrix", "mob", {}, false, GateDistantCalibMatrix},
      // D2b (docs/PLAN_distant_npcs.md): the tier FIGHTS. Both build their
      // creatures near and demote them (DemoteForTest), step the tier past
      // the window, and leave nothing: mobs / debris / tier reset, tuning,
      // items and the id counter put back, ground caches dropped. Neither
      // edits or moves the window; distant-fight's arm B ticks THE tick.
      {"distant-fight", "mob", {}, false, GateDistantFight, /*needsRender=*/false},
      {"distant-calibrate", "mob", {}, false, GateDistantCalibrate, /*needsRender=*/false},
      // D3 (docs/PLAN_distant_npcs.md 2.7): THE EDGE. Two fresh worlds (Fresh:
      // worldgen, Jolt ids rewound, its own id base), each ticked by THE tick
      // with parking bound; leaves nothing -- mobs / debris / tier reset, the
      // store cleared, park function and player actors removed, tuning, items
      // and the id counter put back, worldgen regenerated at home.
      {"distant-edge", "mob", {}, false, GateDistantEdge, /*needsRender=*/false},
      // D8 (docs/PLAN_distant_npcs.md): THE CAP OVERFLOW, and D9: THE PLAYER'S
      // REACH. Two fresh worlds each, THE tick (distant-overflow with parking
      // bound); the same exit contract as distant-edge, plus the overflow log
      // and the shot events cleared.
      {"distant-overflow", "mob", {}, false, GateDistantOverflow, /*needsRender=*/false},
      {"distant-arrow", "mob", {}, false, GateDistantArrow, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
