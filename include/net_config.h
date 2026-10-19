#ifndef USERSPACE_NET_CONFIG_H
#define USERSPACE_NET_CONFIG_H

#include <abi/handle.h>
#include <abi/net_config.h>
#include <stddef.h>

enum call_status net_registry_next_controller(handle_t registry, uint32_t after_id,
    struct net_controller_reply *reply);
enum call_status net_registry_next_interface(handle_t registry, uint64_t after_id,
    struct net_interface_reply *reply);
enum call_status net_registry_lookup(handle_t registry, uint64_t interface_id, handle_t *facet);
enum call_status net_registry_policy(handle_t registry, struct net_policy_reply *reply);
enum call_status net_interface_query(handle_t facet, struct net_interface_reply *reply);
/* The configuration owner retains the generation sampled when its work began.
 * Ethernet uses zero; Wi-Fi uses that connection's nonzero generation. These
 * helpers never query a newer generation on behalf of old configuration work. */
enum call_status net_interface_replace(handle_t facet, uint64_t connection_generation,
    uint32_t address, uint32_t prefix,
    uint32_t gateway, uint32_t dns_server);
enum call_status net_interface_clear(handle_t facet, uint64_t connection_generation);
enum call_status net_interface_report_dhcp(handle_t facet, uint64_t connection_generation,
    uint64_t address_epoch, uint32_t phase);
enum call_status net_policy_define(handle_t policy, const struct net_interface_definition *entries, size_t count);
enum call_status net_policy_bind(handle_t policy, const struct net_interface_binding *entries, size_t count);
enum call_status net_policy_interface(handle_t policy, uint64_t interface_id, uint64_t rights, handle_t *facet);
enum call_status net_policy_factory(handle_t policy, uint64_t protocol, uint64_t rights,
    const uint64_t *interface_ids, size_t count, handle_t *factory);
enum call_status net_policy_replace(handle_t policy, const struct net_policy_request *settings);

#endif
