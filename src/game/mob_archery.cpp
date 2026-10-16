// mob_archery.cpp -- NPC archers: the draw, the aim, the loose, the sidearm,
// and arming a spawn (docs/PLAN_projectiles.md package E; read
// game/mob_archery.h first).

#include "game/mob_archery.h"

#include <algorithm>
#include <cmath>

#include "game/ai_behavior.h"
#include "game/arrows.h"
#include "game/demon_cast.h"  // MobAimCentre / MobTargetNow: the casters' aim
#include "game/equipment.h"
#include "game/mob.h"
#include "game/session.h"
#include "game/spellhand.h"   // SpellHandWorld: where the bow fist is
#include "sim/rng.h"

namespace {

constexpr uint32_t kSaltHold = 0xB0D2A1u;     // a shot's hold, per (mob, tick)
constexpr uint32_t kSaltSpread = 0xB0E77Au;   // its error, per (mob, tick)
constexpr uint32_t kSaltSeed = 0xA770B1u;     // the flight's seed (the player's is 0xA770B0)
// A guard up when the draw starts is let go over this many ticks (the casters'
// raise, MobHandCast::kRaiseTicks).
constexpr int kGuardRelease = 6;

void Log(MobArcheryWorld& a, ArcheryEvent e) {
  if (a.events.size() >= MobArcheryWorld::kEventRing) a.events.erase(a.events.begin());
  a.events.push_back(std::move(e));
}

const ai::RangedTuning* RangedOf(const MobSystem& mobs, uint64_t mobId) {
  const ai::Brain* b = mobs.MobBrain(mobId);
  const ai::Profile* p = b != nullptr ? mobs.Behaviors().At(b->profile) : nullptr;
  return p != nullptr && p->ranged.maxRange > 0.0f ? &p->ranged : nullptr;
}

// A draw needs BOTH hands: the bow fist holds, the other draws the string.
bool HandsCanDraw(const Mob& m, Hand bowHand) {
  return m.HandPart(bowHand) >= 0 && m.HandUsable(bowHand) &&
         m.HandUsable(OtherHand(bowHand));
}

// THE AIM. From the bow fist when the rig can say where it is (else the
// shoulder), at the target's centre raised by `aimRaise` of its height, led
// where it will be at the arrow's speed, held high by the drop it will take.
// `pitch` is the line's from the SHOULDER (the pose follows it; a pitch
// taken from the fist would chase the fist it moves).
struct ShotAim {
  Vec3 from{}, dir{0, 0, 1};
  float pitch = 0.0f;
};

ShotAim AimShot(TickAuthorityCtx& w, const Mob& m, const MobDraw& d,
                const ai::RangedTuning& rt, float speedVox) {
  ShotAim s;
  Vec3 size{};
  const Vec3 c = MobAimCentre(w.mobs, d.mobId, &size);
  const Vec3 shoulder{c.x, c.y + size.y * 0.25f, c.z};
  Vec3 fist{};
  s.from = SpellHandWorld(w.mobs, w.phys, d.mobId, HandAt(d.hand), fist) ? fist : shoulder;
  Vec3 to = d.targetPoint + Vec3{0.0f, d.targetHeight * rt.aimRaise, 0.0f};
  to = ai::LeadPoint(s.from, to, d.targetVel, speedVox / 30.0f, rt.lead, rt.leadCap);
  to = ArrowHoldover(s.from, to, speedVox);
  Vec3 dv = to - s.from;
  if (dv.len() < 1e-3f) dv = m.Facing();
  s.dir = dv.normalized();
  const Vec3 fromShoulder = to - shoulder;
  const float fl = fromShoulder.len();
  s.pitch = fl > 1e-3f ? std::asin(std::clamp(fromShoulder.y / fl, -1.0f, 1.0f)) : 0.0f;
  return s;
}

// THE ERROR OF ONE LOOSE: up to `spread` radians in yaw and in pitch, two
// counter-based draws on (mob, tick) -- a replay misses the same way.
Vec3 Spread(Vec3 dir, float spread, uint64_t mobId, uint32_t tick) {
  if (!(spread > 0.0f)) return dir;
  const uint32_t h = rng::Hash3((uint32_t)mobId ^ kSaltSpread, tick, 11u);
  const float u1 = (float)(h & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
  const float u2 = (float)((h >> 16) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
  const float yaw = std::atan2(dir.x, dir.z) + u1 * spread;
  const float pitch =
      std::clamp(std::asin(std::clamp(dir.y, -1.0f, 1.0f)) + u2 * spread, -1.55f, 1.55f);
  const float cp = std::cos(pitch);
  return Vec3{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
}

// PUT AWAY what a hand holds, as it is (coat, damage, contents): into the
// sheath when `sheathFirst`, the item fits it and it is free, else the first
// free bag slot. The casters' StowHand, without the cast's bookkeeping.
bool Stow(Mob& m, const ItemLibrary& items, Hand h, bool sheathFirst) {
  const std::string name = m.HeldItem(h);
  if (name.empty()) return true;
  if (m.HeldBorrowed(h)) return false;
  ItemStack st;
  st.name = name;
  st.count = 1;
  m.CaptureHeld(h, st.damage);
  st.contents = m.HeldContents(h);
  Kit& kit = m.KitMut();
  KitRef ref;
  const int sheath = (int)EquipSlotId::Sheath;
  const ItemDef* def = items.Named(name);
  if (sheathFirst && def && EquipSlotAccepts(sheath, def->kind) &&
      kit.equip.slots[sheath].Empty()) {
    ref = KitRef{KitSpace::Equip, sheath};
  } else {
    for (int i = 0; i < Bag::kSlots && !ref.Valid(); i++)
      if (kit.bag.slots[i].Empty()) ref = KitRef{KitSpace::Bag, i};
  }
  ItemStack* to = ref.Valid() ? kit.Resolve(ref) : nullptr;
  if (to == nullptr) return false;
  *to = st;
  m.EquipItem(nullptr, h);
  return true;
}

// OUT OF ARROWS: the bow into the bag, the sheathed sidearm into its hand,
// as it was. Nothing changes when there is no sidearm or no room for the
// bow. Returns the sidearm drawn ("" = none).
std::string DrawSidearm(Mob& m, const ItemLibrary& items, Hand h) {
  const int sheath = (int)EquipSlotId::Sheath;
  const KitRef sref{KitSpace::Equip, sheath};
  const ItemStack* st = m.GetKit().Resolve(sref);
  if (st == nullptr || st->Empty()) return {};
  const std::string side = st->name;
  const ItemDef* def = items.Named(side);
  if (def == nullptr || def->IsBow()) return {};
  if (!Stow(m, items, h, /*sheathFirst=*/false)) return {};
  ItemStack taken = m.KitTake(sref);
  if (!m.EquipItemAsWas(def, h, &taken.damage)) {
    *m.KitMut().Resolve(sref) = taken;   // refused: it stays put away
    return {};
  }
  m.SetHeldContents(taken.contents, h);
  return side;
}

void EndDraw(Mob* m, const MobDraw& d) {
  if (m == nullptr) return;
  if (!d.clip.empty()) m->StopClip(d.clip);
  m->SetBowDraw(false);
}

// Refresh the draw's target from the world: where it is, how tall, how it
// moves (the brain's estimate while the brain still holds the same target).
// False = the target is gone (dead, despawned): the point stays where it was.
bool TrackTarget(TickAuthorityCtx& w, std::span<SessionTick> players, MobDraw& d) {
  Vec3 at{};
  float height = 0.0f;
  bool live = MobTargetNow(w, players, d.targetId, at, &height);
  // A DISTANT TARGET (docs/PLAN_distant_npcs.md section 2.7, D3): a creature
  // past the window in the edge band, as the near tier sees it this tick --
  // the tick-start entry the distant tier published (MobSystem::EdgeActor).
  // The arrow flies out of the window at it and the tier lands it.
  if (!live)
    if (const ai::Actor* ea = w.mobs.EdgeActor(d.targetId)) {
      at = ea->centre;
      height = ea->height;
      live = true;
    }
  if (live) {
    d.targetPoint = at;
    d.targetHeight = height;
  }
  if (const ai::Brain* br = w.mobs.MobBrain(d.mobId);
      br != nullptr && br->targetId == d.targetId)
    d.targetVel = br->haveTargetVel ? br->targetVel : Vec3{};
  return live;
}

void BeginDraw(TickAuthorityCtx& w, std::span<SessionTick> players, MobArcheryWorld& a,
               const ai::ShootRequest& r, uint32_t tick) {
  ArcheryEvent ev;
  ev.tick = tick;
  ev.mobId = r.mobId;
  ev.targetId = r.targetId;
  auto refuse = [&](const char* why) {
    ev.kind = ArcheryEvent::Kind::Refused;
    ev.why = why;
    a.stats.refused++;
    Log(a, ev);
  };
  Mob* m = w.mobs.FindMobById(r.mobId);
  if (m == nullptr || !m->Alive() || m->Ragdolled() || !MobSystem::OwnedHere(*m))
    return refuse("no body to draw with");
  if (a.Find(r.mobId) != nullptr) return refuse("already drawing");
  if (m->CastBusy()) return refuse("a spell in the hand");
  if (m->Stroke().Active() && m->Stroke().phase != NpcStroke::Phase::Guard)
    return refuse("mid-swing");
  const ai::RangedTuning* rt = RangedOf(w.mobs, r.mobId);
  if (rt == nullptr) return refuse("its profile does not shoot");
  Hand h = Hand::Right;
  const ItemDef* bow = HeldBow(*m, w.items, &h);
  if (bow == nullptr) return refuse("no bow in hand");
  if (!HandsCanDraw(*m, h)) return refuse("a hand that cannot draw");
  if (KitAmmoCount(m->GetKit(), bow->bow.ammo) <= 0) return refuse("no arrows");
  if (a.draws.size() >= MobArcheryWorld::kMaxDrawing) {
    a.stats.capRefused++;
    return refuse("too many bows drawn");
  }
  // A guard up is let go over the first few ticks of the draw.
  if (m->Stroke().Active() && m->Stroke().phase == NpcStroke::Phase::Guard)
    m->Stroke().StartRelease(kGuardRelease);
  MobDraw d;
  d.mobId = r.mobId;
  d.targetId = r.targetId;
  d.hand = HandIndex(h);
  d.startTick = tick;
  // The windup first (nothing loosed in it), then rt->draw of the pull.
  d.drawTo = bow->bow.windupTicks +
             std::max(1, (int)std::lround((double)bow->bow.drawTicks * (double)rt->draw));
  const int hold =
      (int)rt->holdTicks +
      (rt->holdJitter > 0
           ? (int)(rng::Hash3((uint32_t)r.mobId ^ kSaltHold, tick, 3u) % (rt->holdJitter + 1u))
           : 0);
  d.looseAt = d.drawTo + hold;
  d.targetPoint = r.targetPoint;
  d.targetVel = r.targetVel;
  d.bow = bow->name;
  d.clip = m->HandClip("bow_draw", h);
  TrackTarget(w, players, d);
  if (!d.clip.empty()) m->PlayClip(d.clip);
  const ShotAim s = AimShot(w, *m, d, *rt, BowArrowSpeed(*bow, d.drawTo));
  m->SetBowDraw(true, s.pitch);
  a.draws.push_back(d);
  a.stats.drawn++;
  ev.kind = ArcheryEvent::Kind::Draw;
  ev.item = bow->name;
  Log(a, ev);
}

// One tick of one drawn bow. False = the draw is over (loosed or aborted).
bool StepDraw(TickAuthorityCtx& w, std::span<SessionTick> players, MobArcheryWorld& a,
              MobDraw& d, uint32_t tick) {
  ArcheryEvent ev;
  ev.tick = tick;
  ev.mobId = d.mobId;
  ev.targetId = d.targetId;
  ev.drawn = d.ticks;
  Mob* m = w.mobs.FindMobById(d.mobId);
  auto abort = [&](const char* why) {
    EndDraw(m, d);
    ev.kind = ArcheryEvent::Kind::Abort;
    ev.why = why;
    a.stats.aborted++;
    Log(a, ev);
    return false;
  };
  if (m == nullptr || !m->Alive() || m->Ragdolled() || !MobSystem::OwnedHere(*m))
    return abort("the body is gone");
  if (m->Stunned(tick)) return abort("stunned");
  // A stagger holds attacks back (StaggerBlocksAttack: "neither swings, casts
  // nor looses"), and the gate on new shoot requests does not reach a draw
  // already under way: the string is let down, as a head stagger ends a swing.
  if (m->StaggerBlocksAttack()) return abort("staggered");
  if (m->CastBusy()) return abort("a spell in the hand");
  const Hand h = HandAt(d.hand);
  if (m->HeldItem(h) != d.bow) return abort("the bow left the hand");
  const ItemDef* bow = w.items.Named(d.bow);
  if (bow == nullptr || !bow->IsBow()) return abort("no such bow");
  if (!HandsCanDraw(*m, h)) return abort("a hand that cannot draw");
  if (KitAmmoCount(m->GetKit(), bow->bow.ammo) <= 0) return abort("no arrows");
  const ai::RangedTuning* rt = RangedOf(w.mobs, d.mobId);
  if (rt == nullptr) return abort("its profile does not shoot");
  // A target that died (or left) under the draw is not shot at: the string
  // is let down and the arrow stays in the kit.
  if (!TrackTarget(w, players, d) && d.targetId != 0) return abort("the target is gone");
  d.ticks++;
  if (d.ticks < d.looseAt) {
    const ShotAim s = AimShot(w, *m, d, *rt, BowArrowSpeed(*bow, d.drawTo));
    // The player's walk rule (session.cpp PhaseA): BowSpec drawMoveScale,
    // eased in over the windup as the bow comes up.
    const float k = std::min(1.0f, (float)d.ticks / (float)std::max(1, bow->bow.windupTicks));
    m->SetBowDraw(true, s.pitch, 1.0f + (bow->bow.drawMoveScale - 1.0f) * k);
    return true;
  }

  // ---- THE LOOSE --------------------------------------------------------------
  const ItemDef* ammo = w.items.Named(bow->bow.ammo);
  const float speed = BowArrowSpeed(*bow, d.ticks);
  if (ammo == nullptr || !ammo->IsAmmo()) return abort("no such arrow");
  if (!(speed > 0.0f)) return abort("fumbled (under the bow's minDraw)");
  ShotAim s = AimShot(w, *m, d, *rt, speed);
  // The error, scaled by the rules' aim tune (a hurt archer is a sloppy one).
  float aimTune = 1.0f;
  if (const ai::Brain* br = w.mobs.MobBrain(d.mobId);
      br != nullptr && br->tune[(int)ai::Tune::Aim] > 0.0f)
    aimTune = br->tune[(int)ai::Tune::Aim];
  s.dir = Spread(s.dir, rt->spread * aimTune, d.mobId, tick);
  std::vector<uint64_t> ignore;
  m->AppendLiveLimbBodies(ignore);
  const uint32_t seed = rng::Hash3(kSaltSeed, tick, (uint32_t)d.mobId);
  EndDraw(m, d);
  // WHO AT WHOM rides the flight (game/arrows.h ArrowShot): if it leaves the
  // window at a creature that is distant by then, that tier lands it (D3).
  ArrowShot shot;
  shot.shooter = d.mobId;
  shot.target = d.targetId;
  if (!w.arrows.Launch(*ammo, s.from, s.dir, speed, Vec3{}, seed, std::move(ignore), shot)) {
    ev.kind = ArcheryEvent::Kind::Abort;
    ev.why = "too many arrows in the air";
    a.stats.aborted++;
    Log(a, ev);
    return false;
  }
  KitTakeAmmo(m->KitMut(), ammo->name);
  a.stats.loosed++;
  ev.kind = ArcheryEvent::Kind::Loose;
  ev.from = s.from;
  ev.dir = s.dir;
  ev.speed = speed;
  ev.seed = seed;
  ev.drawn = d.ticks;
  ev.item = ammo->name;
  Log(a, ev);
  // ---- THE LAST ONE: the sidearm comes out ------------------------------------
  if (KitAmmoCount(m->GetKit(), ammo->name) <= 0) {
    const std::string side = DrawSidearm(*m, w.items, h);
    if (!side.empty()) {
      a.stats.sidearms++;
      ArcheryEvent se;
      se.kind = ArcheryEvent::Kind::Sidearm;
      se.tick = tick;
      se.mobId = d.mobId;
      se.targetId = d.targetId;
      se.item = side;
      Log(a, se);
    }
  }
  return false;
}

}  // namespace

void MobArcheryTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick) {
  MobArcheryWorld& a = w.archers;
  // ---- THE DISTANT ARCHERS' ARROWS COME IN (docs/PLAN_distant_npcs.md 2.7) ---
  // An arrow a distant archer loosed at a near creature was flown, for real,
  // by the distant tier (TickDistant, inside this tick's mobs.PreTick) up to
  // the window's boundary; here it becomes an ordinary flight at exactly that
  // state, stepped in phase K with every other arrow -- it hits or misses
  // physically, through PierceAlongRay, as a near archer's does. In the order
  // the tier queued them. Before the rule-2 return below: these are not
  // requests.
  for (const DistantArrowIn& in : w.mobs.TakeEdgeArrowsIn()) {
    const ItemDef* ammo = w.items.Named(in.ammo);
    ArrowShot shot;
    shot.shooter = in.shooter;
    shot.target = in.target;
    if (ammo != nullptr &&
        w.arrows.Inject(*ammo, SpellFxVec{in.pos[0], in.pos[1], in.pos[2]},
                        SpellFxVec{in.vel[0], in.vel[1], in.vel[2]}, in.seed, shot))
      w.mobs.DistantMut().Note(&DistantMobs::Stats::edgeArrowsInjected);
  }
  // RULE 2: nothing asked and nothing drawn = nothing to do.
  if (w.mobs.ShootRequests().empty() && a.draws.empty()) return;
  // ---- this tick's requests, in the order MobSystem issued them ----------------
  if (!w.mobs.ShootRequests().empty()) {
    const std::vector<ai::ShootRequest> reqs = w.mobs.ShootRequests();
    w.mobs.ClearShootRequests();
    int served = 0;
    for (const ai::ShootRequest& r : reqs) {
      a.stats.requests++;
      if (served++ >= MobArcheryWorld::kServedPerTick) {
        ArcheryEvent ev;
        ev.kind = ArcheryEvent::Kind::Refused;
        ev.tick = tick;
        ev.mobId = r.mobId;
        ev.targetId = r.targetId;
        ev.why = "too many requests this tick";
        a.stats.refused++;
        Log(a, ev);
        continue;
      }
      BeginDraw(w, players, a, r, tick);
    }
  }
  // ---- every drawn bow, in the order the draws began -----------------------------
  // (A draw begun this tick does not also step this tick: it was counted from
  // the request, so its first tick back is the next one.)
  for (size_t i = 0; i < a.draws.size();) {
    if (a.draws[i].startTick == tick) {
      i++;
      continue;
    }
    if (StepDraw(w, players, a, a.draws[i], tick)) {
      i++;
    } else {
      a.draws.erase(a.draws.begin() + (ptrdiff_t)i);
    }
  }
}

int ArmArcher(MobSystem& mobs, uint64_t mobId, const ItemLibrary& items, std::string* note) {
  Mob* m = mobs.FindMobById(mobId);
  if (m == nullptr) return -1;
  const ai::RangedTuning* rt = RangedOf(mobs, mobId);
  if (rt == nullptr) return -1;
  Hand h = Hand::Right;
  const ItemDef* bow = HeldBow(*m, items, &h);
  std::string did;
  if (bow == nullptr) {
    const ItemDef* want = items.Named(rt->bow);
    if (want == nullptr || !want->IsBow()) {
      if (note) *note = "no bow named \"" + rt->bow + "\" in the item library";
      return -1;
    }
    // The right fist's item becomes the SIDEARM (the sheath, else the bag);
    // the left fist is the string hand and is emptied into the bag.
    const std::string side = m->HeldItem(Hand::Right);
    if (!Stow(*m, items, Hand::Right, /*sheathFirst=*/true) ||
        !Stow(*m, items, Hand::Left, /*sheathFirst=*/false) ||
        !m->EquipItem(want, Hand::Right)) {
      if (note) *note = "could not put a bow in its hand";
      return -1;
    }
    bow = want;
    h = Hand::Right;
    did = "a " + want->name + " in its right hand" +
          (side.empty() ? std::string() : ", its " + side + " as a sidearm");
  }
  int added = 0;
  const int have = KitAmmoCount(m->GetKit(), bow->bow.ammo);
  const int idx = items.Find(bow->bow.ammo);
  if (idx >= 0 && rt->quiver > have) {
    const ItemStack st = StackOf(items, idx, rt->quiver - have);
    if (!st.Empty() && m->AddCarried(st)) added = rt->quiver - have;
  }
  if (note) {
    *note = did.empty() ? "already holds a " + bow->name : did;
    *note += ", " + std::to_string(added) + " " + bow->bow.ammo + " added (" +
             std::to_string(KitAmmoCount(m->GetKit(), bow->bow.ammo)) + " carried)";
  }
  return added;
}

// ---- THE DRAWN BOW AS SEEN: the Mob's half (game/arrows.h PresentDrawnBow) ----
// Everything here reads limb.xf -- the transform AppendXforms hands the
// renderer -- plus renderOffset_, so what is placed off it lands on the drawn
// body, not on the collider a tick behind or ahead of it.

bool Mob::HeldBodyXf(Hand h, BodyTransform& out) const {
  const int slot = held_[HandIndex(h)].slot;
  if (slot < 0 || slot >= (int)limbs_.size() || !limbs_[slot].body) return false;
  out = limbs_[slot].xf;
  out.pos = out.pos + renderOffset_;
  return true;
}

bool Mob::HeldCarved(Hand h) const {
  const int slot = held_[HandIndex(h)].slot;
  return slot >= 0 && slot < (int)limbs_.size() && limbs_[slot].carved;
}

bool Mob::HandSocketWorld(Hand h, Vec3& out) const {
  if (def_ == nullptr) return false;
  const int si = def_->FindSocket(HandContext(h));
  if (si < 0) return false;
  const MobSocketDef& sock = def_->sockets[si];
  if (sock.partIndex < 0 || sock.partIndex >= (int)limbs_.size()) return false;
  if (!LimbAlive(sock.partIndex)) return false;
  const MobLimb& hand = limbs_[sock.partIndex];
  if (!hand.body) return false;
  // EquipItem's own placement: the socket is `offset - restOffset` from the
  // hand part's frame, through the hand's live rotation.
  const Quat q{hand.xf.quat[0], hand.xf.quat[1], hand.xf.quat[2], hand.xf.quat[3]};
  out = hand.xf.pos + QuatRotate(q, sock.offset - hand.restOffset) + renderOffset_;
  return true;
}

bool Mob::ClipPlaying(const std::string& name) const {
  const int ci = skel_.FindClip(name);
  if (ci < 0) return false;
  for (const ClipInstance& inst : anim_.clips)
    if (inst.clip == ci && !inst.stopping) return true;
  return false;
}

bool Mob::PartCornerWorld(const std::string& part, Vec3& out) const {
  const int pi = skel_.FindPart(part);
  if (pi < 0 || pi >= (int)limbs_.size() || !limbs_[pi].body || !LimbAlive(pi))
    return false;
  out = limbs_[pi].xf.pos + renderOffset_;
  return true;
}
