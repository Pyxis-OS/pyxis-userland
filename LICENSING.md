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

- `third_party/tlsf/`: imported allocator sources and their local adaptations
  retain the BSD terms in [tlsf.h](third_party/tlsf/tlsf.h), with provenance in
  [UPSTREAM.md](third_party/tlsf/UPSTREAM.md).
- `third_party/musl/`: imported and adapted math, conversion, formatting, sorting
  and time routines retain their upstream notices and
  [COPYRIGHT](third_party/musl/COPYRIGHT). The precise source selection and local
  changes are recorded in [UPSTREAM.md](third_party/musl/UPSTREAM.md).
- `libc/include/math.h` includes musl-derived infinity/NaN definitions. Those
  portions retain musl's terms; the original Pyxis portions follow MPL. Preserve
  musl's notice when redistributing this header.
- The shebang parser, ABI headers and libraries consumed from an external SDK or
  ports installation keep their supplied licenses. They are not relicensed by
  inclusion in a userland build.

Pyxis-authored provenance documents are covered by MPL unless separately marked.
The import history in [IMPORT.md](IMPORT.md) does not replace upstream notices.
Executables and runtime libraries may combine original and third-party code;
preserve the licenses for every included component.
