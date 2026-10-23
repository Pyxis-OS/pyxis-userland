#ifndef USERSPACE_PKG_METADATA_H
#define USERSPACE_PKG_METADATA_H

#include <package.h>
#include <stdint.h>

#define PKG_MAX_FILES 65536
#define PKG_PATH_MAX 250
#define PKG_INVENTORY_LINE_MAX 512
#define PKG_FILE_SIZE_MAX UINT64_C(8589934591)
#define PKG_ARCHIVE_PATH_MAX (9 + PACKAGE_ID_MAX + 1 + PACKAGE_VERSION_MAX + 4)
/* YYYY-MM-DDTHH:MM:SSZ, which compares chronologically as text. */
#define PKG_TIME_LENGTH 20
#define PKG_SIGNER_NAME_MAX 63
#define PKG_SERIAL_MAX (UINT64_C(1) << 62)

struct pkg_identity {
  char id[PACKAGE_ID_MAX + 1], version[PACKAGE_VERSION_MAX + 1];
};

#define PKG_PROVIDES_MAX 16

/* abi is set for program, toolchain and dev packages and empty otherwise. A
 * release entry (decision 73) instead lists the ABIs that release provides. */
struct pkg_entry {
  struct pkg_identity identity;
  char kind[10], abi[PACKAGE_ABI_MAX + 1], archive[PKG_ARCHIVE_PATH_MAX + 1];
  uint64_t size;
  char sha256[65];
  size_t provides_count;
  char provides[PKG_PROVIDES_MAX][PACKAGE_ABI_MAX + 1];
};

/* A published repository index (docs/wip/pkg-upgrade.md, decision 71): the
 * repository it was published as, its serial, when it was signed and, for a
 * signed one, the certificate file beside it to check the signature with. */
struct pkg_index_header {
  char repository[PACKAGE_ID_MAX + 1];
  uint64_t serial;
  char signed_at[PKG_TIME_LENGTH + 1];
  char signer[PKG_SIGNER_NAME_MAX + 1]; /* Empty when the index names none. */
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
  /* An empty version is satisfied by any version of that ID. */
  struct pkg_identity *dependencies;
  size_t dependency_count;
  struct pkg_command *commands;
  size_t command_count;
};

bool pkg_version_valid(const char *version);
/* Program, toolchain and dev packages are built for one system and must record
 * its ABI; only program and toolchain packages may export commands. A dev
 * package's include/ and lib/ files form sdk:// (docs/wip/sdk-view.md). */
bool pkg_kind_bound(const char *kind);
bool pkg_kind_commands(const char *kind);
/* A release is never installed as a package; only pkg upgrade reads it. */
bool pkg_kind_release(const char *kind);
/* Successful outputs own their arrays, independent of the literal document.
 * Free index entries with free(), manifests with pkg_manifest_free(). */
bool pkg_time_valid(const char *text);
bool pkg_index_parse(const char *bytes, size_t size, struct pkg_index_header *header,
    struct pkg_entry **entries, size_t *count, const char **reason);
bool pkg_manifest_parse(const char *bytes, size_t size,
    struct pkg_manifest *manifest, const char **reason);
void pkg_manifest_free(struct pkg_manifest *manifest);

#endif
