// demon_seals.cpp — seals, channels, strength, release, gaze, tells. See
// demon_seals.h.
#include "game/demon_seals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

#include "game/demon.h"
#include "game/demon_talk.h"
#include "game/mob.h"
#include "game/session.h"
#include "sim/materials.h"
#include "sim/rng.h"
#include "sim/scale.h"
#include "sim/tickinput.h"
#include "sim/world.h"   // PowderMassOfState
#include "ui/overlay.h"

using json = nlohmann::json;

namespace demon {

namespace {

constexpr const char* kChannelNames[kChannels] = {"move", "cast_out", "blink"};

LiveDemon* FindLive(TickAuthorityCtx& w, uint64_t id) {
  if (!w.demons) return nullptr;
  for (LiveDemon& ld : w.demons->live)
    if (ld.mobId == id) return &ld;
  return nullptr;
}
const LiveDemon* FindLive(const TickAuthorityCtx& w, uint64_t id) {
  if (!w.demons) return nullptr;
  for (const LiveDemon& ld : w.demons->live)
    if (ld.mobId == id) return &ld;
  return nullptr;
}

int32_t ResistOf(const DemonDef* def, Channel c) {
  if (def == nullptr) return 0;
  for (const auto& [k, v] : def->resist)
    if (k == kChannelNames[(int)c]) return v;
  return 0;   // a channel the demon does not list: it does not have it
}

uint32_t MatByName(const std::vector<MaterialDef>& mats, const std::string& n) {
  for (size_t i = 1; i < mats.size(); i++)
    if (mats[i].name == n) return (uint32_t)i;
  return 0;
}

// A cell's MASS in eighths: powder its grain mass, a liquid its fullness, any
// other matter a whole cell.
int32_t EighthsOf(const std::vector<MaterialDef>& mats, uint32_t word) {
  const uint32_t m = word & 0xFFFu, st = (word >> 12) & 15u;
  if (m >= mats.size()) return 8;
  switch (mats[m].gpu.klass) {
    case CLASS_POWDER: return (int32_t)PowderMassOfState(st);
    case CLASS_LIQUID: return (int32_t)std::min<uint32_t>(st + 1u, 8u);
    default: return 8;
  }
}

std::vector<TellBand> ParseBands(const json& a, std::string& log, const std::string& where) {
  std::vector<TellBand> out;
  if (!a.is_array()) {
    log += "demons: tells.json: " + where + " is not a list of bands\n";
    return out;
  }
  for (const json& b : a) {
    if (!b.is_object()) continue;
    TellBand t;
    t.minMargin = b.value("minMargin", 0);
    for (const json& l : b.value("lines", json::array()))
      if (l.is_string()) t.lines.push_back(l.get<std::string>());
    if (!t.lines.empty()) out.push_back(std::move(t));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const TellBand& a, const TellBand& b) { return a.minMargin > b.minMargin; });
  return out;
}

// Re-evaluate one contained demon's channels and strength from its reading.
void Evaluate(LiveDemon& ld, const DemonDef* def, const SealLib& lib) {
  Binding& b = ld.bind;
  b.basePower = def ? def->power : 0;
  b.power = b.basePower;
  b.ironCut = 0;
  if (!b.reading.valid) {
    // Never read (its band out of every store at arrival): everything
    // severed, strength unknown -- D1's containment, nothing more.
    b.severed = (uint8_t)((1u << kChannels) - 1u);
    b.strength = 0;
    return;
  }
  b.severed = 0;
  for (int c = 0; c < kChannels; c++)
    if (b.reading.potency[(size_t)c] >= ResistOf(def, (Channel)c)) b.severed |= (uint8_t)(1u << c);
  // IRON WEAKENS (D6): potency x susceptibility, capped at weakenCapPct of the
  // demon's power -- iron alone never zeroes a greater demon.
  if (def != nullptr && b.reading.weakenPotency > 0) {
    const int64_t cut = (int64_t)b.reading.weakenPotency * std::max(0, def->ironSusceptibility) / 100;
    const int64_t cap = (int64_t)b.basePower * std::clamp(lib.weakenCapPct, 0, 100) / 100;
    b.ironCut = (int32_t)std::clamp<int64_t>(cut, 0, cap);
    b.power = b.basePower - b.ironCut;
  }
  b.strength = b.reading.sealPoints +
               std::min(b.reading.candlesLit, std::max(0, lib.candlesMax)) * lib.perCandle -
               b.strainLoss;
}

void Gaze(LiveDemon& ld, const SealLib& lib, MobSystem& mobs,
          std::span<SessionTick> players) {
  Binding& b = ld.bind;
  b.gazeBroken = false;
  const SessionTick* who = nullptr;
  for (const SessionTick& p : players)
    if (p.s->index == ld.session) who = &p;
  const Mob* m = mobs.FindMobById(ld.mobId);
  bool inRange = false, broken = false;
  if (who != nullptr && m != nullptr && m->Def() != nullptr) {
    const Vec3 ws = m->Def()->worldSize;
    const Vec3 head = m->Origin() + Vec3{ws.x * 0.5f, ws.y * 0.85f, ws.z * 0.5f};
    const Vec3 v = head - who->s->player.EyePos();
    const float dist = v.len();
    inRange = dist > 1e-3f && dist <= MetresToCells(lib.gazeRangeM);
    if (inRange) {
      const float c = std::cos(lib.gazeConeDeg * 3.14159265f / 180.0f);
      const bool looking = (v * (1.0f / dist)).dot(who->ti.lookFwd.normalized()) >= c;
      broken = b.gazeHold ? !looking : looking;
    }
  }
  if (inRange && broken) {
    b.gazeBroken = true;
    b.strain += std::max(0, lib.strainRise);
    if (b.strain >= std::max(1, lib.strainMax)) {
      // THE LAPSE: the circle loses a chunk; the meter empties. Never a fail.
      b.strain = 0;
      b.strainLoss = std::min(std::max(0, lib.strainLossMax), b.strainLoss + lib.strainPenalty);
      b.lapses++;
    }
  } else {
    b.strain = std::max(0, b.strain - std::max(0, lib.strainFall));
  }
}

}  // namespace

