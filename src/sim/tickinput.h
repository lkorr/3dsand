// tickinput.h — THE PLAYER'S COMMAND FOR ONE TICK
// (docs/PLAN_multiplayer_now.md package N2, L2/L3).
//
// Before this file the player controller was driven by `PlayerInput`, a
// per-FRAME movement intent filled straight from GLFW polling, and by six
// ad-hoc sticky latches in main.cpp (`castQueued`, `strikeQueued`,
// `ui.placePrefab`, `ui.spawnMob`, `dropStatusQueued`, the RMB beam) that
// existed for one reason: the fixed-tick loop runs ZERO times on most frames
// at 60+ fps against a 30 Hz tick, so a frame-local one-shot was discarded
// unread eight tries out of nine. Each latch was a hand-rolled half of the
// same missing type.
//
// THIS IS THAT TYPE. One value per TICK carrying everything the player asked
// for during the frames since the previous tick:
//
//   * HELD state (movement axes, jump/crouch/sprint, mouse buttons) — sampled
//     at the tick, and the same for every tick of a multi-tick frame because
//     GLFW is polled once per frame and there is nothing newer to read.
//   * PRESSED EDGES — latched by the frame layer and consumed by EXACTLY ONE
//     tick. This is what the ad-hoc latches were doing by hand.
//   * The LOOK DELTA in raw pixels accumulated since the previous tick, so the
//     strike picker and the swing driver integrate a whole tick's motion at
//     kTickDt instead of a frame's motion at a frame dt.
//   * The CAMERA BASIS as it stood at tick time. The controller used to take
//     three loose Vec3s; they belong in the command, because a replay or a
//     remote client has to reproduce the basis a decision was made under and
//     cannot re-derive it from a camera it does not have.
//
// WHY IT LIVES IN sim/ AND NOT IN game/player.h. The plan named player.h, and
// `game/player.h` does include this header so every existing consumer still
// gets the type from there. But package N3's op record (sim/oprecord.h) has to
// CARRY this struct — a record that replays the world without the input that
// produced it is not a replay — and `src/game/` includes `src/sim/`, never the
// other way round. A POD wire message is sim/'s layer anyway: it sits beside
// world.h's op structs and bytestream.h, which is what it will be serialized
// with.
//
// POD, FIXED-SIZE, VERSIONED. No pointers, no std::string, no padding holes
// (static_assert below). `version` is bumped when a field changes meaning so a
// record or a peer from another build is refused rather than misread.

#pragma once

#include <cstdint>

#include "math3d.h"

// Button bits. ONE numbering shared by the `held` and `pressed` masks: a bit
// in `held` means "down at this tick", the same bit in `pressed` means "went
// down since the previous tick". Two masks rather than two enums because every
// consumer asks one of exactly those two questions about the same button.
enum TickButton : uint32_t {
  TB_JUMP = 1u << 0,    // space (rise, in fly/swim)
  TB_CROUCH = 1u << 1,  // ctrl (descend, in fly/swim)
  TB_SPRINT = 1u << 2,
  TB_ATTACK = 1u << 3,  // LMB: brush/laser paint, melee guard, strike press
  TB_ALT = 1u << 4,     // RMB: the beam, and the cast press in magic mode
  TB_CAST = 1u << 5,    // a cast was ASKED FOR (RMB in magic mode)
  TB_PLACE = 1u << 6,   // stamp the selected prefab
  TB_SPAWN = 1u << 7,   // spawn the selected mob def
  TB_DROP = 1u << 8,    // drop the newest status effect
  TB_LASER = 1u << 9,   // the laser is firing (F, or LMB with the laser tool)
  TB_THROW = 1u << 10,  // Q: winding up a throw of the held vessel; release throws
};

// Bumped whenever a field is added, removed or changes meaning. Carried in the
// op record's frame header (sim/oprecord.h) and refused on mismatch.
constexpr uint32_t kTickInputVersion = 2;   // 2: TB_THROW

struct TickInput {
  uint32_t version = kTickInputVersion;

  // Movement intent, -1..1, in the basis below. Floats rather than quantized
  // integers: the controller is float physics on the CPU mirror and is NOT
  // hashed sim state (CLAUDE.md rule 1 governs the CA, not this), so
  // quantizing here would only change feel.
  float forward = 0;
  float strafe = 0;

