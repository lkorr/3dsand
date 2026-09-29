// refs_kinds.cpp — THE KINDS THIS BUILD KNOWS (world/refs.h RefKindRegistry).
//
// RegisterAllKinds() is the ONE place a package adds its kinds: write a
// `RegisterXxxKinds()` in your own file, declare it below, call it here. The
// registry REPLACES a kind of the same name, so P7's `npc` simply registers
// over P1's temporary one (put its call after RegisterP1Kinds).
//
// P1 ships two:
//   marker — a named point ("harrowby/green_well"). Inert: no activation
//            effect, not usable. props.tags (array of strings) is what
//            schedules and editors search by.
//   npc    — TEMPORARY (P7 replaces the brain). Spawns the mob def named by
//            `base` (default "human") at pos/yaw the first time the ref comes
//            into the window, stamps Mob::RefId, and remembers that it did
//            (delta "spawned"): from then on the creature is the world's --
//            it parks, saves and unparks as itself, and the ref never spawns
//            a second one. An EDIT to the ref despawns it and clears the
//            delta, so the villager re-appears where the file now says.
//            Use: "Talk to <name>" -> a line saying they have nothing to say
//            (P3's dialogue::Begin lands here).

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "game/mob.h"
#include "sim/bytestream.h"
#include "sim/world.h"
#include "world/refs.h"

namespace refs {

namespace {

constexpr uint32_t kNpcSpawned = 1;

std::string NpcName(const Ref& r) {
  if (r.props.contains("name") && r.props["name"].is_string())
    return r.props["name"].get<std::string>();
  const size_t s = r.id.rfind('/');
  return s == std::string::npos ? r.id : r.id.substr(s + 1);
}

std::string NpcDefName(const Ref& r) { return r.base.empty() ? "human" : r.base; }

int FloorDiv(int v, int d) { return (v >= 0) ? v / d : -((-v + d - 1) / d); }

// The ground wait (MobParking::GroundKnown's rule, persist.h): a creature is
// placed only onto ground the fetch cache has answered for SINCE this ref
// first asked -- a cached copy from before the chunk last left the window is
// stale terrain. Per (ref id): the tick of the first ask; forgotten when the
// ref activates or leaves.
struct NpcState {
  std::map<std::string, uint32_t> askedAt;
};

bool GroundKnown(World& world, NpcState& st, const Ref& r, uint32_t tick) {
  auto it = st.askedAt.find(r.id);
  const bool first = it == st.askedAt.end();
  if (first) it = st.askedAt.emplace(r.id, tick).first;
  bool known = !first;
  const int cx = FloorDiv(r.pos.x, (int)kChunk), cz = FloorDiv(r.pos.z, (int)kChunk);
  const int cy = FloorDiv(r.pos.y, (int)kChunk);
  for (int dy = 0; dy >= -1; dy--) {
    const IVec3 wc{cx, cy + dy, cz};
    const CachedChunk* cc = world.Cached(wc);
    if (first || cc == nullptr || cc->voxels.size() != kChunkVol ||
        cc->version < it->second) {
      world.RequestChunkFetch(wc, World::FetchSource::Mob);
      known = false;
    }
  }
  return known;
}

void RegisterP1Kinds() {
  // ---- marker ----
  RefKind marker;
  marker.name = "marker";
  marker.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (!r.props.contains("tags")) return;
    const Json& t = r.props["tags"];
    bool ok = t.is_array();
    if (ok)
      for (const Json& v : t) ok &= v.is_string();
    if (!ok) p.push_back("props.tags: must be a list of words, e.g. [\"gather\"]");
  };
  Kinds().Register(std::move(marker));

