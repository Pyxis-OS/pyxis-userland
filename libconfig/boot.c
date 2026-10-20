#include "boot.h"
#include "config.h"
#include <abi/input_policy.h>
#include <abi/wifi.h>
#include <bundle_launch.h>
#include <package.h>
#include <package_maintenance.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdarg.h>
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

static bool take_access(lua_State *state, int index, const char *what)
{
  char *access = take_string(state, index, 1, 16, what);
  bool read_write = !strcmp(access, "read-write");
  bool valid = read_write || !strcmp(access, "read-only");
  free(access);
  if (!valid) {
    luaL_error(state, "%s must be read-only or read-write", what);
  }
  return read_write;
}

static unsigned guid_digit(lua_State *state, char digit)
{
  if (digit >= '0' && digit <= '9') {
    return (unsigned)(digit - '0');
  }
  if (digit >= 'a' && digit <= 'f') {
    return (unsigned)(digit - 'a') + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return (unsigned)(digit - 'A') + 10;
  }
  luaL_error(state, "disk_guid must be a canonical nonzero GPT GUID");
  return 0;
}

static void take_guid(lua_State *state, int index, uint8_t guid[16])
{
  size_t length = 0;
  const char *text = lua_type(state, index) == LUA_TSTRING ?
      lua_tolstring(state, index, &length) : NULL;
  if (!text || length != 36 || strlen(text) != length || text[8] != '-' ||
      text[13] != '-' || text[18] != '-' || text[23] != '-') {
    luaL_error(state, "disk_guid must be a canonical nonzero GPT GUID");
  }
  /* GPT stores the first three canonical fields little-endian. */
  static const uint8_t positions[16] = {
    6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34,
  };
  unsigned nonzero = 0;
  for (size_t i = 0; i < 16; ++i) {
    size_t position = positions[i];
    guid[i] = (uint8_t)((guid_digit(state, text[position]) << 4) |
        guid_digit(state, text[position + 1]));
    nonzero |= guid[i];
  }
  if (!nonzero) {
    luaL_error(state, "disk_guid must be a canonical nonzero GPT GUID");
  }
}

