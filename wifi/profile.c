#include "profile.h"
#include "../libconfig/config.h"
#include <lauxlib.h>
#include <file.h>
#include <handle.h>
#include <path.h>
#include <pyxis/working_path.h>
#include <stdlib.h>
#include <string.h>

#define PROFILE_BYTES 4096u
#define PROFILE_PATH_BYTES 1024u

static int hex_digit(char ch)
{
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static int decode(lua_State *state)
{
  struct wifi_open_target *target = lua_touserdata(state, 2);
  lua_settop(state, 1);
  static const char *const keys[] = {"ssid", "bssid", "channel", "security"};
  config_keys(state, 1, keys, 4);
  config_field(state, 1, "security");
  size_t bytes = 0;
  const char *value = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &bytes) : NULL;
  if (!value || bytes != 4 || memcmp(value, "open", 4)) {
    return luaL_error(state, "security must be explicitly open");
  }
  target->security = WIFI_SECURITY_OPEN;
  lua_pop(state, 1);
  config_field(state, 1, "ssid");
  value = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &bytes) : NULL;
  if (!value || !bytes || bytes > sizeof(target->ssid)) {
    return luaL_error(state, "ssid must contain 1-32 bytes");
  }
  target->ssid_bytes = bytes;
  memcpy(target->ssid, value, bytes);
  lua_pop(state, 1);
  config_field(state, 1, "bssid");
  value = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &bytes) : NULL;
  if (!value || bytes != 17) {
    return luaL_error(state, "bssid must be six hexadecimal octets separated by colons");
  }
  uint8_t nonzero = 0;
  for (size_t i = 0; i < 6; ++i) {
    int high = hex_digit(value[i * 3]), low = hex_digit(value[i * 3 + 1]);
    if (high < 0 || low < 0 || (i < 5 && value[i * 3 + 2] != ':')) {
      return luaL_error(state, "invalid bssid octet");
    }
    target->bssid[i] = (uint8_t)((high << 4) | low);
    nonzero |= target->bssid[i];
  }
  if (!nonzero || (target->bssid[0] & 1)) {
    return luaL_error(state, "bssid must be nonzero unicast");
  }
  lua_pop(state, 1);
  config_field(state, 1, "channel");
  lua_Number channel = luaL_checknumber(state, -1);
  if (!(channel >= 1 && channel <= 11) || channel != (uint32_t)channel) {
    return luaL_error(state, "channel must be an integer from 1 to 11");
  }
  target->channel = (uint8_t)channel;
  return 0;
}

static void erase(void *memory, size_t bytes)
{
  volatile unsigned char *cursor = memory;
  while (bytes--) *cursor++ = 0;
}

enum call_status wifi_read_profile(const char *path, struct wifi_open_target *target)
{
  *target = (struct wifi_open_target){0};
  size_t path_bytes = strlen(path);
  if (!path_bytes || path_bytes > PROFILE_PATH_BYTES) {
    return CALL_BAD_REQUEST;
  }
  const struct path_context *context;
  enum call_status status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    return status;
  }
  size_t slots = context->count + path_bytes + 1;
  if (slots < context->count || slots > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(path_bytes + 3);
  if (!directories || !component) {
    free(directories);
    free(component);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = path_bytes + 3,
  };
  handle_t file = HANDLE_INVALID;
  status = path_resolve_native(context, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &workspace, &file);
  free(directories);
  free(component);
  struct handle_info info;
  if (status == CALL_OK) {
    status = handle_query(file, &info);
  }
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE ||
      info.transport || info.rights != FILE_RIGHT_READ)) {
    status = CALL_DENIED;
  }
  uint64_t length = 0;
  if (status == CALL_OK) {
    status = file_size(file, &length);
  }
  if (status == CALL_OK && length > PROFILE_BYTES) {
    status = CALL_FILE_TOO_LARGE;
  }
  char bytes[PROFILE_BYTES + 1] = {0};
  size_t offset = 0;
  while (status == CALL_OK && offset < length) {
    size_t count = 0;
    status = file_read(file, offset, bytes + offset, (size_t)length - offset, &count);
    if (status == CALL_OK && !count) status = CALL_IO;
    offset += count;
  }
  if (status == CALL_OK) {
    size_t extra = 0;
    status = file_read(file, length, bytes + length, 1, &extra);
    if (status == CALL_OK && extra) status = CALL_IO;
  }
  if (file && handle_close(file) != 0 && status == CALL_OK) status = CALL_BAD_HANDLE;
  if (status == CALL_OK && config_read_literal_bytes("Wi-Fi private profile", bytes,
      (size_t)length, decode, target) != CONFIG_OK) {
    status = CALL_BAD_REQUEST;
  }
  erase(bytes, sizeof(bytes));
  if (status != CALL_OK) erase(target, sizeof(*target));
  return status;
}
