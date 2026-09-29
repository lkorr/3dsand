#pragma once
#include <cmath>

// ============================================================================
// THE DIRECTION PICK — the thin input layer in front of discrete strikes.
//
// Reads NOTHING but raw mouse deltas and answers one question at the moment
// the attack button goes down: "which way did the player flick?". The stroke
// pathway (game/strokes.h StepStrokeProgram) never sees this type, and this
// type never sees a MeleeState — that seam is the whole point, because the
// pick scheme is the part of discrete combat most likely to be swapped (WASD
// modifiers, a stance HUD) and swapping it must touch one call site in
// main.cpp and nothing else.
//
// THE LAW IS THE SAME SHAPE AS THE DRIVER'S (melee.cpp mouseVel_): per-frame
// deltas integrated into an exponentially-smoothed velocity. A DELIBERATE OWN
// COPY rather than a read of MeleeState::MouseSpeed(): in discrete mode the
// driver is fed by the program, not the mouse, so its smoothed velocity is
// synthetic — reading it back would quantize the program's own cut into the
// next pick.
//
// Pure presentation state on the input side of everything: never saved, never
// hashed, and the selftest can drive it with fabricated deltas.
// ============================================================================
struct StrikePicker {
  // Smoothed mouse velocity, px/s in screen coordinates (+y is DOWN, exactly
  // as the deltas arrive — the caller maps to stroke space, not this).
  float vx = 0, vy = 0;
  // Which horizontal cut a directionless click gets next. Public so the HUD
  // can show it and a gate can pin it; flipped by NeutralStrike's caller.
  bool altRight = true;

  // Feed one frame's raw deltas. Same halflife law as the driver's
  // dirSmoothing so the two read a flick the same way (0.06 s of history).
  //
  // `minSpeed` > 0 also runs the GESTURE READ that feeds the remembered
  // direction (below): a motion starts when the smoothed speed reaches
  // minSpeed and ends when it falls under half of it; it is a FLICK — and
  // becomes the remembered direction — only if it lasted at most
  // `flickTicks` feeds and moved at most `flickPx` pixels. Anything longer
  // or bigger is the camera being turned, and changes nothing: that is what
  // lets a charged strike held on the left be carried round to the right.
  void Feed(float dx, float dy, float dt, float minSpeed = 0.0f,
            int flickTicks = 1 << 30, float flickPx = 1e30f) {
    if (dt <= 1e-6f) return;
    const float kHalflife = 0.06f;
    const float a = 1.0f - std::pow(0.5f, dt / kHalflife);
    vx += (dx / dt - vx) * a;
    vy += (dy / dt - vy) * a;
    if (minSpeed <= 0.0f) return;
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (!inGesture && speed >= minSpeed) {
      inGesture = true;
      gTicks = 0;
      gX = gY = gPath = 0.0f;
    }
    if (!inGesture) return;
    gTicks++;
    gX += dx;
    gY += dy;
    gPath += std::sqrt(dx * dx + dy * dy);
    if (speed >= 0.5f * minSpeed) return;
    // The motion has stopped: judge it whole — unless a press already took
    // it (Seed), in which case it has been spent on that strike.
    inGesture = false;
    if (gestureSpent) {
      gestureSpent = false;
      lastGestureFlick = false;
      return;
    }
    lastGestureFlick = gTicks <= flickTicks && gPath <= flickPx;
    const float len = std::sqrt(gX * gX + gY * gY);
    if (lastGestureFlick && len > 1e-6f) {
      dirX = gX / len;
      dirY = gY / len;
      hasDir = true;
    }
  }

  // ---- the gesture in progress (Feed) ----
  bool inGesture = false;
  int gTicks = 0;
  float gX = 0, gY = 0, gPath = 0;   // net displacement and path length, px
  bool lastGestureFlick = false;     // how the last finished motion was read
  bool gestureSpent = false;         // the motion in progress belongs to a press

  // ---- THE REMEMBERED DIRECTION (the charged strike's, 2026-09-28) -------
  // The last FLICK (a short motion, see Feed), KEPT until the next one. A
  // NORMAL click never reads it — it reads the live flick (Pick), which
  // forgets in ~0.1 s, and a settled mouse alternates L/R. A CHARGED hold
  // does (Remembered): the arm re-aims to whatever this names, so a flick
  // while holding stays chosen after the mouse settles. The press re-seeds it
  // (Seed) with the direction the click actually threw, so a hold starts from
  // its own strike rather than from a flick made seconds earlier.
  bool hasDir = false;
  float dirX = 0, dirY = 0;

  // The live flick at the press, or false when the mouse was effectively
  // still (below minSpeed px/s) and the caller should alternate L/R instead.
  // `outX`/`outY` are a unit direction in SCREEN space (+x right, +y down).
  bool Pick(float minSpeed, float& outX, float& outY) const {
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (speed < minSpeed || speed < 1e-6f) return false;
    outX = vx / speed;
    outY = vy / speed;
    return true;
  }
  // The remembered direction, false when there is none.
  bool Remembered(float& outX, float& outY) const {
    if (!hasDir) return false;
    outX = dirX;
    outY = dirY;
    return true;
  }
  // The press: remember what it threw (`flicked` false = a neutral click,
  // which remembers nothing, so the hold re-aims only on a fresh flick).
  //
  // THE MOTION THE PRESS LANDED IN IS THE PRESS'S. A click usually comes in
  // the middle of the flick that picked it, and that motion is only judged
  // when it stops, a few ticks later; judged then, its tail could name a
  // different sector and re-aim the hold away from the strike the compass
  // showed at the click. So it is marked spent and never judged.
  void Seed(bool flicked, float x, float y) {
    hasDir = flicked;
    dirX = x;
    dirY = y;
    gestureSpent = inGesture;
  }

  void Reset() {
    vx = vy = 0;
    hasDir = false;
    inGesture = false;
    gestureSpent = false;
  }
};
