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
  void Feed(float dx, float dy, float dt) {
    if (dt <= 1e-6f) return;
    const float kHalflife = 0.06f;
    const float a = 1.0f - std::pow(0.5f, dt / kHalflife);
    vx += (dx / dt - vx) * a;
    vy += (dy / dt - vy) * a;
  }

  // The flick at the press, or false when the mouse was effectively still
  // (below minSpeed px/s) and the caller should alternate L/R instead.
  // `outX`/`outY` are a unit direction in SCREEN space (+x right, +y down).
  bool Pick(float minSpeed, float& outX, float& outY) const {
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (speed < minSpeed || speed < 1e-6f) return false;
    outX = vx / speed;
    outY = vy / speed;
    return true;
  }

  void Reset() { vx = vy = 0; }
};
