#ifndef LIBC_LIMITS_H
#define LIBC_LIMITS_H

/* Pyxis x86-64 LP64 limits, with stateless UTF-8 multibyte conversion. */
#define CHAR_BIT 8
#define MB_LEN_MAX 4
#define RE_DUP_MAX 255
#define CHARCLASS_NAME_MAX 14
/* Buffer bound for libc realpath only; ordinary native path lookup has no
 * fixed path-length bound. Includes the terminating NUL. */
#define PATH_MAX 4096
/* Longest path component, in bytes, that native (npfs) and host directories
 * accept; longer names fail with ENAMETOOLONG there. RAM directories accept
 * longer names. There is no pathconf: libc cannot ask which backing a
 * directory has. */
#define NAME_MAX 255

#define SCHAR_MAX 127
#define SCHAR_MIN (-SCHAR_MAX - 1)
#define UCHAR_MAX 255
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif

#define SHRT_MAX 32767
#define SHRT_MIN (-SHRT_MAX - 1)
#define USHRT_MAX 65535
#define INT_MAX 2147483647
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX 4294967295U
#define LONG_MAX 9223372036854775807L
#define LONG_MIN (-LONG_MAX - 1L)
#define ULONG_MAX 18446744073709551615UL
#define SSIZE_MAX LONG_MAX
#define LLONG_MAX 9223372036854775807LL
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX 18446744073709551615ULL

#ifndef __STRICT_ANSI__
#define LONG_LONG_MIN LLONG_MIN
#define LONG_LONG_MAX LLONG_MAX
#define ULONG_LONG_MAX ULLONG_MAX
#endif

#if defined(__STDC_WANT_IEC_60559_BFP_EXT__) || __STDC_VERSION__ > 201710L
#define CHAR_WIDTH 8
#define SCHAR_WIDTH 8
#define UCHAR_WIDTH 8
#define SHRT_WIDTH 16
#define USHRT_WIDTH 16
#define INT_WIDTH 32
#define UINT_WIDTH 32
#define LONG_WIDTH 64
#define ULONG_WIDTH 64
#define LLONG_WIDTH 64
#define ULLONG_WIDTH 64
#endif

#if __STDC_VERSION__ > 201710L
#define BOOL_MAX 1
#define BOOL_WIDTH 1
/* Bit-precise integer support is a compiler capability, not an LP64 property. */
#ifdef __BITINT_MAXWIDTH__
#define BITINT_MAXWIDTH __BITINT_MAXWIDTH__
#endif
#define __STDC_VERSION_LIMITS_H__ 202311L
#endif

#endif
