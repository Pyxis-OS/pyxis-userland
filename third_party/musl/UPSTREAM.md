musl 1.2.5, pinned to commit
`0784374d561435f7c787a555aeab8ede699ed298`:
https://git.musl-libc.org/cgit/musl/commit/?id=0784374d561435f7c787a555aeab8ede699ed298

`COPYRIGHT` retains the complete upstream MIT license, contributor list and
third-party notices. The selected files fall under the MIT terms described
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

`src/math/fmod.c` replaces only `isnan` with the compiler's `__builtin_isnan`,
as used elsewhere in this subset. It does not add a public classification API.
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

`src/math/atan.c` and `src/math/atan2.c` replace only `isnan` with
`__builtin_isnan`, as in `fmod.c`. `src/internal/libm.h` adds upstream's
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
retain the digit table terminator, map named formatting flags, use compiler builtins for FP classification, and
widen precision/rounding-position arithmetic to int64_t so extreme requested
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
an explicit month-length table, normalize negative weekdays before 1970, and
bound the year estimate before arithmetic while allowing neighboring UTC years
at the local calendar limits. Exact New Year belongs to the new rule year.

Pyxis's `libc/timezone.c` owns checked TZif parsing, capability-path file loading,
cache lifetime and error propagation. It evaluates adjacent rule years to handle
transitions across UTC New Year, including southern-hemisphere/all-year DST.
Musl's host filesystem lookup, mmap, POSIX TZ environment strings, reverse-time
conversion, global timezone names and silent UTC fallback are not imported.
TZif handling follows RFC 9636; only leap-free version 2/3/4 data is supported.
