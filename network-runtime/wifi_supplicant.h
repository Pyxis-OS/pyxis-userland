#ifndef NETWORK_RUNTIME_WIFI_SUPPLICANT_H
#define NETWORK_RUNTIME_WIFI_SUPPLICANT_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

/* The network owner (session or installer-init) supervises; the worker receives no launcher,
 * terminal, filesystem root or owner controller. */
struct network_wifi_supplicant {
  handle_t setup, launcher, image, memory, random, clock;
  handle_t auth, observation, process, group, group_launcher;
  uint64_t retry_deadline, retry_delay, claim_deadline, ready_since, retire_deadline;
  bool enabled, claimed, stopping, uncertain;
};

void network_wifi_start(struct network_wifi_supplicant *worker);
bool network_wifi_active(const struct network_wifi_supplicant *worker);
bool network_wifi_wait(struct network_wifi_supplicant *worker, uint64_t deadline);
bool network_wifi_stop(struct network_wifi_supplicant *worker);

#endif
