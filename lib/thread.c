#include <thread.h>
#include <syscall.h>

static enum call_status checked_status(struct syscall_result call, size_t reply_size)
{
  if (call.status >= CALL_STATUS_COUNT ||
      call.reply_size != (call.status == CALL_OK ? reply_size : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return call.status;
}

enum call_status thread_create(handle_t threads, const struct thread_create *request,
    handle_t *completion)
{
  if (!completion) {
    return CALL_BAD_REQUEST;
  }
  *completion = HANDLE_INVALID;
  if (!request) {
    return CALL_BAD_REQUEST;
  }
  struct thread_message message = {
    .header = {PROTOCOL_THREAD_SERVICE, THREAD_CREATE},
    .body.create = *request,
  };
  struct thread_reply reply;
  enum call_status status = checked_status(syscall_call(threads, &message,
      sizeof(message), &reply, sizeof(reply)), sizeof(reply));
  if (status == CALL_OK) {
    if (reply.completion == HANDLE_INVALID) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *completion = reply.completion;
  }
  return status;
}

enum call_status thread_set_fs(handle_t threads, struct thread_fs_context context)
{
  struct thread_message message = {0};
  message.header = (struct message_header){PROTOCOL_THREAD_SERVICE, THREAD_SET_FS};
  message.body.set_fs = context;
  return checked_status(syscall_call(threads, &message, sizeof(message), NULL, 0), 0);
}

enum call_status thread_exit(handle_t threads, uint64_t value)
{
  struct thread_message message = {0};
  message.header = (struct message_header){PROTOCOL_THREAD_SERVICE, THREAD_EXIT};
  message.body.exit_value = value;
  return checked_status(syscall_call(threads, &message, sizeof(message), NULL, 0), 0);
}

enum call_status thread_wait(handle_t completion, struct thread_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct thread_result){0};
  struct message_header message = {PROTOCOL_THREAD_COMPLETION, THREAD_WAIT};
  struct thread_result reply;
  enum call_status status = checked_status(syscall_call(completion, &message,
      sizeof(message), &reply, sizeof(reply)), sizeof(reply));
  if (status == CALL_OK) {
    if ((reply.kind != THREAD_EXITED && reply.kind != THREAD_STOPPED) ||
        (reply.kind == THREAD_STOPPED && reply.value)) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *result = reply;
  }
  return status;
}