static void read_volume(lua_State *state, struct boot_volume *volume)
{
  static const char *const keys[] = {"kind", "partition", "volume", "disk_guid", "access"};
  config_keys(state, -1, keys, sizeof(keys) / sizeof(keys[0]));
  config_field(state, -1, "kind");
  char *kind = take_string(state, -1, 1, 16, "volume kind");
  bool npfs = !strcmp(kind, "npfs"), virtio_fs = !strcmp(kind, "virtio-fs"),
      ram = !strcmp(kind, "ram"), ram_npfs = !strcmp(kind, "ram-npfs"),
      boot_esp = !strcmp(kind, "boot-esp"), fat32 = !strcmp(kind, "fat32");
  free(kind);
  lua_pop(state, 1);
  if (!npfs && !virtio_fs && !ram && !ram_npfs && !boot_esp && !fat32) {
    luaL_error(state, "volume %s: kind must be npfs, ram-npfs, virtio-fs, ram, boot-esp or fat32",
        volume->name);
  }
  volume->kind = npfs ? BOOT_VOLUME_NPFS : ram_npfs ? BOOT_VOLUME_RAM_NPFS :
      virtio_fs ? BOOT_VOLUME_VIRTIO_FS : ram ? BOOT_VOLUME_RAM : boot_esp ? BOOT_VOLUME_BOOT_ESP : BOOT_VOLUME_FAT32;
  config_field(state, -1, "partition");
  if (npfs || fat32) {
    volume->partition = take_integer(state, -1, 1, UINT32_MAX, "volume partition");
  } else if (!lua_isnil(state, -1)) {
    luaL_error(state, "volume %s: this kind has no partition selector", volume->name);
  }
  lua_pop(state, 1);
  config_field(state, -1, "volume");
  if (npfs || ram_npfs) {
    volume->volume = take_string(state, -1, 1, MOUNT_VOLUME_NAME_MAX, "volume name");
  } else if (!lua_isnil(state, -1)) {
    luaL_error(state, "volume %s: only npfs and ram-npfs have a volume name", volume->name);
  }
  lua_pop(state, 1);
  config_field(state, -1, "disk_guid");
  if (fat32) {
    take_guid(state, -1, volume->disk_guid);
  } else if (!lua_isnil(state, -1)) {
    luaL_error(state, "volume %s: only fat32 has a disk_guid selector", volume->name);
  }
  lua_pop(state, 1);
  config_field(state, -1, "access");
  if (fat32) {
    volume->read_write = take_access(state, -1, "FAT32 admission access");
  } else if (!lua_isnil(state, -1)) {
    luaL_error(state, "volume %s: only fat32 has admission access", volume->name);
  }
  lua_pop(state, 1);
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
    static const char *const keys[] = {"access", "optional", "package"};
    config_keys(state, -1, keys, sizeof(keys) / sizeof(keys[0]));
    config_field(state, -1, "optional");
    if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
      luaL_error(state, "space %s: root %s: optional must be a boolean", space->name,
          root->name);
    }
    root->optional = lua_toboolean(state, -1);
    lua_pop(state, 1);
    config_field(state, -1, "package");
    if (!lua_isnil(state, -1)) {
      size_t length = 0;
      const char *package = lua_type(state, -1) == LUA_TSTRING ?
          lua_tolstring(state, -1, &length) : NULL;
      if (!package || strlen(package) != length || !package_id_valid(package)) {
        luaL_error(state, "space %s: root %s: package must be a package ID", space->name,
            root->name);
      }
      root->package = strdup(package);
      if (!root->package) {
        luaL_error(state, "cannot allocate package name");
      }
      lua_pop(state, 1);
      config_field(state, -1, "access");
      if (!lua_isnil(state, -1) && (lua_type(state, -1) != LUA_TSTRING ||
          strcmp(lua_tostring(state, -1), "read-only"))) {
        luaL_error(state, "space %s: root %s: package roots are read-only", space->name,
            root->name);
      }
      lua_pop(state, 1);
      return;
    }
    lua_pop(state, 1);
    config_field(state, -1, "access");
    access_name = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
  } else {
    lua_pushvalue(state, -1);
    access_name = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
  }
  if (!access_name || (strcmp(access_name, "read-only") && strcmp(access_name, "read-write"))) {
    luaL_error(state, "space %s: root %s: access must be \"read-only\" or \"read-write\"",
        space->name, root->name);
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
    if (!name || !valid_name(name, length) || reserved_volume(name) ||
        !strcmp(name, BOOT_ACPI_ROOT)) {
      luaL_error(state, "space %s: roots must name configured volumes or packages", space->name);
    }
    struct boot_root *root = &space->roots[space->root_count++];
    root->name = strdup(name);
    if (!root->name) {
      luaL_error(state, "cannot allocate root name");
    }
    read_root(state, space, root);
    root->volume = strdup(root->package ? BOOT_PACKAGES_VOLUME : name);
    if (!root->volume) {
      luaL_error(state, "cannot allocate root name");
    }
    lua_pop(state, 1);
  }
}

