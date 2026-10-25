# libsha256

`include/sha256.h` exposes caller-owned incremental SHA-256 and compression-only
integration for existing SHA-224/SHA-256 providers. No allocator, clock,
entropy source or capability is needed. Contexts require initialization and
cannot be concurrently mutated. Updates and finalization report failure; a
finished or failed stream must be initialized before reuse.

```sh
make -f runtime.mk SDK=/absolute/sdk BUILD=/absolute/runtime libsha256
```

Link the standalone archive with `-lsha256` before the normal runtime libraries.
The parent exports the header, archive, static pkg-config metadata and licences
with the SDK. The streaming code is generic; dispatch and legacy-SSE SHA-NI
live in `arch/x86_64`. The portable core and x86 adaptation each retain their
pinned upstream licence/provenance. AVX state is never required or enabled.
