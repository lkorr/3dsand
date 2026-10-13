#pragma once
// meleehit.h — ONE MELEE BLOW, AS THE STRUCK CREATURE'S OWNER APPLIES IT
// (docs/PLAN_multiplayer_m9.md "Melee across the wire", decided 2026-10-08).
//
// THE PROBLEM. A sword's damage is resolved by `MeleeSweepDamage` on the
// ATTACKER's machine, against whatever bodies that machine holds. In a
// two-machine game the other player, and every NPC the other machine steps,
// is a GHOST there (Mob::IsGhost): a posed copy whose owner never hears of a
// blow landed on it. So a sword could not hurt a peer, and an NPC owned by one
// machine swinging at the other machine's player hurt nobody.
//
// THE DECISIONS, and why each is the one this engine wants:
//
//   1. THE ATTACKER DECIDES. Its machine is the only one holding the exact
//      blade pose of the tick, and the contact was judged against what its
//      player SAW -- the rule explosions already follow (an ExplosionOp is
//      applied by every owner wherever it lands). The owner applies this
//      record through the same Damage / CutLimb / BluntHit / BiteHit calls a
//      local blow makes, unchecked. Cheating is not a co-op design concern.
//
//   2. THE ATTACKER JUDGES A PARRY TOO. A parry here is GEOMETRY, not a guard
//      flag: `MobSystem::FindParry` tests the swept blade against the
//      defender's blade. The attacker has both (the defender's is the ghost's
//      posed held item, and MobPose now carries whether that blade is live --
//      cutting or held in guard; a peer PLAYER's ghost holds the sword its
//      owner streams, net::PlayerBody), so the parry is judged where the attacker's
//      sword is, and its stroke is ARRESTED on the tick it meets the guard.
//      Judging it on the defender's machine instead would let the attacker's
//      sword pass through the guard on screen and be stopped a round trip
//      later. What the parry DOES to the defender (its weapon's wear, the
//      flinch, the guard beaten open) travels as a record of kind `Parry`.
//
//   3. A HANDOFF IN FLIGHT IS ROUTED, NOT RACED. Ownership is derived, so the
//      two machines agree on it tick by tick (net/authority.h); a record is
//      applied by whichever machine owns the target WHEN IT ARRIVES. One that
//      reaches a machine which has just given the creature away is sent on
//      (`hops`, bounded); one that reaches the new owner while the handoff
//      record carrying the creature's state is still in flight WAITS for it
//      (Mob::AwaitingHandoff) -- applying it first would have the record's
//      overlay erase the wound four ticks later.
//
//   4. THE ATTACKER SEES THE WOUND AT ONCE. Its ghost is carved by the same
//      record, as PRESENTATION: the lattice changes and nothing else does --
//      no gore enters its op batch, no fragment or limb becomes its debris,
//      the ghost cannot die or lose a limb here (Mob::Sever and Die refuse a
//      ghost). The owner's carve is the real one, its gore enters the owner's
//      batch, and the STRUCTURE it changes (a limb off, a death) reaches the
//      attacker the way any owner-side change does: the pose stream omits a
//      severed limb (MobSystem::ApplyPose drops it; a peer player's ghost
//      avatar is posed from its owner's net::PlayerBody stream the same way,
//      Mob::PlaceFromOwnerPose), and a death is MobPose.alive. Waiting for the
//      owner's result instead is not available: a living creature's wounds
//      have no state stream at all, so the attacker would never see them.
//      With rule 2 the record is what the owner applies, so nothing has to
//      snap back.
//
// LOOSE MATTER GOES THE SAME WAY (v2): a blow on a peer's corpse piece,
// dropped item or crate is a `Loose` record against the body's global id, in
// its body frame, applied by its owner through ResolveOnLooseMatter. The
// attacker's ghost body is carved as presentation, and a piece that parts
// from it is dropped rather than made this machine's debris
// (DebrisSystem::CutBody / BluntBody on a ghost) -- the owner's fragments
// arrive as its ghost bodies.
//
// ALL GEOMETRY IS IN THE STRUCK SLOT'S BODY FRAME. The ghost is posed from a
// stream a few ticks old; the owner's body is where it is now. A world-space
// kerf applied there would miss the limb it was cut into. In the slot's own
// frame it lands where it landed.
//
// The parts (kerf, trauma, tear) are REBUILT on the owner from the inputs
// `BuildStrikeParts` took, rather than carried: the same function, the same
// tuning (the handshake pins it), the same draw key -- so the owner tears
// exactly the rim the attacker's ghost was torn with.
#include <cstdint>
#include <string>

#include "game/impact.h"
#include "math3d.h"
#include "phys/kerf.h"  // Penetrator: a MeleeHitKind::Pierce record's projectile
#include "sim/bytestream.h"

// A STRUCK THING, ACROSS TWO MACHINES. A mob is its mob id on both (the id
// bands make it global). A PLAYER is not: its avatar's mob id is per-machine
// (a local session's is 0x5A11ED + i, a peer's ghost 64 above), so a player is
// named by the playerId of the machine that drives it -- the `owner_` its
// avatar carries everywhere (MobSystem::SetAvatars, SetAvatarOwner). LOOSE
// MATTER (a corpse piece, a dropped sword, a crate) is its debris GLOBAL id
// (net::MakeGlobalBodyId), the name every BodyAnnounce/Pose/Handoff uses.
struct CombatantRef {
  static constexpr uint32_t kMob = 0, kPlayer = 1, kBody = 2;
  uint32_t kind = kMob;
  uint64_t id = 0;
};

