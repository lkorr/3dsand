#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

// ---- masked word scans over a chunk's 4,096 u32s --------------------------
//
// The page table classifies and re-verifies whole chunks by asking the same
// question in three places: "is every word in this block equal to X under mask
// M, and if not, WHICH one is the first that is not?" Each of those was its own
// hand-written loop with a data-dependent `break`, which MSVC's auto-vectorizer
// refuses to touch — so despite the build running with /arch:AVX2 they all ran
// one word per iteration.
//
// TWO OF THOSE LOOPS WERE VERBATIM COPIES of the same stainless-air predicate
// (PageTable::Classify's EMPTY test and the hysteresis free probe's demotable
// test). Collapsing them onto one primitive is a "two places must agree"
// removal on top of the speedup, which is the more durable half of this file.
//
// THE SCALAR FORM IS THE DEFINITION, and it stays compiled unconditionally
// rather than living in the #else arm. A `#if` proves nothing about the branch
// it did not compile, so the gate compares scalar against SIMD in the SAME
// binary instead of trusting that two build configurations agree.
//
// /arch:AVX2 is load-bearing here and reaches this target explicitly (see the
// sandvox target in CMakeLists.txt). It used to arrive only as a side effect of
// Jolt's PUBLIC compile options, which would have made every function below
// silently revert to scalar on a Jolt bump with no diagnostic.
namespace scan {

// Index of the first i in [0,n) with (p[i] & mask) != want; n if there is none.
inline size_t ScalarFirstIndexWhereMasked(const uint32_t* p, size_t n,
                                          uint32_t mask, uint32_t want) {
  for (size_t i = 0; i < n; i++)
    if ((p[i] & mask) != want) return i;
  return n;
}

inline size_t FirstIndexWhereMasked(const uint32_t* p, size_t n, uint32_t mask,
                                    uint32_t want) {
#if defined(__AVX2__)
  const __m256i vm = _mm256_set1_epi32((int)mask);
  const __m256i vw = _mm256_set1_epi32((int)want);
  size_t i = 0;
  // `i + 8 <= n` bounds the load, so the over-read is prevented by arithmetic
  // rather than by luck. loadu and never load: callers pass interior offsets
  // into std::vector storage (demoteScratch_.data() + i * kChunkVol and
  // friends), where only 4-byte alignment is guaranteed and the aligned form
  // would fault.
  for (; i + 8 <= n; i += 8) {
    const __m256i v = _mm256_loadu_si256((const __m256i*)(p + i));
    const __m256i eq = _mm256_cmpeq_epi32(_mm256_and_si256(v, vm), vw);
    // cmpeq_epi32 sets each 32-bit lane to all-ones or all-zeros, and
    // movemask_ps extracts the SIGN BIT of each lane — one bit per element in
    // bits 0..7. castsi256_ps is a pure type pun: no instruction, no FP
    // arithmetic, no exception, no NaN canonicalisation. Do NOT substitute
    // movemask_epi8, whose mask carries FOUR bits per element and would make
    // the index below wrong by a factor of four.
    //
    // Only cmpeq is used, never cmpgt: cmpgt is SIGNED, and these masks cover
    // bit 31 (kAirDemoteMask is 0xFF000FFF, stain type sits at 28..30), so
    // words that compare as negative genuinely occur in this data.
    const uint32_t eqm = (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(eq));
    const uint32_t bad = (~eqm) & 0xFFu;  // bit k set => element i+k differs
    if (bad) return i + (size_t)std::countr_zero(bad);
  }
  // Scalar tail, deliberately not a maskload — n < 8 leftovers cost nothing and
  // a hand-built lane vector is one more thing to get wrong. Every production
  // call passes n == kChunkVol (4,096) or a suffix of it, so this path is
  // exercised ONLY by the gate, which is why the gate must call it with
  // n = 0,1,7,8,9,15,16,17 explicitly.
  for (; i < n; i++)
    if ((p[i] & mask) != want) return i;
  return n;
#else
  return ScalarFirstIndexWhereMasked(p, n, mask, want);
#endif
}

// The two predicates the page table actually asks. Wrappers, not second scans.
inline bool AllEqualMasked(const uint32_t* p, size_t n, uint32_t mask,
                           uint32_t want) {
  return FirstIndexWhereMasked(p, n, mask, want) == n;
}
inline bool AllEqual(const uint32_t* p, size_t n, uint32_t w) {
  return FirstIndexWhereMasked(p, n, 0xFFFFFFFFu, w) == n;
}

// True when this build compiled the vector path — for the startup banner, so a
// silent revert to scalar shows up in the log and not only in the frame time.
inline constexpr bool kHaveAvx2 =
#if defined(__AVX2__)
    true;
#else
    false;
#endif

}  // namespace scan
