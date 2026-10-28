#ifndef SESSION_CONFIG_H
#define SESSION_CONFIG_H

#include "../libconfig/session.h"
#include <abi/launcher.h>
#include <pyxis/environment.h>
#include <stdbool.h>
#include <stddef.h>

/* Fresh output only. Success owns all strings; failure reports a diagnostic
 * and leaves nothing to free. No terminal or launch side effects. */
bool session_config_read(struct session_config *config);

/* Resolve the configured path from held native roots/cwd and create an owned
 * read-only list. No publication or launch side effects. An absent setting
 * returns NOT_FOUND. Failure leaves list invalid; failed_entry identifies a
 * resolution failure, or SIZE_MAX for list construction/allocation failures. */
enum call_status session_path_create(const struct session_config *config,
    handle_t *list, size_t *failed_entry);

/* The successor's environment: inherited variables, then configured ones,
 * then TZ. DNS remains an inherited startup snapshot until trusted network
 * selection pairs it with an explicitly permitted resolver factory.
 * The caller frees the array; strings borrow snapshot/config. */
struct launch_environment_entry *session_environment(const struct session_config *config,
    const struct pyxis_environment_snapshot *inherited,
    size_t *count);

#endif
