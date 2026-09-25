// tickrig.cpp — see tickrig.h.
#include "test/tickrig.h"

#include "sim/materials.h"
#include "test/selftest.h"
#include "test/support.h"

namespace sandvox::support {

namespace {
TickEngine EngineOf(selftest::Ctx& c) {
  return TickEngine{c.ctx,  c.world, c.sim,  c.stream, c.phys,
                    c.mobs, c.debris, c.mats, c.items};
}
}  // namespace

TickRig::TickRig(selftest::Ctx& c, uint32_t lastTick, IVec3 fixtureChunk)
    : TickRig(EngineOf(c), lastTick, fixtureChunk) {}

TickRig::TickRig(const TickEngine& c, uint32_t lastTick, IVec3 fixtureChunk)
    : e_(c) {
  tick = lastTick;
  classOf_ = BuildCollisionClasses(c.mats);
  // THE HARNESS BODY: a camera, not a creature. Fly mode takes it out of every
  // collision and gravity rule, and with `fly` the tick's own avatar block
  // (phase I: `wantAvatar = enabled && !player.fly`) never spawns a rig, so
  // nothing a gate measures can be touched, targeted or crowded by it. It
  // stands at the fixture so the questions the tick asks of "the player"
  // (the far-plume eye, the laser ray it will never fire) have a sensible
  // answer.
  session_ = std::make_unique<PlayerSession>();
  PlayerSession& s = *session_;
  s.index = 0;
  s.localView = true;
  s.player.fly = true;
  s.player.pos = Vec3{(float)(fixtureChunk.x * (int)kChunk) + 8.0f,
                      (float)(fixtureChunk.y * (int)kChunk) + 8.0f,
                      (float)(fixtureChunk.z * (int)kChunk) + 8.0f};
  World* world = &c.world;
  const std::vector<uint32_t>* classOf = &classOf_;
  s.kindAt = [world, classOf](IVec3 cell) {
    if (world->FluidEighthsAt(cell) >= 2) return CellKind::Liquid;
    return world->KindAt(cell, *classOf);
  };
  // Nobody is watching: the HUD's "is the player alive" readout, which phase H
  // hands the NPC targeting layer when the rig lets it, says the harness body
  // is not a live target.
  ui_.playerAlive = false;

  MicroBodySet& mb = c.mobs.MicroSet() != nullptr ? *c.mobs.MicroSet() : ownMb_;
  w_.reset(new TickAuthorityCtx{
      .ctx = c.ctx,
      .world = c.world,
      .sim = c.sim,
      .stream = c.stream,
      .far = far_,
      .phys = c.phys,
      .mobs = c.mobs,
      .debris = c.debris,
      .mbSet = mb,
      .mats = c.mats,
      .glyphs = glyphs_,
      .items = c.items,
      .prefabs = prefabs_,
      .classOf = classOf_,
      .ui = ui_,
      .hitStop = hitStop_,
      .deathBody = deathBody_,
      .deathFrozen = deathFrozen_,
      .liveTimed = liveTimed_,
      .liveTimer = liveTimer_,
      .liveSample = liveSample_,
      .liveFrameNo = liveFrameNo_,
      .tickMsSmooth = tickMsSmooth_,
      .farEntries = farEntries_,
      .farTicks = farTicks_,
      .farBiggest = farBiggest_,
  });
  w_->harness.pinWindow = true;
  w_->harness.ownsActors = true;
  SetFixtureChunk(fixtureChunk);
  // The gate's own ops for the tick about to run, handed over at its top.
  w_->harness.inject = [this](uint32_t, OpBatch& out) {
    out.ops.insert(out.ops.end(), pending_.ops.begin(), pending_.ops.end());
    out.cells.insert(out.cells.end(), pending_.cells.begin(),
                     pending_.cells.end());
    out.spawns.insert(out.spawns.end(), pending_.spawns.begin(),
                      pending_.spawns.end());
  };
}

TickRig::~TickRig() = default;

void TickRig::SetFixtureChunk(IVec3 chunk) {
  w_->harness.haveSubmitChunk = true;
  w_->harness.submitChunk = chunk;
}

void RunTicks(TickRig& rig, int n,
              const std::function<void(uint32_t tick, TickOps& ops)>& ops) {
  const TickEngine& c = rig.e_;
  for (int i = 0; i < n; i++) {
    const uint32_t t = rig.tick + 1;
    rig.pending_ = TickOps{};
    if (ops) ops(t, rig.pending_);
    // Explosions go off in the primary's blast slot (phase K), not with the
    // other ops: see TickAuthorityCtx::Harness::blasts.
    rig.w_->harness.blasts = rig.pending_.exps;
    // An empty command: the harness body is not being played.
    const TickInput ti{};
    const FrameIntent intent{};
    TickAuthority(*rig.w_, *rig.session_, intent, ti, t, rig.batch_);
    rig.tick = t;
    rig.w_->harness.blasts.clear();
    // THE PUMP THE FRAME LAYER OWNS (session.h: "the frame loop keeps only
    // pacing, the readback pump, the park probe"), at harness pacing: every
    // hand-rolled gate ticker drained the device after its submit, and the
    // gates read the world through blocking reads either side of a tick.
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
  }
}

TickRig& TickCursor::Rig() {
  if (!rig) rig = std::make_unique<TickRig>(c, t, chunk);
  return *rig;
}

void TickCursor::operator()(const TickOps& ops) {
  TickRig& r = Rig();
  r.tick = t;              // the gate may have moved its own clock
  r.SetFixtureChunk(chunk);
  if (ops.ops.empty() && ops.exps.empty() && ops.cells.empty() &&
      ops.spawns.empty()) {
    RunTicks(r, 1);
  } else {
    RunTicks(r, 1, [&ops](uint32_t, TickOps& out) { out = ops; });
  }
  t = r.tick;
}

void TickCursor::operator()(const std::vector<BrushOp>& brush,
                            const std::vector<CellOp>& cells) {
  TickOps o;
  o.ops = brush;
  o.cells = cells;
  (*this)(o);
}

}  // namespace sandvox::support
