#include "game/spell.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

// Counter-based hash, identical in shape to the one mob.cpp uses and to the
// sim shaders' hash3. Stateless by construction: a stateful stream here would
// desync a replay the moment a frame boundary moved.
uint32_t Pcg(uint32_t v) {
  uint32_t s = v * 747796405u + 2891336453u;
  uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) {
  return Pcg(a ^ Pcg(b ^ Pcg(c)));
}

BackfireKind ParseBackfire(const std::string& s) {
  if (s == "burn") return BackfireKind::Burn;
  if (s == "dissolve") return BackfireKind::Dissolve;
  if (s == "drown") return BackfireKind::Drown;
  if (s == "bury") return BackfireKind::Bury;
  return BackfireKind::Generic;
}

int32_t ClampI(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

// ---- loading ---------------------------------------------------------------

bool LoadGlyphs(const std::string& path, const std::vector<MaterialDef>& mats,
                GlyphLibrary& out, std::string& errors) {
  out = GlyphLibrary{};
  std::ifstream f(path);
  if (!f) {
    errors += "glyphs: cannot open " + path + "\n";
    return false;
  }
  json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    errors += std::string("glyphs: parse error: ") + e.what() + "\n";
    return false;
  }

  // Material NAME -> 12-bit id. Never hardcode a material id in spell code
  // (project convention): a glyph names "acid" and the id is whatever
  // materials.json order made it.
  auto resolveMat = [&](const std::string& name, uint32_t& outId) {
    for (size_t i = 0; i < mats.size(); i++) {
      if (mats[i].name == name) {
        outId = (uint32_t)i;
        return true;
      }
    }
    return false;
  };

  if (j.contains("budgets")) {
    const json& b = j["budgets"];
    auto rd = [&](const char* k, int32_t& dst) {
      if (b.contains(k) && b[k].is_number_integer()) dst = b[k].get<int32_t>();
    };
    rd("maxLiveProjectiles", out.budgets.maxLiveProjectiles);
    rd("maxGeneration", out.budgets.maxGeneration);
    rd("maxTrailVoxels", out.budgets.maxTrailVoxels);
    rd("maxLifetimeTicks", out.budgets.maxLifetimeTicks);
  }
  // Rule 2: these are hard engine ceilings, so a content edit cannot author an
  // unbounded spell. Clamped rather than trusted.
  out.budgets.maxLiveProjectiles = ClampI(out.budgets.maxLiveProjectiles, 1, 256);
  out.budgets.maxGeneration = ClampI(out.budgets.maxGeneration, 0, 8);
  out.budgets.maxTrailVoxels = ClampI(out.budgets.maxTrailVoxels, 1, 4096);
  out.budgets.maxLifetimeTicks = ClampI(out.budgets.maxLifetimeTicks, 1, 1800);

  if (!j.contains("glyphs") || !j["glyphs"].is_array()) {
    errors += "glyphs: missing \"glyphs\" array\n";
    return false;
  }

  for (const json& g : j["glyphs"]) {
    GlyphDef d;
    if (!g.contains("id") || !g["id"].is_string()) {
      errors += "glyphs: a glyph has no string \"id\"\n";
      return false;
    }
    d.id = g["id"].get<std::string>();
    if (out.Find(d.id) >= 0) {
      errors += "glyphs: duplicate glyph id \"" + d.id + "\"\n";
      return false;
    }
    if (g.contains("desc")) d.desc = g["desc"].get<std::string>();
    if (g.contains("mana")) d.mana = g["mana"].get<int32_t>();
    if (d.mana < 0) {
      errors += "glyphs: \"" + d.id + "\" has negative mana\n";
      return false;
    }

    const std::string type = g.value("type", std::string());
    if (type == "element") {
      d.type = GlyphType::Element;
      d.materialName = g.value("material", std::string());
      if (!resolveMat(d.materialName, d.material)) {
        // Loud, not silent: a spell that quietly conjures air is far harder to
        // diagnose than one that refuses to load (DESIGN.md §6).
        errors += "glyphs: \"" + d.id + "\" names unknown material \"" +
                  d.materialName + "\"\n";
        return false;
      }
      if (d.material == 0) {
        errors += "glyphs: \"" + d.id + "\" resolves to air\n";
        return false;
      }
      d.backfire = ParseBackfire(g.value("backfire", std::string()));
    } else if (type == "form") {
      d.type = GlyphType::Form;
      const std::string form = g.value("form", std::string());
      if (form == "projectile") {
        d.form = SpellForm::Projectile;
      } else {
        errors += "glyphs: \"" + d.id + "\" has unknown form \"" + form + "\"\n";
        return false;
      }
      d.speed = ClampI(g.value("speed", 48), 1, 512);
      d.lifetimeTicks =
          ClampI(g.value("lifetimeTicks", 150), 1, out.budgets.maxLifetimeTicks);
      d.impactRadius = ClampI(g.value("impactRadius", 3), 0, 8);
    } else if (type == "modifier") {
      d.type = GlyphType::Modifier;
      const std::string m = g.value("modifier", std::string());
      if (m == "trail") d.modifier = SpellModifier::Trail;
      else if (m == "transmute_to") d.modifier = SpellModifier::TransmuteTo;
      else {
        errors += "glyphs: \"" + d.id + "\" has unknown modifier \"" + m + "\"\n";
        return false;
      }
      // Brush ops are capped at radius 8 by the mutate kernel's 16^3 footprint.
      d.radius = ClampI(g.value("radius", 1), 0, 8);
      d.voxelBudget =
          ClampI(g.value("voxelBudget", 64), 1, out.budgets.maxTrailVoxels);
      d.everyTicks = ClampI(g.value("everyTicks", 1), 1, 60);
    } else {
      errors += "glyphs: \"" + d.id + "\" has unknown type \"" + type + "\"\n";
      return false;
    }
    out.glyphs.push_back(std::move(d));
  }

  // conjoined (§G stub): just a saved list of glyph ids.
  if (j.contains("conjoined") && j["conjoined"].contains("entries")) {
    for (const json& c : j["conjoined"]["entries"]) {
      ConjoinedGlyph cg;
      cg.id = c.value("id", std::string());
      cg.desc = c.value("desc", std::string());
      bool ok = true;
      for (const json& gid : c.value("glyphs", json::array())) {
        int idx = out.Find(gid.get<std::string>());
        if (idx < 0) {
          errors += "glyphs: conjoined \"" + cg.id + "\" names unknown glyph \"" +
                    gid.get<std::string>() + "\"\n";
          ok = false;
          break;
        }
        cg.glyphs.push_back(idx);
      }
      if (!ok) return false;
      out.conjoined.push_back(std::move(cg));
    }
  }

  // wards (§G stub): shape only, no behaviour.
  if (j.contains("wards") && j["wards"].contains("entries")) {
    for (const json& w : j["wards"]["entries"]) {
      WardDef wd;
      wd.id = w.value("id", std::string());
      wd.desc = w.value("desc", std::string());
      wd.filter = w.value("filter", std::string());
      wd.drainPerMille = ClampI(w.value("drainPerMille", 0), 0, 900);
      out.wards.push_back(std::move(wd));
    }
  }

  if (out.glyphs.empty()) {
    errors += "glyphs: no glyphs loaded\n";
    return false;
  }
  return true;
}

