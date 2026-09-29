// selftest_refs.cpp — REFERENCES (docs/PLAN_world_editor.md P1, world/refs.h).
//
//   refs-roundtrip     CPU. The group file is a file a person edits: the
//                      fixture on disk is canonical, parse -> write -> parse
//                      -> write is byte-identical, unknown props / fields /
//                      kinds survive, the four authoring actions write the
//                      file and inverting them restores it byte for byte,
//                      bad input is refused naming file, id and field.
//   refs-activate      Activation follows the window in ID order (twice, and
//                      the two logs agree), Retry is retried, an edit
//                      re-applies and a delete undoes -- then THE USE VERB
//                      through the real tick: a TB_USE press carrying a ref's
//                      hash runs its onUse; out of reach / not active is
//                      refused and recorded.
//   door-cycle         P6. A door opens into one hinged body (cells empty,
//                      swung toward its yaw), a blocked close waits, the
//                      close writes the exact words back, and an open door
//                      survives save + load.
//   container-persist  P6, CPU. Take / put / a full pack refusing, and the
//                      contents exact through the REFS save records.
//   refs-npc-identity  An NPC spawned from a ref carries Mob::RefId through a
//                      save + load and through park + unpark, the ref never
//                      spawns a second body, a duplicate record is refused,
//                      and a saved delta whose id the map no longer has is
//                      dropped with a warning that names it.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "game/mob.h"
#include "game/persist.h"
#include "game/session.h"
#include "sim/bytestream.h"
#include "sim/chunkstore.h"
#include "sim/worldgen_run.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "world/refs.h"
#include "world/refs_game.h"
#include "world/refs_doors.h"
#include "game/equipment.h"
#include "phys/debris.h"
#include "phys/physics.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;

// LINE ENDINGS ARE NOT CONTENT. The writer writes "\n"; a checkout with
// core.autocrlf=true hands the fixture back with "\r\n" (there is no
// .gitattributes pinning it), and a hand edit in a Windows editor may do the
// same. Every byte comparison here is over the LF form.
std::string ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

bool Contains(const std::vector<std::string>& v, const char* a, const char* b = nullptr) {
  for (const std::string& s : v)
    if (s.find(a) != std::string::npos && (b == nullptr || s.find(b) != std::string::npos))
      return true;
  return false;
}

// ---- the gate's probe kind ------------------------------------------------
//
// Records every hook call, in order, so a gate can assert the ORDER the store
// ran them in. `retry_left` makes activation answer Retry that many times
// first (per id), to exercise the Retry path.
struct ProbeLog {
  std::vector<std::string> events;   // "on:<id>", "off:<id>:<why>", "use:<id>"
  std::map<std::string, int> retryLeft;
};
ProbeLog& Probe() {
  static ProbeLog p;
  return p;
}

void RegisterProbeKind() {
  refs::RegisterAllKinds();
  refs::RefKind k;
  k.name = "selftest_probe";
  k.activate = [](refs::RefCtx&, const refs::Ref& r) {
    int& left = Probe().retryLeft[r.id];
    if (left > 0) {
      left--;
      Probe().events.push_back("retry:" + r.id);
      return refs::Activation::Retry;
    }
    Probe().events.push_back("on:" + r.id);
    return refs::Activation::Done;
  };
  k.deactivate = [](refs::RefCtx&, const refs::Ref& r, refs::RefEvent why) {
    Probe().events.push_back("off:" + r.id + ":" + refs::RefEventName(why));
  };
  k.usePrompt = [](refs::RefCtx&, const refs::Ref& r) {
    if (r.props.contains("prompt") && r.props["prompt"].is_string())
      return r.props["prompt"].get<std::string>();
    return std::string("Poke ") + r.id;
  };
  k.onUse = [](refs::RefCtx&, const refs::Ref& r, refs::RefUse& u) {
    Probe().events.push_back("use:" + r.id);
    u.message = "poked " + r.id;
  };
  k.useRadius = 5.0f;
  refs::Kinds().Register(std::move(k));
}

std::string FixturePath() { return AssetDir() + "/worldmap/harness/refs/fixture.json"; }

// A scratch map dir holding a COPY of the fixture: edits never touch assets/.
std::string ScratchRoot(const char* name) {
  const std::string root = std::string("build/selftest_refs_") + name;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root + "/worldmap/rt/refs", ec);
  fs::copy_file(FixturePath(), root + "/worldmap/rt/refs/fixture.json",
                fs::copy_options::overwrite_existing, ec);
  return root;
}

