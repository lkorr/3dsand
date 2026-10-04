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
  // THE TWO HANDS (dual wielding, 2026-09-27): with the hands up, LMB is the
  // RIGHT hand's button and RMB the LEFT's — a strike, or its vessel's use.
  TB_ATTACK = 1u << 3,  // LMB: brush/laser paint, the right hand
  TB_ALT = 1u << 4,     // RMB: brush erase, the beam, cast in magic, the left hand
  TB_CAST = 1u << 5,    // a cast was ASKED FOR (RMB in magic mode)
  TB_PLACE = 1u << 6,   // stamp the selected prefab
  TB_SPAWN = 1u << 7,   // spawn the selected mob def
  TB_DROP = 1u << 8,    // drop the newest status effect
  TB_LASER = 1u << 9,   // the laser is firing (F, or LMB with the laser tool)
  TB_THROW = 1u << 10,  // G held: winding up a throw of a held vessel; release throws
  // A HELD VESSEL'S MODE, per hand (F cycles it: pour -> scoop -> apply).
  // APPLY: the hand's button brushes the contents onto the body under the
  // crosshair instead of pouring a stream; SCOOP: it takes loose matter in.
  // Neither = pour. HELD while the mode is on, so a replay or a peer reads
  // the button the same way. TB_APPLY/TB_SCOOP are the RIGHT hand's.
  TB_APPLY = 1u << 11,
  TB_SCOOP = 1u << 12,
  TB_APPLY_L = 1u << 13,
  TB_SCOOP_L = 1u << 14,
  // THE USE VERB (docs/PLAN_world_editor.md §2.5, world/refs_game.h): a
  // press that USES the reference named by TickInput::useRef -- open a door,
  // talk to a villager, search a chest. An edge, carried with its target so
  // the tick acts on what the HUD prompt promised, not on a re-aimed ray.
  TB_USE = 1u << 15,
  // RELEASE THE DEMON (docs/PLAN_demons.md D3, game/demon_seals.h): an edge
  // that lets the presser's most recent CONTAINED demon out of its circle,
  // weighed there and then -- held iff circle strength >= power + contract
  // weight, else it overpowers the binding and is loose and hostile.
  TB_DEMON_RELEASE = 1u << 16,
  // (bits 17..19 are left for D4.) THE DEMON CONVERSATION AND THE CONTRACT
  // (docs/PLAN_demons.md D5, game/demon_talk.h):
  //   TALK      an edge: open the conversation with the presser's most recent
  //             CONTAINED demon (T).
  //   PRESENT   an edge carried with `contractHash` (the page, by name hash)
  //             and `demonRef` (the demon, low 32 bits of its mob id; 0 = the
  //             one you are talking to): bind it under that contract.
  //   DISMISS   an edge with `demonRef` (0 = your most recent): send it home.
  //   LOOKAWAY  HELD: while talking to a demon, the summoner looks away from
  //             it (the gaze rule, D3), else at it.
  TB_DEMON_TALK = 1u << 20,
  TB_DEMON_PRESENT = 1u << 21,
  TB_DEMON_DISMISS = 1u << 22,
  TB_DEMON_LOOKAWAY = 1u << 23,
};

// Bumped whenever a field is added, removed or changes meaning. Carried in the
// op record's frame header (sim/oprecord.h) and refused on mismatch.
// 4: dual wielding — TB_ALT is the left hand, per-hand vessel modes.
// 5: TB_USE + `useRef` (the use verb, PLAN_world_editor.md P1).
// 6: `talk` (was pad0) — the conversation command (game/dialogue.h).
// 7: TB_DEMON_RELEASE (demons D3).
// 8: TB_DEMON_TALK / PRESENT / DISMISS / LOOKAWAY + `contractHash`,
//    `demonRef` (demons D5; 76 -> 84 bytes).
constexpr uint32_t kTickInputVersion = 8;   // 2: TB_THROW, 3: TB_APPLY

// THE CONVERSATION COMMAND (TickInput::talk, game/dialogue.h). A choice made
// in the conversation panel is a player INPUT like a strike or a use, so it
// rides the command and is applied by TickAuthority, never by the panel: a
// replay or a peer sees the same answer given at the same tick. 1..9 pick the
// n-th choice the panel SHOWS (the node's visible list, fixed when the node
// was entered), so the number on the key is the number on the screen.
enum TalkCommand : int16_t {
  kTalkNone = 0,
  // 1..9: choose the n-th visible choice
  kTalkContinue = 10,  // a node with no choices: "[continue]"
  kTalkLeave = 11,     // Esc, when the node allows leaving
};

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
  // A TalkCommand (above), or kTalkNone. An EDGE: delivered to one tick.
  int16_t talk = kTalkNone;
  // WHAT A TB_USE PRESS USES: refs::RefHash of the reference id the frame's
  // use prompt showed (world/refs.h), 0 = nothing. A hash, not the string,
  // because this is a fixed-size wire POD; the store detects collisions at
  // load. One-shot like the edge it rides with (TickInputFeeder::Consume).
  uint32_t useRef = 0;
  // WHAT A TB_DEMON_PRESENT PRESS PRESENTS (demons D5): contract::PageHash of
  // the page's name, and WHICH demon (low 32 bits of its mob id, 0 = the one
  // the presser is talking to / its most recent). One-shot with their edges.
  uint32_t contractHash = 0;
  uint32_t demonRef = 0;

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

static_assert(sizeof(TickInput) == 84, "TickInput is a wire message");

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
  // The newest conversation answer since the last tick wins (two keys in one
  // frame is a fumble, not two answers).
  void Talk(int code) { pend.talk = (int16_t)code; }
  // A use press and its target, together (the newest press wins).
  void Use(uint32_t refHash) {
    pend.pressed |= TB_USE;
    pend.useRef = refHash;
  }
  // A contract presented to a demon / a demon dismissed (demons D5).
  void Present(uint32_t pageHash, uint32_t demon) {
    pend.pressed |= TB_DEMON_PRESENT;
    pend.contractHash = pageHash;
    pend.demonRef = demon;
  }
  void Dismiss(uint32_t demon) {
    pend.pressed |= TB_DEMON_DISMISS;
    pend.demonRef = demon;
  }
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
    pend.talk = kTalkNone;
    pend.useRef = 0;
    pend.contractHash = 0;
    pend.demonRef = 0;
    return out;
  }
};
