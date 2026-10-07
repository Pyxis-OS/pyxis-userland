# Import provenance

Imported from `chronium/pyxis-os` at
`14c71ccd179fb02ca01209fa63c8fd7b6ab4a27e` (the merged Pyxis toolchain milestone).
The source repository remains the archive of the complete kernel/userland
history. Commit messages referring to original PR numbers refer to Pyxis OS.

The extraction retained `userspace/` at the new repository root and
`third_party/tlsf/` at its existing path. Git filter-repo 2.47.0 preserved the
relevant authors, dates, messages and file history while rewriting commits to
contain those paths only:

```sh
git filter-repo --path userspace/ --path third_party/tlsf/ \
  --path-rename userspace/:
```

The filtered import ends at `a362c01`; subsequent commits adapt the extracted
build to a standalone checkout. Existing TLSF source, upstream pin and BSD
license were retained. Its userland adapter now selects libc support directly;
Pyxis retains its kernel copy. No new license was assigned to first-party code.

Pyxis continues to own the public ABI/format headers and shebang parser.
Userland consumes their exports through the SDK. Application
and runtime implementation files were not otherwise rewritten during extraction.
