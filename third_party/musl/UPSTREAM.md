musl 1.2.5, pinned to commit
`0784374d561435f7c787a555aeab8ede699ed298`:
https://git.musl-libc.org/cgit/musl/commit/?id=0784374d561435f7c787a555aeab8ede699ed298

`COPYRIGHT` retains the complete upstream MIT license, contributor list and
third-party notices. The selected files have no separate file-level license;
they fall under musl's MIT terms as described there.

The following files are copied without changes, including upstream formatting:

- `src/math/scalbn.c`
- `src/math/scalbnl.c`
- `src/math/ldexpl.c`
- `src/math/fabs.c`
- `src/math/fabsl.c`
- `src/math/copysignl.c`
- `src/math/x86_64/fmodl.c`

Local adaptation: `src/internal/libm.h` retains only musl's little-endian 80-bit
`ldshape` union, with Pyxis includes and compile-time layout checks. Unused
architectures and internal math machinery are not imported. The public
`libc/include/math.h` is a Pyxis header exposing these functions, evaluation
types and musl's infinity/NaN constants. The math functions retain musl's
FP-exception-only error convention (`errno` is unchanged).

The build uses the Pyxis SDK's x86-64 SSE2/x87 ABI, with musl's
`-frounding-math -fexcess-precision=standard` options for these sources. The
x86-64 `fmodl` uses x87 FPREM and repeats while status bit C2 is set: large
exponent differences can require several partial-remainder steps. Its quotient
is truncated toward zero independently of the rounding mode. No libgcc or
host-libc replacement is imported.

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