// ---- refs-roundtrip -----------------------------------------------------------
Status GateRefsRoundtrip(Ctx&, std::string& detail) {
  RegisterProbeKind();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) why.push_back(what);
    return ok;
  };

  // A. THE FIXTURE ON DISK IS CANONICAL, AND THE ROUND TRIP IS EXACT.
  const std::string text = ReadAll(FixturePath());
  refs::RefGroupFile g1, g2;
  std::vector<std::string> w1, w2;
  const bool p1 = refs::ParseGroup(text, "refs/fixture.json", g1, w1);
  const std::string out1 = refs::WriteGroup(g1);
  const bool p2 = refs::ParseGroup(out1, "refs/fixture.json", g2, w2);
  const std::string out2 = refs::WriteGroup(g2);
  check(p1 && p2, "fixture did not parse");
  check(w1.empty(), "fixture parse warned: " + (w1.empty() ? std::string() : w1[0]));
  const bool canonical = check(out1 == text, "fixture on disk is not in WriteGroup's form");
  const bool stable = check(out2 == out1, "parse->write->parse->write moved bytes");

  // B. UNKNOWN THINGS SURVIVE: an unknown kind, an unknown top-level field,
  //    an unknown nested prop, and the author's row order.
  bool keptUnknown = false, order = false, extraTop = false;
  for (const refs::Ref& r : g2.refs)
    if (r.id == "fixture/mystery")
      keptUnknown = r.kind == "teapot" && r.extra.contains("note") &&
                    r.props.contains("zz_unknown") &&
                    r.props["zz_unknown"]["nested"].dump() == "[1,2.5,\"x\",null,true]" &&
                    r.yaw == 45;
  order = g2.refs.size() == 6 && g2.refs[0].id == "fixture/villager" &&
          g2.refs[1].id == "fixture/probe_b";
  extraTop = g2.extra.contains("about");
  check(keptUnknown, "unknown kind/field/prop not kept verbatim");
  check(order, "author row order not kept");
  check(extraTop, "unknown top-level field not kept");

  // C. BAD INPUT NAMES FILE, ID AND FIELD, and skips only the bad row.
  {
    refs::RefGroupFile gb;
    std::vector<std::string> wb;
    const std::string bad =
        "{ \"group\": \"bad\", \"refs\": [ { \"id\": \"bad/nopos\", \"kind\": \"marker\" },"
        " { \"id\": \"Bad Id\", \"kind\": \"marker\", \"pos\": [0,0,0] },"
        " { \"id\": \"bad/ok\", \"kind\": \"marker\", \"pos\": [1,2,3], \"yaw\": 12.6 } ] }";
    refs::ParseGroup(bad, "refs/bad.json", gb, wb);
    check(Contains(wb, "refs/bad.json: bad/nopos: pos"), "missing pos not named");
    check(Contains(wb, "Bad Id", "id:"), "bad id not named");
    check(Contains(wb, "bad/ok: yaw"), "fractional yaw not named");
    check(gb.refs.size() == 1 && gb.refs[0].yaw == 13, "bad rows not skipped / yaw not rounded");
    refs::RefGroupFile gj;
    std::vector<std::string> wj;
    check(!refs::ParseGroup("{ \"group\": ", "refs/broken.json", gj, wj) &&
              Contains(wj, "refs/broken.json", "not valid JSON"),
          "malformed JSON not refused by name");
  }

  // D. THE STORE: load a scratch copy; the unknown kind is a WARNING naming
  //    the file, the id and the field -- nothing else warns.
  const std::string root = ScratchRoot("rt");
  refs::RefStore st;
  st.LoadMap(root, "rt");
  const std::string path = st.GroupPath("fixture");
  const std::string before = ReadAll(path);
  check(st.All().size() == 6, std::to_string(st.All().size()) + " refs loaded, want 6");
  check(st.Warnings().size() == 1 &&
            Contains(st.Warnings(), "refs/fixture.json: fixture/mystery: kind:"),
        "want exactly one warning (the teapot), got " + std::to_string(st.Warnings().size()) +
            (st.Warnings().empty() ? "" : ": " + st.Warnings()[0]));

  // E. THE FOUR AUTHORING ACTIONS write the file; their inverses restore it.
  std::string err;
  bool acts = true;
  refs::Ref nw;
  nw.id = "fixture/new_well";
  nw.kind = "marker";
  nw.pos = {1, 2, 3};
  nw.props["tags"] = refs::Json::array({"gather"});
  acts &= refs::Place(st, nw, &err);
  const bool placedInFile = ReadAll(path).find("\"fixture/new_well\"") != std::string::npos;
  acts &= refs::Move(st, "fixture/villager", {300, 190, 300}, 90, &err);
  acts &= refs::SetProp(st, "fixture/villager", "name", refs::Json("Fred II"), &err);
  acts &= refs::SetProp(st, "fixture/villager", "schedule", refs::Json(), &err);   // remove
  acts &= refs::SetField(st, "fixture/villager", "base", "dummy", &err);
  refs::Ref gone;
  int goneRow = -1;
  acts &= refs::Delete(st, "fixture/probe_b", &err, &gone, &goneRow);
  const std::string edited = ReadAll(path);
  bool editsVisible = edited.find("[300, 190, 300], \"yaw\": 90") != std::string::npos &&
                      edited.find("Fred II") != std::string::npos &&
                      edited.find("\"schedule\"") == std::string::npos &&
                      edited.find("\"base\": \"dummy\"") != std::string::npos &&
                      edited.find("fixture/probe_b") == std::string::npos &&
                      edited.find("zz_unknown") != std::string::npos;
  // Refusals leave the file alone.
  refs::Ref dup = nw;
  const bool refusedDup = !refs::Place(st, dup, &err);
  refs::Ref badId;
  badId.id = "Fixture/Bad";
  badId.kind = "marker";
  const bool refusedBad = !refs::Place(st, badId, &err) && err.find("lowercase") != std::string::npos;
  const bool refusedField = !refs::SetField(st, "fixture/villager", "pos", "1", &err);
  const bool untouched = ReadAll(path) == edited;
  // ...and the inverses, newest first -- P5's undo, as data.
  acts &= refs::Place(st, gone, &err, goneRow);
  acts &= refs::SetField(st, "fixture/villager", "base", "human", &err);
  acts &= refs::SetProp(st, "fixture/villager", "schedule", refs::Json("none"), &err);
  acts &= refs::SetProp(st, "fixture/villager", "name", refs::Json("Fixture Fred"), &err);
  acts &= refs::Move(st, "fixture/villager", {256, 181, 256}, 180, &err);
  acts &= refs::Delete(st, "fixture/new_well", &err);
  const std::string after = ReadAll(path);
  // Removing `schedule` and setting it again puts it back after `name`,
  // which is where it was, so the whole file comes back byte for byte.
  const bool restored = after == before;
  // The rename-free restore: a Move + its inverse alone is byte-identical.
  refs::Move(st, "fixture/probe_a", {1, 1, 1}, 0, &err);
  refs::Move(st, "fixture/probe_a", {248, 170, 236}, 0, &err);
  const bool moveExact = ReadAll(path) == after;
  check(acts, "an authoring action failed: " + err);
  check(placedInFile && editsVisible, "edits not visible in the file");
  check(refusedDup && refusedBad && refusedField && untouched, "a refusal wrote the file");
  check(restored && moveExact, "inverse actions did not restore the file");

  // F. A DELTA RECORD round-trips; the wrong version is refused.
  refs::RefDelta d{"npc", 1, {1, 0, 0, 0}};
  std::vector<uint8_t> rec;
  refs::RefStore::EncodeDelta("fixture/villager", d, rec);
  std::string id2;
  refs::RefDelta d2;
  const bool deltaOk = refs::RefStore::DecodeDelta(rec.data(), rec.size(), id2, d2) &&
                       id2 == "fixture/villager" && d2.kind == "npc" && d2.bytes == d.bytes;
  rec[0] = 99;
  const bool deltaVer = !refs::RefStore::DecodeDelta(rec.data(), rec.size(), id2, d2);
  check(deltaOk && deltaVer, "delta record encode/decode");

  std::error_code ec;
  fs::remove_all(root, ec);
  detail = Format(
      "A canonical %d stable %d | B unknown kept %d, order %d, top field %d | C bad input "
      "named | D %zu refs, %zu warning(s) | E actions %d, visible %d, refusals clean %d, "
      "inverse restores %d, move exact %d | F delta %d",
      canonical ? 1 : 0, stable ? 1 : 0, keptUnknown ? 1 : 0, order ? 1 : 0, extraTop ? 1 : 0,
      st.All().size(), st.Warnings().size(), acts ? 1 : 0, editsVisible ? 1 : 0,
      (refusedDup && refusedBad && refusedField && untouched) ? 1 : 0, restored ? 1 : 0,
      moveExact ? 1 : 0, (deltaOk && deltaVer) ? 1 : 0);
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

