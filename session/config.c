#include "config.h"
#include <abi/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <startup.h>
#include "../libconfig/config.h"

static bool reserved_variable(const char *name)
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
    if (reserved_variable(name)) {
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

static int decode_config(lua_State *state)
{
  struct session_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"timezone", "terminal", "environment"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));
  read_environment(state, config);

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

  const char prefix[] = "boot://share/zoneinfo/";
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
    session_config_free(config);
    return false;
  }
  if (!config->timezone) {
    fputs("session: cannot allocate timezone setting\n", stderr);
    session_config_free(config);
    return false;
  }
  if (!check_timezone(config->timezone)) {
    fprintf(stderr, "session: unavailable or invalid timezone '%s'\n", config->timezone);
    session_config_free(config);
    return false;
  }
  return true;
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

static bool configured_variable(const struct session_config *config, const char *name)
{
  for (size_t i = 0; i < config->environment_count; ++i) {
    if (!strcmp(config->environment[i].name, name)) {
      return true;
    }
  }
  return false;
}

struct startup_variable *session_environment(const struct session_config *config,
    const char *dns_server, const struct pyxis_environment_snapshot *snapshot,
    size_t *count)
{
  size_t inherited = snapshot->count;
  size_t limit = SIZE_MAX / sizeof(struct startup_variable);
  if (inherited > limit - 2 || config->environment_count > limit - 2 - inherited) {
    fputs("session: environment too large\n", stderr);
    return NULL;
  }
  struct startup_variable *environment =
      malloc((inherited + config->environment_count + 2) * sizeof(*environment));
  if (!environment) {
    fputs("session: cannot allocate environment\n", stderr);
    return NULL;
  }

  size_t used = 0;
  const struct startup_variable *source = snapshot->variables;
  for (size_t i = 0; i < inherited; ++i) {
    const char *name = (const char *)source[i].name;
    /* The boot-carried discovery name cannot be shadowed by configuration. */
    if (!strcmp(name, "PYXIS_REMOTE_BEACON") ||
        (!reserved_variable(name) && !configured_variable(config, name))) {
      environment[used++] = source[i];
    }
  }
  for (size_t i = 0; i < config->environment_count; ++i) {
    environment[used++] = (struct startup_variable){
      (uintptr_t)config->environment[i].name, (uintptr_t)config->environment[i].value,
    };
  }
  environment[used++] = (struct startup_variable){
    (uintptr_t)"TZ", (uintptr_t)config->timezone,
  };
  environment[used++] = (struct startup_variable){
    (uintptr_t)"DNS_SERVER", (uintptr_t)dns_server,
  };
  *count = used;
  return environment;
}