  // RAW mouse pixels accumulated since the previous tick (+y is DOWN, exactly
  // as GLFW delivers them). Not scaled by the melee look-damping — that is a
  // CAMERA effect and applying it here would move MeleeTuning::commitSpeed,
  // which is calibrated in true mouse pixels per second.
  float lookDx = 0;
  float lookDy = 0;

  uint32_t held = 0;     // TickButton bits down at this tick
  uint32_t pressed = 0;  // TickButton bits that went down since the last tick

  // The attack style a discrete strike press resolved to, or -1. Carried as a
  // FIELD rather than as a bare TB_ bit because the flick direction is read at
  // the press (game/strike_pick.h) and the index is the command: "swing THIS
  // cut", not "swing".
  int16_t strikeStyle = -1;
  // The selection the tick acts under. The frame layer owns changing them (a
  // scroll wheel and the number row are UI, not simulation); the command
  // carries the CURRENT value so a replay or a remote peer knows which tool
  // and which hotbar slot a brush op or a strike came from.
  int16_t hotbar = -1;
  int16_t tool = -1;
  int16_t pad0 = 0;

  // The camera basis AT TICK TIME. flatFwd is the horizontal walk forward,
  // right the horizontal strafe axis, lookFwd the full 3D aim (fly and swim).
  Vec3 flatFwd{1, 0, 0};
  Vec3 right{0, 0, 1};
  Vec3 lookFwd{1, 0, 0};

  bool Held(uint32_t bits) const { return (held & bits) != 0; }
  bool Pressed(uint32_t bits) const { return (pressed & bits) != 0; }
  void SetHeld(uint32_t bits, bool on) {
    held = on ? (held | bits) : (held & ~bits);
  }
  void SetPressed(uint32_t bits, bool on) {
    pressed = on ? (pressed | bits) : (pressed & ~bits);
  }
};

static_assert(sizeof(TickInput) == 72, "TickInput is a wire message");

// ---- THE FRAME LAYER'S ACCUMULATOR ---------------------------------------
//
// One of these lives in the frame loop. The frame layer writes HELD state and
// the axes wholesale every frame (they are a sample of the keyboard, so the
// newest sample wins), ORs in any pressed EDGE it saw, and adds the frame's
// mouse pixels. Each tick calls Consume(), which hands over the command and
// clears exactly the parts that are one-shot: the edges, the look delta and
// the strike index.
//
// THAT SPLIT IS THE WHOLE CONTRACT, and it is what the `tick-input` gate
// pins: held state is broadcast to every tick of a multi-tick frame, an edge
// is delivered to exactly one tick and never to zero.
struct TickInputFeeder {
  TickInput pend;

  void SetAxes(float forward, float strafe) {
    pend.forward = forward;
    pend.strafe = strafe;
  }
  // Held state is a SAMPLE, so the whole mask is replaced, never OR-ed: a key
  // released between two ticks must read as released.
  void SetHeldMask(uint32_t mask) { pend.held = mask; }
  void Hold(uint32_t bits, bool on) { pend.SetHeld(bits, on); }
  // Edges ACCUMULATE until a tick takes them.
  void Press(uint32_t bits) { pend.pressed |= bits; }
  // Drop a pending edge without delivering it. The one caller is the pause
  // rule: a click made while paused must not discharge at whatever is under
  // the crosshair when the game resumes.
  void Cancel(uint32_t bits) { pend.pressed &= ~bits; }
  void Look(float dx, float dy) {
    pend.lookDx += dx;
    pend.lookDy += dy;
  }
  void SetStrike(int style) { pend.strikeStyle = (int16_t)style; }
  void SetSelection(int tool, int hotbarSlot) {
    pend.tool = (int16_t)tool;
    pend.hotbar = (int16_t)hotbarSlot;
  }

  TickInput Consume(const Vec3& flatFwd, const Vec3& right,
                    const Vec3& lookFwd) {
    pend.flatFwd = flatFwd;
    pend.right = right;
    pend.lookFwd = lookFwd;
    TickInput out = pend;
    pend.pressed = 0;
    pend.lookDx = pend.lookDy = 0;
    pend.strikeStyle = -1;
    return out;
  }
};
