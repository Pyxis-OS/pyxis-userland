#include "../common/bootstrap_environment.h"
#include <net_config.h>
#include <clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int usage(FILE *output, int status)
{
  fputs("usage: net [--help]\n", output);
  return status;
}

static const char *driver_name(uint32_t driver)
{
  switch (driver) {
  case NET_DRIVER_VIRTIO: return "virtio";
  case NET_DRIVER_RTL8111: return "rtl8111";
  case NET_DRIVER_CDC_NCM: return "cdc-ncm";
  default: return "unknown";
  }
}

static const char *link_name(uint32_t flags)
{
  if (!(flags & NET_CONTROLLER_CARRIER_KNOWN)) {
    return "unknown";
  }
  return flags & NET_CONTROLLER_LINK_UP ? "up" : "down";
}

static void format_mac(const uint8_t mac[6], char text[18])
{
  static const uint8_t unknown[6];
  if (!memcmp(mac, unknown, sizeof(unknown))) {
    strcpy(text, "unknown");
    return;
  }
  snprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Addresses in the ABI are host-order IPv4. */
static void format_ipv4(uint32_t address, char text[16])
{
  snprintf(text, 16, "%u.%u.%u.%u", address >> 24, address >> 16 & 0xff, address >> 8 & 0xff,
      address & 0xff);
}

static bool print_controllers(handle_t authority)
{
  uint32_t after_id = 0;
  size_t count = 0;
  for (;;) {
    struct net_controller_reply controller;
    enum call_status status = net_registry_next_controller(authority, after_id, &controller);
    if (status != CALL_OK) {
      fprintf(stderr, "net: cannot read the controller inventory (status %u)\n", status);
      return false;
    }
    if (!controller.controller_id) {
      if (!count) {
        puts("no network controllers found");
      }
      if (!(controller.flags & NET_CONTROLLER_INVENTORY_COMPLETE)) {
        puts("inventory incomplete: controller discovery has not finished; run net again later");
      }
      return true;
    }
    if (controller.controller_id <= after_id) {
      fputs("net: the controller inventory is out of order\n", stderr);
      return false;
    }
    if (!count) {
      printf("%-4s %-8s %-17s %-8s %-9s %s\n", "ID", "DRIVER", "MAC", "LINK", "PREPARED", "BOUND");
    }
    char mac[18];
    format_mac(controller.mac, mac);
    printf("%-4u %-8s %-17s %-8s %-9s %s\n", controller.controller_id, driver_name(controller.driver), mac,
        link_name(controller.flags), controller.flags & NET_CONTROLLER_PREPARED ? "yes" : "no",
        controller.flags & NET_CONTROLLER_BOUND ? "yes" : "-");
    after_id = controller.controller_id;
    ++count;
  }
}

static bool print_interfaces(handle_t authority)
{
  uint64_t after_id = 0;
  puts("Native interfaces:");
  for (;;) {
    struct net_interface_reply interface;
    enum call_status status = net_registry_next_interface(authority, after_id, &interface);
    if (status != CALL_OK) {
      fprintf(stderr, "net: cannot read the interface inventory (status %u)\n", status);
      return false;
    }
    if (!interface.interface_id) {
      return true;
    }
    printf("%s: id %llu class %s mtu %u epoch %llu route %llu", interface.name,
        (unsigned long long)interface.interface_id,
        interface.interface_class == NET_INTERFACE_CLASS_LOOPBACK ? "loopback" :
        interface.interface_class == NET_INTERFACE_CLASS_WIFI ? "wifi" : "ethernet",
        interface.mtu, (unsigned long long)interface.address_epoch,
        (unsigned long long)interface.route_revision);
    if (interface.flags & NET_INTERFACE_BOUND) {
      printf(" controller %u driver %s", interface.controller_id, driver_name(interface.driver));
    }
    if (interface.flags & NET_INTERFACE_MAC_VALID) {
      char mac[18];
      format_mac(interface.mac, mac);
      printf(" mac %s", mac);
    }
    if (interface.flags & NET_INTERFACE_CARRIER_KNOWN) {
      printf(" link %s", interface.flags & NET_INTERFACE_LINK_UP ? "up" : "down");
    }
    if (interface.flags & NET_INTERFACE_ADDRESS_VALID) {
      char text[16];
      format_ipv4(interface.address, text);
      printf(" ipv4 %s/%u", text, interface.prefix);
      if (interface.gateway) {
        format_ipv4(interface.gateway, text);
        printf(" gateway %s", text);
      }
    }
    printf(" admin %s ready %s", interface.flags & NET_INTERFACE_BOUND ? "bound" : "unbound",
        interface.flags & NET_INTERFACE_READY ? "yes" : "no");
    printf(" authentication %s", !(interface.flags & NET_INTERFACE_AUTHENTICATION_VALID) ? "unknown" :
        interface.flags & NET_INTERFACE_AUTHENTICATED ? "admitted" : "not admitted");
    if (interface.flags & NET_INTERFACE_DHCP_REPORT_VALID) {
      static const char *const phases[] = {"none", "wait-link", "selecting", "bound", "renewing",
        "rebinding", "expired", "failed"};
      printf(" DHCP %s", interface.dhcp_phase < sizeof(phases) / sizeof(*phases) ? phases[interface.dhcp_phase] : "unknown");
      uint64_t now;
      if (clock_now(bootstrap_capability("clock"), &now) == CALL_OK && now >= interface.dhcp_report_time_ns) {
        printf(" reported %llu ms ago", (unsigned long long)((now - interface.dhcp_report_time_ns) / UINT64_C(1000000)));
      }
    }
    putchar('\n');
    after_id = interface.interface_id;
  }
}

static bool print_policy(handle_t registry)
{
  struct net_policy_reply policy;
  enum call_status status = net_registry_policy(registry, &policy);
  if (status != CALL_OK) {
    fprintf(stderr, "net: cannot read policy (status %u)\n", status);
    return false;
  }
  printf("Policy revision %llu: default %llu DNS interface %llu remote %llu log %llu debug %llu\n",
      (unsigned long long)policy.revision, (unsigned long long)policy.default_interface_id,
      (unsigned long long)policy.dns_interface_id, (unsigned long long)policy.remote_interface_id,
      (unsigned long long)policy.log_interface_id, (unsigned long long)policy.debug_interface_id);
  if (policy.flags & NET_POLICY_DNS_VALID) {
    char text[16];
    format_ipv4(policy.dns_server, text);
    printf("DNS: %s (%s)\n", text, policy.dns_mode == NET_DNS_NUMERIC ? "numeric" : "DHCP");
  } else {
    puts("DNS: unavailable");
  }
  return true;
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    return usage(stdout, EXIT_SUCCESS);
  }
  if (argc != 1) {
    return usage(stderr, EXIT_FAILURE);
  }
  handle_t authority = bootstrap_capability("net_registry");
  if (authority == HANDLE_INVALID) {
    fputs("net: no net_registry grant (the launching shell did not supply one)\n", stderr);
    return EXIT_FAILURE;
  }
  bool ok = print_controllers(authority) && print_interfaces(authority) && print_policy(authority);
  if (fflush(stdout) == EOF) {
    perror("net: output");
    return EXIT_FAILURE;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
