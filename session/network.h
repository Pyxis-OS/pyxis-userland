#ifndef SESSION_NETWORK_H
#define SESSION_NETWORK_H

#include "../libconfig/network.h"
#include "dhcp.h"

struct interface_runtime {
  struct dhcp_client dhcp;
  struct dhcp_lease lease;
  handle_t authority, udp;
  uint64_t id, reported_epoch;
  uint32_t reported_phase;
  bool configured, owns_dhcp_settings, failed, needs_clear;
};

struct network_runtime {
  struct interface_runtime *interfaces;
  size_t count;
  handle_t registry, policy, clock, random;
  uint64_t *ids;
  bool waiting_link, background, binding_failed;
};

bool network_config_apply(struct network_config *config, struct network_runtime *runtime);
bool network_config_wait_address(const struct network_config *config, struct network_runtime *runtime);
int network_config_maintain(const struct network_config *config, struct network_runtime *runtime);
bool network_config_stop(const struct network_config *config, struct network_runtime *runtime);
/* Remove unrelated bootstrap grants after successor handoff while retaining
 * each independent client's declared facets, timers and packet endpoint. */
bool network_runtime_release(struct network_runtime *runtime);

#endif
