#pragma once
#include <cstdint>

#include "sim/rng.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

// ---- eight lanes of rng::Pcg, for the whole-chunk JITTER paths ------------
//
// rng::Pcg (sim/rng.h) is THE definition and stays the definition. This is the
// same arithmetic, eight cells at a time, for the two paths that hash all 4,096
// cells of a chunk: PageTable::Classify's JITTER verify and the sentinel RLE.
// Same framing as world.h's JitterRowSeed/JitterStateInRow row form — a
// strictly derived helper, not a second rule. If the definition changes, this
// changes with it.
//
// WHY: measured over one --autofly-hard run (1,200 frames), Classify cost
// 6,007 ms across 425,890 demoted chunks — 14.1 us each, by a wide margin the
// largest CPU item on the paged path. At 4,096 cells that is ~20 cycles/cell,
// which is almost exactly the latency of the two serially-dependent Pcg rounds
// the row form leaves behind, plus the %3. The word scans around it are ~6% of
// the call; the HASH is the other ~80%. So this file, not scan.h, is the win.
//
// BIT-IDENTICAL, per operation:
//   v * 747796405u   -> mullo_epi32 : low 32 bits of a 32x32 product, which is
//                       exactly C++ unsigned wraparound multiply. Sign-agnostic.
//   + 2891336453u    -> add_epi32   : wraparound add. Sign-agnostic.
//   s >> 28u         -> srli_epi32  : logical shift, constant count 28 < 32.
//   s >> ((s>>28)+4) -> srlv_epi32  : SEE THE PROOF BELOW.
//   ^, w >> 22u      -> xor / srli  : exact.
//   % 3u             -> Mod3_8      : magic-number division, exact for all 2^32.
namespace rng {

#if defined(__AVX2__)

inline __m256i Pcg8(__m256i v) {
  const __m256i kA = _mm256_set1_epi32((int)747796405u);
  const __m256i kB = _mm256_set1_epi32((int)2891336453u);
  const __m256i kC = _mm256_set1_epi32((int)277803737u);
  const __m256i k4 = _mm256_set1_epi32(4);
  // s = v * 747796405u + 2891336453u
  const __m256i s = _mm256_add_epi32(_mm256_mullo_epi32(v, kA), kB);
  // sh = (s >> 28) + 4.
  //
  // THE ONE PLACE THIS IS NOT A MECHANICAL SUBSTITUTION. vpsrlvd yields 0 for
  // a shift count >= 32, whereas C++ `>>` by >= 32 on a uint32_t is undefined
  // behaviour — the two agree only because the count cannot reach 32 here:
  // (s >> 28) is a 4-bit quantity in [0,15], so sh is in [4,19]. That is a
  // proof about the CONSTANTS 28 and 4, not about the shapes. Change either
  // one and this has to be re-derived.
  const __m256i sh = _mm256_add_epi32(_mm256_srli_epi32(s, 28), k4);
  // w = ((s >> sh) ^ s) * 277803737u
  const __m256i w = _mm256_mullo_epi32(
      _mm256_xor_si256(_mm256_srlv_epi32(s, sh), s), kC);
  // return (w >> 22) ^ w
  return _mm256_xor_si256(_mm256_srli_epi32(w, 22), w);
}

// n % 3, exact for every uint32_t. This is the one piece of arithmetic in the
// file with no operation-for-operation counterpart in the scalar source, so it
// is the one the gate sweeps exhaustively: n/3 == mulhi_u32(n, 0xAAAAAAAB) >> 1
// is precisely the sequence the compiler already emits for `n / 3u`.
inline __m256i Mod3_8(__m256i n) {
  const __m256i kInv = _mm256_set1_epi32((int)0xAAAAAAABu);
  // vpmuludq multiplies the EVEN dword of each qword, 32x32 -> 64, so the odd
  // lanes need a second multiply on the operand shifted down by 32.
  // _mm256_mul_epu32 (this) is NOT _mm256_mullo_epi32 (used above): swapping
  // the two compiles fine and produces plausible garbage. It is the single
  // most likely silent bug in this file.
  const __m256i pe = _mm256_mul_epu32(n, kInv);                         // 0,2,4,6
  const __m256i po = _mm256_mul_epu32(_mm256_srli_epi64(n, 32), kInv);  // 1,3,5,7
  const __m256i he = _mm256_srli_epi64(pe, 32);  // even hi -> dword 0 of qword
  // po's high dword already sits at dword 1 of its qword = the odd lane slot.
  const __m256i hi = _mm256_blend_epi32(he, po, 0xAA);
  const __m256i q = _mm256_srli_epi32(hi, 1);                        // n / 3
  const __m256i q3 = _mm256_add_epi32(_mm256_slli_epi32(q, 1), q);    // q * 3
  return _mm256_sub_epi32(n, q3);
}

// Eight consecutive x in one row of world.h's JitterStateInRow:
//   Pcg((seed ^ 0xC0FFEE) ^ Pcg(x ^ rowSeed)) % 3
// rowSeed and seed are loop-invariant across the row, so they broadcast once
// per row rather than per lane.
inline __m256i JitterStateInRow8(uint32_t rowSeed, __m256i x, uint32_t seed) {
  const __m256i a = _mm256_set1_epi32((int)(seed ^ 0xC0FFEEu));
  const __m256i r = _mm256_set1_epi32((int)rowSeed);
  return Mod3_8(Pcg8(_mm256_xor_si256(a, Pcg8(_mm256_xor_si256(x, r)))));
}

#endif  // __AVX2__

}  // namespace rng
