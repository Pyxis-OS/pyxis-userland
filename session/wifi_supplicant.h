#ifndef SESSION_WIFI_SUPPLICANT_H
#define SESSION_WIFI_SUPPLICANT_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

/* The configured session owns supervision; the worker receives no launcher,
 * terminal, filesystem root or session controller. */
struct session_wifi_supplicant {
  handle_t setup, launcher, image, memory, random, clock;
  handle_t auth, observation, process, group, group_launcher;
  uint64_t retry_deadline, retry_delay, claim_deadline, ready_since, retire_deadline;
  bool enabled, claimed, stopping, uncertain;
};

void session_wifi_start(struct session_wifi_supplicant *worker);
bool session_wifi_active(const struct session_wifi_supplicant *worker);
bool session_wifi_wait(struct session_wifi_supplicant *worker, uint64_t deadline);
bool session_wifi_stop(struct session_wifi_supplicant *worker);

#endif
