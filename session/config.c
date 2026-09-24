#include "config.h"
#include <abi/console.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

struct config_reader {
  FILE *file;
  char bytes[512];
  struct session_config *config;
};

static const char *read_source(lua_State *state, void *data, size_t *size)
{
  (void)state;
  struct config_reader *reader = data;
  *size = fread(reader->bytes, 1, sizeof(reader->bytes), reader->file);
  return *size ? reader->bytes : NULL;
}

static void check_keys(lua_State *state, int index, const char *first, const char *second)
{
  luaL_checktype(state, index, LUA_TTABLE);
  lua_pushnil(state);
  while (lua_next(state, index)) {
    if (lua_type(state, -2) != LUA_TSTRING) {
      luaL_error(state, "settings must have string keys");
    }
    size_t length;
    const char *key = lua_tolstring(state, -2, &length);
    if (strlen(key) != length ||
        (strcmp(key, first) && (!second || strcmp(key, second)))) {
      luaL_error(state, "unknown setting '%s'", key);
    }
    lua_pop(state, 1);
  }
}

static void raw_field(lua_State *state, int index, const char *key)
{
  lua_pushstring(state, key);
  lua_rawget(state, index);
}

static int evaluate_config(lua_State *state)
{
  struct config_reader *reader = lua_touserdata(state, 1);
  lua_settop(state, 0);
  luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
  lua_pop(state, 1);
  /* Configuration can compute values, but cannot load more code or perform
   * console/file operations. The launcher keeps those capabilities in C. */
  const char *removed[] = {"load", "loadfile", "dofile", "print", "warn"};
  for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
    lua_pushnil(state);
    lua_setglobal(state, removed[i]);
  }
  int status = lua_load(state, read_source, reader, "@" SESSION_CONFIG_PATH, "t");
  if (ferror(reader->file)) {
    return luaL_error(state, "cannot read configuration");
  }
  if (status != LUA_OK) {
    return lua_error(state);
  }
  lua_call(state, 0, LUA_MULTRET);
  if (lua_gettop(state) != 1) {
    return luaL_error(state, "configuration must return exactly one table");
  }
  check_keys(state, 1, "timezone", "terminal");

  raw_field(state, 1, "terminal");
  if (!lua_isnil(state, 2)) {
    check_keys(state, 2, "tab_width", NULL);
    raw_field(state, 2, "tab_width");
    if (!lua_isnil(state, 3)) {
      int integer;
      lua_Integer width = lua_tointegerx(state, 3, &integer);
      if (lua_type(state, 3) != LUA_TNUMBER || !integer ||
          width < (lua_Integer)CONSOLE_TAB_WIDTH_MIN || width > (lua_Integer)CONSOLE_TAB_WIDTH_MAX) {
        return luaL_error(state, "terminal.tab_width must be an integer from 1 to 32");
      }
      reader->config->tab_width = (size_t)width;
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);

  raw_field(state, 1, "timezone");
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
  reader->config->timezone = strdup(timezone);
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
  FILE *file = fopen(SESSION_CONFIG_PATH, "rb");
  if (!file) {
    if (errno != ENOENT) {
      perror("session: " SESSION_CONFIG_PATH);
      return false;
    }
    config->timezone = strdup("UTC");
  } else {
    lua_State *state = luaL_newstate();
    if (!state) {
      fclose(file);
      fputs("session: cannot allocate configuration evaluator\n", stderr);
      return false;
    }
    struct config_reader reader = {.file = file, .config = config};
    lua_pushcfunction(state, evaluate_config);
    lua_pushlightuserdata(state, &reader);
    int status = lua_pcall(state, 1, 0, 0);
    if (status != LUA_OK) {
      const char *error = lua_type(state, -1) == LUA_TSTRING ?
          lua_tostring(state, -1) : "non-string configuration error";
      fprintf(stderr, "session: %s: %s\n", SESSION_CONFIG_PATH, error);
    }
    lua_close(state);
    int closed = fclose(file);
    if (closed) {
      perror("session: closing configuration");
    }
    if (status != LUA_OK || closed) {
      free(config->timezone);
      config->timezone = NULL;
      return false;
    }
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
