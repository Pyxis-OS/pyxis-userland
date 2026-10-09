#include <system_info.h>
#include <string.h>
#include <syscall.h>

/* PCI addressing and configuration-header limits. */
#define PCI_DEVICES_PER_BUS 32
#define PCI_FUNCTIONS_PER_DEVICE 8
#define PCI_HEADER_TYPE_MASK 0x7f
#define PCI_NO_VENDOR 0xffff

enum call_status system_info_get_hostname(handle_t system_info,
    struct system_info_hostname *hostname)
{
  if (!hostname) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_HOSTNAME};
  struct system_info_hostname reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    const char *end = memchr(reply.name, '\0', sizeof(reply.name));
    if (!end || end == reply.name) {
      return CALL_BAD_REQUEST;
    }
    for (const char *c = reply.name; c < end; ++c) {
      if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) {
        return CALL_BAD_REQUEST;
      }
    }
    *hostname = reply;
  }
  return result.status;
}

enum call_status system_info_set_hostname_once(handle_t system_info,
    const struct system_info_hostname *hostname)
{
  if (!hostname) {
    return CALL_BAD_REQUEST;
  }
  struct system_info_hostname_set_request request = {
    .header = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_HOSTNAME},
    .value = *hostname,
  };
  struct syscall_result result = syscall_call(system_info, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}

enum call_status system_info_get_identity(handle_t system_info,
    struct system_info_identity *identity)
{
  if (!identity) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_IDENTITY};
  struct system_info_identity reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (!memchr(reply.os_name, '\0', sizeof(reply.os_name)) ||
        !memchr(reply.kernel_name, '\0', sizeof(reply.kernel_name)) ||
        !memchr(reply.architecture, '\0', sizeof(reply.architecture)) ||
        !memchr(reply.build_revision, '\0', sizeof(reply.build_revision))) {
      return CALL_BAD_REQUEST;
    }
    *identity = reply;
  }
  return result.status;
}

enum call_status system_info_get_cpu(handle_t system_info, struct system_info_cpu *cpu)
{
  if (!cpu) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_CPU};
  struct system_info_cpu reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (!reply.online_count || !memchr(reply.brand, '\0', sizeof(reply.brand))) {
      return CALL_BAD_REQUEST;
    }
    *cpu = reply;
  }
  return result.status;
}

enum call_status system_info_get_memory(handle_t system_info, struct system_info_memory *memory)
{
  if (!memory) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_MEMORY};
  struct system_info_memory reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.allocated_bytes > reply.total_bytes ||
        reply.free_bytes != reply.total_bytes - reply.allocated_bytes) {
      return CALL_BAD_REQUEST;
    }
    *memory = reply;
  }
  return result.status;
}

enum call_status system_info_get_pci(handle_t system_info, struct system_info_pci *pci)
{
  if (!pci) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_PCI};
  struct system_info_pci reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.state != SYSTEM_INFO_PCI_UNAVAILABLE && reply.state != SYSTEM_INFO_PCI_INCOMPLETE &&
        reply.state != SYSTEM_INFO_PCI_COMPLETE) {
      return CALL_BAD_REQUEST;
    }
    if (reply.state == SYSTEM_INFO_PCI_UNAVAILABLE && reply.function_count) {
      return CALL_BAD_REQUEST;
    }
    *pci = reply;
  }
  return result.status;
}

enum call_status system_info_get_pci_function(handle_t system_info, uint64_t index,
    struct system_info_pci_function *function)
{
  if (!function) {
    return CALL_BAD_REQUEST;
  }
  struct system_info_pci_function_request request = {
    .header = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_PCI_FUNCTION},
    .index = index,
  };
  struct system_info_pci_function reply;
  struct syscall_result result = syscall_call(system_info, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.device >= PCI_DEVICES_PER_BUS || reply.function >= PCI_FUNCTIONS_PER_DEVICE ||
        (reply.header_type & ~PCI_HEADER_TYPE_MASK) || reply.vendor_id == PCI_NO_VENDOR ||
        reply.reserved) {
      return CALL_BAD_REQUEST;
    }
    *function = reply;
  }
  return result.status;
}

enum call_status system_info_get_usb(handle_t system_info, struct system_info_usb *usb)
{
  if (!usb) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_USB};
  struct system_info_usb reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.state < SYSTEM_INFO_USB_UNAVAILABLE || reply.state > SYSTEM_INFO_USB_COMPLETE ||
        ((reply.state == SYSTEM_INFO_USB_UNAVAILABLE ||
          reply.state == SYSTEM_INFO_USB_INITIALIZING) &&
         (reply.controller_count || reply.device_count || reply.interface_count))) {
      return CALL_BAD_REQUEST;
    }
    *usb = reply;
  }
  return result.status;
}

