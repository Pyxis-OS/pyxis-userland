#include <wifi.h>
#include <syscall.h>
#include <string.h>

static enum call_status invoke(handle_t handle, const void *request, size_t bytes,
    void *reply, size_t capacity)
{
  struct syscall_result result = syscall_call(handle, request, bytes, reply, capacity);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? capacity : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status wifi_status(handle_t wifi, struct wifi_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI, WIFI_STATUS};
  struct wifi_status_reply reply;
  enum call_status result = invoke(wifi, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (!memchr(reply.detail, 0, sizeof(reply.detail))) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_scan_start(handle_t wifi, uint32_t channels,
    uint64_t deadline_ns, handle_t *operation)
{
  if (!operation) {
    return CALL_BAD_REQUEST;
  }
  *operation = HANDLE_INVALID;
  struct wifi_scan_request request = {
    .header = {PROTOCOL_WIFI, WIFI_SCAN_START},
    .channels = channels, .deadline_ns = deadline_ns,
  };
  struct wifi_scan_start_reply reply;
  enum call_status result = invoke(wifi, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.operation == HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    *operation = reply.operation;
  }
  return result;
}

enum call_status wifi_scan_status(handle_t operation,
    struct wifi_scan_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_scan_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_SCAN, WIFI_SCAN_STATUS};
  struct wifi_scan_status_reply reply;
  enum call_status result = invoke(operation, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.status >= CALL_STATUS_COUNT || reply.record_count > WIFI_SCAN_RECORDS || reply.reserved ||
        (reply.flags & ~(WIFI_SCAN_COMPLETE | WIFI_SCAN_TAKEN | WIFI_SCAN_CANCEL_REQUESTED)) ||
        (!(reply.flags & WIFI_SCAN_COMPLETE) && reply.status != CALL_WOULD_BLOCK)) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_scan_take(handle_t operation, handle_t *file)
{
  if (!file) {
    return CALL_BAD_REQUEST;
  }
  *file = HANDLE_INVALID;
  struct message_header request = {PROTOCOL_WIFI_SCAN, WIFI_SCAN_TAKE};
  struct wifi_scan_take_reply reply;
  enum call_status result = invoke(operation, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.file == HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    *file = reply.file;
  }
  return result;
}

enum call_status wifi_scan_cancel(handle_t operation)
{
  struct message_header request = {PROTOCOL_WIFI_SCAN, WIFI_SCAN_CANCEL};
  return invoke(operation, &request, sizeof(request), NULL, 0);
}

enum call_status wifi_install_policy(handle_t wifi, const char country[2],
    uint32_t channels)
{
  if (!country) {
    return CALL_BAD_REQUEST;
  }
  struct wifi_policy_request request = {
    .header = {PROTOCOL_WIFI, WIFI_INSTALL_POLICY}, .channels = channels,
    .country = {country[0], country[1]},
  };
  return invoke(wifi, &request, sizeof(request), NULL, 0);
}

static enum call_status returned_handle(handle_t target, const void *request, size_t bytes, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  handle_t returned = HANDLE_INVALID;
  enum call_status status = invoke(target, request, bytes, &returned, sizeof(returned));
  if (status == CALL_OK) {
    if (!returned) {
      return CALL_BAD_REQUEST;
    }
    *handle = returned;
  }
  return status;
}

enum call_status wifi_connect_start(handle_t radio, const struct wifi_open_target *target,
    uint64_t deadline_ns, handle_t *operation)
{
  if (!target) {
    return CALL_BAD_REQUEST;
  }
  struct wifi_connect_request request = {
    .header = {PROTOCOL_WIFI, WIFI_CONNECT_START}, .deadline_ns = deadline_ns, .target = *target,
  };
  return returned_handle(radio, &request, sizeof(request), operation);
}

enum call_status wifi_connect_status(handle_t operation, struct wifi_connect_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_connect_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_CONNECT, WIFI_CONNECT_STATUS};
  struct wifi_connect_status_reply reply = {0};
  enum call_status result = invoke(operation, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.status >= CALL_STATUS_COUNT || reply.reserved[0] || reply.reserved[1] ||
        (reply.flags & ~(WIFI_SCAN_COMPLETE | WIFI_SCAN_TAKEN | WIFI_SCAN_CANCEL_REQUESTED))) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_connect_take(handle_t operation, handle_t *link)
{
  struct message_header request = {PROTOCOL_WIFI_CONNECT, WIFI_CONNECT_TAKE};
  return returned_handle(operation, &request, sizeof(request), link);
}

enum call_status wifi_connect_cancel(handle_t operation)
{
  struct message_header request = {PROTOCOL_WIFI_CONNECT, WIFI_CONNECT_CANCEL};
  return invoke(operation, &request, sizeof(request), NULL, 0);
}

enum call_status wifi_link_status(handle_t link, struct wifi_link_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_link_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_LINK, WIFI_LINK_STATUS};
  struct wifi_link_status_reply reply = {0};
  enum call_status result = invoke(link, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (!reply.generation || reply.reserved || reply.status >= CALL_STATUS_COUNT ||
        (reply.flags & ~(WIFI_LINK_ATTEMPTING | WIFI_LINK_ASSOCIATED | WIFI_LINK_OPEN |
            WIFI_LINK_DATA_READY | WIFI_LINK_TERMINAL | WIFI_LINK_CLOSE_REQUESTED))) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_link_disconnect(handle_t link)
{
  struct message_header request = {PROTOCOL_WIFI_LINK, WIFI_LINK_DISCONNECT};
  return invoke(link, &request, sizeof(request), NULL, 0);
}
