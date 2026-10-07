#ifndef LIBC_STDINT_H
#define LIBC_STDINT_H

/* Pyxis x86-64 LP64 types, shared by SDK consumers regardless of compiler. */
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

typedef int8_t int_least8_t;
typedef int16_t int_least16_t;
typedef int32_t int_least32_t;
typedef int64_t int_least64_t;
typedef uint8_t uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

/*
 * The Pyxis ABI, first set by its GCC target: even the 8-bit fast types use
 * 32-bit integers. Clang's __INT_FAST*_TYPE__ predefines instead follow the
 * least-width types; these typedefs are authoritative.
 */
typedef int int_fast8_t;
typedef int int_fast16_t;
typedef int int_fast32_t;
typedef long int_fast64_t;
typedef unsigned int uint_fast8_t;
typedef unsigned int uint_fast16_t;
typedef unsigned int uint_fast32_t;
typedef unsigned long uint_fast64_t;

typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef long intmax_t;
typedef unsigned long uintmax_t;

#define INT8_MAX 127
#define INT8_MIN (-INT8_MAX - 1)
#define UINT8_MAX 255
#define INT16_MAX 32767
#define INT16_MIN (-INT16_MAX - 1)
#define UINT16_MAX 65535
#define INT32_MAX 2147483647
#define INT32_MIN (-INT32_MAX - 1)
#define UINT32_MAX 4294967295U
#define INT64_MAX 9223372036854775807L
#define INT64_MIN (-INT64_MAX - 1L)
#define UINT64_MAX 18446744073709551615UL

#define INT_LEAST8_MIN INT8_MIN
#define INT_LEAST8_MAX INT8_MAX
#define UINT_LEAST8_MAX UINT8_MAX
#define INT_LEAST16_MIN INT16_MIN
#define INT_LEAST16_MAX INT16_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MIN INT32_MIN
#define INT_LEAST32_MAX INT32_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MIN INT64_MIN
#define INT_LEAST64_MAX INT64_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MIN INT32_MIN
#define INT_FAST8_MAX INT32_MAX
#define UINT_FAST8_MAX UINT32_MAX
#define INT_FAST16_MIN INT32_MIN
#define INT_FAST16_MAX INT32_MAX
#define UINT_FAST16_MAX UINT32_MAX
#define INT_FAST32_MIN INT32_MIN
#define INT_FAST32_MAX INT32_MAX
#define UINT_FAST32_MAX UINT32_MAX
#define INT_FAST64_MIN INT64_MIN
#define INT_FAST64_MAX INT64_MAX
#define UINT_FAST64_MAX UINT64_MAX

#define INTPTR_MIN INT64_MIN
#define INTPTR_MAX INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX
#define SIZE_MAX UINT64_MAX
#define SIG_ATOMIC_MIN INT32_MIN
#define SIG_ATOMIC_MAX INT32_MAX
#define WCHAR_MIN INT32_MIN
#define WCHAR_MAX INT32_MAX
#define WINT_MIN 0U
#define WINT_MAX UINT32_MAX

/* Small unsigned constants still have type int after integer promotion. */
#define INT8_C(value) value
#define UINT8_C(value) value
#define INT16_C(value) value
#define UINT16_C(value) value
#define INT32_C(value) value
#define UINT32_C(value) value ## U
#define INT64_C(value) value ## L
#define UINT64_C(value) value ## UL
#define INTMAX_C(value) value ## L
#define UINTMAX_C(value) value ## UL

#if defined(__STDC_WANT_IEC_60559_BFP_EXT__) || __STDC_VERSION__ > 201710L
#define INT8_WIDTH 8
#define UINT8_WIDTH 8
#define INT16_WIDTH 16
#define UINT16_WIDTH 16
#define INT32_WIDTH 32
#define UINT32_WIDTH 32
#define INT64_WIDTH 64
#define UINT64_WIDTH 64

#define INT_LEAST8_WIDTH INT8_WIDTH
#define UINT_LEAST8_WIDTH UINT8_WIDTH
#define INT_LEAST16_WIDTH INT16_WIDTH
#define UINT_LEAST16_WIDTH UINT16_WIDTH
#define INT_LEAST32_WIDTH INT32_WIDTH
#define UINT_LEAST32_WIDTH UINT32_WIDTH
#define INT_LEAST64_WIDTH INT64_WIDTH
#define UINT_LEAST64_WIDTH UINT64_WIDTH

#define INT_FAST8_WIDTH INT32_WIDTH
#define UINT_FAST8_WIDTH UINT32_WIDTH
#define INT_FAST16_WIDTH INT32_WIDTH
#define UINT_FAST16_WIDTH UINT32_WIDTH
#define INT_FAST32_WIDTH INT32_WIDTH
#define UINT_FAST32_WIDTH UINT32_WIDTH
#define INT_FAST64_WIDTH INT64_WIDTH
#define UINT_FAST64_WIDTH UINT64_WIDTH

#define INTPTR_WIDTH INT64_WIDTH
#define UINTPTR_WIDTH UINT64_WIDTH
#define INTMAX_WIDTH INT64_WIDTH
#define UINTMAX_WIDTH UINT64_WIDTH
#define PTRDIFF_WIDTH INT64_WIDTH
#define SIZE_WIDTH UINT64_WIDTH
#define SIG_ATOMIC_WIDTH INT32_WIDTH
#define WCHAR_WIDTH INT32_WIDTH
#define WINT_WIDTH UINT32_WIDTH
#endif

#if __STDC_VERSION__ > 201710L
#define __STDC_VERSION_STDINT_H__ 202311L
#endif

#endif
