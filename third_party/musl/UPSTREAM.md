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
- `src/math/fabsl.c`
- `src/math/copysignl.c`
- `src/math/x86_64/fmodl.c`

Local adaptation: `src/internal/libm.h` retains only musl's little-endian 80-bit
`ldshape` union, with Pyxis includes and compile-time layout checks. Unused
architectures and internal math machinery are not imported. The public
`libc/include/math.h` is a Pyxis header exposing just these functions, evaluation
types and musl's FP-exception-only error convention (`errno` is unchanged).

The build uses the Pyxis SDK's x86-64 SSE2/x87 ABI, with musl's
`-frounding-math -fexcess-precision=standard` options for these sources. The
x86-64 `fmodl` uses x87 FPREM and repeats while status bit C2 is set: large
exponent differences can require several partial-remainder steps. Its quotient
is truncated toward zero independently of the rounding mode. No libgcc or
host-libc replacement is imported.

All six functions are part of libc. No separate libm archive, allocation,
locale state, stream implementation or floating-point formatting is included.
