#include <shared_memory.h>
#include <syscall.h>

static enum call_status checked_status(struct syscall_result result, size_t reply_size)
{
  if (result.status == (uint64_t)CALL_UNKNOWN_SYSTEM_CALL && !result.reply_size) {
    return CALL_UNKNOWN_SYSTEM_CALL;
  }
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status shared_memory_create(handle_t memory, size_t size,
    struct shared_memory_created *created)
{
  if (!created) {
    return CALL_BAD_REQUEST;
  }
  *created = (struct shared_memory_created){0};
  if (!size || size > SIZE_MAX - (MEMORY_PAGE_SIZE - 1)) {
    return CALL_BAD_REQUEST;
  }
  struct memory_message request = {
    .header = {PROTOCOL_MEMORY, MEMORY_CREATE_SHARED},
    .body.create_shared.size = size,
  };
  struct shared_memory_created reply = {0};
  enum call_status status = checked_status(syscall_call(memory, &request, sizeof(request),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  size_t rounded = (size + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
  if (reply.handle == HANDLE_INVALID || reply.size != rounded) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *created = reply;
  return CALL_OK;
}

enum call_status shared_memory_map(handle_t region, uint64_t rights,
    struct memory_reservation *view)
{
  if (!view) {
    return CALL_BAD_REQUEST;
  }
  *view = (struct memory_reservation){0};
  if (rights != SHARED_MEMORY_RIGHT_READ && rights != SHARED_MEMORY_RIGHTS) {
    return CALL_BAD_REQUEST;
  }
  struct shared_memory_message request = {
    .header = {PROTOCOL_SHARED_MEMORY, SHARED_MEMORY_MAP},
    .body.rights = rights,
  };
  struct memory_reservation reply = {0};
  enum call_status status = checked_status(syscall_call(region, &request, sizeof(request),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (!reply.identity || !reply.address || !reply.size ||
      reply.address % MEMORY_PAGE_SIZE || reply.size % MEMORY_PAGE_SIZE ||
      reply.address > UINTPTR_MAX || reply.size > UINTPTR_MAX - reply.address) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *view = reply;
  return CALL_OK;
}
