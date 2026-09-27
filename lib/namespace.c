#include <namespace.h>
#include <syscall.h>

static enum call_status checked_status(struct syscall_result result)
{
  return result.status < CALL_STATUS_COUNT ? result.status : CALL_UNAVAILABLE;
}

static bool make_name(char target[NAMESPACE_NAME_MAX + 1], const char *name)
{
  if (!name) {
    return false;
  }
  size_t length = 0;
  while (name[length]) {
    char byte = name[length];
    if (length == NAMESPACE_NAME_MAX ||
        !((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
          byte == '.' || byte == '+')) {
      return false;
    }
    target[length] = byte;
    ++length;
  }
  target[length] = 0;
  return length != 0;
}

enum call_status namespace_create(handle_t service, handle_t *namespace_handle)
{
  if (!namespace_handle) {
    return CALL_BAD_REQUEST;
  }
  *namespace_handle = HANDLE_INVALID;
  struct message_header request = {PROTOCOL_NAMESPACE_SERVICE, NAMESPACE_CREATE};
  struct namespace_reply reply = {0};
  struct syscall_result result = syscall_call(service, &request, sizeof(request),
      &reply, sizeof(reply));
  enum call_status status = checked_status(result);
  if (status != CALL_OK) {
    return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size != sizeof(reply) || reply.handle == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *namespace_handle = reply.handle;
  return CALL_OK;
}

static enum call_status bind(handle_t namespace_handle, uint64_t operation,
    const char *name, handle_t client, uint64_t rights, uint64_t transport)
{
  struct namespace_bind_message request = {
    .header = {PROTOCOL_NAMESPACE, operation},
    .client = client, .rights = rights, .transport = transport,
  };
  if (!make_name(request.name, name)) {
    return CALL_BAD_REQUEST;
  }
  struct syscall_result result = syscall_call(namespace_handle, &request,
      sizeof(request), NULL, 0);
  enum call_status status = checked_status(result);
  return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
}

enum call_status namespace_publish(handle_t namespace_handle, const char *name,
    handle_t client, uint64_t rights, uint64_t transport)
{
  return bind(namespace_handle, NAMESPACE_PUBLISH, name, client, rights, transport);
}

enum call_status namespace_replace(handle_t namespace_handle, const char *name,
    handle_t client, uint64_t rights, uint64_t transport)
{
  return bind(namespace_handle, NAMESPACE_REPLACE, name, client, rights, transport);
}

enum call_status namespace_remove(handle_t namespace_handle, const char *name)
{
  struct namespace_name_message request = {
    .header = {PROTOCOL_NAMESPACE, NAMESPACE_REMOVE},
  };
  if (!make_name(request.name, name)) {
    return CALL_BAD_REQUEST;
  }
  struct syscall_result result = syscall_call(namespace_handle, &request,
      sizeof(request), NULL, 0);
  enum call_status status = checked_status(result);
  return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
}

enum call_status namespace_lookup(handle_t namespace_handle, const char *name,
    handle_t *client)
{
  if (!client) {
    return CALL_BAD_REQUEST;
  }
  *client = HANDLE_INVALID;
  struct namespace_name_message request = {
    .header = {PROTOCOL_NAMESPACE, NAMESPACE_LOOKUP},
  };
  if (!make_name(request.name, name)) {
    return CALL_BAD_REQUEST;
  }
  struct namespace_reply reply = {0};
  struct syscall_result result = syscall_call(namespace_handle, &request,
      sizeof(request), &reply, sizeof(reply));
  enum call_status status = checked_status(result);
  if (status != CALL_OK) {
    return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size != sizeof(reply) || reply.handle == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *client = reply.handle;
  return CALL_OK;
}
