#ifndef USERSPACE_PKG_FLOOR_H
#define USERSPACE_PKG_FLOOR_H

#include "fetch.h"
#include "metadata.h"

#define PKG_REPOSITORY_FLOORS "boot://share/pyxis/trust/repository-floors.lua"

struct pkg_repository_floor {
  char name[PACKAGE_ID_MAX + 1];
  char url[PKG_BASE_URL_MAX + 1];
  uint64_t serial;
  char signed_at[PKG_TIME_LENGTH + 1];
  char sha256[65];
};

struct pkg_repository_floors {
  size_t count;
  struct pkg_repository_floor entries[PKG_REPOSITORIES_MAX];
};

/* A missing archive file is an empty snapshot. Malformed data refuses all
 * signed repositories; callers decide whether an empty stock floor can rely
 * on a previously accepted volume record. */
bool pkg_repository_floors_load(struct pkg_repository_floors *floors,
    const char **reason);
const struct pkg_repository_floor *pkg_repository_floor_find(
    const struct pkg_repository_floors *floors, const struct pkg_repository *repository);

#endif
