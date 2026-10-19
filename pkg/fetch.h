#ifndef PKG_FETCH_H
#define PKG_FETCH_H

#include <abi/describe.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PKG_REPOSITORIES_MAX 8
#define PKG_BASE_URL_MAX 2048

struct pkg_repositories {
  size_t count;
  char urls[PKG_REPOSITORIES_MAX][PKG_BASE_URL_MAX + 1];
};

struct pkg_fetch_authority {
  handle_t http, https, clock;
};

/* Borrows the explicitly granted system/config DIRECTORY. On failure the
 * repository list is empty and reason is static text. Never executes Lua. */
enum call_status pkg_repositories_load(handle_t package_config,
    struct pkg_repositories *repositories, const char **reason);
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
