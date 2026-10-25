# x86 SHA-NI backend

`sha_ni.c` adapts the state permutations, four-round SHA-NI sequence and message
schedule of Go's `blockSHANI` from Go 1.27.3, pinned revision
`f93462692a63de8d56cc3059e94f558f253fbbbb`:

- `src/crypto/internal/fips140/sha256/sha256block_amd64.s`, `blockSHANI` only.
- Its generator, `src/crypto/internal/fips140/sha256/_asm/sha256block_amd64_asm.go`,
  carries `Copyright 2024 The Go Authors. All rights reserved.`

The source was read from the existing pinned toolchain source, without fetching
another upstream revision. The full upstream BSD license is retained in
[LICENSE.go-bsd](LICENSE.go-bsd). The adapted backend retains those BSD terms;
this provenance document follows the repository's MPL-2.0 terms.

Local adaptations use readable C intrinsics, caller-owned host-endian state,
complete-block counts and the shared scalar core's canonical round constants.
The four-vector schedule rolls forward sixteen words at a time. Runtime CPUID
checks require SHA, SSSE3 and SSE4.1. The compressor uses the function target
`sha,ssse3,sse4.1,no-avx`, and the translation unit must be compiled with the
baseline `-march=x86-64`. Only legacy SSE and SHA-NI encodings are permitted;
inspect generated code when changing compilers or flags. No AVX state support,
padding, digest initialization, allocation or runtime dispatch is added here.

Source SHA-256 checksums from the pinned toolchain checkout:

```
0c972dea5ff318728ecc5966ec82ae726e26b4f48de7925b7613f54dbad95514  sha256block_amd64.s
ab47a37cf77635b63764c1b613ffd8dbe5e2be3d224a7fa91be429a7799ac411  _asm/sha256block_amd64_asm.go
911f8f5782931320f5b8d1160a76365b83aea6447ee6c04fa6d5591467db9dad  LICENSE
```