static bool valid_source_name(const char *name, size_t length)
{
  if (!length || length > BOOT_NAME_MAX || strlen(name) != length ||
      !strcmp(name, "fat_mounts") || !strcmp(name, PACKAGE_MANAGEMENT_SOURCE) ||
      !strcmp(name, PACKAGE_INSPECT_SOURCE) || !strcmp(name, PACKAGE_CONFIG_SOURCE) ||
      !strcmp(name, PACKAGE_CONFIG_WRITE_SOURCE)) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = name[i];
    if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

static bool valid_source_path(const char *path)
{
  size_t components = 0;
  for (size_t start = 0;;) {
    size_t end = start;
    while (path[end] && path[end] != '/') ++end;
    size_t size = end - start;
    if (!size || (size == 1 && path[start] == '.') ||
        (size == 2 && path[start] == '.' && path[start + 1] == '.') ||
        ++components > BOOT_SOURCE_PATH_COMPONENTS) {
      return false;
    }
    if (!path[end]) return true;
    start = end + 1;
  }
}

static void read_grants(lua_State *state, struct boot_space *space)
{
  if (lua_type(state, -1) != LUA_TTABLE) {
    luaL_error(state, "space %s: grants must be a table of named sources", space->name);
  }
  int table = lua_gettop(state);
  size_t count = 0;
  lua_pushnil(state);
  while (lua_next(state, table)) {
    ++count;
    lua_pop(state, 1);
  }
  if (count > BUNDLE_SOURCE_LIMIT) {
    luaL_error(state, "space %s: too many selected sources", space->name);
  }
  space->grants = allocate_array(state, count, sizeof(*space->grants), "selected sources");
  lua_pushnil(state);
  while (lua_next(state, table)) {
    size_t length = 0;
    const char *name = lua_type(state, -2) == LUA_TSTRING ?
        lua_tolstring(state, -2, &length) : NULL;
    if (!name || !valid_source_name(name, length)) {
      luaL_error(state, "space %s: source names use a-z, 0-9, '-' and '_'; "
          "management and inspection source names are reserved", space->name);
    }
    static const char *const keys[] = {"volume", "kind", "access", "optional", "path"};
    config_keys(state, -1, keys, sizeof(keys) / sizeof(keys[0]));
    struct boot_grant *grant = &space->grants[space->grant_count++];
    grant->name = strdup(name);
    if (!grant->name) {
      luaL_error(state, "cannot allocate selected source name");
    }
    config_field(state, -1, "volume");
    grant->volume = take_string(state, -1, 1, BOOT_NAME_MAX, "selected source volume");
    lua_pop(state, 1);
    config_field(state, -1, "kind");
    char *kind = take_string(state, -1, 1, 16, "selected source kind");
    bool directory = !strcmp(kind, "directory"), mount = !strcmp(kind, "mount");
    free(kind);
    if (!directory && !mount) {
      luaL_error(state, "selected source kind must be directory or mount");
    }
    grant->kind = directory ? BOOT_GRANT_DIRECTORY : BOOT_GRANT_MOUNT;
    lua_pop(state, 1);
    config_field(state, -1, "path");
    if (!lua_isnil(state, -1)) {
      if (!directory) luaL_error(state, "selected mount source cannot have a path");
      grant->path = take_string(state, -1, 1, BOOT_SOURCE_PATH_MAX, "selected source path");
      if (!valid_source_path(grant->path)) {
        luaL_error(state, "selected source path must contain 1 to %u relative components without empty, . or ..",
            BOOT_SOURCE_PATH_COMPONENTS);
      }
    }
    lua_pop(state, 1);
    config_field(state, -1, "access");
    grant->read_write = take_access(state, -1, "selected source access");
    lua_pop(state, 1);
    config_field(state, -1, "optional");
    if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
      luaL_error(state, "selected source optional must be a boolean");
    }
    grant->optional = lua_toboolean(state, -1);
    lua_pop(state, 1);
    lua_pop(state, 1);
  }
}

