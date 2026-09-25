// tickrig.h — the selftest harness ticks through THE tick (rule-unification
// W2-O).
//
// WHY. Until W2-O no gate called `TickAuthority` (game/session.cpp). Every gate
// that needed a tick hand-rolled one: ~123 `.PreTick(` call sites across 14
// selftest files and 79 `phys.Step(kTickDt)`, each a copy of SOME of the tick
// as it stood the day the gate was written — and each silently missing
// whatever the real tick grew afterwards (the day phase and the rain word the
// body reactions read, the vessel pass, the contact-damage pass after the
// physics step, the celestial clock, the edit-layer drain, the op order). The
// cost is recorded in memory as gotcha-harness-stops-where-the-game-keeps-
// going: corpse limbs popped off in the game while the gate that owned the
// behaviour passed, because the gate stopped where the game kept going.
//
// WHAT. A `TickRig` builds the `TickAuthorityCtx` a gate's `selftest::Ctx`
// lacks — an inert far field (render-only; never Init'ed, so it never
// drains), an empty glyph library and prefab list, a collision-class table,
// a presentation seam nobody reads — and one PlayerSession whose body is a
// fly-mode camera with no avatar and no input. `RunTicks` then runs the real
// tick, followed by the pump the frame layer owns (WaitIdle + ProcessEvents,
// the harness pacing every gate ticker already had).
//
// The harness body must not drag the gate's world around with it, so the rig
// sets TickAuthorityCtx::harness (session.h section J): the residency window
// is pinned, the submit centres its CPU mirror on the gate's FIXTURE chunk,
// and the NPC targeting layer's player list is the gate's (the harness body
// is not a combatant). A gate's own ops go in through `inject`, at the top of
// the tick — the lowest op indices, where every hand-rolled ticker put them.
//
// THE RULE FOR NEW GATES: tick with `RunTicks`. Call a single phase directly
// (`mobs.PreTick`, `phys.Step`, ...) ONLY when the gate deliberately tests
// that one phase in isolation, and say why in a comment at the call.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "game/session.h"
#include "math3d.h"
#include "measure/perfnodes.h"
#include "sim/world.h"

namespace selftest {
struct Ctx;
}

namespace sandvox::support {

// A gate's own ops for ONE tick. Pushed ahead of everything the tick authors —
// except `exps`, which go off in the primary's explosion slot (phase K) so a
// gate's blast carves, launches and crater-scans exactly as a grenade does.
struct TickOps {
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> exps;
  std::vector<CellOp> cells;
  std::vector<ParticleSpawn> spawns;
};

// The engine a rig ticks: what TickAuthorityCtx takes by reference and a
// harness already owns. A gate builds it from its selftest::Ctx; a `--shot*`
// harness in main.cpp from its own locals.
struct TickEngine {
  GpuContext& ctx;
  World& world;
  Simulation& sim;
  Stream& stream;
  Physics& phys;
  MobSystem& mobs;
  DebrisSystem& debris;
  const std::vector<MaterialDef>& mats;
  const ItemLibrary& items;
};

class TickRig {
 public:
  // `lastTick` is the tick the gate's world is AT: the first RunTicks runs
  // lastTick + 1. `fixtureChunk` is the chunk the CPU mirror centres on (what
  // the gate's ticker passed as SubmitTick's playerChunk).
  TickRig(const TickEngine& e, uint32_t lastTick, IVec3 fixtureChunk);
  TickRig(selftest::Ctx& c, uint32_t lastTick, IVec3 fixtureChunk);
  ~TickRig();
  TickRig(const TickRig&) = delete;
  TickRig& operator=(const TickRig&) = delete;

  // The last tick that ran (or `lastTick` before the first).
  uint32_t tick = 0;

  const TickEngine& Engine() const { return e_; }
  PlayerSession& Session() { return *session_; }
  TickAuthorityCtx& Authority() { return *w_; }
  // What the last tick submitted, after every system authored into it.
  const OpBatch& LastBatch() const { return batch_; }
  // Where in LastBatch() the MOB PHASE's own authoring sits
  // (TickAuthorityCtx::Harness::mobPhase): for a claim about what the creature
  // system emitted, scoped away from the other authors in the same tick.
  const TickAuthorityCtx::Harness::Span& MobPhase() const {
    return w_->harness.mobPhase;
  }
  // Move the mirror's centre (a fixture that walks).
  void SetFixtureChunk(IVec3 c);
  // Hand the NPC targeting layer's player list back to the tick (the harness
  // body becomes a live actor where it stands) — for a gate that wants the
  // real tick's player-actor behaviour rather than its own list.
  void SetOwnsActors(bool own) { w_->harness.ownsActors = own; }

 private:
  friend void RunTicks(TickRig&, int,
                       const std::function<void(uint32_t, TickOps&)>&);
  TickEngine e_;
  // What the TickAuthorityCtx references and a selftest::Ctx does not carry.
  FarField far_;
  GlyphLibrary glyphs_;
  std::vector<Prefab> prefabs_;
  std::vector<uint32_t> classOf_;
  MicroBodySet ownMb_;
  UIState ui_;
  HitStop hitStop_;
  DeathBody deathBody_;
  bool deathFrozen_ = false;
  bool liveTimed_ = false;
  PassTimer liveTimer_;
  sandvox::PerfSample liveSample_;
  uint32_t liveFrameNo_ = 0;
  float tickMsSmooth_ = 0.0f;
  uint64_t farEntries_ = 0, farTicks_ = 0, farBiggest_ = 0;
  std::unique_ptr<PlayerSession> session_;
  std::unique_ptr<TickAuthorityCtx> w_;
  OpBatch batch_;
  TickOps pending_;
};

// Run `n` ticks of THE tick (TickAuthority) on the rig's world, each followed
// by the harness pump. `ops`, when given, is asked for the gate's own ops for
// each tick before it runs.
void RunTicks(TickRig& rig, int n,
              const std::function<void(uint32_t tick, TickOps& ops)>& ops = {});

// THE SHAPE EVERY HAND-ROLLED TICKER HAD, on the real tick: a gate's own tick
// counter (`t`, which the gate keeps reading — ForceAttack seeds, snapshot
// comparisons) advanced by one per call, with the CPU mirror centred on the
// gate's fixture chunk. The rig is built on first use, so a cursor declared
// beside the counter costs nothing until the gate ticks. `chunk` may be moved
// between calls (a fixture that walks off its column).
struct TickCursor {
  selftest::Ctx& c;
  uint32_t& t;
  IVec3 chunk;
  std::unique_ptr<TickRig> rig;
  TickCursor(selftest::Ctx& ctx, uint32_t& tick, IVec3 fixtureChunk)
      : c(ctx), t(tick), chunk(fixtureChunk) {}
  // One tick; `ops` are the gate's own, pushed first.
  void operator()(const TickOps& ops = {});
  void operator()(const std::vector<BrushOp>& brush,
                  const std::vector<CellOp>& cells = {});
  TickRig& Rig();
};

}  // namespace sandvox::support
