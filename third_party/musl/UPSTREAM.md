musl 1.2.5, pinned to commit
`0784374d561435f7c787a555aeab8ede699ed298`:
https://git.musl-libc.org/cgit/musl/commit/?id=0784374d561435f7c787a555aeab8ede699ed298

The BusyBox upstream tar prerequisites add `src/regex/fnmatch.c` from this
same pin. `libc/include/fnmatch.h` copies the upstream header, stripping
trailing whitespace only. These files retain musl's formatting and license
under `COPYRIGHT`.

`fnmatch.c` drops only the unused private `locale_impl.h` include. It uses the
existing stateless UTF-8 conversion and ASCII-only wide classification/folding;
there is no new locale state or Unicode table. The POSIX flags, leading-directory
and case-fold flags retain upstream behavior. Matching does not access files.

`COPYRIGHT` retains the complete upstream MIT license, contributor list and
third-party notices. The math/formatting subset falls under the MIT terms described
there; the pow implementation and coefficient tables also retain their Arm
copyright and MIT SPDX notices.

The following files are copied without changes, including upstream formatting:

- `src/math/scalbn.c`
- `src/math/scalbnl.c`
- `src/math/ldexpl.c`
- `src/math/fabs.c`
- `src/math/fabsl.c`
- `src/math/copysignl.c`
- `src/math/x86_64/fmodl.c`
- `src/math/modf.c` (libpng's conventional floating-point sCAL prerequisite)

Local adaptation: `src/internal/libm.h` retains musl's little-endian 80-bit
`ldshape` union, with Pyxis includes and compile-time layout checks, plus the
evaluation/bit helpers described below. Unused architectures and unrelated
internal math machinery are not imported. The public
`libc/include/math.h` is a Pyxis header exposing these functions, evaluation
types and musl's infinity/NaN constants. The math functions retain musl's
FP-exception-only error convention (`errno` is unchanged).

The build uses the Pyxis SDK's x86-64 SSE2/x87 ABI, with musl's
`-frounding-math -fexcess-precision=standard` options for these sources. The
x86-64 `fmodl` uses x87 FPREM and repeats while status bit C2 is set: large
exponent differences can require several partial-remainder steps. Its quotient
is truncated toward zero independently of the rounding mode. No libgcc or
host-libc replacement is imported.

The Lua core prerequisites add the following unmodified files from the same pin:

- `src/math/floor.c`, `src/math/frexp.c`, `src/math/ldexp.c`, `src/math/pow.c`
- `src/math/exp_data.c`, `src/math/pow_data.c`
- `src/math/__math_xflow.c`, `src/math/__math_uflow.c`,
  `src/math/__math_oflow.c`, `src/math/__math_invalid.c`

`src/math/fmod.c` is copied unmodified from the same pin.
`src/math/exp_data.h` and `src/math/pow_data.h` omit musl's `features.h` include
and internal `hidden` visibility marker; Pyxis links these libraries statically.
The coefficient tables and numerical algorithms are unchanged.

`src/internal/libm.h` now also extracts the required evaluation barriers,
forced-evaluation helpers, double bit conversions, branch prediction macros and
pow configuration from upstream. Internal error-helper declarations omit
`hidden`. The generic `TOINT_INTRINSICS=0` path matches musl's x86-64 build;
`WANT_ROUNDING=1` and `WANT_SNAN=0` retain upstream's policy. Signaling NaNs are
not promised. The build suppresses `-Wunused-but-set-variable` for musl sources
because its forced-evaluation helpers intentionally write unread volatile locals.

These functions remain in libc with no separate `-lm`: `floor` rounds downward,
`fmod` truncates the quotient toward zero, `frexp` splits a value into a fraction
and binary exponent, `ldexp` reuses `scalbn`, and `pow` includes its range/domain
handling. Errors leave errno unchanged and set FP exception flags. No public
fenv API, full math library or changes to compiler defaults are introduced.

The fastfetch formatting prerequisites add `src/math/round.c` unmodified from
the same 1.2.5 pin. It rounds to the nearest integral double, with halfway values
away from zero independently of the current FP rounding mode. Signed zero,
infinities and quiet NaNs are preserved; fractional finite inputs may raise
inexact. It uses the existing libm helpers and rounding-aware compilation flags,
leaves errno unchanged and lives in libc without a separate `-lm`.

The Quake port's prerequisites add these unmodified files from the same pin:

- `src/math/ceil.c` and `src/math/x86_64/sqrt.c` (SSE2 `sqrtsd`)
- `src/math/sin.c`, `src/math/cos.c`, `src/math/tan.c`
- `src/math/__sin.c`, `src/math/__cos.c`, `src/math/__tan.c`
- `src/math/__rem_pio2.c`, `src/math/__rem_pio2_large.c`

The C++ runtime prerequisites add `src/math/ceilf.c` unmodified from the same
pin; libc++'s unordered containers use it to size their buckets.

The SDL2 port's prerequisites add these unmodified files from the same pin:

- `src/math/roundf.c`, rounding halfway cases away from zero like `round`;
- `src/math/x86_64/sqrtf.c`, using SSE `sqrtss`;
- `src/string/wcslen.c`, the only wide-string function so far.

Like the rest of the subset, they live in libc and leave errno unchanged.

The Chocolate Doom port's prerequisites add these files from the same pin:

- `src/math/log.c`, `src/math/log_data.c` and `src/math/log10.c`, unmodified;
- `src/math/__math_divzero.c`, unmodified, which `log` uses for zero;
- `src/math/log_data.h`, omitting `features.h` and the `hidden` marker like
  `exp_data.h` and `pow_data.h`.

Chocolate Doom's textscreen sizes its number fields with `log`. `log10` comes
with it as the standard base-10 form. `src/internal/libm.h` declares
`__math_divzero` without `hidden`. Zero raises divide-by-zero and negative
values invalid; errno is unchanged.

The Chocolate Quake port's prerequisites add these files from the same pin:

- `src/math/powf.c`, `src/math/powf_data.c` and `src/math/exp2f_data.c`,
  unmodified;
- `src/math/__math_xflowf.c`, `src/math/__math_uflowf.c`,
  `src/math/__math_oflowf.c` and `src/math/__math_invalidf.c`, unmodified;
- `src/math/powf_data.h`, omitting the `hidden` marker, and
  `src/math/exp2f_data.h`, omitting `features.h` and `hidden`.

Chocolate Quake's gamepad stick easing uses `powf`. `src/internal/libm.h` adds
upstream's `eval_as_float`, `fp_barrierf`, `asuint` and `asfloat` helpers and
declares the four float error helpers without `hidden`. Overflow, underflow
and invalid operands raise FP exception flags; errno is unchanged.

The EDuke32 port's prerequisites add these files from the same pin:

- `src/math/sinf.c`, `cosf.c`, `tanf.c`, `__sindf.c`, `__cosdf.c`, `__tandf.c`
  and `__rem_pio2f.c`, unmodified; large arguments use the existing
  `__rem_pio2_large`;
- `src/math/acosf.c`, `atanf.c` and `atan2f.c`, unmodified;
- `src/math/expf.c`, unmodified, using the existing `exp2f_data`;
- `src/math/logf.c`, `logf_data.c` and `log10f.c`, unmodified, and
  `logf_data.h`, omitting `features.h` and the `hidden` marker;
- `src/math/floorf.c`, `fabsf.c`, `rintf.c` and `fmodf.c`, unmodified;
- `src/math/x86_64/lrint.c`, `lrintf.c` and `llrintf.c`, unmodified, using
  SSE `cvtsd2si` and `cvtss2si` in the current rounding mode;
- `src/math/__math_divzerof.c`, unmodified, which `logf` uses for zero.

`src/internal/libm.h` adds upstream's `GET_FLOAT_WORD` and `SET_FLOAT_WORD`
and declares `__rem_pio2f`, `__sindf`, `__cosdf`, `__tandf` and
`__math_divzerof` without `hidden`. The public `math.h` also gains POSIX's
`M_*` constants with musl's values. Errors raise FP exception flags; errno is
unchanged.

`src/math/atan.c` and `src/math/atan2.c` are copied unmodified from the pin.
`src/internal/libm.h` adds upstream's
`EXTRACT_WORDS`, `GET_HIGH_WORD`, `INSERT_WORDS` and `SET_LOW_WORD` macros and
the `__rem_pio2*`, `__sin`, `__cos` and `__tan` declarations without `hidden`.
Trigonometric arguments of any magnitude use musl's exact reduction modulo pi/2.
`atan2.c` keeps upstream's sign-bit expression under `-Wno-parentheses`, and
`__rem_pio2_large.c` builds with `-Wno-maybe-uninitialized`: its callers pass
precisions that always fill the reported array. Errors leave errno unchanged;
everything lives in libc without a separate `-lm`.

`src/internal/floatscan.c` comes from the same pin, with local adaptations:

- Replace FILE/shgetc plumbing with the direct string cursor in the local
  `floatscan.h`. The scanner consumes the NUL at most once before returning or
  pushing it back. Public `strto*` wrappers always enable musl's existing
  prefix-permitted mode, including rollback of incomplete exponents and NaNs.
- Name the three precision selectors and apply target-specific normal/maximal
  bounds to numeric results. All nonzero subnormals set ERANGE, even exact ones.
  Literal infinity/NaN bypass those checks. Detect hexadecimal overflow before
  scaling as well, since directed rounding can produce a finite result.
- Keep the decimal/hex numerical algorithms, rounding and upstream formatting.
  Compile this file with -Wno-sign-compare and -Wno-parentheses for its existing
  unsigned digit tests and ring-buffer expressions; other warnings remain on.

The Pyxis `libc/strtod.c` wrappers replace musl's stream-based wrappers. They
restore errno when no conversion occurs, instead of exposing musl's EINVAL,
and use the cursor directly for end pointers. They share the scanner's
rounding-aware compilation flags. Normal successful conversions preserve errno;
overflow, nonzero subnormal results and nonzero input rounded to zero set ERANGE.
Syntax is ASCII, uses '.', and accepts case-insensitive infinity/NaN. NaN payload
text is consumed without selecting payload bits or preserving its sign.

The decimal scanner uses an 8 KiB automatic digit workspace on x86-64. It keeps
sticky information when that workspace fills, continuing to consume digits;
there is no heap allocation or artificial input-length limit.

All functions are part of libc. No separate libm archive, locale state, stream
implementation is imported.

`src/time/__secs_to_tm.c` is copied unmodified from the same pin. The local
`src/internal/time_impl.h` declares only that UTC conversion routine. Pyxis owns
`time.h`, the clock-backed time/timespec_get calls, and gmtime/gmtime_r wrappers.
The conversion uses the proleptic Gregorian calendar and rejects years that
cannot fit tm_year. Existing COPYRIGHT covers this file too. Additional
local-time arithmetic is described below; locale is not imported.

`src/math/frexpl.c` is copied unmodified from the same pin.
`src/stdio/format_float.c` extracts `fmt_u` and `fmt_fp` from that pin's
`src/stdio/vfprintf.c`, retaining the numerical algorithm and upstream formatting.
Local adaptations replace FILE output/padding with libc's bounded buffer helpers,
retain the digit table terminator, map named formatting flags, and widen
precision/rounding-position arithmetic to int64_t so extreme requested
precisions cannot overflow before the output-length checks. The entry point is
renamed `format_float`; unrelated stream, integer and wide formatting is omitted.
The source uses the same two warning exceptions as floatscan for upstream idioms.

Pyxis's existing format parser supplies double or long-double arguments and owns
termination, truncation and errno handling. Padding remains proportional to the
available buffer, even for enormous precision/width. The decimal conversion has
an approximately 8 KiB automatic workspace and does not allocate. It uses the
current FP rounding mode; the process default is round-to-nearest, ties-to-even.
No locale or separate libm is required.

Local-time conversion adds `src/time/__year_to_secs.c` and
`src/time/__month_to_secs.c` unmodified from the same 1.2.5 pin.
`src/time/rule_to_secs.c` extracts rule-to-calendar and UTC-year calculations from that
pin's `src/time/__tz.c`, retaining upstream formatting. Local adaptations use
named rule fields, a long-long year for neighboring years at tm_year's limits,
an explicit month-length table, and bound the year estimate before arithmetic
while allowing neighboring UTC years at the local calendar limits. Weekday
arithmetic and the rule-year boundary comparison retain the pinned expressions.

Pyxis's `libc/timezone.c` owns checked TZif parsing, capability-path file loading,
cache lifetime and error propagation. It evaluates adjacent rule years to handle
transitions across UTC New Year, including southern-hemisphere/all-year DST.
Musl's host filesystem lookup, mmap, POSIX TZ environment strings, `mktime`,
global timezone names and silent UTC fallback are not imported.
TZif handling follows RFC 9636; only leap-free version 2/3/4 data is supported.

The POSIX regex subset adds these files from the same pin:

- `src/regex/regcomp.c` (also implements `regfree`), `src/regex/regexec.c`
  and `src/regex/tre-mem.c`, copied without algorithm or formatting changes.
- `src/regex/tre.h`, omitting only the internal `hidden` visibility markers.
- `src/regex/regerror.c`, omitting the `locale_impl.h` include and locale
  translation, and marking its unused `preg` argument explicitly.

TRE's 2-clause BSD notices remain in each applicable source file;
`TRE-COPYRIGHT` retains the notice for SDK/binary distribution, alongside musl's
complete `COPYRIGHT`. The Pyxis public `regex.h` retains musl's flag/error values
and LP64 `regoff_t`, but provides only `re_nsub` and the engine pointer in
`regex_t`, without musl's compatibility padding. The existing runtime archive
owns all four functions; there is no separate regex library.

The build suppresses `-Wsign-compare` for the compiler/matcher and
`-Wimplicit-fallthrough` for the parser's intentional fallthroughs, preserving
upstream code. Regex uses Pyxis's stateless UTF-8 `mbtowc` and ASCII-only wide
classification/folding, not musl's locale state or Unicode tables.

Owner decision, 2026-10-07: retain the pinned engine's back-reference behavior.
Back-reference comparison remains bytewise even with `REG_ICASE`, and the
backtracking path assumes a one-byte lookahead and does not fully restore
variable-width decoding state. UTF-8 back-reference matching is not reliable.
Invalid subject UTF-8 returns `REG_NOMATCH` if the matcher encounters it; no
whole-string validation is added. These limits are documented by Pyxis and are
not silently repaired in the vendor import.

Lua 5.1's math library adds these files from the same pin:

- `src/math/asin.c` and `src/math/acos.c`, unmodified, using the existing
  `sqrt` and `fabs`;
- `src/math/exp.c`, unmodified, using the existing `exp_data` tables and error
  helpers;
- `src/math/sinh.c`, `cosh.c` and `tanh.c`, unmodified, with
  `src/math/__expo2.c`, unmodified, and `src/math/expm1.c`, also unmodified,
  which they use for large and small arguments.

`src/internal/libm.h` adds upstream's `GET_LOW_WORD` macro and declares
`__expo2` without `hidden`; the public `math.h` also declares `expm1`. Out-of-domain arguments return NaN with invalid,
and overflow and underflow raise FP exception flags; errno is unchanged.

The hosted Clang/LLD closure adds these files from the same pin, using the
existing local source cache at `pyxis-regex/build/upstream-musl` (Git HEAD
`0784374d561435f7c787a555aeab8ede699ed298`), without a new download:

- `src/math/log2.c`, `log2_data.c`, `log2f.c` and `log2f_data.c`, unmodified;
- `src/math/log2_data.h` and `log2f_data.h`, omitting `features.h` and `hidden`
  like the other static coefficient tables;
- `src/math/logb.c` and its `src/math/ilogb.c` dependency, unmodified;
- `src/math/log1p.c` and `erf.c`, unmodified; `erf.c` also implements `erfc`;
- `src/math/exp2.c`, unmodified, reusing `exp_data` and existing error helpers;
- `src/math/lround.c`, unmodified, reusing `round`.

The additions retain the existing FP-exception-only profile, rounding-aware
build flags and unsupported signaling-NaN limit. Logarithm zero poles raise
divide-by-zero and negative-domain inputs raise invalid. `log1p` preserves
accuracy near zero; `logb` and `ilogb` handle subnormal exponents. `ilogb` raises
invalid for zero, infinity and NaN with the declared exceptional results.
`exp2`, `erf` and `erfc` retain upstream's range and special-value handling.
`lround` rounds halfway values away from zero independently of rounding mode;
because it calls `round`, fractional inputs may raise inexact. Its integer
result is unspecified for NaN, infinity or values outside `long` range; no
Annex F or POSIX exception-profile promise is added. Errno remains unchanged,
and all functions live in libc without a separate `-lm`.

Neovim's calendar and encoding prerequisites add these files from the same pin:

- `src/time/__tm_to_secs.c`, unmodified, which normalizes `tm_mon` into the
  year and adds the other fields as 64-bit seconds;
- `src/math/trunc.c`, unmodified;
- `src/locale/iconv_close.c`, unmodified;
- `src/locale/iconv.c`, reduced to UTF-8, ASCII, ISO-8859-1 and UTF-16LE/BE.

Pyxis's `libc/time.c` owns `mktime`. Instead of musl's single DST adjustment,
it tries every offset the selected zone can report and accepts the instants
whose `localtime` offset matches, so gaps and ambiguous folds fail with ENOTSUP
rather than being guessed.

`iconv.c` keeps upstream's charmap format, fuzzy name matching, descriptor
encoding, UTF-16 surrogate handling, error paths and formatting, with these
local changes, each marked `Pyxis:` in the source:

- Only the five encodings above remain, with upstream's aliases for them
  (`utf8`, `utf16le`, `utf16be`, `ascii`, `usascii`, `iso646`, `iso646us`,
  `iso88591`, `latin1`). The `char` alias, wide, UCS-2, UTF-32, BOM-detecting
  UTF-16, multibyte East Asian and legacy codepage converters and their tables
  are left out, so no `codepages.h`, `legacychars.h` or CJK tables are imported.
  ISO-8859-1 keeps upstream's codepage entry, which has no table.
- An empty name fails instead of selecting the locale charset, and no
  descriptor allocates, because no remaining encoding has state.
- UTF-8 input uses a local strict decoder instead of switching the locale to
  run `mbrtowc`. It returns EINVAL only when the input ends inside a prefix of
  a valid sequence; overlong forms, surrogates, values above U+10FFFF and bytes
  that cannot continue a sequence are EILSEQ. UTF-8 output uses a local
  encoder instead of `wctomb`.
- A valid character the destination cannot represent fails with EILSEQ at that
  character, instead of being counted and replaced with `*`.

The build suppresses `-Wsign-compare` for upstream's `(size_t)-1` lookup
comparisons. Errors are reported through errno: `E2BIG` is added for a full
output buffer.

The jq port's prerequisites add the remaining double-precision functions of
ISO C `<math.h>` from the same pin. `src/math/acosh.c`, `asinh.c`, `atanh.c`,
`cbrt.c`, `copysign.c`, `fdim.c`, `fmax.c`, `fmin.c`, `hypot.c`, `nearbyint.c`,
`nextafter.c`, `nexttoward.c`, `remquo.c`, `scalbln.c` and `tgamma.c` are
unmodified. Three files change one construct each:

- `fma.c` drops `atomic.h` and calls `__builtin_clzll` where musl used its
  `a_clz_64`;
- `remainder.c` drops the `drem` alias and `lgamma_r.c` the `lgamma_r` alias,
  because `weak_alias` is musl's internal macro and neither name is ISO C.

`lgamma` is Pyxis's own `libc/lgamma.c`: it calls musl's `__lgamma_r` and
discards the sign. musl's `lgamma` instead stores the sign in the `signgam`
global shared by all threads, which Pyxis does not provide. The float and long
double forms, the `lround` and `llround` families, `j0` to `yn`, `gamma`, `drem`,
`significand`, `scalb`, `exp10` and `pow10` are not imported. Errors raise FP
exception flags and leave errno unchanged, as in the rest of the subset.

`fma.c` is built with `-Wno-shift-op-parentheses` to keep upstream's
expressions. The files were read from a GitHub mirror of this commit through the
`raw-github` repository on `repo.internal`
(`kraj/musl/archive/0784374d561435f7c787a555aeab8ede699ed298.tar.gz`, SHA-256
`6ad0ee58d5be7b0d8c0f1d8e9773e1d83c761f87c1f527a3e8cdf77128696964`), because the
upstream cgit is not mirrored; a commit id fixes the tree, and five files
already in this directory matched it byte for byte.

The raylib port's prerequisites add `src/math/asinf.c`, `fminf.c`, `fmaxf.c`
and `hypotf.c`, unmodified from the same pin, using the existing local source
cache at `pyxis-regex/build/upstream-musl` (Git HEAD
`0784374d561435f7c787a555aeab8ede699ed298`), without a new download.
`asinf` reuses `fabsf` and `sqrt`; `hypotf` scales its operands before squaring
and reuses `sqrtf`. `fminf` and `fmaxf` select the non-NaN operand when exactly
one is NaN and preserve the minimum/maximum signed zero respectively.
The existing FP-exception-only error profile, rounding-aware compilation flags
and unsupported signaling-NaN limit apply. All four functions live in libc,
leave errno unchanged and require no separate `-lm`.

Mesa softpipe prerequisites add these unmodified files from the same pin:
`src/math/fmaf.c`, `truncf.c`, `ldexpf.c`, `scalbnf.c`, `frexpf.c`, `exp2f.c`,
`copysignf.c`, `llrint.c`, `llround.c` and `lroundf.c` (all under `src/math/`).
The existing exp2f coefficient table and math helpers supply their remaining
closure. `fmaf.c` retains David Schultz's BSD-2-Clause notice; its floating-point
exception/rounding operations use the real libc fenv implementation. Other
files retain the upstream musl notices, including Arm's MIT notice in exp2f.
No port-local numerical approximations or errno changes are introduced.

The `getopt`, `getopt_long` and `getopt_long_only` functions come from the same
1.2.5 pin: `src/misc/getopt.c` and `src/misc/getopt_long.c`, unmodified, from a
local copy of the release tarball (SHA-256 of `musl-1.2.5.tar.gz`
`a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`), whose
`COPYRIGHT` equals the one here and whose other files already imported match
byte for byte. They include musl's private `locale_impl.h` and `stdio_impl.h`,
which Pyxis replaces with minimal local headers in `src/internal`:

- `locale_impl.h` defines `__lctrans_cur` as the identity, since libc has no
  message catalogs, and musl's `weak_alias`, used for the BSD `optreset` and an
  unused `__posix_getopt` alias.
- `stdio_impl.h` includes `stdio.h`, defines `FLOCK` and `FUNLOCK` as nothing,
  because libc's FILE synchronizes itself, and declares `__getopt_msg`.

Diagnostics go to `stderr` with `argv[0]` as the prefix, as in musl, in four
separate writes without musl's stream lock. All state is process-wide:
`optarg`, `optind`, `opterr`, `optopt` and the internal position are not
per-thread, so concurrent parsing needs the caller's own exclusion. Pyxis
supplies `getopt.h` and the `unistd.h` declarations; musl's `getopt.h` is not
copied. Multibyte option characters decode through the stateless UTF-8
`mbtowc`, and `getopt_long_only` measures them with libc's `mblen`; an invalid
byte reports as U+FFFD.