// ---- compilation -----------------------------------------------------------

Spell CompileSpell(const GlyphLibrary& lib, const SpellStack& stack) {
  Spell s;
  // A fold over the stack. Deliberately total: any sequence compiles, because
  // an illegal one must be CASTABLE and misfire rather than be rejected. The
  // ancient language punishes imprecision; a parse error would just be
  // annoying.
  //
  // LAST element wins and LAST form wins — speaking "water lava projectile"
  // throws lava. That is the least surprising reading of "the most recent word
  // qualifies the spell", and it means a mis-spoken word can be corrected by
  // speaking the right one rather than by clearing and starting over.
  for (int gi : stack.spoken) {
    if (gi < 0 || gi >= (int)lib.glyphs.size()) continue;
    const GlyphDef& g = lib.glyphs[gi];
    s.manaCost += g.mana;
    switch (g.type) {
      case GlyphType::Element:
        s.element = g.material;
        s.backfire = g.backfire;
        break;
      case GlyphType::Form:
        s.form = g.form;
        s.speed = g.speed;
        s.lifetimeTicks = g.lifetimeTicks;
        s.impactRadius = g.impactRadius;
        break;
      case GlyphType::Modifier:
        s.modifiers.push_back(gi);
        break;
    }
  }
  return s;
}