// ---- refs-activate ------------------------------------------------------------
//
// Part 1 drives RefStore::Update with window origins directly (the store's
// contract is "given this window"), no GPU: activation, deactivation, order,
// Retry, edits. Part 2 is the use verb through THE tick (support::RunTicks):
// the harness body presses TB_USE with a ref's hash in TickInput::useRef.
Status GateRefsActivate(Ctx& c, std::string& detail) {
  RegisterProbeKind();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) why.push_back(what);
    return ok;
  };
  const IVec3 home{0, 0, 0};
  const IVec3 away{100, 0, 100};
  const IVec3 far{4000 / 16 - 16, 0, 4000 / 16 - 16};

  // One scripted run of the store, returning its activity log as text.
  auto run = [&](refs::RefStore& st, std::string& log, int retries) {
    Probe().events.clear();
    Probe().retryLeft.clear();
    Probe().retryLeft["fixture/probe_b"] = retries;
    refs::RefCtx ctx{&st};
    uint32_t tick = 1;
    auto upd = [&](IVec3 o) {
      ctx.tick = tick++;
      st.Update(ctx, o);
    };
    upd(home);
    for (int i = 0; i < retries; i++) upd(home);   // Retry is asked again
    upd(away);
    upd(home);
    for (int i = 0; i < retries; i++) upd(home);
    upd(far);
    for (const refs::RefActivity& a : st.Log())
      log += (a.on ? "+" : "-") + a.id + (a.on ? "" : std::string(":") + refs::RefEventName(a.why)) + " ";
    log += "| ";
    for (const std::string& e : Probe().events) log += e + " ";
  };
  refs::RefStore s1, s2;
  s1.LoadMap(AssetDir(), "harness");
  s2.LoadMap(AssetDir(), "harness");
  std::string log1, log2;
  run(s1, log1, 2);
  run(s2, log2, 2);
  // Activation in ID order at home: green_well, mystery, probe_a, (probe_b
  // retries twice, then on), villager. Away: all off in id order. Far: only
  // far_marker.
  const std::string wantHome =
      "+fixture/green_well +fixture/mystery +fixture/probe_a +fixture/villager +fixture/probe_b ";
  const bool orderOk = log1.rfind(wantHome, 0) == 0;
  const bool offOrder =
      log1.find("-fixture/green_well:window-left -fixture/mystery:window-left "
                "-fixture/probe_a:window-left -fixture/probe_b:window-left "
                "-fixture/villager:window-left ") != std::string::npos;
  const bool farOk = s1.IsActive("fixture/far_marker") && s1.ActiveCount() == 1;
  const bool retried = log1.find("retry:fixture/probe_b retry:fixture/probe_b on:fixture/probe_b") !=
                       std::string::npos;
  check(orderOk, "activation not in id order: " + log1.substr(0, 160));
  check(offOrder, "deactivation not in id order");
  check(farOk, "far window: want only far_marker active, have " + std::to_string(s1.ActiveCount()));
  check(retried, "Retry was not retried");
  check(log1 == log2, "two runs disagree");

  // Edits re-apply (Edited off, then on) and a delete undoes (Deleted).
  const std::string root = ScratchRoot("act");
  refs::RefStore se;
  se.LoadMap(root, "rt");
  Probe().events.clear();
  Probe().retryLeft.clear();
  refs::RefCtx ectx{&se};
  se.Update(ectx, home);
  std::string err;
  refs::Move(se, "fixture/probe_a", {250, 170, 236}, 0, &err);
  refs::Delete(se, "fixture/probe_b", &err);
  Probe().events.clear();
  ectx.tick = 2;
  se.Update(ectx, home);
  std::string elog;
  for (const std::string& e : Probe().events) elog += e + " ";
  const bool editOk = elog == "off:fixture/probe_b:deleted off:fixture/probe_a:edited on:fixture/probe_a ";
  check(editOk, "edit/delete re-apply: " + elog);
  std::error_code ec;
  fs::remove_all(root, ec);

  // ---- Part 2: the use verb through the real tick -------------------------
  const IVec3 savedOrigin = c.world.WindowOrigin();
  c.debris.Reset();
  c.mobs.Reset();
  c.world.SetWindowOrigin(home);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  refs::RefStore su;
  su.LoadMap(AssetDir(), "harness");
  Probe().retryLeft.clear();
  uint32_t t = 9000;
  support::TickRig rig(c, t, IVec3{15, 10, 15});
  rig.Authority().refs = &su;
  const refs::Ref* pa = su.Find("fixture/probe_a");
  const refs::Ref* fm = su.Find("fixture/far_marker");
  PlayerSession& ps = rig.Session();
  ps.player.pos = Vec3{(float)pa->pos.x + 0.5f, (float)pa->pos.y + 2.0f,
                       (float)pa->pos.z - 12.0f};
  ps.player.vel = Vec3{0, 0, 0};
  support::RunTicks(rig, 2);
  const bool activeInTick = su.IsActive("fixture/probe_a");
  // The PROMPT: a ray from the eye at the probe finds it and says its text.
  refs::RefCtx pctx{&su, &c.mobs, &c.world, &c.stream.Store(), nullptr, rig.tick};
  const Vec3 eye = ps.player.EyePos();
  Vec3 dir = Vec3{(float)pa->pos.x + 0.5f, (float)pa->pos.y + 0.5f, (float)pa->pos.z + 0.5f} - eye;
  const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  dir = dir * (1.0f / dl);
  std::string prompt;
  const refs::Ref* picked = refs::PickUsable(su, pctx, eye, dir, eye, &prompt);
  const bool promptOk = picked == pa && prompt == "Poke probe A";
  // A press on it; a press on something not active; a press from too far.
  auto press = [&](uint32_t h) {
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.input.SetPressed(TB_USE, true);
      o.input.useRef = h;
    });
  };
  su.ClearUses();
  Probe().events.clear();
  press(pa->hash);
  press(fm->hash);
  ps.player.pos = Vec3{(float)pa->pos.x + 200.0f, (float)pa->pos.y + 2.0f, (float)pa->pos.z};
  ps.player.vel = Vec3{0, 0, 0};
  press(pa->hash);
  press(0x12345678u);
  const std::vector<refs::UseRecord>& u = su.Uses();
  const bool usedOk = u.size() == 4 && u[0].used && u[0].id == "fixture/probe_a" &&
                      u[0].message == "poked fixture/probe_a" && !u[1].used &&
                      u[1].message.find("not active") != std::string::npos && !u[2].used &&
                      u[2].message.find("out of reach") != std::string::npos && !u[3].used &&
                      Probe().events.size() == 1;
  check(activeInTick, "the tick did not activate the fixture");
  check(promptOk, "PickUsable: prompt '" + prompt + "'");
  check(usedOk, "use verb: " + (u.empty() ? std::string("no records") : u.back().message));

  c.debris.Reset();
  c.mobs.Reset();
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = Format(
      "order %d, off order %d, far %d, retry %d, twice-run same %d, edit/delete %d | tick "
      "active %d, prompt %d, use/refusals %d (%zu records)",
      orderOk ? 1 : 0, offOrder ? 1 : 0, farOk ? 1 : 0, retried ? 1 : 0, log1 == log2 ? 1 : 0,
      editOk ? 1 : 0, activeInTick ? 1 : 0, promptOk ? 1 : 0, usedOk ? 1 : 0, u.size());
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

