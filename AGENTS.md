# Pyxis userland development

- Use freestanding GNU C23, snake_case, two spaces, K&R control braces and
  function braces on their own line. Preserve upstream vendor formatting.
- Build against an explicitly selected Pyxis SDK and prebuilt target compiler.
  Public ABI/format headers and the shared shebang parser are owned by Pyxis;
  consume them through the SDK, not parent-checkout paths or manual copies.
- Keep libc, libpyxis, libterm and applications distinct. Never link host libc.
  Keep allocation ownership, overflow checks and failure unwinding explicit.
- Pin dependencies and record their licenses and local adaptations.
- Validate with ordinary builds, QEMU boots through Pyxis and debugger
  inspection. Do not add tests, self-tests, fault injection, CI or boot/output
  automation unless explicitly requested.
- Make focused commits and keep README short and practical. Document interface
  invariants beside the code.
- Do not preserve backwards compatibility unless requested. Replace obsolete
  interfaces, formats and implementations outright and update consumers together.
- Do not increment version fields merely because implementation changed; new
  versions are for intentional coexistence or required migration.