SpellReadout DescribeSpell(const GlyphLibrary& lib, const SpellStack& stack,
                           const Spell& spell) {
  SpellReadout r;
  for (size_t i = 0; i < stack.spoken.size(); i++) {
    int gi = stack.spoken[i];
    if (gi < 0 || gi >= (int)lib.glyphs.size()) continue;
    if (!r.text.empty()) r.text += " + ";
    r.text += lib.glyphs[gi].id;
  }
  r.wellFormed = spell.HasElement() && spell.HasForm();
  if (stack.spoken.empty()) {
    r.verdict = "silence";
  } else if (!spell.HasForm()) {
    r.verdict = "no form spoken - will misfire at your feet";
  } else if (!spell.HasElement()) {
    r.verdict = "no element spoken - will misfire";
  } else {
    r.verdict = "ready";
  }
  return r;
}

// ---- caster ----------------------------------------------------------------

void CasterState::Tick() {
  const int32_t cap = EffectiveMax();
  if (mana >= cap) {
    // A ward that just came up can leave mana above the new effective cap;
    // bleed it down rather than leaving the bar overfull.
    if (mana > cap) mana = cap;
    regenAccum = 0;
    return;
  }
  regenAccum += regenPerMillePerTick;
  while (regenAccum >= 1000 && mana < cap) {
    regenAccum -= 1000;
    mana++;
  }
  if (mana >= cap) regenAccum = 0;
}

CastResult ResolveCast(const CasterState& caster, int32_t health,
                       int32_t manaCost) {
  CastResult r;
  if (manaCost <= 0) {
    r.outcome = CastOutcome::Nothing;
    return r;
  }
  if (health < 0) health = 0;
  const int32_t mana = caster.mana < 0 ? 0 : caster.mana;

  if (manaCost <= mana) {
    r.outcome = CastOutcome::Normal;
    r.manaSpent = manaCost;
    return r;
  }
  const int32_t overflow = manaCost - mana;
  if (overflow <= health) {
    // Casting into health: the spell goes off, but IMPRECISELY. Instability is
    // how deep into health it went, as a fraction of the health available —
    // which is what makes the mana bar a PRECISION meter rather than a second
    // HP bar.
    r.outcome = CastOutcome::Unstable;
    r.manaSpent = mana;
    r.healthSpent = overflow;
    r.instability = health > 0 ? ClampI((int32_t)((int64_t)overflow * 1000 / health), 0, 1000) : 1000;
    return r;
  }
  // Beyond mana + health: the spell still HAPPENS, it just happens here.
  r.outcome = CastOutcome::Fatal;
  r.manaSpent = mana;
  r.healthSpent = health;
  r.instability = 1000;
  return r;
}