// ---- refs-npc-identity --------------------------------------------------------
Status GateRefsNpcIdentity(Ctx& c, std::string& detail) {
  RegisterProbeKind();
  const char* kPath = "selftest_refs_npc.svd";
  const char* kId = "fixture/villager";
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) why.push_back(what);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint64_t idCounterWas = c.mobs.NextIdCounter();
  MobParking parking;
  auto countRef = [&]() {
    int n = 0;
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++)
      if (const Mob* m = c.mobs.MobAt(i); m && m->RefId() == kId) n++;
    return n;
  };
  c.stream.Store().Clear();
  std::error_code ec;
  fs::remove_all(kPath, ec);
  c.debris.Reset();
  c.mobs.Reset();
  const IVec3 home{0, 0, 0};
  c.world.SetWindowOrigin(home);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  parking.Bind(c.mobs, c.stream.Store(), true);

  refs::RefStore st;
  st.LoadMap(AssetDir(), "harness");
  st.BindChunkStore(&c.stream.Store());
  uint32_t t = 12000;
  auto rig = std::make_unique<support::TickRig>(c, t, IVec3{16, 11, 16});
  rig->Authority().refs = &st;
  auto ticks = [&](int n) {
    for (int i = 0; i < n; i++) {
      support::RunTicks(*rig, 1);
      parking.Unpark(c.mobs, c.stream.Store(), c.world, rig->tick);
    }
  };

  // A. THE REF SPAWNS ITS VILLAGER, ONCE, stamped with the ref id.
  int waited = 0;
  while (countRef() == 0 && waited < 90) {
    ticks(1);
    waited++;
  }
  ticks(20);
  const int liveA = countRef();
  const refs::RefDelta* dA = st.Delta(kId);
  const bool okA = check(liveA == 1 && dA != nullptr && dA->kind == "npc",
                         "A: villager count " + std::to_string(liveA) + " after " +
                             std::to_string(waited) + " ticks");
  const uint64_t idA = c.mobs.FindMobByRef(kId) ? c.mobs.FindMobByRef(kId)->Id() : 0;

  // B. SAVE + LOAD: the same villager, by ref, not by mob id.
  EntityIO eio = MakeEntityIO(c.debris, c.mobs, nullptr, nullptr, nullptr, &st);
  const bool saved = SaveWorld(c.ctx, c.world, c.stream, kPath, c.mats, &eio);
  c.mobs.Reset();
  EntityFileReport lr;
  const bool loaded =
      LoadWorld(c.ctx, c.world, c.sim, c.stream, kPath, c.mats, &eio, nullptr, &lr);
  const int liveB0 = countRef();
  const bool deltaBack = st.Delta(kId) != nullptr;
  t = rig->tick;
  rig = std::make_unique<support::TickRig>(c, t, IVec3{16, 11, 16});
  rig->Authority().refs = &st;
  ticks(15);
  const int liveB = countRef();
  const Mob* mB = c.mobs.FindMobByRef(kId);
  const bool okB = check(saved && loaded && liveB0 == 1 && deltaBack && liveB == 1 && mB != nullptr,
                         Format("B: saved %d loaded %d live %d->%d delta %d", saved, loaded,
                                liveB0, liveB, deltaBack));
  const uint64_t idB = mB ? mB->Id() : 0;

  // C. PARK + UNPARK: teleport away (it parks, the ref goes dormant), come
  //    back (the ref activates and must NOT spawn; the unpark brings it back).
  auto teleport = [&](IVec3 origin) {
    c.world.InvalidateSnapshot();
    c.stream.ReloadWindow(origin);
    rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
    c.sim.EncodeLoadReset(enc);
    rhi::CommandBuffer cmd = enc.Finish();
    c.ctx.queue.Submit(1, &cmd);
    c.ctx.WaitIdle();
  };
  const int n = (int)kNChunk;
  const IVec3 away{home.x + n + 8, home.y, home.z + n + 8};
  teleport(away);
  t = rig->tick;
  rig = std::make_unique<support::TickRig>(c, t, IVec3{away.x + n / 2, 11, away.z + n / 2});
  rig->Authority().refs = &st;
  ticks(3);
  const int liveAway = countRef();
  const bool dormant = !st.IsActive(kId);
  const uint64_t parkedNow = parking.GetStats().parked;
  teleport(home);
  parking.ResetWaits();
  t = rig->tick;
  rig = std::make_unique<support::TickRig>(c, t, IVec3{16, 11, 16});
  rig->Authority().refs = &st;
  int back = 0;
  while (countRef() == 0 && back < 240) {
    ticks(1);
    back++;
  }
  ticks(10);
  const int liveC = countRef();
  const bool activeAgain = st.IsActive(kId);
  check(liveAway == 0 && dormant && parkedNow >= 1,
        Format("C: away live %d dormant %d parked %llu", liveAway, dormant,
               (unsigned long long)parkedNow));
  check(liveC == 1 && activeAgain,
        Format("C: back live %d after %d ticks, active %d", liveC, back, activeAgain));
  const bool okC = liveAway == 0 && dormant && parkedNow >= 1 && liveC == 1 && activeAgain;

  // D. ONE REF, ONE BODY: a copy of the villager's record is refused.
  bool okD = false;
  if (const Mob* m = c.mobs.FindMobByRef(kId)) {
    std::vector<uint8_t> b;
    ByteWriter w{b};
    m->SaveOne(w);
    ByteReader rd{b.data(), b.size()};
    bool refused = false;
    okD = c.mobs.LoadOne(rd, MobSystem::kSaveVersion, true, &refused) == nullptr && !refused &&
          countRef() == 1;
  }
  check(okD, "D: a duplicate record was not refused");

  // E. A SAVED DELTA FOR AN ID THE MAP NO LONGER HAS: dropped, and said so.
  bool okE = false;
  for (const EntitySection& sec : eio.sections) {
    if (!sec.loadRecord || sec.id != ('R' | ('E' << 8) | ('F' << 16) | ((uint32_t)'S' << 24)))
      continue;
    std::vector<uint8_t> rec;
    refs::RefStore::EncodeDelta("fixture/gone_since_the_save", refs::RefDelta{"npc", 1, {1, 0, 0, 0}},
                                rec);
    const RecordLoad r = sec.loadRecord(rec.data(), rec.size(), refs::RefStore::kSaveVersion);
    okE = r == RecordLoad::Dropped && Contains(st.Warnings(), "fixture/gone_since_the_save", "dropped");
  }
  check(okE, "E: missing-id delta not dropped with a warning");

  // Leave nothing behind.
  rig.reset();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetParkFn(nullptr);
  c.mobs.ClearPlayerActors();
  c.mobs.SetNextIdCounter(idCounterWas);
  c.stream.Store().Clear();
  fs::remove_all(kPath, ec);
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = Format(
      "A %s: 1 villager after %d ticks, delta set | B %s: save+load keeps ref (mob id %llu -> "
      "%llu), applied %u records | C %s: parked %llu, back after %d ticks, still one | D %s: "
      "duplicate record refused | E %s: missing-id delta dropped + warned",
      okA ? "ok" : "FAIL", waited, okB ? "ok" : "FAIL", (unsigned long long)idA,
      (unsigned long long)idB, lr.recordsApplied, okC ? "ok" : "FAIL",
      (unsigned long long)parkedNow, back, okD ? "ok" : "FAIL", okE ? "ok" : "FAIL");
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}


