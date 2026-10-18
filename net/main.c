#include "../common/bootstrap_environment.h"
#include <net_config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET0_NAME "net0"

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
    enum call_status status = net_config_next_controller(authority, after_id, &controller);
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
        controller.flags & NET_CONTROLLER_BOUND ? NET0_NAME : "-");
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
    enum call_status status = net_config_next_interface(authority, after_id, &interface);
    if (status != CALL_OK) {
      fprintf(stderr, "net: cannot read the interface inventory (status %u)\n", status);
      return false;
    }
    if (!interface.interface_id) {
      return true;
    }
    printf("%s: id %llu class %s mtu %u epoch %llu route %llu", interface.name,
        (unsigned long long)interface.interface_id,
        interface.interface_class == NET_INTERFACE_CLASS_LOOPBACK ? "loopback" : "ethernet",
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
    putchar('\n');
    after_id = interface.interface_id;
  }
}

static bool print_net0(handle_t authority)
{
  struct net_config_reply net0;
  enum call_status status = net_config_query(authority, &net0);
  if (status != CALL_OK) {
    fprintf(stderr, "net: cannot read " NET0_NAME " (status %u)\n", status);
    return false;
  }
  char text[16];
  if (!(net0.flags & NET_CONFIG_BOUND)) {
    fputs(NET0_NAME ": unbound", stdout);
  } else if (!(net0.flags & NET_CONFIG_ASSIGNED)) {
    fputs(NET0_NAME ": bound, no IPv4 address assigned", stdout);
  } else {
    format_ipv4(net0.address, text);
    printf(NET0_NAME ": %s/%u", text, net0.prefix);
    if (net0.gateway) {
      format_ipv4(net0.gateway, text);
      printf(" gateway %s", text);
    } else {
      fputs(" gateway none", stdout);
    }
  }
  /* The chosen DNS server is independent of binding and assignment. */
  if (net0.dns_server) {
    format_ipv4(net0.dns_server, text);
    printf(" dns %s\n", text);
  } else {
    fputs(" dns none\n", stdout);
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
  handle_t authority = bootstrap_capability("net_config");
  if (authority == HANDLE_INVALID) {
    fputs("net: no net_config grant (the launching shell did not supply one)\n", stderr);
    return EXIT_FAILURE;
  }
  bool ok = print_controllers(authority) && print_interfaces(authority) && print_net0(authority);
  if (fflush(stdout) == EOF) {
    perror("net: output");
    return EXIT_FAILURE;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
