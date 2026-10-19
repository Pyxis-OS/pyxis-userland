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
    if (reply.status >= CALL_STATUS_COUNT || reply.record_count > WIFI_SCAN_RECORDS || reply.reserved) {
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
