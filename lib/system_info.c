#include <system_info.h>
#include <string.h>
#include <syscall.h>

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
