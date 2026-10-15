#include <retention.h>
#include <stdbool.h>
#include <string.h>
#include <syscall.h>

static enum call_status response_status(struct syscall_result result, size_t size,
    bool mutation)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return mutation ? CALL_OUTCOME_UNKNOWN : CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status retention_create_account(handle_t factory,
    const struct retention_limits *limits, handle_t *account)
{
  if (!limits || !account || !limits->value_limit || !limits->session_limit) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct retention_limits limits;
  } request = {{PROTOCOL_RETENTION_FACTORY, RETENTION_CREATE_ACCOUNT}, *limits};
  struct retention_account_reply reply;
  enum call_status status = response_status(syscall_call(factory, &request, sizeof(request),
      &reply, sizeof(reply)), sizeof(reply), true);
  if (status == CALL_OK) {
    *account = reply.account;
  }
  return status;
}

enum call_status retention_status(handle_t account, struct retention_status_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_RETENTION_ACCOUNT, RETENTION_STATUS};
  struct retention_status_reply response;
  enum call_status status = response_status(syscall_call(account, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response), false);
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status retention_create_failure(handle_t account, uint64_t outcome,
    const char *detail, size_t detail_size, handle_t *value)
{
  if (!value || outcome == RESOURCE_OK || outcome >= RESOURCE_OUTCOME_COUNT ||
      detail_size > RESOURCE_DETAIL_MAX_BYTES || (detail_size && !detail)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct retention_failure_request body;
    char detail[RESOURCE_DETAIL_MAX_BYTES];
  } request = {
    .header = {PROTOCOL_RETENTION_ACCOUNT, RETENTION_CREATE_FAILURE},
    .body = {outcome, detail_size},
  };
  if (detail_size) {
    memcpy(request.detail, detail, detail_size);
  }
  struct retention_failure_reply reply;
  size_t request_size = sizeof(request.header) + sizeof(request.body) + detail_size;
  enum call_status status = response_status(syscall_call(account, &request, request_size,
      &reply, sizeof(reply)), sizeof(reply), true);
  if (status == CALL_OK) {
    *value = reply.value;
  }
  return status;
}
