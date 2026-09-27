#include <handle.h>
#include <syscall.h>

int handle_close(handle_t handle)
{
  struct syscall_result result = syscall_close(handle);
  return result.status == CALL_OK && result.reply_size == 0 ? 0 : -1;
}

enum call_status handle_rights(handle_t handle, uint64_t *rights, uint64_t *transport)
{
  if (!rights && !transport) {
    return CALL_BAD_REQUEST;
  }
  if (rights) {
    *rights = 0;
  }
  if (transport) {
    *transport = 0;
  }
  struct handle_authority granted = {0};
  struct syscall_result result = syscall_handle_rights(handle, &granted);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(granted) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (rights) {
      *rights = granted.rights;
    }
    if (transport) {
      *transport = granted.transport;
    }
  }
  return result.status;
}

static enum call_status copy(handle_t source, uint64_t rights, uint64_t transport,
    uint64_t flags,
                              handle_t *destination)
{
  if (!destination) {
    return CALL_BAD_REQUEST;
  }
  *destination = HANDLE_INVALID;
  struct syscall_result result = syscall_copy(source, rights, transport, flags, destination);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(*destination) : 0) ||
      (result.status == CALL_OK && *destination == HANDLE_INVALID)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status handle_copy(handle_t source, handle_t *destination)
{
  return copy(source, 0, 0, HANDLE_COPY_SAME_RIGHTS, destination);
}

enum call_status handle_copy_restricted(handle_t source, uint64_t rights,
    uint64_t transport, handle_t *destination)
{
  return copy(source, rights, transport, 0, destination);
}
