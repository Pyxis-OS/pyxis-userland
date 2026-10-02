#include <system_info.h>
#include <string.h>
#include <syscall.h>

/* PCI addressing and configuration-header limits. */
#define PCI_DEVICES_PER_BUS 32
#define PCI_FUNCTIONS_PER_DEVICE 8
#define PCI_HEADER_TYPE_MASK 0x7f
#define PCI_NO_VENDOR 0xffff

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