static void read_space(lua_State *state, struct boot_config *config, struct boot_space *space)
{
  static const char *const keys[] = {"name", "title", "init", "cpus", "roots", "start",
    "network", "focus", "launch", "jobs", "multiplexer", "remote_multiplexer", "clipboard_local", "clipboard_shared",
    "screenshot", "acpi_tables", "power", "remote_power", "grants", "fat32_mount", "package_management", "wifi"};
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
  config_field(state, -1, "grants");
  if (!lua_isnil(state, -1)) {
    read_grants(state, space);
  }
  lua_pop(state, 1);
  config_field(state, -1, "fat32_mount");
  if (!lua_isnil(state, -1)) {
    space->fat32_mount = true;
    space->fat32_mount_write = take_access(state, -1, "fat32_mount");
  }
  lua_pop(state, 1);
  if (space->grant_count + space->fat32_mount > BUNDLE_SOURCE_LIMIT) {
    luaL_error(state, "space %s: too many selected sources", space->name);
  }
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
  config_field(state, -1, "focus");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: focus must be a boolean", space->name);
  }
  space->focus = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "launch");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: launch must be a boolean", space->name);
  }
  space->launch = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "jobs");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: jobs must be a boolean", space->name);
  }
  space->jobs = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "package_management");
  if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TBOOLEAN) {
    luaL_error(state, "space %s: package_management must be a boolean", space->name);
  }
  space->package_management = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "multiplexer");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: multiplexer must be a boolean", space->name);
  }
  space->multiplexer = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "remote_multiplexer");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: remote_multiplexer must be a boolean", space->name);
  }
  space->remote_multiplexer = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "clipboard_local");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: clipboard_local must be a boolean", space->name);
  }
  space->clipboard_local = lua_isnil(state, -1) ? space->multiplexer : lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "clipboard_shared");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: clipboard_shared must be a boolean", space->name);
  }
  space->clipboard_shared = lua_isnil(state, -1) ? space->multiplexer : lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "screenshot");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: screenshot must be a boolean", space->name);
  }
  space->screenshot = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "acpi_tables");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: acpi_tables must be a boolean", space->name);
  }
  space->acpi_tables = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "power");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: power must be a boolean", space->name);
  }
  space->power = lua_toboolean(state, -1);
  lua_pop(state, 1);
  config_field(state, -1, "wifi");
  if (!lua_isnil(state, -1)) {
    static const char *const wifi_keys[] = {"read", "scan", "connect"};
    static const uint64_t wifi_rights[] = {WIFI_RIGHT_READ, WIFI_RIGHT_SCAN, WIFI_RIGHT_CONNECT};
    if (!lua_istable(state, -1)) {
      luaL_error(state, "space %s: wifi must be a table", space->name);
    }
    config_keys(state, -1, wifi_keys, 3);
    for (size_t i = 0; i < 3; ++i) {
      config_field(state, -1, wifi_keys[i]);
      if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
        luaL_error(state, "space %s: wifi.%s must be boolean", space->name, wifi_keys[i]);
      }
      if (lua_toboolean(state, -1)) {
        space->wifi_rights |= wifi_rights[i];
      }
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  config_field(state, -1, "remote_power");
  if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
    luaL_error(state, "space %s: remote_power must be a boolean", space->name);
  }
  space->remote_power = lua_toboolean(state, -1);
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

static bool read_scroll_direction(lua_State *state, int input, const char *key)
{
  config_field(state, input, key);
  bool natural = true;
  if (!lua_isnil(state, -1)) {
    size_t length = 0;
    const char *value = lua_type(state, -1) == LUA_TSTRING ?
        lua_tolstring(state, -1, &length) : NULL;
    if (!machine_scroll_direction_parse(value, length, &natural)) {
      luaL_error(state, "input.%s must be natural or traditional", key);
    }
  }
  lua_pop(state, 1);
  return natural;
}

static void read_touchpad_speed(lua_State *state, int input, struct boot_config *config)
{
  config_field(state, input, "touchpad_speed");
  if (!lua_isnil(state, -1)) {
    lua_Number speed = lua_tonumber(state, -1);
    if (lua_type(state, -1) != LUA_TNUMBER || !__builtin_isfinite(speed) ||
        speed < -1 || speed > 1) {
      luaL_error(state, "input.touchpad_speed must be a finite number from -1 to 1");
    }
    lua_Number permille = speed * INPUT_TOUCHPAD_SPEED_MAX;
    config->touchpad_speed = (int32_t)(permille + (permille < 0 ? -0.5 : 0.5));
    config->touchpad_speed_present = true;
  }
  lua_pop(state, 1);
}