  // ---- npc (temporary; P7 replaces) ----
  auto st = std::make_shared<NpcState>();
  RefKind npc;
  npc.name = "npc";
  npc.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (r.props.contains("name") && !r.props["name"].is_string())
      p.push_back("props.name: must be text");
  };
  npc.activate = [st](RefCtx& c, const Ref& r) -> Activation {
    if (c.mobs == nullptr || c.refs == nullptr) return Activation::Done;   // store-only
    MobSystem& mobs = *c.mobs;
    // Already standing (a load or an unpark brought it back): nothing to do.
    if (mobs.FindMobByRef(r.id) != nullptr) {
      st->askedAt.erase(r.id);
      return Activation::Done;
    }
    // Spawned before and not standing: it is parked elsewhere, saved in a
    // bucket, or dead and gone. Either way the world owns it now.
    if (const RefDelta* d = c.refs->Delta(r.id); d != nullptr && d->bytes.size() >= 4) {
      ByteReader rd{d->bytes.data(), d->bytes.size()};
      uint32_t state = 0;
      rd.U32(state);
      if (state == kNpcSpawned) {
        st->askedAt.erase(r.id);
        return Activation::Done;
      }
    }
    const int def = mobs.FindDef(NpcDefName(r));
    if (def < 0) {
      c.refs->Warn("refs: " + r.id + ": base: no mob def named \"" + NpcDefName(r) +
                   "\" (assets/mobs); the npc is not spawned");
      return Activation::Done;
    }
    if (c.world != nullptr && !GroundKnown(*c.world, *st, r, c.tick)) return Activation::Retry;
    if (!mobs.HasRoomToSpawn()) return Activation::Retry;
    const uint64_t id = mobs.Spawn(def, r.pos);
    if (id == 0) return Activation::Retry;
    st->askedAt.erase(r.id);
    if (Mob* m = mobs.FindMobById(id)) m->SetRefId(r.id);
    mobs.SetHeading(id, (float)r.yaw * 3.14159265358979f / 180.0f);
    std::vector<uint8_t> b;
    ByteWriter w{b};
    w.U32(kNpcSpawned);
    c.refs->SetDelta(r.id, "npc", 1, std::move(b));
    return Activation::Done;
  };
  npc.deactivate = [st](RefCtx& c, const Ref& r, RefEvent why) {
    st->askedAt.erase(r.id);
    if (why != RefEvent::Edited && why != RefEvent::Deleted) return;
    // The author changed or removed the villager: take the old body away so
    // the new line can spawn fresh (Deleted: nothing comes back).
    if (c.mobs != nullptr) {
      if (Mob* m = c.mobs->FindMobByRef(r.id)) c.mobs->RemoveMob(m->Id());
    }
    if (c.refs != nullptr) c.refs->ClearDelta(r.id);
  };
  npc.usePoint = [](RefCtx& c, const Ref& r, Vec3& at) {
    if (c.mobs == nullptr) return false;
    const Mob* m = c.mobs->FindMobByRef(r.id);
    if (m == nullptr || !m->Alive() || m->Def() == nullptr) return false;
    const Vec3 o = m->Origin();
    const Vec3 s = m->Def()->worldSize;
    at = Vec3{o.x + s.x * 0.5f, o.y + s.y * 0.6f, o.z + s.z * 0.5f};
    return true;
  };
  npc.usePrompt = [](RefCtx&, const Ref& r) { return "Talk to " + NpcName(r); };
  npc.onUse = [](RefCtx&, const Ref& r, RefUse& u) {
    u.message = NpcName(r) + " has nothing to say yet.";
  };
  npc.useRadius = 7.0f;
  Kinds().Register(std::move(npc));
}

}  // namespace

void RegisterAllKinds() {
  static bool done = false;
  if (done) return;
  done = true;
  RegisterP1Kinds();
  // ---- later packages: one line each, AFTER P1 (a later registration of the
  // same name replaces the earlier one) ----
  // RegisterStructureKinds();   // P4
  // RegisterDoorKinds();        // P6: door, container, bed
  // RegisterResidentKinds();    // P7: npc (replaces P1's), waynode
}

}  // namespace refs
