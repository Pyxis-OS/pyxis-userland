# ln

`ln [--] source... destination` creates exclusive hard links to regular files
within one writable NPFS volume. An existing directory destination receives each
source basename. Multiple-source operation may partially succeed; failure exits
nonzero. No symbolic-link, force or copying fallback is supported.

Source-parent grants need LOOKUP, READ_FILES and WRITE_FILES; destination-parent
grants need LOOKUP and CREATE. Links share contents and inode identity. Publication
and durability remain separate: link a new version to a temporary name, rename
that name over the stable name, then explicitly sync the native parent.
