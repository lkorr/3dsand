#pragma once
#include <cstdint>

// The CPU mirror of common.wgsl's `pcg`/`hash3` (assets/shaders/common.wgsl).
// This is the ONE definition — it used to be copy-pasted into six translation
// units (mob, avatar, debris, spell, tuning, world), which meant six places to
// get a constant wrong and no way for the compiler to notice.
//
// Why it must be counter-based and stateless (CLAUDE.md rule 1): every caller
// authors values INTO a tick's spawn/op stream, and a replay has to reproduce
// that stream bit-for-bit. A stateful stream desyncs the moment a frame
// boundary moves, because the number of draws per tick changes. So the only
// sanctioned form is a pure function of (seed, tick, index) — never a member
// that advances, never anything seeded by iteration or dispatch order.
//
// The constants here are load-bearing and must stay byte-identical to
// common.wgsl:789-796. If you change one, you change the world hash and every
// recorded replay, and --selftest's twice-run comparison will say so.

namespace rng {

inline uint32_t Pcg(uint32_t v) {
  uint32_t s = v * 747796405u + 2891336453u;
  uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}

inline uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) {
  return Pcg(a ^ Pcg(b ^ Pcg(c)));
}

// Uniform in [-1, 1) from a hash word. 16 bits, so the ladder is coarse by
// design — it is used for spray directions, where banding is invisible.
inline float SignedUnit(uint32_t h) {
  return (float)(int32_t)(h & 0xFFFFu) / 32768.0f - 1.0f;
}

// Uniform in [0, 1). 24 bits is well inside f32's exact-integer range, so this
// is reproducible bit-for-bit rather than merely close.
inline float Unit01(uint32_t h) {
  return (float)(h & 0x00FFFFFFu) / 16777216.0f;
}

}  // namespace rng
