#ifndef USERSPACE_PKG_METADATA_H
#define USERSPACE_PKG_METADATA_H

#include <package.h>
#include <stdint.h>

#define PKG_MAX_FILES 65536
#define PKG_PATH_MAX 250
#define PKG_INVENTORY_LINE_MAX 512
#define PKG_FILE_SIZE_MAX UINT64_C(8589934591)
#define PKG_ARCHIVE_PATH_MAX (9 + PACKAGE_ID_MAX + 1 + PACKAGE_VERSION_MAX + 4)

struct pkg_identity {
  char id[PACKAGE_ID_MAX + 1], version[PACKAGE_VERSION_MAX + 1];
};

/* abi is set for program and toolchain packages and empty for data. */
struct pkg_entry {
  struct pkg_identity identity;
  char kind[10], abi[PACKAGE_ABI_MAX + 1], archive[PKG_ARCHIVE_PATH_MAX + 1];
  uint64_t size;
  char sha256[65];
};

struct pkg_command {
  /* An empty command selects the plain file; otherwise it selects a bundle alias. */
  char name[256], path[PKG_PATH_MAX + 1], command[256];
};

struct pkg_manifest {
  struct pkg_identity identity;
  char kind[10], abi[PACKAGE_ABI_MAX + 1];
  uint64_t files, bytes;
  char inventory_sha256[65];
  struct pkg_identity *dependencies;
  size_t dependency_count;
  struct pkg_command *commands;
  size_t command_count;
};

bool pkg_version_valid(const char *version);
/* Program and toolchain packages hold native programs and must record an ABI. */
bool pkg_kind_bound(const char *kind);
/* Successful outputs own their arrays, independent of the literal document.
 * Free index entries with free(), manifests with pkg_manifest_free(). */
bool pkg_index_parse(const char *bytes, size_t size, struct pkg_entry **entries,
    size_t *count, const char **reason);
bool pkg_manifest_parse(const char *bytes, size_t size,
    struct pkg_manifest *manifest, const char **reason);
void pkg_manifest_free(struct pkg_manifest *manifest);

#endif
