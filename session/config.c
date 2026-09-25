#include "config.h"
#include <abi/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include "../libconfig/config.h"

static int decode_config(lua_State *state)
{
  struct session_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"timezone", "terminal"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));

  config_field(state, 1, "terminal");
  if (!lua_isnil(state, 2)) {
    const char *terminal_keys[] = {"tab_width"};
    config_keys(state, 2, terminal_keys, 1);
    config_field(state, 2, "tab_width");
    if (!lua_isnil(state, 3)) {
      int integer;
      lua_Integer width = lua_tointegerx(state, 3, &integer);
      if (lua_type(state, 3) != LUA_TNUMBER || !integer ||
          width < (lua_Integer)CONSOLE_TAB_WIDTH_MIN || width > (lua_Integer)CONSOLE_TAB_WIDTH_MAX) {
        return luaL_error(state, "terminal.tab_width must be an integer from 1 to 32");
      }
      config->tab_width = (size_t)width;
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);

  config_field(state, 1, "timezone");
  const char *timezone = "UTC";
  if (!lua_isnil(state, 2)) {
    if (lua_type(state, 2) != LUA_TSTRING) {
      return luaL_error(state, "timezone must be an IANA name string");
    }
    size_t length;
    timezone = lua_tolstring(state, 2, &length);
    if (strlen(timezone) != length) {
      return luaL_error(state, "timezone contains a NUL byte");
    }
    if (!length) {
      timezone = "UTC";
    }
  }
  /* No more Lua calls after publishing the native allocation. */
  config->timezone = strdup(timezone);
  return 0;
}

static bool check_timezone(const char *name)
{
  if (!strcmp(name, "UTC")) {
    return true;
  }
  bool component = false;
  for (const char *p = name; *p; ++p) {
    if (*p == '/') {
      if (!component) {
        return false;
      }
      component = false;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '+') {
      component = true;
    } else {
      return false;
    }
  }
  if (!component) {
    return false;
  }

  const char prefix[] = "app://share/zoneinfo/";
  size_t length = strlen(name);
  if (length > SIZE_MAX - sizeof(prefix)) {
    return false;
  }
  char *path = malloc(sizeof(prefix) + length);
  if (!path) {
    return false;
  }
  memcpy(path, prefix, sizeof(prefix) - 1);
  memcpy(path + sizeof(prefix) - 1, name, length + 1);
  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) {
    return false;
  }
  /* The packaged database is trusted; libc validates the complete TZif when
   * a child first converts local time. Reject non-zone metadata files here. */
  char magic[4];
  bool valid = fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
      !memcmp(magic, "TZif", sizeof(magic));
  return fclose(file) == 0 && valid;
}

bool session_config_read(struct session_config *config)
{
  *config = (struct session_config){.tab_width = 8};
  enum config_result result = config_read(SESSION_CONFIG_PATH, decode_config, config);
  if (result == CONFIG_MISSING) {
    config->timezone = strdup("UTC");
  } else if (result == CONFIG_ERROR) {
    free(config->timezone);
    config->timezone = NULL;
    return false;
  }
  if (!config->timezone) {
    fputs("session: cannot allocate timezone setting\n", stderr);
    return false;
  }
  if (!check_timezone(config->timezone)) {
    fprintf(stderr, "session: unavailable or invalid timezone '%s'\n", config->timezone);
    free(config->timezone);
    config->timezone = NULL;
    return false;
  }
  return true;
}