enum call_status system_info_get_usb_controller(handle_t system_info, uint64_t index,
    struct system_info_usb_controller *controller)
{
  if (!controller) {
    return CALL_BAD_REQUEST;
  }
  struct system_info_usb_request request = {
    .header = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_USB_CONTROLLER},
    .index = index,
  };
  struct system_info_usb_controller reply;
  struct syscall_result result = syscall_call(system_info, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.pci.device >= PCI_DEVICES_PER_BUS ||
        reply.pci.function >= PCI_FUNCTIONS_PER_DEVICE ||
        (reply.pci.header_type & ~PCI_HEADER_TYPE_MASK) ||
        reply.pci.vendor_id == PCI_NO_VENDOR || reply.pci.reserved ||
        reply.state < SYSTEM_INFO_USB_CONTROLLER_UNSUPPORTED ||
        reply.state > SYSTEM_INFO_USB_CONTROLLER_COMPLETE) {
      return CALL_BAD_REQUEST;
    }
    *controller = reply;
  }
  return result.status;
}

enum call_status system_info_get_usb_device(handle_t system_info, uint64_t index,
    struct system_info_usb_device *device)
{
  if (!device) {
    return CALL_BAD_REQUEST;
  }
  struct system_info_usb_request request = {
    .header = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_USB_DEVICE},
    .index = index,
  };
  struct system_info_usb_device reply;
  struct syscall_result result = syscall_call(system_info, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (!reply.root_port || reply.speed > SYSTEM_INFO_USB_SPEED_SUPER_PLUS || reply.reserved ||
        (reply.flags & ~(SYSTEM_INFO_USB_DEVICE_IDENTIFIED |
          SYSTEM_INFO_USB_DEVICE_INCOMPLETE | SYSTEM_INFO_USB_DEVICE_HUB)) ||
        reply.interface_count > UINT64_MAX - reply.interface_first) {
      return CALL_BAD_REQUEST;
    }
    if (!(reply.flags & SYSTEM_INFO_USB_DEVICE_IDENTIFIED) &&
        (reply.vendor_id || reply.product_id || reply.device_class || reply.device_subclass ||
         reply.device_protocol || reply.configuration_count || reply.interface_count ||
         (reply.flags & SYSTEM_INFO_USB_DEVICE_HUB))) {
      return CALL_BAD_REQUEST;
    }
    *device = reply;
  }
  return result.status;
}

enum call_status system_info_get_usb_interface(handle_t system_info, uint64_t index,
    struct system_info_usb_interface *interface)
{
  if (!interface) {
    return CALL_BAD_REQUEST;
  }
  struct system_info_usb_request request = {
    .header = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_USB_INTERFACE},
    .index = index,
  };
  struct system_info_usb_interface reply;
  struct syscall_result result = syscall_call(system_info, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.reserved || !reply.configuration) {
      return CALL_BAD_REQUEST;
    }
    *interface = reply;
  }
  return result.status;
}

enum call_status system_info_get_power(handle_t system_info, struct system_info_power *power)
{
  if (!power) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_POWER};
  struct system_info_power reply;
  struct syscall_result result = syscall_call(system_info, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.ac > SYSTEM_INFO_AC_ONLINE) {
      return CALL_BAD_REQUEST;
    }
    *power = reply;
  }
  return result.status;
}

static bool battery_unit_valid(uint32_t unit)
{
  return unit == SYSTEM_INFO_BATTERY_UNIT_MWH || unit == SYSTEM_INFO_BATTERY_UNIT_MAH ||
         unit == SYSTEM_INFO_BATTERY_UNKNOWN;
}

enum call_status system_info_get_battery(handle_t system_info, uint64_t index,
    struct system_info_battery *battery)
{
  if (!battery) {
    return CALL_BAD_REQUEST;
  }
  struct system_info_battery_request request = {
    .header = {PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_BATTERY},
    .index = index,
  };
  struct system_info_battery reply;
  struct syscall_result result = syscall_call(system_info, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    uint32_t known_flags = SYSTEM_INFO_BATTERY_PRESENT | SYSTEM_INFO_BATTERY_DISCHARGING |
        SYSTEM_INFO_BATTERY_CHARGING | SYSTEM_INFO_BATTERY_CRITICAL;
    if ((reply.flags & ~known_flags) || reply.reserved || !battery_unit_valid(reply.unit) ||
        (reply.percent > 100 && reply.percent != SYSTEM_INFO_BATTERY_UNKNOWN) ||
        !memchr(reply.model, '\0', sizeof(reply.model)) ||
        !memchr(reply.serial, '\0', sizeof(reply.serial)) ||
        !memchr(reply.type, '\0', sizeof(reply.type)) ||
        !memchr(reply.oem, '\0', sizeof(reply.oem))) {
      return CALL_BAD_REQUEST;
    }
    *battery = reply;
  }
  return result.status;
}