static void read_num_lock(lua_State *state, int input, struct boot_config *config)
{
  config_field(state, input, "num_lock");
  if (!lua_isnil(state, -1)) {
    if (lua_type(state, -1) != LUA_TBOOLEAN) {
      luaL_error(state, "input.num_lock must be a boolean");
    }
    config->num_lock = lua_toboolean(state, -1);
    config->num_lock_present = true;
  }
  lua_pop(state, 1);
}

static void read_input(lua_State *state, struct boot_config *config, bool archive)
{
  config_field(state, 1, "input");
  if (!lua_isnil(state, -1)) {
    static const char *const archive_keys[] = {"mouse_scroll_direction", "touchpad_scroll_direction",
        "diagnostics", "touchpad_speed", "num_lock"};
    static const char *const override_keys[] = {"touchpad_speed", "num_lock"};
    if (archive) {
      config_keys(state, -1, archive_keys, sizeof(archive_keys) / sizeof(archive_keys[0]));
    } else {
      config_keys(state, -1, override_keys, sizeof(override_keys) / sizeof(override_keys[0]));
    }
    int input = lua_absindex(state, -1);
    read_touchpad_speed(state, input, config);
    read_num_lock(state, input, config);
    if (archive) {
      config->mouse_scroll_natural = read_scroll_direction(state, input, archive_keys[0]);
      config->touchpad_scroll_natural = read_scroll_direction(state, input, archive_keys[1]);
      config_field(state, input, "diagnostics");
      if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TBOOLEAN) {
        luaL_error(state, "input.diagnostics must be a boolean");
      }
      config->input_diagnostics = lua_toboolean(state, -1);
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
}

static int decode_override(lua_State *state)
{
  struct boot_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  static const char *const keys[] = {"input", "volumes", "spaces"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));
  read_input(state, config, false);
  read_volumes(state, config);
  read_spaces(state, config);
  return 0;
}

static int decode_archive(lua_State *state)
{
  struct boot_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  static const char *const keys[] = {"hostname", "input", "volumes", "spaces", "ram_npfs_bytes", "wifi_policy"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));
  config_field(state, 1, "hostname");
  const char *name = MACHINE_HOSTNAME_DEFAULT;
  size_t length = sizeof(MACHINE_HOSTNAME_DEFAULT) - 1;
  if (!lua_isnil(state, -1)) {
    name = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &length) : NULL;
  }
  if (!machine_hostname_valid(name, length)) {
    luaL_error(state, "hostname must be 1 to %d ASCII letters, digits or internal hyphens",
        MACHINE_HOSTNAME_MAX);
  }
  memcpy(config->hostname, name, length);
  config->hostname[length] = '\0';
  lua_pop(state, 1);
  config_field(state, 1, "ram_npfs_bytes");
  if (!lua_isnil(state, -1)) {
    config->ram_npfs_bytes = take_integer(state, -1,
        MOUNT_RAM_MIN_BYTES, MOUNT_RAM_MAX_BYTES, "ram_npfs_bytes");
    if (config->ram_npfs_bytes % MOUNT_RAM_ALIGNMENT) {
      luaL_error(state, "ram_npfs_bytes must be aligned to 4096 bytes");
    }
  }
  lua_pop(state, 1);
  config_field(state, 1, "wifi_policy");
  if (!lua_isnil(state, -1)) {
    static const char *const policy_keys[] = {"country", "channels"};
    config_keys(state, -1, policy_keys, 2);
    config_field(state, -1, "country");
    size_t country_bytes = 0;
    const char *country = lua_type(state, -1) == LUA_TSTRING ?
        lua_tolstring(state, -1, &country_bytes) : NULL;
    if (!country || country_bytes != 2 || country[0] < 'A' || country[0] > 'Z' ||
        country[1] < 'A' || country[1] > 'Z' || (country[0] == 'Z' && country[1] == 'Z')) {
      return luaL_error(state, "wifi_policy.country must be an explicit upper-case country excluding ZZ");
    }
    memcpy(config->wifi_country, country, 2);
    lua_pop(state, 1);
    config_field(state, -1, "channels");
    lua_Number mask = luaL_checknumber(state, -1);
    if (!(mask >= 1 && mask <= WIFI_SCAN_CHANNELS) || mask != (uint32_t)mask) {
      return luaL_error(state, "wifi_policy.channels must be a nonempty 2.4 GHz channel mask");
    }
    config->wifi_channels = (uint32_t)mask;
    lua_pop(state, 1);

  }
  lua_pop(state, 1);
  read_input(state, config, true);
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
  *config = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
  return result_of(config_read(path, decode_archive, config), config);
}

