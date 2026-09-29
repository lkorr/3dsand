// refs_game.cpp — see refs_game.h.

#include "world/refs_game.h"

#include <cmath>

#include "game/mob.h"
#include "game/session.h"
#include "sim/bytestream.h"
#include "sim/chunkstore.h"

namespace refs {

namespace {

constexpr uint32_t kRefsFourCC =
    'R' | ('E' << 8) | ('F' << 16) | ((uint32_t)'S' << 24);

float Len(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

bool UsePointOf(RefCtx& ctx, const RefKind& k, const Ref& r, Vec3& at) {
  if (k.usePoint) return k.usePoint(ctx, r, at);
  at = Vec3{(float)r.pos.x + 0.5f, (float)r.pos.y + 0.5f, (float)r.pos.z + 0.5f};
  return true;
}

}  // namespace

// ---- the prompt ---------------------------------------------------------------

const Ref* PickUsable(RefStore& s, RefCtx& ctx, Vec3 from, Vec3 dir, Vec3 eye,
                      std::string* prompt) {
  const Ref* best = nullptr;
  float bestT = 1e30f;
  std::string bestPrompt;
  for (const auto& [id, r] : s.All()) {
    if (!s.IsActive(id)) continue;
    const RefKind* k = Kinds().Find(r.kind);
    if (k == nullptr || !k->onUse || !k->usePrompt) continue;
    Vec3 p;
    if (!UsePointOf(ctx, *k, r, p)) continue;
    if (Len(p - eye) > kUseReach + k->useRadius) continue;
    const Vec3 d = p - from;
    const float t = d.x * dir.x + d.y * dir.y + d.z * dir.z;
    if (t < 0.0f || t >= bestT) continue;
    const Vec3 onRay = from + dir * t;
    if (Len(onRay - p) > k->useRadius) continue;
    std::string text = k->usePrompt(ctx, r);
    if (text.empty()) continue;
    best = &r;
    bestT = t;
    bestPrompt = std::move(text);
  }
  if (prompt) *prompt = best ? bestPrompt : std::string();
  return best;
}

// ---- the tick -------------------------------------------------------------------

void TickRefs(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
              OpBatch& out) {
  if (w.refs == nullptr) return;
  RefStore& s = *w.refs;
  s.BindChunkStore(&w.stream.Store());
  RefCtx ctx{&s, &w.mobs, &w.world, &w.stream.Store(), &out, tick};
  s.Update(ctx, w.world.WindowOrigin());

  // THE USE VERB, per session in index order (the op-order rule: session 0's
  // effects first). Validated HERE, against this player's own eye, so a
  // command naming something out of reach or not active does nothing -- and
  // says so in the record.
  for (size_t i = 0; i < players.size(); i++) {
    const TickInput& ti = players[i].ti;
    if (!ti.Pressed(TB_USE)) continue;
    UseRecord rec;
    rec.tick = tick;
    rec.player = (uint32_t)i;
    const Ref* r = ti.useRef != 0 ? s.FindByHash(ti.useRef) : nullptr;
    const RefKind* k = r != nullptr ? Kinds().Find(r->kind) : nullptr;
    Vec3 at{};
    if (r == nullptr) {
      rec.message = "use: no ref has that id hash";
    } else if (rec.id = r->id; !s.IsActive(r->id)) {
      rec.message = "use: not active (outside the window or waiting)";
    } else if (k == nullptr || !k->onUse) {
      rec.message = "use: kind \"" + r->kind + "\" is not usable";
    } else if (!UsePointOf(ctx, *k, *r, at)) {
      rec.message = "use: not usable right now";
    } else if (Len(at - players[i].s->player.EyePos()) > kUseReach + k->useRadius + 8.0f) {
      rec.message = "use: out of reach";
    } else {
      RefUse u;
      u.player = (uint32_t)i;
      u.session = players[i].s;
      k->onUse(ctx, *r, u);
      rec.used = true;
      rec.message = u.message;
      // The HUD line belongs to the window's player only (the presentation
      // seam, session.h section C).
      if (players[i].s->localView && !u.message.empty()) {
        w.ui.kitMessage = u.message;
        w.ui.kitMessageAge = 0.0f;
      }
    }
    s.NoteUse(std::move(rec));
  }
}

// ---- the save ---------------------------------------------------------------------

EntitySection MakeRefsSection(RefStore& s, MobSystem* mobs) {
  RefStore* sp = &s;
  EntitySection sec{
      kRefsFourCC, RefStore::kSaveVersion,
      // Every load, section present or not: an older save has no ref states,
      // so every ref starts as authored.
      [sp, mobs] {
        RefCtx c{sp, mobs};
        sp->ResetForLoad(c);
      },
      // The whole-payload form (a pre-S4 entities.sve, the gates that round-
      // trip one section): u32 count, then (pos, record bytes) per delta.
      [sp, mobs](std::vector<uint8_t>& out) {
        RefCtx c{sp, mobs};
        std::vector<std::pair<Vec3, std::vector<uint8_t>>> recs;
        sp->SaveDeltas(c, recs);
        ByteWriter w{out};
        w.U32((uint32_t)recs.size());
        for (const auto& [p, b] : recs) {
          w.Pod(p);
          w.PodVec(b);
        }
      },
      [sp](const uint8_t* d, size_t n, uint32_t v) {
        if (v != RefStore::kSaveVersion) return false;
        ByteReader r{d, n};
        uint32_t count = 0;
        r.U32(count);
        for (uint32_t i = 0; i < count && r.ok; i++) {
          Vec3 p{};
          std::vector<uint8_t> b;
          r.Pod(p);
          r.PodVec(b);
          std::string id;
          RefDelta delta;
          if (r.ok && RefStore::DecodeDelta(b.data(), b.size(), id, delta))
            sp->AcceptDelta(id, std::move(delta), "the save");
        }
        return r.ok;
      }};
  sec.scope = EntityScope::Region;
  sec.saveRecords = [sp, mobs](std::vector<EntityRecord>& out) {
    RefCtx c{sp, mobs};
    std::vector<std::pair<Vec3, std::vector<uint8_t>>> recs;
    sp->SaveDeltas(c, recs);
    for (auto& [p, b] : recs) {
      EntityRecord r;
      r.pos = p;
      r.bytes = std::move(b);
      out.push_back(std::move(r));
    }
  };
  sec.loadRecord = [sp](const uint8_t* d, size_t n, uint32_t v) {
    std::string id;
    RefDelta delta;
    if (v != RefStore::kSaveVersion || !RefStore::DecodeDelta(d, n, id, delta)) {
      sp->Warn("save: an unreadable ref state record is dropped");
      return RecordLoad::Dropped;
    }
    return sp->AcceptDelta(id, std::move(delta), "the save") ? RecordLoad::Applied
                                                              : RecordLoad::Dropped;
  };
  return sec;
}

}  // namespace refs
