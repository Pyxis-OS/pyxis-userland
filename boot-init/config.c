#include "config.h"
#include "../libconfig/config.h"
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool valid_name(const char *name, size_t length)
{
  if (!length || length > BOOT_NAME_MAX) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = name[i];
    if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') {
      return false;
    }
  }
  return true;
}

static bool printable(const char *text, size_t length)
{
  for (size_t i = 0; i < length; ++i) {
    if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) {
      return false;
    }
  }
  return true;
}

/* Every root a space receives from boot init, besides configured volumes. */
static bool reserved_volume(const char *name)
{
  return !strcmp(name, "boot") || !strcmp(name, "tmp") || !strcmp(name, "bin");
}

/* Boot init's fallback space and Caelum's own space use these names. */
static bool reserved_space(const char *name)
{
  return !strcmp(name, "rescue") || !strcmp(name, "caelum");
}

/* Duplicates the string at INDEX, which must have MIN..MAX bytes. */
static char *take_string(lua_State *state, int index, size_t min, size_t max, const char *what)
{
  if (lua_type(state, index) != LUA_TSTRING) {
    luaL_error(state, "%s must be a string", what);
  }
  size_t length;
  const char *text = lua_tolstring(state, index, &length);
  if (length < min || length > max || strlen(text) != length || !printable(text, length)) {
    luaL_error(state, "%s must be %zu to %zu printable characters", what, min, max);
  }
  char *copy = strdup(text);
  if (!copy) {
    luaL_error(state, "cannot allocate %s", what);
  }
  return copy;
}

static uint64_t take_integer(lua_State *state, int index, uint64_t min, uint64_t max,
    const char *what)
{
  if (!lua_isinteger(state, index)) {
    luaL_error(state, "%s must be an integer", what);
  }
  lua_Integer value = lua_tointeger(state, index);
  if (value < 0 || (uint64_t)value < min || (uint64_t)value > max) {
    luaL_error(state, "%s must be from %llu to %llu", what, (unsigned long long)min,
        (unsigned long long)max);
  }
  return (uint64_t)value;
}

/* A sequence 1..n with no other keys. Returns n. */
static size_t sequence_length(lua_State *state, int index, const char *what)
{
  index = lua_absindex(state, index);
  if (lua_type(state, index) != LUA_TTABLE) {
    luaL_error(state, "%s must be a list", what);
  }
  size_t length = lua_rawlen(state, index), keys = 0;
  lua_pushnil(state);
  while (lua_next(state, index)) {
    ++keys;
    lua_pop(state, 1);
  }
  if (keys != length) {
    luaL_error(state, "%s must be a list without other keys", what);
  }
  return length;
}

static void *allocate_array(lua_State *state, size_t count, size_t size, const char *what)
{
  void *array = calloc(count ? count : 1, size);
  if (!array) {
    luaL_error(state, "cannot allocate %s", what);
  }
  return array;
}

static void read_volume(lua_State *state, struct boot_volume *volume)
{
  static const char *const keys[] = {"kind", "partition", "volume"};
  config_keys(state, -1, keys, sizeof(keys) / sizeof(keys[0]));
  config_field(state, -1, "kind");
  char *kind = take_string(state, -1, 1, 16, "volume kind");
  bool npfs = !strcmp(kind, "npfs"), virtio_fs = !strcmp(kind, "virtio-fs"),
      ram = !strcmp(kind, "ram");
  free(kind);
  lua_pop(state, 1);
  if (!npfs && !virtio_fs && !ram) {
    luaL_error(state, "volume %s: kind must be \"npfs\", \"virtio-fs\" or \"ram\"",
        volume->name);
  }
  volume->kind = npfs ? BOOT_VOLUME_NPFS : virtio_fs ? BOOT_VOLUME_VIRTIO_FS : BOOT_VOLUME_RAM;
  config_field(state, -1, "partition");
  config_field(state, -2, "volume");
  if (!npfs) {
    if (!lua_isnil(state, -2) || !lua_isnil(state, -1)) {
      luaL_error(state, "volume %s: only npfs volumes have a partition or volume",
          volume->name);
    }
  } else {
    volume->partition = take_integer(state, -2, 1, UINT32_MAX, "volume partition");
    volume->volume = take_string(state, -1, 1, MOUNT_VOLUME_NAME_MAX, "volume name");
  }
  lua_pop(state, 2);
}

