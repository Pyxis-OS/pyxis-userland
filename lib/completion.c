#include <completion.h>
#include <syscall.h>

static enum call_status completion_status(struct syscall_result result)
{
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status completion_submit(handle_t target, handle_t receiver,
    uint64_t correlation_id, uint64_t protocol, uint64_t operation,
    uint64_t deadline_ns, const void *payload, size_t size)
{
  if (!correlation_id || size > COMPLETION_PAYLOAD_MAX || (size && !payload)) {
    return CALL_BAD_REQUEST;
  }
  struct completion_submit_message request = {
    .header = {PROTOCOL_COMPLETION, COMPLETION_SUBMIT},
    .receiver = receiver, .correlation_id = correlation_id,
    .protocol = protocol, .operation = operation, .deadline_ns = deadline_ns,
    .buffer = (uintptr_t)payload, .size = size,
  };
  return completion_status(syscall_call(target, &request, sizeof(request), NULL, 0));
}

enum call_status completion_submit_file_read_bulk(handle_t target, handle_t receiver,
    uint64_t correlation_id, uint64_t offset, size_t capacity, uint64_t deadline_ns)
{
  if (!deadline_ns) {
    return CALL_TIMED_OUT;
  }
  if (capacity > FILE_READ_BULK_MAX_BYTES || capacity > UINT64_MAX - offset) {
    return CALL_BAD_REQUEST;
  }
  struct file_bulk_read_request request = {offset, capacity, deadline_ns};
  return completion_submit(target, receiver, correlation_id, PROTOCOL_FILE,
      FILE_READ_BULK, deadline_ns, &request, sizeof(request));
}

enum call_status completion_cancel(handle_t receiver, uint64_t correlation_id)
{
  if (!correlation_id) {
    return CALL_BAD_REQUEST;
  }
  struct completion_cancel_message request = {
    .header = {PROTOCOL_COMPLETION, COMPLETION_CANCEL},
    .correlation_id = correlation_id,
  };
  return completion_status(syscall_call(receiver, &request, sizeof(request), NULL, 0));
}
