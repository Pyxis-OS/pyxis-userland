#ifndef PKG_FETCH_H
#define PKG_FETCH_H

#include <abi/describe.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <package.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PKG_REPOSITORIES_MAX 8
#define PKG_BASE_URL_MAX 2048

/* A configured repository. The name follows the package ID rules, so REPO/ID
 * parses without ambiguity; it is chosen by the user, never derived from the
 * URL, so a server's path layout does not become a contract. */
struct pkg_repository {
  char name[PACKAGE_ID_MAX + 1];
  char url[PKG_BASE_URL_MAX + 1];
  /* signed = false in its entry, which only development images write: its
   * index is read without a signature (decision 72). */
  bool unsigned_index;
};

struct pkg_repositories {
  size_t count;
  struct pkg_repository entries[PKG_REPOSITORIES_MAX];
};

struct pkg_fetch_authority {
  handle_t http, https, clock;
};

bool pkg_repository_url_valid(const char *base);

/* Borrows the explicitly granted system/config DIRECTORY. Missing file means
 * an empty list. On failure the
 * repository list is empty and reason is static text. Never executes Lua. */
enum call_status pkg_repositories_load(handle_t package_config,
    struct pkg_repositories *repositories, const char **reason);
/* Under the shared package writer lock, replace only the fixed temporary
 * sibling, reread/validate it and synchronize its FILE before publication.
 * Never retries an uncertain mutation. The caller owns rename/parent sync. */
enum call_status pkg_repositories_stage(handle_t config,
    const struct pkg_repositories *repositories, const char **reason);
enum call_status pkg_timeout_parse(const char *text, uint64_t *seconds);

/* Borrows captured provider roots/clock. timeout_seconds == 0 selects the
 * size-derived default (30 seconds for an index). One GET, no retry or fallback.
 * Success owns a completed READ-only FILE; failure clears it. Keep it through
 * verification/extraction and close it before the next fetch. Detail retains
 * the provider's human failure text, or a local reason. */
enum call_status pkg_fetch(const struct pkg_fetch_authority *authority,
    const char *base, const char *relative, bool index, uint64_t size,
    uint64_t timeout_seconds, handle_t *file,
    char detail[RESOURCE_DETAIL_MAX_BYTES + 1]);

#endif
