#include <process.h>
#include <syscall.h>

enum call_status process_wait(handle_t process, struct process_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct process_result){0};
  struct message_header message = {PROTOCOL_PROCESS, PROCESS_WAIT};
  struct process_result reply;
  struct syscall_result call = syscall_call(process, &message, sizeof(message),
      &reply, sizeof(reply));
  if (call.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (call.reply_size != (call.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (call.status != CALL_OK) {
    return call.status;
  }
  if ((reply.kind != PROCESS_EXITED && reply.kind != PROCESS_FAULTED &&
      reply.kind != PROCESS_TERMINATED) ||
      (reply.kind != PROCESS_EXITED && reply.exit_status) ||
      reply.exit_status < INT32_MIN || reply.exit_status > INT32_MAX) {
    return CALL_BAD_REQUEST;
  }
  *result = reply;
  return CALL_OK;
}

enum call_status process_terminate(handle_t process)
{
  struct message_header message = {PROTOCOL_PROCESS, PROCESS_TERMINATE};
  struct syscall_result call = syscall_call(process, &message, sizeof(message), NULL, 0);
  if (call.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (call.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return call.status;
}