static void read_volumes(lua_State *state, struct boot_config *config)
{
  config_field(state, 1, "volumes");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  if (lua_type(state, -1) != LUA_TTABLE) {
    luaL_error(state, "volumes must be a table of named volumes");
  }
  int table = lua_gettop(state);
  size_t count = 0;
  lua_pushnil(state);
  while (lua_next(state, table)) {
    ++count;
    lua_pop(state, 1);
  }
  config->volumes = allocate_array(state, count, sizeof(*config->volumes), "volumes");
  lua_pushnil(state);
  while (lua_next(state, table)) {
    size_t length;
    const char *name = lua_type(state, -2) == LUA_TSTRING ?
        lua_tolstring(state, -2, &length) : NULL;
    if (!name || !valid_name(name, length) || reserved_volume(name)) {
      luaL_error(state, "volume names must be 1 to %d of a-z, 0-9 and '-', "
          "other than boot, tmp and bin", BOOT_NAME_MAX);
    }
    if (lua_type(state, -1) != LUA_TTABLE) {
      luaL_error(state, "volume %s must be a table", name);
    }
    struct boot_volume *volume = &config->volumes[config->volume_count++];
    volume->name = strdup(name);
    if (!volume->name) {
      luaL_error(state, "cannot allocate volume name");
    }
    read_volume(state, volume);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

static void read_cpus(lua_State *state, struct boot_space *space)
{
  size_t count = sequence_length(state, -1, "cpus");
  if (!count) {
    luaL_error(state, "space %s: cpus must not be empty", space->name);
  }
  space->cpus = allocate_array(state, count, sizeof(*space->cpus), "cpus");
  for (size_t i = 1; i <= count; ++i) {
    lua_rawgeti(state, -1, (lua_Integer)i);
    uint64_t cpu = take_integer(state, -1, 0, BOOT_CPU_INDEX_MAX, "a CPU index");
    lua_pop(state, 1);
    for (size_t j = 0; j < space->cpu_count; ++j) {
      if (space->cpus[j] == cpu) {
        luaL_error(state, "space %s: CPU %llu is listed twice", space->name,
            (unsigned long long)cpu);
      }
    }
    space->cpus[space->cpu_count++] = cpu;
  }
}

static void read_root(lua_State *state, struct boot_space *space, struct boot_root *root)
{
  const char *access_name = NULL;
  if (lua_type(state, -1) == LUA_TTABLE) {
    static const char *const keys[] = {"access", "optional"};
    config_keys(state, -1, keys, sizeof(keys) / sizeof(keys[0]));
    config_field(state, -1, "optional");
    if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
      luaL_error(state, "space %s: root %s: optional must be a boolean", space->name,
          root->volume);
    }
    root->optional = lua_toboolean(state, -1);
    lua_pop(state, 1);
    config_field(state, -1, "access");
    access_name = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
  } else {
    lua_pushvalue(state, -1);
    access_name = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
  }
  if (!access_name || (strcmp(access_name, "read-only") && strcmp(access_name, "read-write"))) {
    luaL_error(state, "space %s: root %s: access must be \"read-only\" or \"read-write\"",
        space->name, root->volume);
  }
  root->read_write = !strcmp(access_name, "read-write");
  lua_pop(state, 1);
}

static void read_roots(lua_State *state, struct boot_space *space)
{
  if (lua_type(state, -1) != LUA_TTABLE) {
    luaL_error(state, "space %s: roots must be a table of volume names", space->name);
  }
  int table = lua_gettop(state);
  size_t count = 0;
  lua_pushnil(state);
  while (lua_next(state, table)) {
    ++count;
    lua_pop(state, 1);
  }
  space->roots = allocate_array(state, count, sizeof(*space->roots), "roots");
  lua_pushnil(state);
  while (lua_next(state, table)) {
    size_t length;
    const char *name = lua_type(state, -2) == LUA_TSTRING ?
        lua_tolstring(state, -2, &length) : NULL;
    if (!name || !valid_name(name, length) || reserved_volume(name)) {
      luaL_error(state, "space %s: roots must name configured volumes", space->name);
    }
    struct boot_root *root = &space->roots[space->root_count++];
    root->volume = strdup(name);
    if (!root->volume) {
      luaL_error(state, "cannot allocate root name");
    }
    read_root(state, space, root);
    lua_pop(state, 1);
  }
}

static void read_space(lua_State *state, struct boot_config *config, struct boot_space *space)
{
  static const char *const keys[] = {"name", "title", "init", "cpus", "roots", "start",
    "network", "launch"};
  if (lua_type(state, -1) != LUA_TTABLE) {
    luaL_error(state, "spaces must be tables");
  }
  config_keys(state, -1, keys, sizeof(keys) / sizeof(keys[0]));
  config_field(state, -1, "name");
  space->name = take_string(state, -1, 1, BOOT_NAME_MAX, "space name");
  lua_pop(state, 1);
  if (!valid_name(space->name, strlen(space->name)) || reserved_space(space->name)) {
    luaL_error(state, "space %s: names use a-z, 0-9 and '-', other than rescue and caelum",
        space->name);
  }
  for (size_t i = 0; i + 1 < config->space_count; ++i) {
    if (!strcmp(config->spaces[i].name, space->name)) {
      luaL_error(state, "space %s is configured twice", space->name);
    }
  }
  config_field(state, -1, "title");
  space->title = lua_isnil(state, -1) ? strdup(space->name) :
      take_string(state, -1, 1, SPACE_TITLE_MAX, "space title");
  if (!space->title) {
    luaL_error(state, "cannot allocate space title");
  }
  lua_pop(state, 1);
  config_field(state, -1, "init");
  space->init = take_string(state, -1, strlen(BOOT_INIT_PREFIX) + 1, BOOT_INIT_MAX, "init");
  lua_pop(state, 1);
  if (strncmp(space->init, BOOT_INIT_PREFIX, strlen(BOOT_INIT_PREFIX))) {
    luaL_error(state, "space %s: init must name a " BOOT_INIT_PREFIX " archive entry",
        space->name);
  }
  config_field(state, -1, "cpus");
  if (!lua_isnil(state, -1)) {
    read_cpus(state, space);
  }
  lua_pop(state, 1);
  config_field(state, -1, "roots");
  if (!lua_isnil(state, -1)) {
    read_roots(state, space);
  }
  lua_pop(state, 1);
  config_field(state, -1, "start");
  if (!lua_isnil(state, -1)) {
    space->start = take_string(state, -1, 1, BOOT_NAME_MAX, "start");
    if (!valid_name(space->start, strlen(space->start))) {
      luaL_error(state, "space %s: start must name a root", space->name);
    }
  }
  lua_pop(state, 1);
  config_field(state, -1, "network");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: network must be a boolean", space->name);
  }
  space->network = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "launch");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: launch must be a boolean", space->name);
  }
  space->launch = lua_toboolean(state, -1);
  lua_pop(state, 1);
}

