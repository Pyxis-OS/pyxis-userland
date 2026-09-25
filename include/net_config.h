#ifndef USERSPACE_NET_CONFIG_H
#define USERSPACE_NET_CONFIG_H

#include <abi/net_config.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Query clears output on failure. Addresses are host-order IPv4 integers;
 * replacement validates the complete address/prefix/optional gateway together.
 * No helper looks up authority by name or parses userspace configuration. */
enum call_status net_config_query(handle_t authority, struct net_config_reply *reply);
enum call_status net_config_replace(handle_t authority, uint32_t address,
    uint32_t prefix, uint32_t gateway);
enum call_status net_config_clear(handle_t authority);

#endif
