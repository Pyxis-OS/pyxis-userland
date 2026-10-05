#ifndef SESSION_CONFIG_H
#define SESSION_CONFIG_H

#include <abi/startup.h>
#include <stdbool.h>
#include <stddef.h>

#define SESSION_CONFIG_PATH "app://config/session.lua"

/* Values are strings for now. Capability-valued variables are a later idea,
 * so this is not the permanent shape of an environment entry. */
struct session_variable {
  char *name;
  char *value;
};

struct session_config {
  char *timezone;
  size_t tab_width;
  struct session_variable *environment;
  size_t environment_count;
};

/* Fresh output only. Success owns all strings; failure reports a diagnostic
 * and leaves nothing to free. No terminal or launch side effects. */
bool session_config_read(struct session_config *config);
void session_config_free(struct session_config *config);

/* The successor's environment: inherited variables, then configured ones,
 * then TZ and DNS_SERVER, each replacing an inherited variable of that name.
 * The caller frees the array; its strings borrow startup data, config and
 * dns_server. NULL reports a diagnostic. */
struct startup_variable *session_environment(const struct session_config *config,
    const char *dns_server, size_t *count);

#endif