SpellFxVec SpellWobble(SpellFxVec dirFx, int32_t instabilityMille,
                       uint32_t tick, uint64_t casterId) {
  if (instabilityMille <= 0) return dirFx;
  // Magnitude of the perturbation as a fraction of the direction's own scale,
  // so wobble is proportional rather than absolute. Squared so a shallow dip
  // into health barely wavers while a deep one sprays wildly — the interesting
  // part of the curve is the far end.
  const int64_t mag = (int64_t)instabilityMille * instabilityMille / 1000;
  auto jitter = [&](int32_t component, uint32_t salt) {
    uint32_t h = Hash3((uint32_t)casterId ^ salt, tick, (uint32_t)component);
    // [-1, 1) in 1/32768 units, integer throughout.
    int32_t signed15 = (int32_t)(h & 0xFFFFu) - 32768;
    // scale: full instability perturbs by up to ~40% of the vector's length.
    int64_t amp = (int64_t)kSpellFxOne * 2 * mag / 1000;  // ~0.4 vox at 1000
    return (int32_t)((int64_t)signed15 * amp / 32768);
  };
  // Perturbation is scaled against the direction's magnitude so a fast spell
  // and a slow one wobble by the same ANGLE, not the same distance.
  int64_t len2 = (int64_t)dirFx.x * dirFx.x + (int64_t)dirFx.y * dirFx.y +
                 (int64_t)dirFx.z * dirFx.z;
  int32_t len = (int32_t)0;
  {  // integer sqrt, deterministic across machines (no float, no libm)
    int64_t x = len2, r = 0, b = 1LL << 30;
    while (b > x) b >>= 2;
    while (b) {
      if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
      else r >>= 1;
      b >>= 2;
    }
    len = (int32_t)r;
  }
  if (len <= 0) return dirFx;
  SpellFxVec o = dirFx;
  o.x += (int32_t)((int64_t)jitter(dirFx.x, 0x9E37u) * len / kSpellFxOne);
  o.y += (int32_t)((int64_t)jitter(dirFx.y, 0x85EBu) * len / kSpellFxOne);
  o.z += (int32_t)((int64_t)jitter(dirFx.z, 0xC2B2u) * len / kSpellFxOne);
  return o;
}

// ---- the effect payload (thesis 2) -----------------------------------------

void ApplySpellEffect(const GlyphLibrary& lib, const Spell& spell,
                      SpellFxVec atFx, SpellFxVec dirFx, int32_t strengthMille,
                      SpellEmission& out) {
  (void)dirFx;
  strengthMille = ClampI(strengthMille, 0, 1000);
  if (strengthMille == 0) return;

  const int32_t cx = SpellFxFloor(atFx.x);
  const int32_t cy = SpellFxFloor(atFx.y);
  const int32_t cz = SpellFxFloor(atFx.z);

  // Scale a radius by strength, never below 1 when the effect fires at all —
  // a 0-radius brush op is a wasted slot in the tick budget.
  auto scaled = [&](int32_t r) {
    int32_t v = (int32_t)((int64_t)r * strengthMille / 1000);
    return v < 1 ? 1 : v;
  };

  // Transmutation: convert the impact region to the spell's element.
  //
  // This is an OVERWRITE brush op (mode 1), NOT the laser's melt mode (2).
  // Worth being explicit, because the brief suggested following melt: melt
  // converts each cell to ITS OWN authored `molten` product (stone->lava,
  // sand->molten_glass — see sim_mutate.wgsl), which is exactly right for a
  // heat beam and exactly wrong for "transmute to acid", where the TARGET
  // material is chosen by the caster rather than by the material being hit.
  // Mode 1 is the existing primitive for that and needs no new conversion path.
  bool didTransmute = false;
  for (int gi : spell.modifiers) {
    if (gi < 0 || gi >= (int)lib.glyphs.size()) continue;
    if (lib.glyphs[gi].modifier != SpellModifier::TransmuteTo) continue;
    if (!spell.HasElement()) continue;
    out.ops.push_back({cx, cy, cz, scaled(lib.glyphs[gi].radius), spell.element,
                       1u /*overwrite*/, 0, 0});
    didTransmute = true;
  }

  switch (spell.backfire) {
    case BackfireKind::Burn:
      // Fire/lava: a real explosion at the position, plus the caller carving
      // the caster if this is a self-cast. Both go through existing pipelines.
      out.explosions.push_back(
          {cx, cy, cz, ClampI(scaled(6), 1, kMaxExplosionRadius),
           (int32_t)((int64_t)220 * strengthMille / 1000), 0, 0, 0});
      break;
    case BackfireKind::Dissolve:
    case BackfireKind::Drown:
    case BackfireKind::Bury:
    default:
      // Everything else deposits its element. A fatal acid cast therefore
      // fills the caster's own volume with acid — the avatar's voxels do
      // become acid, by the acid then eating them through the ordinary CA.
      if (spell.HasElement() && !didTransmute) {
        out.ops.push_back({cx, cy, cz, ClampI(scaled(3), 1, 8), spell.element,
                           0u /*paint into air*/, 0, 0});
      }
      break;
  }
}

