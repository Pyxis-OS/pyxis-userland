#include <thread.h>
#include <syscall.h>

static enum call_status checked_status(struct syscall_result call, size_t reply_size)
{
  if (call.status == (uint64_t)CALL_UNKNOWN_SYSTEM_CALL) {
    return CALL_UNKNOWN_SYSTEM_CALL;
  }
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

enum call_status thread_address_wait(handle_t threads, uintptr_t address,
    uint64_t expected, uint64_t word_size, uint64_t deadline)
{
  struct thread_message message = {0};
  message.header = (struct message_header){PROTOCOL_THREAD_SERVICE, THREAD_ADDRESS_WAIT};
  message.body.address_wait = (struct thread_address_wait){
    .address = address, .expected = expected, .deadline = deadline, .word_size = word_size,
  };
  return checked_status(syscall_call(threads, &message, sizeof(message), NULL, 0), 0);
}

enum call_status thread_address_wake(handle_t threads, uintptr_t address,
    uint64_t word_size, uint64_t maximum, uint64_t *woken)
{
  if (!woken) {
    return CALL_BAD_REQUEST;
  }
  *woken = 0;
  if (!maximum) {
    return CALL_BAD_REQUEST;
  }
  struct thread_message message = {0};
  message.header = (struct message_header){PROTOCOL_THREAD_SERVICE, THREAD_ADDRESS_WAKE};
  message.body.address_wake = (struct thread_address_wake){
    .address = address, .word_size = word_size, .maximum = maximum,
  };
  struct thread_address_wake_reply reply;
  enum call_status status = checked_status(syscall_call(threads, &message,
      sizeof(message), &reply, sizeof(reply)), sizeof(reply));
  if (status == CALL_OK) {
    if (reply.woken > maximum) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *woken = reply.woken;
  }
  return status;
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
