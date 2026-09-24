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
implementation or floating-point formatting is included.