// ---- the system ------------------------------------------------------------

CastResult SpellSystem::Cast(const Spell& spell, CasterState& caster,
                             const CasterHealth& health, uint64_t casterId,
                             SpellFxVec originFx, SpellFxVec dirFx,
                             uint32_t tick, SpellEmission& out) {
  CastResult r;
  if (!lib_) return r;
  if (spell.manaCost <= 0 && spell.modifiers.empty() && !spell.HasForm())
    return r;  // nothing spoken

  const int32_t hp = health.Get();
  r = ResolveCast(caster, hp, spell.manaCost);
  if (r.outcome == CastOutcome::Nothing) return r;

  caster.mana -= r.manaSpent;
  if (caster.mana < 0) caster.mana = 0;
  if (r.healthSpent > 0) health.Spend(r.healthSpent);

  // THESIS 2 IN ONE BRANCH. A fatal cast is not special-cased: it is the same
  // effect payload, applied at the caster instead of at the muzzle. Adding a
  // new element or form gets a thematic death for free, with no per-spell
  // backfire code anywhere.
  if (r.outcome == CastOutcome::Fatal) {
    ApplySpellEffect(*lib_, spell, originFx, dirFx, 1000, out);
    out.carveCaster = true;
    out.carveAt = Vec3{SpellFxToFloat(originFx.x), SpellFxToFloat(originFx.y),
                       SpellFxToFloat(originFx.z)};
    out.carveRadius = (float)std::max(2, spell.impactRadius);
    return r;
  }

  // Instability makes the spell IMPRECISE, not merely costly.
  SpellFxVec aim = SpellWobble(dirFx, r.instability, tick, casterId);

  if (spell.form == SpellForm::Projectile) {
    // Rule 2: a hard cap on live projectiles, checked before the spawn. Over
    // budget, the spell misfires at the caster rather than silently doing
    // nothing — a spell that sometimes doesn't fire is miserable to diagnose.
    if ((int)live_.size() >= lib_->budgets.maxLiveProjectiles ||
        spell.gen > lib_->budgets.maxGeneration) {
      ApplySpellEffect(*lib_, spell, originFx, aim, 400, out);
      return r;
    }
    SpellProjectile p;
    p.spell = spell;
    p.pos = originFx;
    p.casterId = casterId;
    p.ticksLeft = ClampI(spell.lifetimeTicks, 1, lib_->budgets.maxLifetimeTicks);
    // Normalize the aim to the form's speed, in fixed point. Integer sqrt so
    // two machines agree exactly (no libm, no float).
    int64_t len2 = (int64_t)aim.x * aim.x + (int64_t)aim.y * aim.y +
                   (int64_t)aim.z * aim.z;
    int64_t x = len2, rr = 0, b = 1LL << 40;
    while (b > x) b >>= 2;
    while (b) {
      if (x >= rr + b) { x -= rr + b; rr = (rr >> 1) + b; }
      else rr >>= 1;
      b >>= 2;
    }
    if (rr <= 0) {
      p.vel = {0, 0, 0};
    } else {
      const int64_t sp = (int64_t)spell.speed * kSpellFxOne;  // fx voxels/tick
      p.vel.x = (int32_t)((int64_t)aim.x * sp / rr);
      p.vel.y = (int32_t)((int64_t)aim.y * sp / rr);
      p.vel.z = (int32_t)((int64_t)aim.z * sp / rr);
    }
    // Trail budget: a HARD voxel count that only ever decreases. This is the
    // rule-2 guarantee — the trail cannot outlive its budget, so it cannot
    // hold a chunk awake indefinitely.
    for (int gi : spell.modifiers) {
      if (gi >= 0 && gi < (int)lib_->glyphs.size() &&
          lib_->glyphs[gi].modifier == SpellModifier::Trail)
        p.trailBudget = std::min(lib_->glyphs[gi].voxelBudget,
                                 lib_->budgets.maxTrailVoxels);
    }
    live_.push_back(p);
  } else {
    // No form spoken: the spell has nowhere to go, so it goes off here. This
    // is the misfire the design asks for — an ill-formed sequence is castable
    // and punishing rather than rejected.
    ApplySpellEffect(*lib_, spell, originFx, aim, 700, out);
  }
  return r;
}