enum boot_config_result boot_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config)
{
  *config = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
  return result_of(config_read_bytes(name, bytes, size, decode_override, config), config);
}

enum boot_config_result boot_config_read_archive_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config)
{
  *config = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
  return result_of(config_read_bytes(name, bytes, size, decode_archive, config), config);
}

enum boot_config_result boot_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config)
{
  *config = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
  return result_of(config_read_literal_bytes(name, bytes, size, decode_override, config), config);
}

enum boot_config_result boot_config_read_archive_literal_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config)
{
  *config = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
  return result_of(config_read_literal_bytes(name, bytes, size, decode_archive, config), config);
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
      free(space->roots[j].name);
      free(space->roots[j].volume);
      free(space->roots[j].package);
    }
    free(space->roots);
    for (size_t j = 0; j < space->grant_count; ++j) {
      free(space->grants[j].name);
      free(space->grants[j].volume);
      free(space->grants[j].path);
    }
    free(space->grants);
    free(space->start);
    free(space->cpus);
    free(space->init);
    free(space->title);
    free(space->name);
  }
  free(config->spaces);
  free(config->volumes);
  *config = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
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

static bool same_backing(const struct boot_volume *first, const struct boot_volume *second)
{
  return first == second || (first->kind == second->kind &&
      (first->kind == BOOT_VOLUME_NPFS || first->kind == BOOT_VOLUME_RAM_NPFS) &&
      first->partition == second->partition && !strcmp(first->volume, second->volume));
}

static bool private_http_backing(const struct boot_plan *plan, const struct boot_volume *backing)
{
  const struct boot_volume *system = boot_plan_volume(plan, BOOT_SYSTEM_VOLUME);
  const struct boot_volume *packages = boot_plan_volume(plan, BOOT_PACKAGES_VOLUME);
  if ((system && same_backing(backing, system)) ||
      (packages && same_backing(backing, packages))) {
    return false;
  }
  if (backing->kind == BOOT_VOLUME_NPFS && system &&
      system->kind == BOOT_VOLUME_NPFS && backing->partition == system->partition &&
      !strcmp(backing->volume, "bin")) {
    return false;
  }
  for (size_t i = 0; i < plan->space_count; ++i) {
    const struct boot_space *space = plan->spaces[i];
    for (size_t j = 0; j < space->root_count; ++j) {
      const struct boot_volume *volume = boot_plan_volume(plan, space->roots[j].volume);
      if (volume && same_backing(backing, volume)) {
        return false;
      }
    }
    for (size_t j = 0; j < space->grant_count; ++j) {
      const struct boot_grant *source = &space->grants[j];
      const struct boot_volume *volume = boot_plan_volume(plan, source->volume);
      if (volume && strcmp(source->name, BOOT_HTTP_STAGING_SOURCE) &&
          same_backing(backing, volume)) {
        return false;
      }
    }
  }
  return true;
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
    if (!strcmp(space->roots[i].name, name)) {
      return true;
    }
  }
  return false;
}

void boot_plan_free(struct boot_plan *plan)
{
  free(plan->volumes);
  free(plan->volume_sources);
  free(plan->spaces);
  free(plan->space_sources);
  *plan = (struct boot_plan){0};
}

static bool plan_fail(struct boot_plan *plan, const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "boot-init: ");
  vfprintf(stderr, format, arguments);
  va_end(arguments);
  fputc('\n', stderr);
  boot_plan_free(plan);
  return false;
}