enum class MeleeHitKind : uint8_t {
  Flesh = 0,   // a rig slot below AppendedBase (impact.h StruckKind::Flesh)
  Shell,       // a worn garment took it (the cover retarget, or the probe)
  Parry,       // the held blade stopped the stroke (rule 2 above)
  Loose,       // loose matter (CombatantRef::kBody): DebrisSystem resolves it
  // A POINT DRIVEN ALONG A RAY (game/pierce.h, docs/PLAN_projectiles.md C):
  // an arrow the shooter's flight met a peer's creature with. The owner runs
  // the channel on its own slot (Mob::PierceLimb) from `at` along `sweepDir`
  // with `pierce`, and when it EMBEDS, the arrow `MeleeHit::embedItem` stays
  // in that limb on the owner (Mob::EmbedArrow). The shooter's ghost was
  // carved as presentation, rule 4's way.
  Pierce,
};

// Everything fixed-size, so the record is one Pod and three strings.
struct MeleeHitCore {
  CombatantRef target;
  CombatantRef attacker;
  uint32_t kind = 0;          // MeleeHitKind
  // ---- geometry, in the `slot` body's frame ----
  Vec3 at{};                  // the contact (after the cover retarget)
  Vec3 clothAt{};             // where the edge crossed `clothSlot`
  Vec3 sweepDir{};            // the blade's travel, unit
  Vec3 seg{};                 // this sub-step's edge segment, base -> tip
  Vec3 flat{};                // the blade's flat normal (zero = unauthored)
  // ---- the blow ----
  StrikeProfile strike;       // the whole profile (charged, if it was)
  float radius = 0.0f;        // carve half-width, world voxels
  float power = 0.0f;         // speed x edge x powerScale (the sweep's one)
  float tipSpeed = 0.0f;      // world voxels / s
  float heft = 1.0f;          // EdgeSweep::heft
  float repeat = 1.0f;        // repeatHitScale^(n-1), already decided
  float cuePower = 0.0f;      // the StrikeEvent's power (speed x edge)
  float cueGainDb = 0.0f;     // ...and its level
  float edgeU = 0.0f;         // the coat contact: where on the edge
  uint32_t hitSeed = 0;       // every part's draw key
  uint32_t flags = 0;         // kFlag* below
  uint32_t hops = 0;          // re-routes so far (rule 3)
  uint32_t waitTick = 0;      // local: first tick it waited (not meaningful
                              // on the wire; the receiver restarts it)
  // LOOSE only: the body that is SHOVED, by global id, when the blow went to
  // a garment strapped to it (the corpse-armour retarget). 0 = the target.
  uint64_t reactBodyGid = 0;
  // PIERCE only: the projectile as it met this body (its speed at the entry,
  // its point, its flat hp, its soak seed). Plain floats and words: a POD.
  Penetrator pierce{};

  static constexpr uint32_t kSelfMounted = 1u << 0;   // a fist / jaws
  static constexpr uint32_t kFirstContact = 1u << 1;  // impulses may land
  static constexpr uint32_t kBiteAllowed = 1u << 2;   // the stroke's one bite
  static constexpr uint32_t kFirstOnVictim = 1u << 3; // push the strike cue
  static constexpr uint32_t kHaft = 1u << 4;          // the haft, not the edge
  bool Has(uint32_t f) const { return (flags & f) != 0; }
};

struct MeleeHit {
  MeleeHitCore c;
  std::string slot;       // the rig slot that takes the blow, BY NAME
  std::string reactSlot;  // the flesh that flinches ("" = `slot`)
  std::string clothSlot;  // cloth the edge passed through ("" = none)
  // PIERCE only: the item that stays in the limb when the point EMBEDS on
  // the owner (an arrow, Mob::EmbedArrow); "" = nothing stays.
  std::string embedItem;
};

// Bumped whenever MeleeHitCore or the string list changes. A record of
// another version fails to decode (the reader is poisoned).
// v2 (2026-10-08): loose matter (CombatantRef::kBody, MeleeHitKind::Loose,
// reactBodyGid); CombatantRef's first word is a kind.
// v3 (2026-10-12): MeleeHitKind::Pierce -- MeleeHitCore::pierce and the
// `embedItem` string (docs/PLAN_projectiles.md C).
inline constexpr uint32_t kMeleeHitVersion = 3;

inline void EncodeMeleeHit(ByteWriter& w, const MeleeHit& h) {
  w.U32(kMeleeHitVersion);
  w.Pod(h.c);
  w.Str(h.slot);
  w.Str(h.reactSlot);
  w.Str(h.clothSlot);
  w.Str(h.embedItem);
}

inline bool DecodeMeleeHit(ByteReader& r, MeleeHit& h) {
  uint32_t v = 0;
  if (!r.U32(v) || v != kMeleeHitVersion) {
    r.ok = false;
    return false;
  }
  r.Pod(h.c);
  r.Str(h.slot);
  r.Str(h.reactSlot);
  r.Str(h.clothSlot);
  r.Str(h.embedItem);
  h.c.waitTick = 0;
  return r.ok;
}
