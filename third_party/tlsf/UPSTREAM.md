Matthew Conte TLSF 3.1, pinned to commit
`deff9ab509341f264addbd3c8ada533678591905`:
https://github.com/mattconte/tlsf/commit/deff9ab509341f264addbd3c8ada533678591905

`tlsf.h` retains the complete BSD-3-Clause license. Both source files retain
upstream formatting and line endings. Local change to `tlsf.c`: replace the six
host includes with `caelum.h`, the adapter retained from the Pyxis import. It
includes `libc/tlsf_user.h` for freestanding memory routines, diagnostics and
always-enabled assertions. Assertion failure logs without allocating and exits
its own process. `_DEBUG` is not enabled, so
upstream's optional bit-scan self-test is not compiled or run. Assertions are
independent of `_DEBUG` and `NDEBUG`.

Second local change to `tlsf.c`: the four block flag setters write the size word
through `block_store_size`, a relaxed `__atomic_store_n` of the whole word. Clang
narrowed `size |= bit` to a one-byte store, and TLSF's next whole-word read of the
same field then stalled on store forwarding. Under Clang 23 this made userland
`allocbench heap` about 30% slower than with GCC. A relaxed atomic store is an
ordinary full-width store that compilers do not narrow; it adds no
synchronization.

The capacity adaptation derives the exclusive size-class maximum from positive
`ptrdiff_t` representability (2^63 on this target), with a `size_t` first-level
bitmap, full-width scans/masks and bounded terminal-bin searches. Request
normalization, search-class rounding and leading-split allowance are checked
before arithmetic. `tlsf_memalign_pool_size` shares those calculations and returns
the exact base-aligned pool footprint required by `tlsf_memalign`, or zero for
an unrepresentable request. Pool admission rejects undersized inputs before
subtraction and rejects the exclusive maximum. Failed realloc normalization
leaves the original allocation intact. Alignment pointer arithmetic is unsigned.

The libc wrapper uses `tlsf_memalign(..., 16, ...)` for ordinary allocation and
accepts larger power-of-two alignments. Growth adds its native pool prefix and
page rounding with overflow checks, rather than a fractional size allowance or
half-maximum request ceiling. Native allocation failures retain meaningful errno
values. No host libc is linked. Pyxis maintains a separate kernel copy at the
same upstream pin; its adapter and future dependency updates are maintained
independently.
