#include <output.h>
#include <handle.h>
#include <stdbool.h>
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

enum call_status output_create(handle_t service, struct output_create_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_PIPE_SERVICE, PIPE_CREATE_OUTPUT};
  struct output_create_reply response;
  enum call_status status = response_status(syscall_call(service, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response), true);
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status output_status(handle_t collector, struct output_status_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_OUTPUT_COLLECTOR, OUTPUT_STATUS};
  struct output_status_reply response;
  enum call_status status = response_status(syscall_call(collector, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response), false);
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status output_producer_status(handle_t producer, struct output_status_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct handle_info info;
  enum call_status status = handle_query(producer, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE || info.transport) {
    return CALL_BAD_OPERATION;
  }
  if (!(info.rights & FILE_RIGHT_WRITE)) {
    return CALL_DENIED;
  }
  struct message_header request = {PROTOCOL_FILE, FILE_OUTPUT_STATUS};
  struct output_status_reply response;
  status = response_status(syscall_call(producer, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response), false);
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

static enum call_status collector_mutation(handle_t collector, uint64_t operation)
{
  struct message_header request = {PROTOCOL_OUTPUT_COLLECTOR, operation};
  return response_status(syscall_call(collector, &request, sizeof(request), NULL, 0), 0, true);
}

enum call_status output_seal(handle_t collector)
{
  return collector_mutation(collector, OUTPUT_SEAL);
}

enum call_status output_cancel(handle_t collector)
{
  return collector_mutation(collector, OUTPUT_CANCEL);
}

enum call_status output_take(handle_t collector, struct output_take_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_OUTPUT_COLLECTOR, OUTPUT_TAKE};
  struct output_take_reply response;
  enum call_status status = response_status(syscall_call(collector, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response), true);
  if (status == CALL_BAD_BUFFER) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status output_handoff(handle_t producer, struct output_handoff_request *owned)
{
  if (!owned || !owned->value || (owned->observation && owned->value == owned->observation)) {
    return CALL_BAD_REQUEST;
  }
  struct handle_info info;
  enum call_status status = handle_query(producer, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE || info.transport) {
    return CALL_BAD_OPERATION;
  }
  if (!(info.rights & FILE_RIGHT_WRITE)) {
    return CALL_DENIED;
  }
  struct {
    struct message_header header;
    struct output_handoff_request body;
  } request = {{PROTOCOL_FILE, FILE_HANDOFF}, *owned};
  status = response_status(syscall_call(producer, &request, sizeof(request), NULL, 0), 0, true);
  if (status == CALL_OK || status == CALL_OUTCOME_UNKNOWN) {
    *owned = (struct output_handoff_request){0};
  }
  return status;
}
