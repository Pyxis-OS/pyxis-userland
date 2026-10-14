#ifndef USERSPACE_INSTALLER_MANIFEST_H
#define USERSPACE_INSTALLER_MANIFEST_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define INSTALL_MANIFEST_NAME ".pyxis-revision-manifest"
#define INSTALL_MANIFEST_MAX_BYTES (16u * 1024u * 1024u)
#define INSTALL_TREE_MAX_DEPTH 32u
#define INSTALL_TREE_MAX_ENTRIES 65536u
#define INSTALL_TREE_NAME_MAX 255u

enum install_manifest_status {
  INSTALL_MANIFEST_OK,
  INSTALL_MANIFEST_INVALID,
  INSTALL_MANIFEST_NO_MEMORY,
};

struct install_manifest_entry {
  char *path;
  uint64_t kind, size;
  char sha256[65];
};

/* Owns paths and entries. SEALED means bytewise-sorted, validated canonical
 * records; TEXT_BYTES includes the header. The manifest itself is excluded,
 * but its installed file reserves one of the tree's entry slots. */
struct install_manifest {
  char revision[64];
  struct install_manifest_entry *entries;
  size_t count, capacity, files, text_bytes;
  uint64_t bytes;
  bool sealed;
};

/* Initialize an empty destination before use. add copies its borrowed path and
 * hash; directories require size zero and a NULL hash. Failure preserves an
 * owned partial builder, which the caller destroys. seal sorts and validates
 * every ancestor; a failed seal is not usable for encoding or comparison. */
enum install_manifest_status install_manifest_init(struct install_manifest *manifest,
    const char *revision);
enum install_manifest_status install_manifest_add(struct install_manifest *manifest,
    const char *path, uint64_t kind, uint64_t size, const char *sha256);
enum install_manifest_status install_manifest_seal(struct install_manifest *manifest);
void install_manifest_destroy(struct install_manifest *manifest);

/* parse accepts exactly the canonical encoding, including sorted paths and
 * final newlines. Outputs own their allocations; encode adds an uncounted NUL.
 * parse failure leaves a zeroed output. */
enum install_manifest_status install_manifest_parse(const void *text, size_t bytes,
    struct install_manifest *manifest);
enum install_manifest_status install_manifest_encode(const struct install_manifest *manifest,
    char **text, size_t *bytes);
bool install_manifest_equal(const struct install_manifest *left,
    const struct install_manifest *right);

struct install_programs;
/* Boot and selected roots are borrowed and immutable. This read-only scan
 * closes every acquired file/directory handle and reports hashing progress. */
enum install_manifest_status install_manifest_source(handle_t boot,
    const struct install_programs *programs, const char *revision,
    struct install_manifest *manifest);

/* NULL previous describes Install; unknown previous revisions must be handled
 * explicitly by the caller. Summaries always include counts and sizes; verbose
 * reports additionally escape and print each program name. */
bool install_manifest_report(FILE *output, const struct install_manifest *previous,
    const struct install_manifest *source, bool verbose);

#endif