// ---- P6: door-cycle, container-persist ---------------------------------------
//
// A scratch map dir with NO fixture copied in: these gates place their own
// refs through refs::Place (the authoring path), so the refs-* fixture's
// activation-order strings are not disturbed.
std::string EmptyScratch(const char* name, const char* map) {
  const std::string root = std::string("build/selftest_refs_") + name;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root + "/worldmap/" + map + "/refs", ec);
  return root;
}

uint32_t MatId(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

// door-cycle — a doorway built from ops, a `door` ref placed on its leaf:
//   A. use (TB_USE through the real tick) -> the leaf becomes ONE hinged body,
//      its cells go empty, it swings TOWARD the yaw to the open angle;
//   B. a stone set in the doorway while open -> a use closes the swing but
//      the write-back WAITS (Settling, body kept); clearing it -> closed, body
//      gone, the box's words byte-identical to before the first open;
//   C. open again, save + load: the door comes back open (body re-hung from
//      the delta at the open angle, cells still empty), and closes exact.
Status GateDoorCycle(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) why.push_back(what);
    return ok;
  };
  const char* kPath = "selftest_door_cycle.svd";
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint64_t idCounterWas = c.mobs.NextIdCounter();
  std::error_code ec;
  fs::remove_all(kPath, ec);
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  const IVec3 home{0, 0, 0};
  c.world.SetWindowOrigin(home);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  // ---- the doorway: yaw 0 (opens toward +Z), hinge left (leaf runs +X) ----
  const int X0 = 152, Z0 = 152, W = 5, H = 8;
  const int G = FixtureYOver(X0 - 6, Z0 - 4, X0 + W + 6, Z0 + 10, kDefaultSeed, 0);
  const int Y0 = G + 1;
  const uint32_t stone = MatId(c, "stone"), wood = MatId(c, "wood");
  std::vector<CellOp> build;
  auto put = [&](int x, int y, int z, uint32_t m, uint32_t state) {
    build.push_back({World::SlotCellIndex({x, y, z}), m == 0 ? 0u : PackVoxNew(m, state)});
  };
  // The WALL FIRST: two cell ops on one cell in one tick keep the FIRST
  // (CLAUDE.md rule 3), and the clearing pass below covers the wall's cells.
  for (int x = X0 - 6; x <= X0 + W + 6; x++)
    for (int y = Y0; y <= Y0 + H; y++) {
      const bool doorway = x >= X0 && x < X0 + W && y < Y0 + H;
      if (doorway) put(x, y, Z0, wood, (uint32_t)((x + y) % 3));          // the leaf
      else put(x, y, Z0, stone, 0);                                       // wall + lintel
    }
  for (int x = X0 - 6; x <= X0 + W + 6; x++)
    for (int z = Z0 - 4; z <= Z0 + 10; z++) {
      for (int y = G - 6; y <= G; y++) put(x, y, z, stone, 0);           // the floor slab
      for (int y = Y0; y <= Y0 + H + 4; y++) put(x, y, z, 0, 0);         // clear air above
    }

  const std::string root = EmptyScratch("door", "door");
  refs::RefStore st;
  st.LoadMap(root, "door");
  refs::Ref dr;
  dr.id = "doorfix/front";
  dr.kind = "door";
  dr.pos = {X0, Y0, Z0};
  dr.yaw = 0;
  dr.props = refs::Json{{"width", W}, {"height", H}, {"hinge", "left"}};
  std::string err;
  check(refs::Place(st, dr, &err), "place: " + err);
  st.BindChunkStore(&c.stream.Store());
  const refs::DoorGeom g = refs::DoorGeometry(*st.Find(dr.id));
  check(g.ok, "geometry: " + g.why);
  const std::vector<IVec3> box = g.Cells();

  uint32_t t = 14000;
  const IVec3 fixChunk{X0 >> 4, Y0 >> 4, Z0 >> 4};
  auto rig = std::make_unique<support::TickRig>(c, t, fixChunk);
  rig->Authority().refs = &st;
  PlayerSession& ps = rig->Session();
  auto standInFront = [&]() {
    ps.player.pos = Vec3{(float)X0 + 2.5f, (float)Y0 + 2.0f, (float)Z0 + 16.0f};
    ps.player.vel = Vec3{0, 0, 0};
  };
  standInFront();
  support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = build; });
  support::RunTicks(*rig, 8);

  // The box's words through the fetch cache, no older than now.
  auto boxWords = [&](std::vector<uint32_t>& out) {
    const uint32_t since = rig->tick;
    for (int i = 0; i < 40; i++) {
      bool fresh = true;
      for (const IVec3& cell : box) {
        const IVec3 wc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
        const CachedChunk* cc = c.world.Cached(wc);
        if (cc == nullptr || cc->voxels.size() != kChunkVol || cc->version < since) {
          fresh = false;
          c.world.RequestChunkFetch(wc);
        }
      }
      if (fresh) break;
      support::RunTicks(*rig, 1);
    }
    out.clear();
    for (const IVec3& cell : box) {
      const CachedChunk* cc = c.world.Cached({cell.x >> 4, cell.y >> 4, cell.z >> 4});
      const uint32_t w = cc ? cc->voxels[(size_t)(((cell.z & 15) * 16 + (cell.y & 15)) * 16 +
                                                   (cell.x & 15))]
                            : 0xFFFFFFFFu;
      out.push_back(w & 0x7F00FFFFu);
    }
  };
  auto allAir = [](const std::vector<uint32_t>& w) {
    for (uint32_t v : w)
      if ((v & 0xFFFu) != 0) return false;
    return true;
  };
  auto press = [&]() {
    standInFront();
    support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) {
      o.input.SetPressed(TB_USE, true);
      o.input.useRef = st.Find(dr.id)->hash;
    });
  };
  auto waitPhase = [&](refs::DoorPhase want, int maxTicks) {
    refs::DoorStatus s;
    for (int i = 0; i < maxTicks; i++) {
      if (refs::DoorStatusOf(dr.id, s) && s.phase == want) return i;
      support::RunTicks(*rig, 1);
    }
    return -1;
  };

  std::vector<uint32_t> before;
  boxWords(before);
  int leafCount = 0;
  for (uint32_t w : before) leafCount += (w & 0xFFFu) == wood ? 1 : 0;
  check(leafCount == W * H, Format("fixture: %d wood cells in the box, want %d", leafCount, W * H));
  const bool activeOk = check(st.IsActive(dr.id), "the door ref never activated");

  // A. OPEN.
  press();
  const int openAfter = waitPhase(refs::DoorPhase::Open, 60);
  support::RunTicks(*rig, 45);   // let it swing
  refs::DoorStatus sA;
  refs::DoorStatusOf(dr.id, sA);
  std::vector<uint32_t> openWords;
  boxWords(openWords);
  Vec3 com{};
  const bool haveBody = sA.body != 0 && c.debris.HasBody(sA.body) && c.phys.BodyCenterOfMass(sA.body, com);
  const float target = g.openSign * g.openRad;
  const bool swungToward = haveBody && com.z > g.Center().z + 1.0f;
  const bool angleOk = std::fabs(sA.angle - target) < 0.2f;
  const bool okA = check(openAfter >= 0 && haveBody && allAir(openWords) && swungToward && angleOk &&
                             sA.leafCells == (uint32_t)(W * H) && c.debris.IsFixture(sA.body),
                         Format("A: open after %d ticks, body %d, cells empty %d, com.z %.1f vs %.1f, "
                                "angle %.2f want %.2f, leaf %u",
                                openAfter, haveBody, allAir(openWords), com.z, g.Center().z,
                                sA.angle, target, sA.leafCells));

  // B. BLOCKED CLOSE, then clear. The plug stands on the floor slab (a lone
  // voxel in mid-air is an island the debris system takes over).
  const IVec3 plug{X0 + 2, Y0, Z0};
  const size_t plugIdx = (size_t)(2 * H);
  support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) {
    o.cells.push_back({World::SlotCellIndex(plug), PackVoxNew(stone, 0)});
  });
  press();
  support::RunTicks(*rig, 75);
  refs::DoorStatus sB;
  refs::DoorStatusOf(dr.id, sB);
  const bool waited = sB.phase == refs::DoorPhase::Settling && sB.blockedTicks > 0 &&
                      sB.body != 0 && c.debris.HasBody(sB.body);
  check(waited, Format("B: blocked close: phase %s, blocked %u ticks, body %d (%s)",
                       refs::DoorPhaseName(sB.phase), sB.blockedTicks,
                       sB.body != 0 && c.debris.HasBody(sB.body), sB.note.c_str()));
  support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) {
    o.cells.push_back({World::SlotCellIndex(plug), 0u});
  });
  std::vector<uint32_t> cleared;
  boxWords(cleared);
  const uint32_t plugAfterClear = cleared.size() > plugIdx ? cleared[plugIdx] : 0xFFFFFFFFu;
  const int closedAfter = waitPhase(refs::DoorPhase::Closed, 120);
  support::RunTicks(*rig, 3);
  std::vector<uint32_t> after;
  boxWords(after);
  const bool bodyGone = sB.body == 0 || !c.debris.HasBody(sB.body);
  const bool exact = after == before;
  int diff = 0;
  for (size_t i = 0; i < after.size() && i < before.size(); i++) diff += after[i] != before[i];
  refs::DoorStatus sB2;
  refs::DoorStatusOf(dr.id, sB2);
  uint32_t firstDiff = 0, firstWas = 0;
  for (size_t i = 0; i < after.size() && i < before.size(); i++)
    if (after[i] != before[i]) {
      firstDiff = after[i];
      firstWas = before[i];
      break;
    }
  const bool okB = check(waited && closedAfter >= 0 && bodyGone && exact && st.Delta(dr.id) == nullptr,
                         Format("B: closed after %d ticks, body gone %d, %d of %zu words differ "
                                "(first %08x, was %08x; plug cell %08x right after the clear, "
                                "%08x now); door %s %.2f rad: %s",
                                closedAfter, bodyGone, diff, after.size(), firstDiff, firstWas,
                                plugAfterClear, after.size() > plugIdx ? after[plugIdx] : 0u,
                                refs::DoorPhaseName(sB2.phase), sB2.angle, sB2.note.c_str()));

  // C. OPEN, SAVE, LOAD: comes back open; closes exact.
  press();
  waitPhase(refs::DoorPhase::Open, 60);
  support::RunTicks(*rig, 40);
  EntityIO eio = MakeEntityIO(c.debris, c.mobs, nullptr, nullptr, nullptr, &st);
  const bool saved = SaveWorld(c.ctx, c.world, c.stream, kPath, c.mats, &eio);
  EntityFileReport lr;
  const bool loaded = LoadWorld(c.ctx, c.world, c.sim, c.stream, kPath, c.mats, &eio, nullptr, &lr);
  const bool deltaBack = st.Delta(dr.id) != nullptr;
  t = rig->tick;
  rig = std::make_unique<support::TickRig>(c, t, fixChunk);
  rig->Authority().refs = &st;
  standInFront();
  const int reopenAfter = waitPhase(refs::DoorPhase::Open, 30);
  support::RunTicks(*rig, 20);
  refs::DoorStatus sC;
  refs::DoorStatusOf(dr.id, sC);
  std::vector<uint32_t> loadedWords;
  boxWords(loadedWords);
  const bool backOpen = reopenAfter >= 0 && sC.body != 0 && c.debris.HasBody(sC.body) &&
                        std::fabs(sC.angle - target) < 0.25f && allAir(loadedWords);
  press();
  const int closedC = waitPhase(refs::DoorPhase::Closed, 150);
  support::RunTicks(*rig, 3);
  std::vector<uint32_t> afterC;
  boxWords(afterC);
  const bool okC = check(saved && loaded && deltaBack && backOpen && closedC >= 0 && afterC == before,
                         Format("C: saved %d loaded %d delta %d, open again after %d ticks at "
                                "%.2f rad (body %d, cells empty %d), closed after %d, exact %d",
                                saved, loaded, deltaBack, reopenAfter, sC.angle,
                                sC.body != 0 && c.debris.HasBody(sC.body), allAir(loadedWords),
                                closedC, afterC == before));

  // Leave nothing behind.
  {
    refs::RefCtx rc{&st, &c.mobs, &c.world, &c.stream.Store()};
    rc.phys = &c.phys;
    rc.debris = &c.debris;
    st.DeactivateAll(rc, refs::RefEvent::Reset);
  }
  rig.reset();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(idCounterWas);
  c.stream.Store().Clear();
  fs::remove_all(kPath, ec);
  fs::remove_all(root, ec);
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = Format(
      "active %d | A %s: open in %d ticks, one body, %d cells emptied, swung +Z to %.0f deg | "
      "B %s: blocked close waited %u ticks, closed exact (%d diffs) | C %s: open survives "
      "save/load (%.0f deg), closes exact",
      activeOk, okA ? "ok" : "FAIL", openAfter, W * H, sA.angle * 57.2958f, okB ? "ok" : "FAIL",
      sB.blockedTicks, diff, okC ? "ok" : "FAIL", sC.angle * 57.2958f);
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

