#include <jit.h>
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

enum call_status jit_allocate(handle_t jit, size_t size, uintptr_t hint,
    struct jit_region *region)
{
  if (!region) {
    return CALL_BAD_REQUEST;
  }
  *region = (struct jit_region){0};
  if (!size || size > SIZE_MAX - (JIT_PAGE_SIZE - 1)) {
    return CALL_BAD_REQUEST;
  }
  struct jit_message message = {
    .header = {PROTOCOL_JIT, JIT_ALLOCATE},
    .body.allocate = {.size = size, .hint = hint},
  };
  struct jit_region reply;
  enum call_status status = checked_status(syscall_call(jit, &message,
      sizeof(message), &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  size_t rounded = (size + JIT_PAGE_SIZE - 1) & ~(JIT_PAGE_SIZE - 1);
  if (!reply.identity || !reply.address || reply.address % JIT_PAGE_SIZE ||
      reply.size != rounded || reply.size > UINTPTR_MAX - reply.address) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *region = reply;
  return CALL_OK;
}

enum call_status jit_protect(handle_t jit, uint64_t identity, size_t offset,
    size_t size, enum jit_state state)
{
  if (!identity || !size || (offset | size) % JIT_PAGE_SIZE ||
      size > SIZE_MAX - offset ||
      (state != JIT_WRITABLE && state != JIT_READ_ONLY && state != JIT_EXECUTABLE)) {
    return CALL_BAD_REQUEST;
  }
  struct jit_message message = {
    .header = {PROTOCOL_JIT, JIT_PROTECT},
    .body.protect = {.identity = identity, .offset = offset, .size = size, .state = state},
  };
  return checked_status(syscall_call(jit, &message, sizeof(message), NULL, 0), 0);
}

enum call_status jit_release(handle_t jit, uint64_t identity)
{
  if (!identity) {
    return CALL_BAD_REQUEST;
  }
  struct jit_message message = {
    .header = {PROTOCOL_JIT, JIT_RELEASE},
    .body.release.identity = identity,
  };
  return checked_status(syscall_call(jit, &message, sizeof(message), NULL, 0), 0);
}
