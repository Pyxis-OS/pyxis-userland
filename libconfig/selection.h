#ifndef LIBCONFIG_SELECTION_H
#define LIBCONFIG_SELECTION_H

#include "boot.h"
#include "session.h"
#include "network.h"

enum config_lua_file { CONFIG_LUA_BOOT, CONFIG_LUA_SESSION, CONFIG_LUA_NETWORK, CONFIG_LUA_COUNT };

struct config_bytes {
  const char *bytes;
  size_t size;
};

/* Selection owns its byte snapshots and every decoded value. A plan borrows
 * defaults and override until selection_free; no later path reopen is needed. */
struct config_selection {
  char *bytes[CONFIG_LUA_COUNT];
  size_t sizes[CONFIG_LUA_COUNT];
  struct boot_config defaults, override;
  struct boot_plan boot;
  struct session_config session;
  struct network_config network;
  bool from_esp;
};

/* Validate a complete literal ESP triple against trusted installed defaults.
 * No effects, CPU inventory, volume opening or timezone-file access occur.
 * Failure frees partially decoded state. */
bool config_selection_validate(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes esp[CONFIG_LUA_COUNT], struct config_selection *selection);

/* COMPLETE means the caller verified the committed record and exact triple
 * hashes. Rescue, missing/incomplete selection or invalid ESP inputs use the
 * entire trusted archive triple. No mixed per-file fallback is possible.
 * REASON points to static text; false means archive defaults also failed. */
bool config_selection_choose(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes esp[CONFIG_LUA_COUNT], bool complete, bool rescue,
    struct config_selection *selection, const char **reason);
void config_selection_free(struct config_selection *selection);

#endif