// container-persist — CPU. A `container` ref's initial contents come from
// props.items; take one, take one more, put a stack back (merging), then the
// REFS region records the real save writes go through ResetForLoad and back:
// contents exact, slot for slot. A second chest never touched has no delta and
// still reads as authored. A full pack refuses a take and moves nothing.
Status GateContainerPersist(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) why.push_back(what);
    return ok;
  };
  // Two plain items the library has (whatever they are called today).
  std::vector<std::string> names;
  for (const ItemDef& d : c.items.items)
    if (names.size() < 2 && !d.name.empty()) names.push_back(d.name);
  if (names.size() < 2) {
    detail = "the item library has fewer than two items";
    return Status::Fail;
  }
  const std::string root = EmptyScratch("chest", "chest");
  refs::RefStore st;
  st.LoadMap(root, "chest");
  std::string err;
  refs::Ref a;
  a.id = "chestfix/strongbox";
  a.kind = "container";
  a.pos = {100, 170, 100};
  a.props = refs::Json{{"title", "Strongbox"},
                       {"items", refs::Json::array({refs::Json{{"item", names[0]}, {"count", 3}},
                                                    refs::Json{{"item", names[1]}, {"count", 1}},
                                                    refs::Json{{"item", "no_such_thing"}, {"count", 2}}})}};
  refs::Ref b = a;
  b.id = "chestfix/untouched";
  b.pos = {104, 170, 100};
  check(refs::Place(st, a, &err) && refs::Place(st, b, &err), "place: " + err);
  const refs::Ref* ra = st.Find(a.id);
  Bag start;
  refs::ContainerContents(st, *ra, &c.items, start);
  check(start.Count() == 2 && start.slots[0].name == names[0] && start.slots[0].count == 3,
        Format("authored contents: %d stacks (want 2, the unknown one skipped)", start.Count()));

  // Take stack 0 (x3) and stack 1 into an empty kit; put the first back.
  Kit kit;
  std::string msg;
  const bool took0 = refs::ContainerTake(st, a.id, 0, kit, c.items, &msg);
  const bool took1 = refs::ContainerTake(st, a.id, 0, kit, c.items, &msg);
  Bag mid;
  refs::ContainerContents(st, *st.Find(a.id), &c.items, mid);
  const bool emptyNow = mid.Count() == 0;
  int kitCount = 0;
  for (const ItemStack& s2 : kit.bag.slots) kitCount += s2.Empty() ? 0 : s2.count;
  ItemStack back = kit.bag.slots[0];
  const bool put = refs::ContainerPut(st, a.id, back, &msg) && back.Empty();
  kit.bag.slots[0] = ItemStack{};
  Bag want;
  refs::ContainerContents(st, *st.Find(a.id), &c.items, want);
  check(took0 && took1 && emptyNow && kitCount == 4 && put && want.Count() == 1,
        Format("take/put: took %d %d, empty %d, kit %d items, put %d, %d stacks after", took0,
               took1, emptyNow, kitCount, put, want.Count()));
  // A full pack refuses and moves nothing.
  Kit full;
  for (int i = 0; i < Bag::kSlots + kItemSlots; i++) {
    ItemStack filler;
    filler.name = names[1] + "_filler" + std::to_string(i);
    filler.count = 1;
    if (i < Bag::kSlots) full.bag.slots[i] = filler;
    else full.hotbar.slots[i - Bag::kSlots] = filler;
  }
  const bool refused = !refs::ContainerTake(st, a.id, 0, full, c.items, &msg);
  Bag stillWant;
  refs::ContainerContents(st, *st.Find(a.id), &c.items, stillWant);
  check(refused && stillWant.Count() == 1, "a full pack did not refuse cleanly: " + msg);

  // SAVE -> LOAD through the region records.
  EntitySection sec = refs::MakeRefsSection(st, nullptr);
  std::vector<EntityRecord> recs;
  sec.saveRecords(recs);
  refs::RefCtx rc{&st};
  st.ResetForLoad(rc);
  const bool goneAfterReset = st.Delta(a.id) == nullptr;
  int applied = 0;
  for (const EntityRecord& r : recs)
    applied += sec.loadRecord(r.bytes.data(), r.bytes.size(), refs::RefStore::kSaveVersion) ==
               RecordLoad::Applied;
  Bag got;
  refs::ContainerContents(st, *st.Find(a.id), &c.items, got);
  bool same = got.Count() == want.Count();
  for (int i = 0; i < Bag::kSlots && same; i++)
    same = got.slots[i].name == want.slots[i].name && got.slots[i].count == want.slots[i].count;
  Bag untouched;
  refs::ContainerContents(st, *st.Find(b.id), &c.items, untouched);
  const bool okSave = check(recs.size() == 1 && goneAfterReset && applied == 1 && same &&
                                untouched.Count() == 2 && st.Delta(b.id) == nullptr,
                            Format("save/load: %zu records, reset cleared %d, applied %d, exact %d, "
                                   "untouched chest %d stacks",
                                   recs.size(), goneAfterReset, applied, same, untouched.Count()));
  std::error_code ec;
  fs::remove_all(root, ec);
  detail = Format("authored %d stacks (unknown name skipped) | take x2 + put back | full pack "
                  "refused | %s: %zu REFS record, contents exact after load, untouched chest "
                  "has no delta",
                  start.Count(), okSave ? "ok" : "FAIL", recs.size());
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& RefsGates() {
  static const std::vector<Gate> g = {
      // CPU only: the group file format and the authoring actions.
      {"refs-roundtrip", "world", {}, false, GateRefsRoundtrip, /*needsRender=*/false},
      // Activation order + the use verb through the real tick.
      {"refs-activate", "world", {}, false, GateRefsActivate, /*needsRender=*/false},
      // Stable NPC identity through save/load and park/unpark.
      {"refs-npc-identity", "world", {}, false, GateRefsNpcIdentity, /*needsRender=*/false},
      // P6: a door opens into a hinged body and closes back exact; a chest's
      // contents survive the save.
      {"door-cycle", "world", {}, false, GateDoorCycle, /*needsRender=*/false},
      {"container-persist", "world", {}, false, GateContainerPersist, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
