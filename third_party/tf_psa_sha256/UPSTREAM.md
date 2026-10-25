# TF-PSA scalar SHA-256 compression

TF-PSA-Crypto 1.1.1, commit `a0632be94d883daa0295ac1eabf70359ad94f91b`,
from `drivers/builtin/src/sha256.c`, bundled in the already mirrored
Mbed TLS 4.1.1 release (Mbed TLS commit
`0a8fda272a5a0abef3b47c91bed37185d5a726b1`).

- Upstream: https://github.com/Mbed-TLS/TF-PSA-Crypto
- Cache: https://repo.internal/repository/raw-github/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2
- Archive SHA-256: `3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c`.
- Original sha256.c SHA-256: `f1c3018055c8fe04722a99fdb18ff13bc871113ff5295e3ca5ee2eb3d652b5ad`.

The retained source notice offers Apache-2.0 or GPL-2.0-or-later; Pyxis uses
Apache-2.0. LICENSE is the upstream full licence notice.

Only the round constants, scalar macros and default unrolled compression
function are selected. The smaller-code configuration, context initialization,
padding and ARM backend are not imported. Adaptations: the state is eight
caller-owned words rather than a Mbed TLS context; byte loads use a local
endian helper; round constants have one shared symbol for both backends;
the upstream volatile-memset/assembly-barrier strategy from
`platform/platform_util.c` preserves local-state erasure; a block
loop implements the internal compression interface. Streaming and runtime
architecture dispatch live outside the vendored core. No download beyond the
existing pinned cache is needed.

Original platform_util.c SHA-256: `8b2abd057d159e2d924cc30d824baeb74411460f1d2a5b1b6256bb0c54bc7330`. Only the GNU C zeroization branch is needed by the supported compilers.
