# Licensing

Except for the third-party material identified below and files with their own
license notices, original Pyxis source code, build/configuration files and
project documentation in this repository are licensed under the Mozilla Public
License, version 2.0. The complete, unmodified license is in [LICENSE](LICENSE).

This Source Code Form is subject to the terms of the Mozilla Public License,
v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain
one at https://mozilla.org/MPL/2.0/.

This directory-level notice applies to the original material described above;
it does not replace third-party notices. No Exhibit B incompatibility notice is
applied: MPL's standard secondary-license provisions remain available.

## Third-party and separately licensed material

- `git/patches/`: adaptations of go-git v5.19.3 and go-billy v5.9.2 retain
  their Apache-2.0 terms. The fixed module graph and checksums are in
  `git/go.mod` and `git/go.sum`; the optional build stages the complete notices
  for every compiled dependency. The first-party CLI remains MPL-2.0.
- `third_party/tlsf/`: imported allocator sources and their local adaptations
  retain the BSD terms in [tlsf.h](third_party/tlsf/tlsf.h), with provenance in
  [UPSTREAM.md](third_party/tlsf/UPSTREAM.md).
- `third_party/musl/`: imported and adapted math, conversion, formatting, sorting,
  option-parsing and time routines retain their upstream notices and
  [COPYRIGHT](third_party/musl/COPYRIGHT). The precise source selection and local
  changes are recorded in [UPSTREAM.md](third_party/musl/UPSTREAM.md).
- `libc/include/math.h` includes musl-derived infinity/NaN definitions. Those
  portions retain musl's terms; the original Pyxis portions follow MPL. Preserve
  musl's notice when redistributing this header.
- `libc/include/fnmatch.h` is imported from the pinned musl release and retains
  its MIT terms in [COPYRIGHT](third_party/musl/COPYRIGHT).
- `third_party/stb_truetype/`: pinned font rasterizer and local allocation-failure
  adaptations use the MIT option in [LICENSE](third_party/stb_truetype/LICENSE);
  source, cache checksum and changes are recorded in
  [UPSTREAM.md](third_party/stb_truetype/UPSTREAM.md). Its parser is for trusted
  font files only.
- `third_party/tf_psa_sha256/`: scalar SHA-256 compression extracted from pinned
  TF-PSA-Crypto 1.1.1 uses its Apache-2.0 option. Full licence and local changes
  are in [LICENSE](third_party/tf_psa_sha256/LICENSE) and
  [UPSTREAM.md](third_party/tf_psa_sha256/UPSTREAM.md).
- `libsha256/arch/x86_64/`: the SHA-NI backend adapts pinned Go SHA-256 round
  scheduling, preserving [Go BSD terms](libsha256/arch/x86_64/LICENSE.go-bsd)
  and [provenance](libsha256/arch/x86_64/UPSTREAM.md).
- The shebang parser, ABI headers and libraries consumed from an external SDK or
  ports installation keep their supplied licenses. They are not relicensed by
  inclusion in a userland build.

Pyxis-authored provenance documents are covered by MPL unless separately marked.
The import history in [IMPORT.md](IMPORT.md) does not replace upstream notices.
Executables and runtime libraries may combine original and third-party code;
preserve the licenses for every included component.
