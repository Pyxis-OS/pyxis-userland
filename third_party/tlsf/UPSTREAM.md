Matthew Conte TLSF 3.1, pinned to commit
`deff9ab509341f264addbd3c8ada533678591905`:
https://github.com/mattconte/tlsf/commit/deff9ab509341f264addbd3c8ada533678591905

`tlsf.h` retains the complete BSD-3-Clause license. Both source files retain
upstream formatting and line endings. Local change to `tlsf.c`: replace the six
host includes with `caelum.h`, which supplies freestanding memory routines,
diagnostics and always-enabled assertions. The adapter selects kernel support
by default and `userspace/libc/tlsf_user.h` when compiled with `TLSF_USERSPACE`;
userspace assertion failure logs without allocating and exits its own process.
Allocator logic is unchanged. `_DEBUG` is not enabled, so upstream's optional bit-scan self-test is
not compiled or run. Assertions are independent of `_DEBUG` and `NDEBUG`.

The kernel and userspace wrappers use `tlsf_memalign(..., 16, ...)`, checked pool
sizing and conservative request limits below TLSF's maximum bin. They compile
separate copies of the same pinned allocator; no host libc is linked.
