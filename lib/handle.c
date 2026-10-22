#include <handle.h>
#include <abi/file.h>
#include <resource.h>
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
  struct syscall_result result = syscall_handle_info(handle, &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (!reply.protocol || (reply.kind != HANDLE_KIND_NATIVE &&
        reply.kind != HANDLE_KIND_EXPORTED) ||
        (reply.restrictions & ~HANDLE_RESTRICTIONS)) {
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

static bool description_reply_valid(const struct describe_reply *reply, uint64_t flags)
{
  if (!reply->protocol || (reply->kind != HANDLE_KIND_NATIVE &&
      reply->kind != HANDLE_KIND_EXPORTED) ||
      (reply->transport & ~HANDLE_TRANSPORT_CALL) ||
      (reply->properties & ~(DESCRIBE_IMMUTABLE_SNAPSHOT | DESCRIBE_DETAIL_WITHHELD)) ||
      !resource_description_valid(&reply->description)) {
    return false;
  }
  if ((reply->properties & DESCRIBE_DETAIL_WITHHELD) && reply->description.detail_size) {
    return false;
  }
  if (reply->properties & DESCRIBE_IMMUTABLE_SNAPSHOT) {
    if (reply->protocol != PROTOCOL_FILE || reply->kind != HANDLE_KIND_NATIVE ||
        (reply->rights & ~FILE_RIGHT_READ) || reply->transport) {
      return false;
    }
  }
  bool body = (flags & DESCRIBE_INCLUDE_ERROR_BODY) &&
      (reply->description.flags & RESOURCE_ERROR_BODY);
  return (reply->error_body != HANDLE_INVALID) == body;
}

enum call_status handle_describe(handle_t handle, uint64_t flags,
    struct describe_reply *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct describe_reply){0};
  if (flags & ~DESCRIBE_INCLUDE_ERROR_BODY) {
    return CALL_BAD_REQUEST;
  }
  struct describe_reply reply = {0};
  struct syscall_result result = syscall_describe(handle, &reply, sizeof(reply), flags);
  enum call_status status = result.status < CALL_STATUS_COUNT ?
      (enum call_status)result.status : CALL_BAD_REQUEST;
  if (result.reply_size != (status == CALL_OK ? sizeof(reply) : 0) ||
      (status == CALL_OK && (!description_reply_valid(&reply, flags) ||
      reply.error_body == handle))) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK && reply.error_body != HANDLE_INVALID) {
    struct describe_reply body;
    status = handle_describe(reply.error_body, 0, &body);
    if (status == CALL_OK && (body.protocol != PROTOCOL_FILE ||
        body.kind != HANDLE_KIND_NATIVE || body.rights != FILE_RIGHT_READ ||
        body.transport || body.properties != DESCRIBE_IMMUTABLE_SNAPSHOT ||
        body.description.outcome != RESOURCE_OK ||
        !(body.description.flags & RESOURCE_LENGTH_KNOWN) ||
        (body.description.flags & RESOURCE_ERROR_BODY) ||
        body.description.length > RESOURCE_ERROR_BODY_MAX_BYTES)) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (status == CALL_OK) {
    *info = reply;
  } else if (reply.error_body != HANDLE_INVALID && reply.error_body != handle) {
    handle_close(reply.error_body);
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
