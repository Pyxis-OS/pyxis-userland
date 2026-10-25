// Copyright 2024 The Go Authors. All rights reserved.
// Derived from Go's blockSHANI. Distributed under the BSD license in
// LICENSE.go-bsd; see UPSTREAM.md for the pinned source and local adaptations.

#include "../../internal.h"

#include <cpuid.h>
#include <immintrin.h>

#define SHA256_TARGET __attribute__((target("sha,ssse3,sse4.1,no-avx")))

bool
sha256_x86_available(void)
{
  unsigned int eax, ebx, ecx, edx;
  if (__get_cpuid_max(0, NULL) < 7) {
    return false;
  }
  __cpuid(1, eax, ebx, ecx, edx);
  if ((ecx & (bit_SSSE3 | bit_SSE4_1)) != (bit_SSSE3 | bit_SSE4_1)) {
    return false;
  }
  __cpuid_count(7, 0, eax, ebx, ecx, edx);
  return (ebx & bit_SHA) != 0;
}

// Each vector holds four consecutive words, earliest word in the low lane.
static inline SHA256_TARGET __attribute__((always_inline)) __m128i
schedule_words(__m128i first, __m128i second, __m128i third, __m128i fourth)
{
  first = _mm_sha256msg1_epu32(first, second);
  first = _mm_add_epi32(first, _mm_alignr_epi8(fourth, third, 4));
  return _mm_sha256msg2_epu32(first, fourth);
}

static inline SHA256_TARGET __attribute__((always_inline)) void
four_rounds(__m128i *abef, __m128i *cdgh, __m128i words, size_t round)
{
  __m128i wk = _mm_add_epi32(words,
      _mm_loadu_si128((const __m128i *)(sha256_round_constants + round)));
  *cdgh = _mm_sha256rnds2_epu32(*cdgh, *abef, wk);
  wk = _mm_shuffle_epi32(wk, 0x0e);
  *abef = _mm_sha256rnds2_epu32(*abef, *cdgh, wk);
}

// The caller must establish CPU availability before a nonempty call. State is
// eight initialized host-endian words; input is exactly count complete 64-byte
// blocks. Initialization, partial input buffering and padding belong to callers.
SHA256_TARGET void
sha256_x86_blocks(uint32_t state[8], const unsigned char *blocks, size_t count)
{
  // No data or state access is needed for an empty run.
  if (count == 0) {
    return;
  }

  // SHA256RNDS2 expects high-to-low lanes ABEF and CDGH, unlike state[].
  __m128i abcd = _mm_loadu_si128((const __m128i *)state);
  __m128i efgh = _mm_loadu_si128((const __m128i *)(state + 4));
  __m128i swapped = _mm_shuffle_epi32(abcd, 0xb1);
  __m128i cdgh = _mm_shuffle_epi32(efgh, 0x1b);
  __m128i abef = _mm_alignr_epi8(swapped, cdgh, 8);
  cdgh = _mm_blend_epi16(cdgh, swapped, 0xf0);
  const __m128i byte_swap = _mm_setr_epi8(
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

  do {
    __m128i saved_abef = abef;
    __m128i saved_cdgh = cdgh;
    __m128i m0 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)blocks), byte_swap);
    __m128i m1 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(blocks + 16)), byte_swap);
    __m128i m2 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(blocks + 32)), byte_swap);
    __m128i m3 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(blocks + 48)), byte_swap);

    for (size_t round = 0; ; round += 16) {
      four_rounds(&abef, &cdgh, m0, round);
      four_rounds(&abef, &cdgh, m1, round + 4);
      four_rounds(&abef, &cdgh, m2, round + 8);
      four_rounds(&abef, &cdgh, m3, round + 12);
      if (round == 48) {
        break;
      }
      // Roll the four-vector schedule forward by sixteen words. Each update
      // consumes the previous vectors and the newly generated earlier words.
      m0 = schedule_words(m0, m1, m2, m3);
      m1 = schedule_words(m1, m2, m3, m0);
      m2 = schedule_words(m2, m3, m0, m1);
      m3 = schedule_words(m3, m0, m1, m2);
    }

    abef = _mm_add_epi32(abef, saved_abef);
    cdgh = _mm_add_epi32(cdgh, saved_cdgh);
    blocks += 64;
  } while (--count != 0);

  swapped = _mm_shuffle_epi32(abef, 0x1b);
  cdgh = _mm_shuffle_epi32(cdgh, 0xb1);
  abcd = _mm_blend_epi16(swapped, cdgh, 0xf0);
  efgh = _mm_alignr_epi8(cdgh, swapped, 8);
  _mm_storeu_si128((__m128i *)state, abcd);
  _mm_storeu_si128((__m128i *)(state + 4), efgh);
}