bool boot_plan_build(const struct boot_config *defaults, const struct boot_config *override,
    struct boot_plan *plan, enum boot_plan_error *error)
{
  *plan = (struct boot_plan){0};
  if (error) {
    *error = BOOT_PLAN_INVALID;
  }
  if (override && (override->volume_count > SIZE_MAX - defaults->volume_count ||
      override->space_count > SIZE_MAX - defaults->space_count)) {
    return plan_fail(plan, "boot plan is too large%s", "");
  }
  size_t volume_capacity = defaults->volume_count + (override ? override->volume_count : 0);
  size_t space_capacity = defaults->space_count + (override ? override->space_count : 0);
  plan->volumes = calloc(volume_capacity ? volume_capacity : 1, sizeof(*plan->volumes));
  plan->spaces = calloc(space_capacity ? space_capacity : 1, sizeof(*plan->spaces));
  plan->volume_sources = calloc(volume_capacity ? volume_capacity : 1,
      sizeof(*plan->volume_sources));
  plan->space_sources = calloc(space_capacity ? space_capacity : 1,
      sizeof(*plan->space_sources));
  if (!plan->volumes || !plan->spaces || !plan->volume_sources || !plan->space_sources) {
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
    size_t index;
    for (index = 0; index < plan->volume_count; ++index) {
      if (!strcmp(plan->volumes[index]->name, volume->name)) {
        break;
      }
    }
    plan->volumes[index] = volume;
    plan->volume_sources[index] = BOOT_ENTRY_OVERRIDE;
    if (index == plan->volume_count) {
      ++plan->volume_count;
    }
  }
  for (size_t i = 0; override && i < override->space_count; ++i) {
    size_t index;
    if (plan_space(plan, override->spaces[i].name, &index)) {
      plan->spaces[index] = &override->spaces[i];
      plan->space_sources[index] = BOOT_ENTRY_OVERRIDE;
    } else {
      plan->space_sources[plan->space_count] = BOOT_ENTRY_OVERRIDE;
      plan->spaces[plan->space_count++] = &override->spaces[i];
    }
  }

  const char *focus = NULL;
  for (size_t i = 0; i < plan->space_count; ++i) {
    const struct boot_space *space = plan->spaces[i];
    if (!space->focus) {
      continue;
    }
    if (focus) {
      if (error) {
        *error = BOOT_PLAN_DUPLICATE_FOCUS;
      }
      return plan_fail(plan, "more than one space sets focus = true, including %s and %s",
          focus, space->name);
    }
    focus = space->name;
  }
  for (size_t i = 0; override && i < override->volume_count; ++i) {
    if (!strcmp(override->volumes[i].name, BOOT_SYSTEM_VOLUME)) {
      return plan_fail(plan, "the override cannot redefine the %s volume", BOOT_SYSTEM_VOLUME);
    }
  }

  for (size_t i = 0; i < plan->volume_count; ++i) {
    const struct boot_volume *volume = plan->volumes[i];
    if (volume->kind == BOOT_VOLUME_RAM_NPFS && !defaults->ram_npfs_bytes) {
      return plan_fail(plan, "volume %s requires archive ram_npfs_bytes", volume->name);
    }
    if (!strcmp(volume->name, BOOT_HTTP_WORK_VOLUME) &&
        (volume->kind != BOOT_VOLUME_RAM && volume->kind != BOOT_VOLUME_NPFS &&
         volume->kind != BOOT_VOLUME_RAM_NPFS)) {
      return plan_fail(plan, "volume %s requires private RAM or NPFS backing", volume->name);
    }
    if (!strcmp(volume->name, BOOT_HTTP_WORK_VOLUME) && !private_http_backing(plan, volume)) {
      return plan_fail(plan, "volume %s must remain outside all client roots and sources", volume->name);
    }
    if (volume->kind != BOOT_VOLUME_FAT32) {
      continue;
    }
    for (size_t j = 0; j < i; ++j) {
      const struct boot_volume *alias = plan->volumes[j];
      if (alias->kind == BOOT_VOLUME_FAT32 && alias->partition == volume->partition &&
          !memcmp(alias->disk_guid, volume->disk_guid, sizeof(volume->disk_guid)) &&
          alias->read_write != volume->read_write) {
        return plan_fail(plan, "FAT32 volume %s conflicts with another alias's admission mode",
            volume->name);
      }
    }
  }

