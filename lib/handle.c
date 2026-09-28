#include <handle.h>
#include <syscall.h>

int handle_close(handle_t handle)
{
  struct syscall_result result = syscall_close(handle);
  return result.status == CALL_OK && result.reply_size == 0 ? 0 : -1;
}

enum call_status handle_query(handle_t handle, struct handle_info *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct handle_info){0};
  struct handle_info reply;
  struct syscall_result result = syscall_handle_info(handle, &reply);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (!reply.protocol || (reply.kind != HANDLE_KIND_NATIVE &&
        reply.kind != HANDLE_KIND_EXPORTED)) {
      return CALL_BAD_REQUEST;
    }
    *info = reply;
  }
  return result.status;
}

enum call_status handle_rights(handle_t handle, uint64_t *rights, uint64_t *transport)
{
  if (!rights && !transport) {
    return CALL_BAD_REQUEST;
  }
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (rights) {
    *rights = info.rights;
  }
  if (transport) {
    *transport = info.transport;
  }
  return status;
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
