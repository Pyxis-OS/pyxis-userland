#ifndef SESSION_CONFIG_H
#define SESSION_CONFIG_H

#include "../libconfig/session.h"
#include <abi/startup.h>
#include <pyxis/environment.h>
#include <stdbool.h>
#include <stddef.h>

/* Fresh output only. Success owns all strings; failure reports a diagnostic
 * and leaves nothing to free. No terminal or launch side effects. */
bool session_config_read(struct session_config *config);

/* The successor's environment: inherited variables, then configured ones,
 * then TZ and DNS_SERVER, each replacing an inherited variable of that name.
 * The caller frees the array; its strings borrow the supplied snapshot, config and
 * dns_server. NULL reports a diagnostic. */
struct startup_variable *session_environment(const struct session_config *config,
    const char *dns_server, const struct pyxis_environment_snapshot *inherited,
    size_t *count);

#endif
