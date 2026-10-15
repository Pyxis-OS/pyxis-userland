#include "session.h"
#include <abi/console.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool session_variable_reserved(const char *name)
{
  return !strcmp(name, "TZ") || !strcmp(name, "DNS_SERVER");
}

static bool valid_variable_name(const char *name, size_t length)
{
  if (!length || (name[0] >= '0' && name[0] <= '9')) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

static void read_environment(lua_State *state, struct session_config *config)
{
  config_field(state, 1, "environment");
  if (lua_isnil(state, 2)) {
    lua_pop(state, 1);
    return;
  }
  if (lua_type(state, 2) != LUA_TTABLE) {
    luaL_error(state, "environment must be a table of variables");
  }
  size_t count = 0;
  lua_pushnil(state);
  while (lua_next(state, 2)) {
    size_t length;
    if (lua_type(state, -2) != LUA_TSTRING) {
      luaL_error(state, "environment keys must be variable names");
    }
    const char *name = lua_tolstring(state, -2, &length);
    if (!valid_variable_name(name, length)) {
      luaL_error(state, "invalid environment variable name '%s'", name);
    }
    if (!strcmp(name, "PYXIS_REMOTE_BEACON")) {
      luaL_error(state, "environment.%s is reserved for the remote.beacon boot option", name);
    }
    if (session_variable_reserved(name)) {
      luaL_error(state, "environment.%s is reserved for the timezone and network settings", name);
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
      luaL_error(state, "environment.%s must be a string", name);
    }
    const char *value = lua_tolstring(state, -1, &length);
    if (strlen(value) != length) {
      luaL_error(state, "environment.%s contains a NUL byte", name);
    }
    if (count == SIZE_MAX / sizeof(*config->environment)) {
      luaL_error(state, "environment is too large");
    }
    ++count;
    lua_pop(state, 1);
  }
  if (!count) {
    lua_pop(state, 1);
    return;
  }

  config->environment = calloc(count, sizeof(*config->environment));
  if (!config->environment) {
    luaL_error(state, "cannot allocate environment");
  }
  /* Entries were validated above; only allocation can fail from here. */
  lua_pushnil(state);
  while (lua_next(state, 2)) {
    struct session_variable *variable = &config->environment[config->environment_count++];
    variable->name = strdup(lua_tostring(state, -2));
    variable->value = strdup(lua_tostring(state, -1));
    if (!variable->name || !variable->value) {
      luaL_error(state, "cannot allocate environment");
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

bool session_timezone_valid(const char *name)
{
  if (!name || !*name) {
    return false;
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

  return true;
}

static void read_retention(lua_State *state, struct session_config *config)
{
  config_field(state, 1, "retention");
  if (lua_isnil(state, 2)) {
    lua_pop(state, 1);
    return;
  }
  const char *keys[] = {"value_limit_bytes", "session_limit_bytes"};
  config_keys(state, 2, keys, sizeof(keys) / sizeof(*keys));
  uint64_t *limits[] = {&config->value_limit_bytes, &config->session_limit_bytes};
  for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
    config_field(state, 2, keys[i]);
    if (!lua_isnil(state, 3)) {
      int integer;
      lua_Integer value = lua_tointegerx(state, 3, &integer);
      if (lua_type(state, 3) != LUA_TNUMBER || !integer || value <= 0) {
        luaL_error(state, "retention.%s must be a positive integer", keys[i]);
      }
      *limits[i] = (uint64_t)value;
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

static struct session_config default_config(void)
{
  return (struct session_config){
    .tab_width = 8,
    .value_limit_bytes = UINT64_C(64) * 1024 * 1024,
    .session_limit_bytes = UINT64_C(512) * 1024 * 1024,
  };
}

static int decode_config(lua_State *state)
{
  struct session_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"timezone", "terminal", "environment", "retention"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));
  read_environment(state, config);
  read_retention(state, config);

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
  if (!session_timezone_valid(timezone)) {
    return luaL_error(state, "timezone must be an IANA name with nonempty path components");
  }
  /* No more Lua calls after publishing the native allocation. */
  config->timezone = strdup(timezone);
  return 0;
}

static enum config_result session_result(enum config_result result,
    struct session_config *config)
{
  if (result == CONFIG_MISSING) {
    config->timezone = strdup("UTC");
  }
  if (result == CONFIG_ERROR || !config->timezone) {
    if (result != CONFIG_ERROR) {
      fputs("session: cannot allocate timezone setting\n", stderr);
    }
    session_config_free(config);
    return CONFIG_ERROR;
  }
  return result;
}

enum config_result session_config_read_file(const char *path, struct session_config *config)
{
  *config = default_config();
  return session_result(config_read(path, decode_config, config), config);
}

enum config_result session_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct session_config *config)
{
  *config = default_config();
  return session_result(config_read_bytes(name, bytes, size, decode_config, config), config);
}

enum config_result session_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct session_config *config)
{
  *config = default_config();
  return session_result(config_read_literal_bytes(name, bytes, size, decode_config, config), config);
}

void session_config_free(struct session_config *config)
{
  for (size_t i = 0; i < config->environment_count; ++i) {
    free(config->environment[i].name);
    free(config->environment[i].value);
  }
  free(config->environment);
  free(config->timezone);
  *config = (struct session_config){0};
}