static void read_spaces(lua_State *state, struct boot_config *config)
{
  config_field(state, 1, "spaces");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  size_t count = sequence_length(state, -1, "spaces");
  config->spaces = allocate_array(state, count, sizeof(*config->spaces), "spaces");
  for (size_t i = 1; i <= count; ++i) {
    lua_rawgeti(state, -1, (lua_Integer)i);
    read_space(state, config, &config->spaces[config->space_count++]);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

static int decode(lua_State *state)
{
  struct boot_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  static const char *const keys[] = {"volumes", "spaces"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));
  read_volumes(state, config);
  read_spaces(state, config);
  return 0;
}

static enum boot_config_result result_of(enum config_result result, struct boot_config *config)
{
  if (result != CONFIG_OK) {
    boot_config_free(config);
  }
  return result == CONFIG_OK ? BOOT_CONFIG_OK :
      result == CONFIG_MISSING ? BOOT_CONFIG_MISSING : BOOT_CONFIG_INVALID;
}

enum boot_config_result boot_config_read(const char *path, struct boot_config *config)
{
  *config = (struct boot_config){0};
  return result_of(config_read(path, decode, config), config);
}

enum boot_config_result boot_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config)
{
  *config = (struct boot_config){0};
  return result_of(config_read_bytes(name, bytes, size, decode, config), config);
}

