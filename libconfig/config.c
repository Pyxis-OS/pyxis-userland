#include "config.h"
#include <errno.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

/* Exactly one source: an open FILE, or caller bytes delivered once. */
struct config_reader {
  FILE *file;
  const char *memory;
  size_t memory_size;
  const char *path;
  char bytes[512];
  lua_CFunction decode;
  void *output;
};

static const char *read_source(lua_State *state, void *data, size_t *size)
{
  (void)state;
  struct config_reader *reader = data;
  if (!reader->file) {
    const char *bytes = reader->memory;
    *size = reader->memory_size;
    reader->memory = NULL;
    reader->memory_size = 0;
    return *size ? bytes : NULL;
  }
  *size = fread(reader->bytes, 1, sizeof(reader->bytes), reader->file);
  return *size ? reader->bytes : NULL;
}

void config_keys(lua_State *state, int index, const char *const *keys, size_t count)
{
  index = lua_absindex(state, index);
  luaL_checktype(state, index, LUA_TTABLE);
  lua_pushnil(state);
  while (lua_next(state, index)) {
    if (lua_type(state, -2) != LUA_TSTRING) {
      luaL_error(state, "settings must have string keys");
    }
    size_t length;
    const char *key = lua_tolstring(state, -2, &length);
    bool known = false;
    for (size_t i = 0; i < count; ++i) {
      if (strlen(key) == length && !strcmp(key, keys[i])) {
        known = true;
        break;
      }
    }
    if (!known) {
      luaL_error(state, "unknown setting '%s'", key);
    }
    lua_pop(state, 1);
  }
}

void config_field(lua_State *state, int index, const char *key)
{
  index = lua_absindex(state, index);
  lua_pushstring(state, key);
  lua_rawget(state, index);
}

static int evaluate_config(lua_State *state)
{
  struct config_reader *reader = lua_touserdata(state, 1);
  lua_settop(state, 0);
  luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
  lua_pop(state, 1);
  /* Consumers keep I/O and resource capabilities in C. Configuration may
   * compute values, but cannot load more code or perform console/file I/O. */
  const char *removed[] = {"load", "loadfile", "dofile", "print", "warn"};
  for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
    lua_pushnil(state);
    lua_setglobal(state, removed[i]);
  }
  const char *name = lua_pushfstring(state, "@%s", reader->path);
  int status = lua_load(state, read_source, reader, name, "t");
  if (reader->file && ferror(reader->file)) {
    return luaL_error(state, "cannot read configuration");
  }
  if (status != LUA_OK) {
    return lua_error(state);
  }
  lua_remove(state, 1); /* Chunk name; leave only the loaded function. */
  lua_call(state, 0, LUA_MULTRET);
  if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TTABLE) {
    return luaL_error(state, "configuration must return exactly one table");
  }
  lua_pushcfunction(state, reader->decode);
  lua_insert(state, 1);
  lua_pushlightuserdata(state, reader->output);
  lua_call(state, 2, 0);
  return 0;
}

static bool evaluate(struct config_reader *reader)
{
  lua_State *state = luaL_newstate();
  if (!state) {
    fprintf(stderr, "config: %s: cannot allocate evaluator\n", reader->path);
    return false;
  }
  lua_pushcfunction(state, evaluate_config);
  lua_pushlightuserdata(state, reader);
  int status = lua_pcall(state, 1, 0, 0);
  if (status != LUA_OK) {
    const char *error = lua_type(state, -1) == LUA_TSTRING ?
        lua_tostring(state, -1) : "non-string configuration error";
    fprintf(stderr, "config: %s: %s\n", reader->path, error);
  }
  lua_close(state);
  return status == LUA_OK;
}

enum config_result config_read(const char *path, lua_CFunction decode, void *output)
{
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (errno == ENOENT) {
      return CONFIG_MISSING;
    }
    fprintf(stderr, "config: %s: %s\n", path, strerror(errno));
    return CONFIG_ERROR;
  }
  struct config_reader reader = {.file = file, .path = path, .decode = decode, .output = output};
  bool evaluated = evaluate(&reader);
  int closed = fclose(file);
  if (closed) {
    fprintf(stderr, "config: %s: close failed: %s\n", path, strerror(errno));
  }
  return evaluated && !closed ? CONFIG_OK : CONFIG_ERROR;
}

enum config_result config_read_bytes(const char *name, const char *bytes, size_t size,
    lua_CFunction decode, void *output)
{
  struct config_reader reader = {
    .memory = bytes, .memory_size = size, .path = name, .decode = decode, .output = output,
  };
  return evaluate(&reader) ? CONFIG_OK : CONFIG_ERROR;
}