  const char *owner = NULL;
  for (size_t i = 0; i < plan->space_count; ++i) {
    const struct boot_space *space = plan->spaces[i];
    const struct boot_volume *packages = boot_plan_volume(plan, BOOT_PACKAGES_VOLUME);
    if (space->package_management && (!packages ||
        (packages->kind != BOOT_VOLUME_NPFS && packages->kind != BOOT_VOLUME_RAM_NPFS))) {
      return plan_fail(plan, "space %s: package_management needs an npfs packages volume", space->name);
    }
    size_t package_sources = (packages != NULL) +
        (boot_plan_volume(plan, BOOT_SYSTEM_VOLUME) != NULL) + (space->package_management ? 2 : 0);
    if (space->grant_count + space->fat32_mount + package_sources > BUNDLE_SOURCE_LIMIT) {
      return plan_fail(plan, "space %s: too many selected sources including packages", space->name);
    }
    for (size_t j = 0; j < space->root_count; ++j) {
      const struct boot_volume *volume = boot_plan_volume(plan, space->roots[j].volume);
      if (!volume) {
        return plan_fail(plan, "space %s: root %s names undefined volume %s",
            space->name, space->roots[j].name, space->roots[j].volume);
      }
      /* A tree opened from a mount that boot init retires on exit would vanish. */
      if (space->roots[j].package && volume->kind != BOOT_VOLUME_NPFS &&
          volume->kind != BOOT_VOLUME_RAM_NPFS) {
        return plan_fail(plan, "space %s: package root %s needs an npfs %s volume",
            space->name, space->roots[j].name, BOOT_PACKAGES_VOLUME);
      }
      if (space->roots[j].read_write && (volume->kind == BOOT_VOLUME_FAT32 ||
          volume->kind == BOOT_VOLUME_BOOT_ESP)) {
        return plan_fail(plan, "space %s: ordinary FAT32 root %s must be read-only",
            space->name, volume->name);
      }
    }
    for (size_t j = 0; j < space->grant_count; ++j) {
      const struct boot_grant *grant = &space->grants[j];
      const struct boot_volume *volume = boot_plan_volume(plan, grant->volume);
      if (!volume) {
        return plan_fail(plan, "space %s: source %s names undefined volume %s",
            space->name, grant->name, grant->volume);
      }
      if (!strcmp(grant->name, BOOT_HTTP_STAGING_SOURCE) &&
          (grant->kind != BOOT_GRANT_DIRECTORY || !grant->read_write ||
           (volume->kind != BOOT_VOLUME_RAM && volume->kind != BOOT_VOLUME_NPFS &&
            volume->kind != BOOT_VOLUME_RAM_NPFS) ||
           !private_http_backing(plan, volume))) {
        return plan_fail(plan, "source %s requires a private read-write RAM or NPFS volume",
            grant->name);
      }
      bool fat32 = volume->kind == BOOT_VOLUME_FAT32 || volume->kind == BOOT_VOLUME_BOOT_ESP;
      if (grant->kind == BOOT_GRANT_MOUNT && !fat32) {
        return plan_fail(plan, "space %s: mount source %s requires a FAT32 volume",
            space->name, grant->name);
      }
      if (grant->read_write && fat32 && !volume->read_write) {
        return plan_fail(plan, "space %s: writable source %s requires read-write FAT32 admission",
            space->name, grant->name);
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
  if (error) {
    *error = BOOT_PLAN_OK;
  }
  return true;
}
