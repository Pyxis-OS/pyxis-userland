#include <memory.h>
#include <syscall.h>

static enum call_status call_status(struct syscall_result result, size_t reply_size)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status memory_allocate(handle_t memory, size_t size, struct memory_region *region)
{
  if (!region) {
    return CALL_BAD_REQUEST;
  }
  *region = (struct memory_region){0};
  if (!size || size > SIZE_MAX - (MEMORY_PAGE_SIZE - 1)) {
    return CALL_BAD_REQUEST;
  }
  struct memory_message message = {
    .header = {PROTOCOL_MEMORY, MEMORY_ALLOCATE},
    .body.allocate.size = size,
  };
  struct memory_region reply;
  enum call_status status = call_status(syscall_call(memory, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  size_t rounded = (size + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
  if (!reply.address || (reply.address & (MEMORY_PAGE_SIZE - 1)) ||
      reply.size != rounded || reply.size > UINTPTR_MAX - reply.address) {
    return CALL_BAD_REQUEST;
  }
  *region = reply;
  return CALL_OK;
}

enum call_status memory_release(handle_t memory, struct memory_region region)
{
  struct memory_message message = {
    .header = {PROTOCOL_MEMORY, MEMORY_RELEASE},
    .body.release = region,
  };
  return call_status(syscall_call(memory, &message, sizeof(message), NULL, 0), 0);
}