void boot_config_free(struct boot_config *config)
{
  for (size_t i = 0; i < config->volume_count; ++i) {
    free(config->volumes[i].name);
    free(config->volumes[i].volume);
  }
  for (size_t i = 0; i < config->space_count; ++i) {
    struct boot_space *space = &config->spaces[i];
    for (size_t j = 0; j < space->root_count; ++j) {
      free(space->roots[j].volume);
    }
    free(space->roots);
    free(space->start);
    free(space->cpus);
    free(space->init);
    free(space->title);
    free(space->name);
  }
  free(config->spaces);
  free(config->volumes);
  *config = (struct boot_config){0};
}

const struct boot_volume *boot_plan_volume(const struct boot_plan *plan, const char *name)
{
  for (size_t i = 0; i < plan->volume_count; ++i) {
    if (!strcmp(plan->volumes[i]->name, name)) {
      return plan->volumes[i];
    }
  }
  return NULL;
}

static const struct boot_space *plan_space(const struct boot_plan *plan, const char *name,
    size_t *index)
{
  for (size_t i = 0; i < plan->space_count; ++i) {
    if (!strcmp(plan->spaces[i]->name, name)) {
      *index = i;
      return plan->spaces[i];
    }
  }
  return NULL;
}

bool boot_space_has_root(const struct boot_space *space, const char *name)
{
  if (reserved_volume(name)) {
    return true;
  }
  for (size_t i = 0; i < space->root_count; ++i) {
    if (!strcmp(space->roots[i].volume, name)) {
      return true;
    }
  }
  return false;
}

void boot_plan_free(struct boot_plan *plan)
{
  free(plan->volumes);
  free(plan->spaces);
  *plan = (struct boot_plan){0};
}

static bool plan_fail(struct boot_plan *plan, const char *format, const char *name)
{
  fprintf(stderr, "boot-init: ");
  fprintf(stderr, format, name);
  fputc('\n', stderr);
  boot_plan_free(plan);
  return false;
}

bool boot_plan_build(const struct boot_config *defaults, const struct boot_config *override,
    struct boot_plan *plan)
{
  *plan = (struct boot_plan){0};
  size_t volume_capacity = defaults->volume_count + (override ? override->volume_count : 0);
  size_t space_capacity = defaults->space_count + (override ? override->space_count : 0);
  plan->volumes = calloc(volume_capacity ? volume_capacity : 1, sizeof(*plan->volumes));
  plan->spaces = calloc(space_capacity ? space_capacity : 1, sizeof(*plan->spaces));
  if (!plan->volumes || !plan->spaces) {
    return plan_fail(plan, "cannot allocate the boot plan%s", "");
  }
  for (size_t i = 0; i < defaults->volume_count; ++i) {
    plan->volumes[plan->volume_count++] = &defaults->volumes[i];
  }
  for (size_t i = 0; i < defaults->space_count; ++i) {
    plan->spaces[plan->space_count++] = &defaults->spaces[i];
  }
  for (size_t i = 0; override && i < override->volume_count; ++i) {
    const struct boot_volume *volume = &override->volumes[i];
    if (!strcmp(volume->name, BOOT_SYSTEM_VOLUME)) {
      return plan_fail(plan, "the override cannot redefine the %s volume", BOOT_SYSTEM_VOLUME);
    }
    size_t index;
    for (index = 0; index < plan->volume_count; ++index) {
      if (!strcmp(plan->volumes[index]->name, volume->name)) {
        break;
      }
    }
    plan->volumes[index] = volume;
    if (index == plan->volume_count) {
      ++plan->volume_count;
    }
  }
  for (size_t i = 0; override && i < override->space_count; ++i) {
    size_t index;
    if (plan_space(plan, override->spaces[i].name, &index)) {
      plan->spaces[index] = &override->spaces[i];
    } else {
      plan->spaces[plan->space_count++] = &override->spaces[i];
    }
  }

  const char *owner = NULL;
  for (size_t i = 0; i < plan->space_count; ++i) {
    const struct boot_space *space = plan->spaces[i];
    for (size_t j = 0; j < space->root_count; ++j) {
      if (!boot_plan_volume(plan, space->roots[j].volume)) {
        return plan_fail(plan, "a root names undefined volume %s", space->roots[j].volume);
      }
    }
    if (space->start && !boot_space_has_root(space, space->start)) {
      return plan_fail(plan, "space %s starts in a root it does not have", space->name);
    }
    if (space->network && owner) {
      return plan_fail(plan, "more than one space sets network = true, including %s",
          space->name);
    }
    if (space->network) {
      owner = space->name;
    }
  }
  return true;
}
