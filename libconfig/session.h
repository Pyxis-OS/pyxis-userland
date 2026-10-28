#ifndef LIBCONFIG_SESSION_H
#define LIBCONFIG_SESSION_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESSION_CONFIG_PATH "boot://config/session.lua"

/* Authored environment overrides are text; path is configured separately. */
struct session_variable {
  char *name;
  char *value;
};

struct session_config {
  char *timezone;
  size_t tab_width;
  uint64_t value_limit_bytes, session_limit_bytes;
  struct session_variable *environment;
  size_t environment_count;
  char **path;
  size_t path_count;
  bool path_configured;
};

/* Static validity only: syntax, defaults, names, terminal range and environment.
 * Fresh output owns every allocation; CONFIG_ERROR leaves it empty. Missing file
 * returns CONFIG_MISSING with the ordinary UTC/tab-width defaults allocated.
 * Bytes are borrowed until return. Literal input is never executed.
 * The packaged timezone file's availability/TZif check and path resolution belong
 * to runtime. An absent path preserves the held default list; an empty table is
 * an explicitly empty path. */
enum config_result session_config_read_file(const char *path, struct session_config *config);
enum config_result session_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct session_config *config);
enum config_result session_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct session_config *config);
bool session_timezone_valid(const char *name);
/* TZ, DNS_SERVER and path are supplied by typed settings at successor handoff. */
bool session_variable_reserved(const char *name);
void session_config_free(struct session_config *config);

#endif
