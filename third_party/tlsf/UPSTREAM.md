Matthew Conte TLSF 3.1, pinned to commit
`deff9ab509341f264addbd3c8ada533678591905`:
https://github.com/mattconte/tlsf/commit/deff9ab509341f264addbd3c8ada533678591905

`tlsf.h` retains the complete BSD-3-Clause license. Both source files retain
upstream formatting and line endings. Local change to `tlsf.c`: replace the six
host includes with `caelum.h`, which supplies freestanding memory routines,
serial diagnostics and always-enabled panic assertions. Allocator logic is
unchanged. `_DEBUG` is not enabled, so upstream's optional bit-scan self-test is
not compiled or run. Assertions are independent of `_DEBUG` and `NDEBUG`.

The kernel wrapper uses `tlsf_memalign(..., 16, ...)`, checked pool sizing and
conservative request limits below TLSF's maximum bin. No host libc is linked.
