#ifndef SESSION_WIFI_SUPPLICANT_H
#define SESSION_WIFI_SUPPLICANT_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

/* The configured session owns supervision; the worker receives no launcher,
 * terminal, filesystem root or session controller. */
struct session_wifi_supplicant {
  handle_t observation, process, group, clock;
  bool stopping;
};

bool session_wifi_start(struct session_wifi_supplicant *worker);
bool session_wifi_active(const struct session_wifi_supplicant *worker);
bool session_wifi_wait(struct session_wifi_supplicant *worker, uint64_t deadline);
bool session_wifi_stop(struct session_wifi_supplicant *worker);

#endif
