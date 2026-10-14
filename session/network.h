#ifndef SESSION_NETWORK_H
#define SESSION_NETWORK_H

#include "../libconfig/network.h"
#include <abi/net_config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dhcp.h"

struct network_runtime {
  struct dhcp_client dhcp;
  struct dhcp_lease lease;
  handle_t authority;
  handle_t udp;
  /* Pending controller selection or stable configuration of the bound device. */
  bool waiting_link;
  bool background;
  /* Ownership starts after endpoint reservation and successful initial clear. */
  bool owns_dhcp_settings;
};

/* Keep the owned configuration alive through handoff/maintenance.
 * Link selection considers only prepared controllers with reported carrier,
 * reuses an existing binding, and keeps waiting when none is eligible. optional
 * controls absent explicit selectors only.
 * Exact MAC selection retries that same selector while discovery is pending;
 * ambiguity and a different existing binding fail without fallback. VirtIO
 * selection does not wait for unrelated controller discovery.
 * A prepared binding whose configuration is temporarily unstable stays pending;
 * permanent transport failure stops setup without selecting another controller.
 * dns_server owns canonical dotted decimal, defaulting to 1.1.1.1.
 * Applying selects DNS in userspace and publishes it through NET_CONFIG;
 * Link/MAC selection and initial acquisition share one foreground deadline.
 * Runtime owns pending selection and the DHCP endpoint until stopped; the
 * trusted setup process retains UDP creation authority only until opening it.
 * Keep config and runtime alive through successor handoff and maintenance. */
bool network_config_apply(const struct network_config *config, struct network_runtime *runtime);
/* After observing BOUND without READY, inspect preparation of that binding.
 * Prepared means wait for stable configuration; unprepared means permanent
 * failure. Temporary inventory unavailability also keeps the caller waiting. */
bool network_config_binding_pending(handle_t authority, bool *pending);
/* A configuration-owning remote handoff must keep discovery moving while it
 * waits for its first address; other remote sessions use the existing query wait. */
bool network_config_wait_address(const struct network_config *config, struct network_runtime *runtime);
int network_config_maintain(const struct network_config *config, struct network_runtime *runtime);
bool network_config_stop(const struct network_config *config, struct network_runtime *runtime);

#endif
