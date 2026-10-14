#include "../common/launch_environment.h"
#include "config.h"
#include <abi/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <startup.h>

static bool check_timezone(const char *name)
{
  if (!strcmp(name, "UTC")) {
    return true;
  }
  if (!session_timezone_valid(name)) {
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
  if (session_config_read_file(SESSION_CONFIG_PATH, config) == CONFIG_ERROR) {
    return false;
  }
  if (!check_timezone(config->timezone)) {
    fprintf(stderr, "session: unavailable or invalid timezone '%s'\n", config->timezone);
    session_config_free(config);
    return false;
  }
  return true;
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

struct launch_environment_entry *session_environment(const struct session_config *config,
    const char *dns_server, const struct pyxis_environment_snapshot *snapshot,
    size_t *count)
{
  size_t inherited = snapshot->capture.count;
  size_t limit = SIZE_MAX / sizeof(struct launch_environment_entry);
  if (inherited > limit - 2 || config->environment_count > limit - 2 - inherited) {
    fputs("session: environment too large\n", stderr);
    return NULL;
  }
  struct launch_environment_entry *environment =
      malloc((inherited + config->environment_count + 2) * sizeof(*environment));
  if (!environment) {
    fputs("session: cannot allocate environment\n", stderr);
    return NULL;
  }

  size_t used = 0;
  const struct environment_value *source = snapshot->capture.values;
  for (size_t i = 0; i < inherited; ++i) {
    const char *name = source[i].name;
    /* The boot-carried discovery name cannot be shadowed by configuration. */
    if (!strcmp(name, "PYXIS_REMOTE_BEACON") ||
        (!session_variable_reserved(name) && !configured_variable(config, name))) {
      environment[used++] = (struct launch_environment_entry)PROFILE_TEXT(name, source[i].text);
    }
  }
  for (size_t i = 0; i < config->environment_count; ++i) {
    environment[used++] = (struct launch_environment_entry)PROFILE_TEXT(config->environment[i].name, config->environment[i].value);
  }
  environment[used++] = (struct launch_environment_entry)PROFILE_TEXT("TZ", config->timezone);
  environment[used++] = (struct launch_environment_entry)PROFILE_TEXT("DNS_SERVER", dns_server);
  *count = used;
  return environment;
}
