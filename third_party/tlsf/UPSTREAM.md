Matthew Conte TLSF 3.1, pinned to commit
`deff9ab509341f264addbd3c8ada533678591905`:
https://github.com/mattconte/tlsf/commit/deff9ab509341f264addbd3c8ada533678591905

`tlsf.h` retains the complete BSD-3-Clause license. Both source files retain
upstream formatting and line endings. Local change to `tlsf.c`: replace the six
host includes with `caelum.h`, the adapter retained from the Pyxis import. It
includes `libc/tlsf_user.h` for freestanding memory routines, diagnostics and
always-enabled assertions. Assertion failure logs without allocating and exits
its own process. Allocator logic is unchanged. `_DEBUG` is not enabled, so
upstream's optional bit-scan self-test is not compiled or run. Assertions are
independent of `_DEBUG` and `NDEBUG`.

The libc wrapper uses `tlsf_memalign(..., 16, ...)`, checked pool sizing and
conservative request limits below TLSF's maximum bin. No host libc is linked.
Pyxis maintains a separate kernel copy at the same upstream pin; its adapter
and future dependency updates are maintained independently.
