#ifndef SESSION_CONFIG_H
#define SESSION_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define SESSION_CONFIG_PATH "app://config/session.lua"

struct session_config {
  char *timezone;
  size_t tab_width;
};

/* Fresh output only. Success owns timezone; failure reports a diagnostic and
 * leaves timezone NULL. No terminal or launch side effects. */
bool session_config_read(struct session_config *config);

#endif