const char* ChannelName(Channel c) { return kChannelNames[(int)c]; }

bool ChannelByName(const std::string& n, Channel& out) {
  for (int i = 0; i < kChannels; i++)
    if (n == kChannelNames[i]) {
      out = (Channel)i;
      return true;
    }
  return false;
}

bool IsSealFile(const std::string& stem) { return stem == "seals" || stem == "tells"; }

void LoadSealFile(const std::string& path, const std::string& stem, SealLib& out,
                  std::string& log) {
  json j;
  try {
    std::ifstream f(path);
    j = json::parse(f, nullptr, true, true);
  } catch (const std::exception& ex) {
    log += "demons: " + stem + ".json: " + ex.what() + "\n";
    return;
  }
  if (stem == "tells") {
    out.tells = ParseBands(j.value("default", json::array()), log, "default");
    out.demonTells.clear();
    if (j.contains("demons") && j["demons"].is_object())
      for (auto it = j["demons"].begin(); it != j["demons"].end(); ++it)
        out.demonTells.emplace_back(it.key(), ParseBands(it.value(), log, it.key()));
    std::sort(out.demonTells.begin(), out.demonTells.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return;
  }
  // seals.json
  out.seals.clear();
  for (const json& s : j.value("seals", json::array())) {
    if (!s.is_object()) continue;
    SealDef d;
    d.material = s.value("material", std::string());
    const std::string ch = s.value("severs", std::string());
    d.weakens = s.value("weakens", false);
    if (d.material.empty() || (!d.weakens && !ChannelByName(ch, d.severs))) {
      log += "demons: seals.json: a seal needs `material` and `severs` (move | cast_out | "
             "blink) or `weakens`, got \"" + d.material + "\" / \"" + ch + "\"\n";
      continue;
    }
    d.perCell = std::clamp(s.value("perCell", d.perCell), 0, 1000);
    d.cellsPerPoint = std::clamp(s.value("cellsPerPoint", d.cellsPerPoint), 0, 10000);
    out.seals.push_back(std::move(d));
  }
  if (j.contains("band") && j["band"].is_object()) {
    const json& b = j["band"];
    out.bandInM = std::clamp(b.value("inM", out.bandInM), 0.0f, 4.0f);
    out.bandOutM = std::clamp(b.value("outM", out.bandOutM), 0.0f, 4.0f);
    out.slabAbove = std::clamp(b.value("slabAbove", out.slabAbove), 0, 8);
    out.minEighths = std::clamp(b.value("minEighths", out.minEighths), 1, 8);
  }
  out.weakenCapPct = std::clamp(j.value("weakenCapPct", out.weakenCapPct), 0, 100);
  if (j.contains("candles") && j["candles"].is_object()) {
    const json& c = j["candles"];
    out.candle = c.value("material", out.candle);
    out.candleSlabAbove = std::clamp(c.value("slabAbove", out.candleSlabAbove), 0, 16);
    out.perCandle = std::clamp(c.value("perCandle", out.perCandle), 0, 1000);
    out.candlesMax = std::clamp(c.value("max", out.candlesMax), 0, 64);
  }
  if (j.contains("gaze") && j["gaze"].is_object()) {
    const json& g = j["gaze"];
    out.gazeConeDeg = std::clamp(g.value("coneDeg", out.gazeConeDeg), 1.0f, 90.0f);
    out.gazeRangeM = std::clamp(g.value("rangeM", out.gazeRangeM), 0.5f, 64.0f);
    out.strainRise = std::clamp(g.value("strainRise", out.strainRise), 0, 1000);
    out.strainFall = std::clamp(g.value("strainFall", out.strainFall), 0, 1000);
    out.strainMax = std::clamp(g.value("strainMax", out.strainMax), 1, 100000);
    out.strainPenalty = std::clamp(g.value("strainPenalty", out.strainPenalty), 0, 1000);
    out.strainLossMax = std::clamp(g.value("strainLossMax", out.strainLossMax), 0, 10000);
  }
}

SealReading ScanSeals(const CircleProbe& probe, const SealLib& lib,
                      const std::vector<MaterialDef>& mats, const CircleShape& circle) {
  SealReading r;
  if (!circle.Closed()) return r;
  // Materials by name against the live table (an R reload renumbers nothing
  // a scan holds: it resolves every time).
  std::vector<uint32_t> ids(lib.seals.size());
  for (size_t i = 0; i < lib.seals.size(); i++) ids[i] = MatByName(mats, lib.seals[i].material);
  const uint32_t candle = MatByName(mats, lib.candle);
  const float rIn = std::max(0.0f, circle.radius - MetresToCells(lib.bandInM));
  const float rOut = circle.radius + MetresToCells(lib.bandOutM);
  const float rIn2 = rIn * rIn, rOut2 = rOut * rOut;
  const int32_t x0 = (int32_t)std::floor(circle.cx - rOut), x1 = (int32_t)std::ceil(circle.cx + rOut);
  const int32_t z0 = (int32_t)std::floor(circle.cz - rOut), z1 = (int32_t)std::ceil(circle.cz + rOut);
  const int32_t y0 = circle.feetY - 1;
  const int32_t ySeal = circle.feetY + lib.slabAbove;
  const int32_t yTop = circle.feetY + std::max(lib.slabAbove, lib.candleSlabAbove);
  std::vector<int64_t> perSeal(lib.seals.size(), 0);   // eighths
  for (int32_t z = z0; z <= z1; z++)
    for (int32_t x = x0; x <= x1; x++) {
      const float dx = (float)x + 0.5f - circle.cx, dz = (float)z + 0.5f - circle.cz;
      const float d2 = dx * dx + dz * dz;
      if (d2 < rIn2 || d2 > rOut2) continue;
      for (int32_t y = y0; y <= yTop; y++) {
        bool known = false;
        const uint32_t w = probe.wordAt(probe.ctx, x, y, z, known);
        r.cellsRead++;
        if (!known) {
          r.unknownAt = IVec3{x, y, z};
          return r;   // invalid: the caller holds its last reading
        }
        const uint32_t m = w & 0xFFFu;
        if (m == 0) continue;
        if (m == candle && candle != 0) r.candlesLit++;
        if (y > ySeal) continue;
        for (size_t i = 0; i < ids.size(); i++) {
          if (ids[i] != m || m == 0) continue;
          const int32_t e = EighthsOf(mats, w);
          if (e >= lib.minEighths) perSeal[i] += e;
        }
      }
    }
  for (size_t i = 0; i < lib.seals.size(); i++) {
    const SealDef& s = lib.seals[i];
    if (s.weakens) {
      r.weakenEighths += (int32_t)perSeal[i];
      r.weakenPotency += (int32_t)(perSeal[i] * s.perCell / 8);
      if (s.cellsPerPoint > 0) r.sealPoints += (int32_t)(perSeal[i] / (8 * (int64_t)s.cellsPerPoint));
      continue;
    }
    const size_t c = (size_t)s.severs;
    r.eighths[c] += (int32_t)perSeal[i];
    r.potency[c] += (int32_t)(perSeal[i] * s.perCell / 8);
    if (s.cellsPerPoint > 0) r.sealPoints += (int32_t)(perSeal[i] / (8 * (int64_t)s.cellsPerPoint));
  }
  r.saltEighths = r.eighths[(size_t)Channel::Move];
  r.valid = true;
  return r;
}

bool ChannelSevered(const TickAuthorityCtx& w, uint64_t demonId, Channel ch) {
  const LiveDemon* ld = FindLive(w, demonId);
  return ld != nullptr && ld->state == DemonState::Contained && ld->bind.Severed(ch);
}

bool AllowCastOut(TickAuthorityCtx& w, uint64_t demonId, Vec3 from, Vec3 to) {
  (void)from;
  LiveDemon* ld = FindLive(w, demonId);
  if (ld == nullptr || ld->state != DemonState::Contained) return true;
  if (ld->bind.Severed(Channel::CastOut) && !ld->circle.Inside(to.x, to.z)) {
    ld->bind.castOutRefused++;
    return false;
  }
  ld->bind.castOutAllowed++;
  return true;
}

bool AllowBlink(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t demonId, Vec3 to,
                uint32_t tick) {
  LiveDemon* ld = FindLive(w, demonId);
  if (ld == nullptr || ld->state != DemonState::Contained) return true;
  if (ld->bind.Severed(Channel::Blink)) {
    ld->bind.blinkRefused++;
    return false;
  }
  ld->bind.blinkAllowed++;
  // THE LOOPHOLE: quicksilver too thin for it, and it blinks out -- it has
  // left the circle, so the circle no longer holds it.
  if (!ld->circle.Inside(to.x, to.z))
    DemonUnbind(w, players, *ld, tick, "blinked out of the circle (blink unsevered)");
  return true;
}

ReleaseResult Release(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t demonId,
                      int32_t contractWeight, uint32_t tick) {
  LiveDemon* ld = FindLive(w, demonId);
  if (ld == nullptr || ld->state != DemonState::Contained) return ReleaseResult::None;
  Binding& b = ld->bind;
  const DemonDef* def = w.demons->lib.Find(ld->demon);
  Evaluate(*ld, def, w.demons->lib.seals);
  b.releaseMargin = b.strength - b.power - contractWeight;
  char why[160];
  if (b.releaseMargin >= 0) {
    b.release = ReleaseResult::Held;
    ld->state = DemonState::Released;
    ld->stateTick = tick;
    std::snprintf(why, sizeof why, "released, the binding held (strength %d >= power %d + %d)",
                  b.strength, b.power, contractWeight);
    ld->why = why;
    // Not aimed at anyone. D5's contract decides what it does next; until
    // then the def's `released` profile (a neutral one) stands in.
    if (def != nullptr && !def->released.empty()) w.mobs.SetMobBehavior(ld->mobId, def->released);
    if (ai::Brain* br = w.mobs.MobBrainMut(ld->mobId)) {
      br->hasTarget = false;
      br->targetId = 0;
      br->routine.active = false;   // D6: no longer waiting in the circle
    }
    std::printf("demons: %s %s\n", ld->name.c_str(), why);
    return ReleaseResult::Held;
  }
  b.release = ReleaseResult::Overpowered;
  std::snprintf(why, sizeof why, "overpowered the binding on release (strength %d < power %d + %d)",
                b.strength, b.power, contractWeight);
  DemonUnbind(w, players, *ld, tick, why);
  return ReleaseResult::Overpowered;
}

const std::string& TellFor(const SealLib& lib, const std::string& demonId, int32_t margin,
                           uint32_t salt) {
  static const std::string kNone;
  const std::vector<TellBand>* bands = &lib.tells;
  for (const auto& [id, b] : lib.demonTells)
    if (id == demonId && !b.empty()) bands = &b;
  // Highest band whose floor the margin reaches; below every floor -> the
  // lowest band (the laugh).
  const TellBand* pick = nullptr;
  for (const TellBand& t : *bands)
    if (margin >= t.minMargin) {
      pick = &t;
      break;
    }
  if (pick == nullptr && !bands->empty()) pick = &bands->back();
  if (pick == nullptr || pick->lines.empty()) return kNone;
  uint32_t h = 2166136261u;
  for (char ch : demonId) h = (h ^ (uint8_t)ch) * 16777619u;
  const uint32_t r = rng::Hash3(salt, (uint32_t)pick->minMargin, h);
  return pick->lines[r % pick->lines.size()];
}

void SealsTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
               const CircleProbe& probe) {
  if (!w.demons) return;
  DemonWorld& d = *w.demons;
  const SealLib& lib = d.lib.seals;
  for (LiveDemon& ld : d.live) {
    if (ld.state != DemonState::Contained) continue;
    const DemonDef* def = d.lib.Find(ld.demon);
    ld.bind.gazeHold = def != nullptr && def->gaze == "hold";
    ld.bind.strainMax = std::max(1, lib.strainMax);
    // THE BAND, on the circle's own triggers: D1 re-read the circle this tick
    // (arrival included) -> re-read the band. Unseen -> hold the last reading.
    if (ld.lastCheck == tick) {
      SealReading r = ScanSeals(probe, lib, w.mats, ld.circle);
      if (r.valid) {
        ld.bind.reading = r;
        ld.bind.scanTick = tick;
      }
    }
    Gaze(ld, lib, w.mobs, players);
    Evaluate(ld, def, lib);
    // THE MOVE LOOPHOLE: a ring with less salt than this demon's legs resist
    // does not hold it -- it walks out, so it is loose.
    if (ld.bind.reading.valid && !ld.bind.Severed(Channel::Move)) {
      char why[128];
      std::snprintf(why, sizeof why, "the salt cannot hold it (move %d < resist %d)",
                    ld.bind.reading.potency[(size_t)Channel::Move], ResistOf(def, Channel::Move));
      DemonUnbind(w, players, ld, tick, why);
    }
  }
  // RELEASE (TB_DEMON_RELEASE): the presser's most recent contained demon.
  for (SessionTick& p : players) {
    if (!p.ti.Pressed(TB_DEMON_RELEASE)) continue;
    for (auto it = d.live.rbegin(); it != d.live.rend(); ++it)
      if (it->session == p.s->index && it->state == DemonState::Contained) {
        Release(w, players, it->mobId, ContractWeight(*it), tick);   // D5: the bound contract's weight
        break;
      }
  }
}

void FillHud(UIState& ui, const LiveDemon& ld) {
  const Binding& b = ld.bind;
  ui.demonHasBinding = ld.state == DemonState::Contained && b.reading.valid;
  ui.demonStrength = b.strength;
  ui.demonPower = b.basePower;
  ui.demonIronCut = b.ironCut;
  ui.demonSevered = b.severed;
  ui.demonStrain = std::clamp((float)b.strain / (float)std::max(1, b.strainMax), 0.0f, 1.0f);
  ui.demonGazeHold = b.gazeHold;
  ui.demonGazeBroken = b.gazeBroken;
}

}  // namespace demon
