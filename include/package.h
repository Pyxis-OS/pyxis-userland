#ifndef USERSPACE_PACKAGE_H
#define USERSPACE_PACKAGE_H

#include <abi/directory.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* The packages volume (docs/interfaces/packages.md): store/ID/VERSION/tree,
 * sets/N.lua and a current file naming the active set. */
#define PACKAGE_SOURCE "packages"
#define PACKAGE_ID_MAX 64
#define PACKAGE_VERSION_MAX 64
/* Rights a package tree may carry: packages are always read-only. */
#define PACKAGE_TREE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
                             DIRECTORY_RIGHT_READ_FILES)

/* The ABI a program or toolchain package requires: a release name such as
 * 0.0.11, or the full development version, such as 0.0.11+abcdef012345
 * (docs/interfaces/abi.md). Readers check only this grammar and compare names
 * exactly. */
#define PACKAGE_ABI_MAX 64
#define PACKAGE_OS_RELEASE "boot://os-release"

struct package_set;

/* TEXT is "NAME, NAME" for refusals, or "no ABI" when the list is empty. */
struct package_provided_abi {
  size_t count;
  char **names;
  char *text;
  /* The build commit's UTC time, YYYY-MM-DDTHH:MM:SSZ: pkg accepts no signed
   * index from before it on first contact (decision 71). Empty without Git. */
  char signed_floor[21];
  /* The release tag of a tagged build; empty for a development build. */
  char release[PACKAGE_ABI_MAX + 1];
  /* Display identity: the release tag or full development version. */
  char version[PACKAGE_ABI_MAX + 1];
  /* Actual release ancestor of a development build, never an ABI claim. */
  char upgrade_from[PACKAGE_ABI_MAX + 1];
};

bool package_id_valid(const char *id);
bool package_abi_valid(const char *abi);
/* Read the explicitly provided ABIs from boot://os-release, ordered from
 * fallback to preferred. The revision is provenance, never an implicit ABI.
 * An empty list is valid (for example, an incompatible dirty build).
 * CALL_NOT_FOUND when the file cannot be opened, CALL_BAD_REQUEST when it is
 * malformed. The caller frees a successful result with package_provided_abi_free. */
enum call_status package_provided_abi(struct package_provided_abi *provided);
void package_provided_abi_free(struct package_provided_abi *provided);
bool package_abi_provided(const struct package_provided_abi *provided, const char *abi);

/* Read current and its set record through PACKAGES, a directory with LOOKUP
 * and READ_FILES on the volume root. A missing current is the empty set.
 * Success owns *set (free it with package_set_free); failure clears it and
 * sets *reason (static text) naming what is damaged. One load serves every
 * package of one launch or space start, so they all come from one set. */
enum call_status package_set_load(handle_t packages, struct package_set **set,
    const char **reason);
/* Explicit record for management/recovery; number zero is the empty set.
 * Does not inspect current or select a fallback. */
enum call_status package_set_load_number(handle_t packages, uint64_t number,
    struct package_set **set, const char **reason);
uint64_t package_set_number(const struct package_set *set);
/* Borrowed strings remain valid until set is freed. */
bool package_set_entry(const struct package_set *set, size_t index,
    const char **id, const char **version);
/* NULL outputs identify a legacy entry with no recorded source. New entries
 * carry a repository name and a SHA-256 fingerprint of its URL and policy. */
bool package_set_origin(const struct package_set *set, size_t index,
    const char **name, const char **fingerprint);
size_t package_set_count(const struct package_set *set);
void package_set_free(struct package_set *set);

#endif
