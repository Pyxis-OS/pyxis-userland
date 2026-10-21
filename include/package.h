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

/* The ABI a program or toolchain package requires. For now it is the kernel's
 * 12-hex build revision; with dynamic linking it becomes a system library's ABI
 * name, so readers check only this grammar and compare names exactly. */
#define PACKAGE_ABI_MAX 64
/* For now the running system provides exactly one ABI. */
#define PACKAGE_PROVIDED_ABI_MAX 1

struct package_set;

struct package_provided_abi {
  size_t count;
  char names[PACKAGE_PROVIDED_ABI_MAX][PACKAGE_ABI_MAX + 1];
};

bool package_id_valid(const char *id);
bool package_abi_valid(const char *abi);
/* Read the ABIs the running system provides through SYSTEM_INFO with READ: for
 * now its build revision alone. A kernel without a revision provides none, so
 * every package that records an ABI is refused. */
enum call_status package_provided_abi(handle_t system_info,
    struct package_provided_abi *provided);
bool package_abi_provided(const struct package_provided_abi *provided, const char *abi);
/* "NAME, NAME" for refusals, or "no ABI" when the list is empty. */
void package_provided_abi_text(const struct package_provided_abi *provided,
    char *text, size_t capacity);

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
size_t package_set_count(const struct package_set *set);
void package_set_free(struct package_set *set);

/* Open package ID's tree from SET with RIGHTS, a subset of PACKAGE_TREE_RIGHTS.
 * CALL_NOT_FOUND means SET doesn't list ID or its store tree is missing.
 * Success owns *tree; failure clears it and sets *reason (static text). */
enum call_status package_set_open(const struct package_set *set, handle_t packages,
    const char *id, uint64_t rights, handle_t *tree, const char **reason);

#endif
