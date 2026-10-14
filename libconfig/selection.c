#include "selection.h"
#include "literal.h"
#include <stdlib.h>
#include <string.h>

void config_selection_free(struct config_selection *selection)
{
  boot_plan_free(&selection->boot);
  boot_config_free(&selection->override);
  boot_config_free(&selection->defaults);
  session_config_free(&selection->session);
  network_config_free(&selection->network);
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    free(selection->bytes[i]);
  }
  memset(selection, 0, sizeof(*selection));
}

static bool freeze(const struct config_bytes source[CONFIG_LUA_COUNT],
    struct config_selection *selection)
{
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (!source[i].bytes || !source[i].size || source[i].size > CONFIG_LITERAL_SOURCE_LIMIT) {
      return false;
    }
    selection->bytes[i] = malloc(source[i].size + 1);
    if (!selection->bytes[i]) {
      return false;
    }
    memcpy(selection->bytes[i], source[i].bytes, source[i].size);
    selection->bytes[i][source[i].size] = '\0';
    selection->sizes[i] = source[i].size;
  }
  return true;
}

static bool decode(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes source[CONFIG_LUA_COUNT], bool from_esp,
    struct config_selection *selection)
{
  memset(selection, 0, sizeof(*selection));
  if (!freeze(source, selection) || !archive[CONFIG_LUA_BOOT].bytes ||
      boot_config_read_archive_bytes("archive installed.lua", archive[CONFIG_LUA_BOOT].bytes,
          archive[CONFIG_LUA_BOOT].size, &selection->defaults) != BOOT_CONFIG_OK) {
    goto failure;
  }
  if (from_esp && boot_config_read_literal_bytes("ESP boot.lua", selection->bytes[CONFIG_LUA_BOOT],
      selection->sizes[CONFIG_LUA_BOOT], &selection->override) != BOOT_CONFIG_OK) {
    goto failure;
  }
  if (!boot_plan_build(&selection->defaults, from_esp ? &selection->override : NULL,
      &selection->boot, NULL)) {
    goto failure;
  }
  enum config_result session_result = from_esp ?
      session_config_read_literal_bytes("ESP session.lua", selection->bytes[CONFIG_LUA_SESSION],
          selection->sizes[CONFIG_LUA_SESSION], &selection->session) :
      session_config_read_bytes("archive session.lua", selection->bytes[CONFIG_LUA_SESSION],
          selection->sizes[CONFIG_LUA_SESSION], &selection->session);
  enum config_result network_result = from_esp ?
      network_config_read_literal_bytes("ESP network.lua", selection->bytes[CONFIG_LUA_NETWORK],
          selection->sizes[CONFIG_LUA_NETWORK], &selection->network) :
      network_config_read_bytes("archive network.lua", selection->bytes[CONFIG_LUA_NETWORK],
          selection->sizes[CONFIG_LUA_NETWORK], &selection->network);
  if (session_result != CONFIG_OK || network_result != CONFIG_OK) {
    goto failure;
  }
  selection->from_esp = from_esp;
  return true;

failure:
  config_selection_free(selection);
  return false;
}

bool config_selection_validate(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes esp[CONFIG_LUA_COUNT], struct config_selection *selection)
{
  return decode(archive, esp, true, selection);
}

bool config_selection_choose(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes esp[CONFIG_LUA_COUNT], bool complete, bool rescue,
    struct config_selection *selection, const char **reason)
{
  if (!rescue && complete && decode(archive, esp, true, selection)) {
    *reason = "complete committed ESP selection";
    return true;
  }
  *reason = rescue ? "rescue uses archive defaults" : complete ?
      "invalid ESP selection; using complete archive defaults" :
      "no complete committed ESP selection; using archive defaults";
  return decode(archive, archive, false, selection);
}
