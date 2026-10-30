#include "release_route.h"

#include <string.h>

static bool release_entry(const struct pkg_entry *entry)
{
  return pkg_kind_release(entry->kind) &&
      (!strcmp(entry->identity.id, "pyxis") ||
       !strcmp(entry->identity.id, "pyxis-release") ||
       !strcmp(entry->identity.id, "pyxis-checkpoint"));
}

static bool checkpoint(const struct pkg_entry *entry)
{
  return strcmp(entry->identity.id, "pyxis-release") != 0;
}

bool pkg_release_route(const struct pkg_entry *entries, size_t count,
    const char *running, const char *upgrade_from, const char *requested,
    const struct pkg_entry **next, const struct pkg_entry **target, const char **reason)
{
  *next = NULL;
  *target = NULL;
  size_t from = SIZE_MAX, to = SIZE_MAX;
  bool development = upgrade_from[0] != 0;
  const char *anchor = development ? upgrade_from : running;
  bool modern = false;
  for (size_t i = 0; i < count; ++i) {
    if (!release_entry(&entries[i])) continue;
    if (strchr(entries[i].identity.version, '+')) {
      *reason = "the stock release route contains a development version";
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (release_entry(&entries[j]) &&
          !strcmp(entries[i].identity.version, entries[j].identity.version)) {
        *reason = "the release route repeats a version";
        return false;
      }
    }
    bool legacy = !strcmp(entries[i].identity.id, "pyxis");
    if (legacy && modern) {
      *reason = "the release route changes the permanent legacy checkpoint";
      return false;
    }
    modern |= !legacy;
    if (!strcmp(entries[i].identity.version, anchor)) from = i;
    if (!requested || !strcmp(entries[i].identity.version, requested)) to = i;
  }
  if (from == SIZE_MAX) {
    *reason = development ?
        "the development build's upgrade_from release is absent from the supported upgrade route" :
        "the running release is absent from the supported upgrade route";
    return false;
  }
  if (to == SIZE_MAX) {
    *reason = "the releases repository does not offer the requested release";
    return false;
  }
  if (development && to <= from) {
    *reason = requested ?
        "pkg upgrade cannot select the development build's release anchor or an earlier release" :
        "there is no newer released destination";
    return false;
  }
  if (to < from) {
    *reason = "pkg upgrade cannot select an earlier release";
    return false;
  }
  *target = &entries[to];
  *next = *target;
  for (size_t i = from + 1; i < to; ++i) {
    if (release_entry(&entries[i]) && checkpoint(&entries[i])) {
      *next = &entries[i];
      break;
    }
  }
  *reason = NULL;
  return true;
}
