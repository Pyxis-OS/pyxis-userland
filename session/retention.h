#ifndef SESSION_RETENTION_H
#define SESSION_RETENTION_H

#include "config.h"
#include <retention.h>

/* Consumes one private startup account, or the initial factory and creates an
 * account from this session's configuration. Success returns one owned account.
 * Existing accounts retain their original limits across trusted successors. */
enum call_status session_retention_init(const struct session_config *config, handle_t *account);

#endif