void SpellSystem::Tick(uint32_t tick, const World& world,
                       const std::vector<uint32_t>& classOf,
                       SpellEmission& out) {
  opsDropped_ = 0;
  if (!lib_) return;
  int opsUsed = 0;

  auto solidAt = [&](int32_t x, int32_t y, int32_t z) {
    // OUT OF WINDOW = SOLID. The residency-window rule (DESIGN.md §3):
    // unloaded space is solid and inert, so a projectile leaving the simulated
    // world stops at the boundary rather than flying forever through nothing.
    if (!world.CellInWindow({(int)x, (int)y, (int)z})) return true;
    CellKind k = world.KindAt({(int)x, (int)y, (int)z}, classOf);
    // UNKNOWN = PASSABLE, and this is the opposite of the choice the player
    // controller makes — worth being explicit about, because the obvious
    // reading ("unknown is solid, be conservative") is wrong here and produced
    // a projectile that detonated on its first tick.
    //
    // The CPU mirror is only the 3x3x3 chunks around the PLAYER (world.cpp
    // KindAt), i.e. ~48 voxels of coverage. That is ample for a capsule
    // controller, which never leaves its own neighbourhood, and useless for a
    // 48 vox/tick projectile, which exits the mirror inside one tick and then
    // reads Unknown for the whole rest of its flight. Treating that as solid
    // means every bolt explodes in the caster's face.
    //
    // So a spell flies THROUGH unmirrored space and can only impact on cells
    // the CPU actually knows about. The cost is that a bolt fired at a distant
    // wall passes through it until the player gets close enough for the mirror
    // to cover it. That is a real limitation of this slice and the first thing
    // to fix if projectiles are worth keeping: the honest fix is a swept chunk
    // FETCH along the flight path (World::RequestChunkFetch already exists for
    // exactly this kind of query), not a bigger mirror.
    return k == CellKind::Solid;
  };

  for (size_t i = 0; i < live_.size();) {
    SpellProjectile& p = live_[i];
    bool impact = false;
    SpellFxVec impactAt = p.pos;

    if (--p.ticksLeft <= 0) {
      // Lifetime expiry is a HARD bound (rule 2) and is deliberately NOT an
      // impact: an expired spell simply fizzles, so a spell whose budget runs
      // out cannot deposit a free impact effect somewhere the player never
      // aimed at.
      p.alive = false;
    } else {
      // Swept integration with anti-tunneling: step at most half a voxel at a
      // time, exactly as sim_particle.wgsl does. Without this a 48 vox/tick
      // projectile passes clean through any wall thinner than 48 voxels.
      const int32_t kHalf = kSpellFxOne / 2;
      int64_t maxComp = std::max({(int64_t)std::abs(p.vel.x),
                                  (int64_t)std::abs(p.vel.y),
                                  (int64_t)std::abs(p.vel.z)});
      int32_t steps = (int32_t)((maxComp + kHalf - 1) / kHalf);
      if (steps < 1) steps = 1;
      if (steps > 512) steps = 512;  // bound the inner loop unconditionally

      for (int32_t s = 0; s < steps && !impact; s++) {
        SpellFxVec next = p.pos;
        next.x += p.vel.x / steps;
        next.y += p.vel.y / steps;
        next.z += p.vel.z / steps;
        const int32_t cx = SpellFxFloor(next.x), cy = SpellFxFloor(next.y),
                      cz = SpellFxFloor(next.z);
        if (solidAt(cx, cy, cz)) {
          impact = true;
          impactAt = next;
          break;
        }
        p.pos = next;

        // Trail: lay the element behind us, decrementing a hard budget.
        if (p.trailBudget > 0 && opsUsed < kSpellOpsPerTick) {
          for (int gi : p.spell.modifiers) {
            if (gi < 0 || gi >= (int)lib_->glyphs.size()) continue;
            const GlyphDef& g = lib_->glyphs[gi];
            if (g.modifier != SpellModifier::Trail) continue;
            if (p.trailBudget <= 0) break;
            // One mark per WHOLE VOXEL of travel, not one per sub-step. The
            // sweep is subdivided for anti-tunneling (up to ~96 sub-steps at
            // 48 vox/tick), so marking per sub-step spends the whole budget in
            // a single tick and lays a dozen overlapping ops on the same cell.
            // Gating on the cell actually changing makes the trail's length
            // proportional to distance flown, which is what the budget is
            // meant to measure.
            const int32_t mx = SpellFxFloor(p.pos.x), my = SpellFxFloor(p.pos.y),
                          mz = SpellFxFloor(p.pos.z);
            if (p.markedValid && mx == p.markedX && my == p.markedY &&
                mz == p.markedZ)
              break;
            if (!world.CellInWindow({(int)mx, (int)my, (int)mz})) break;
            if ((p.trailPhase++ % g.everyTicks) != 0) break;
            if (!p.spell.HasElement()) break;
            if (opsUsed >= kSpellOpsPerTick) {
              opsDropped_++;
              break;
            }
            // The budget is a VOXEL count, so it is charged by the op's
            // VOLUME rather than per op — otherwise a radius-8 trail costs the
            // same as a radius-1 one and the bound means nothing.
            //
            // CHARGED BEFORE EMITTING, and refused if it does not fit. The
            // obvious ordering (emit, then subtract, then check <= 0) lets the
            // last op overrun by nearly a whole op's volume, and a projectile
            // sweeping many sub-steps per tick overruns once per sub-step —
            // measured at 343 voxels against a 64 budget before this was
            // fixed. "Bounded eventually" is not what rule 2 asks for.
            int32_t vol = 2 * g.radius + 1;
            vol = vol * vol * vol;
            if (vol < 1) vol = 1;
            if (vol > p.trailBudget) {
              // Cannot afford another mark: the trail is the spell's whole
              // point, so the projectile dies with its budget rather than
              // flying on inertly.
              p.trailBudget = 0;
              p.alive = false;
              break;
            }
            p.trailBudget -= vol;
            out.ops.push_back({mx, my, mz, g.radius, p.spell.element,
                               0u /*paint into air*/, 0, 0});
            p.markedX = mx;
            p.markedY = my;
            p.markedZ = mz;
            p.markedValid = true;
            opsUsed++;
            if (p.trailBudget <= 0) p.alive = false;
            break;
          }
        }
        if (!p.alive) break;
      }
    }

    if (impact) {
      // Impact applies the modifiers in order through the SAME payload
      // function backfire uses (thesis 2), at the impact position.
      if (opsUsed < kSpellOpsPerTick) {
        size_t before = out.ops.size();
        ApplySpellEffect(*lib_, p.spell, impactAt, p.vel, 1000, out);
        opsUsed += (int)(out.ops.size() - before);
      } else {
        opsDropped_++;
      }
      p.alive = false;
    }

    if (!p.alive) {
      live_[i] = live_.back();
      live_.pop_back();
    } else {
      i++;
    }
  }
}
