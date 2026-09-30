#include <launcher.h>
#include <syscall.h>

enum call_status launcher_create_group(handle_t launcher,
    struct execution_group_create_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_LAUNCHER, LAUNCHER_CREATE_GROUP};
  struct execution_group_create_reply response = {0};
  struct syscall_result result = syscall_call(launcher, &request, sizeof(request),
      &response, sizeof(response));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(response) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  if (response.supervision == HANDLE_INVALID || response.launcher == HANDLE_INVALID ||
      response.supervision == response.launcher) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *reply = response;
  return CALL_OK;
}

enum call_status execution_group_seal(handle_t supervision)
{
  struct message_header request = {PROTOCOL_EXECUTION_GROUP, EXECUTION_GROUP_SEAL};
  struct syscall_result result = syscall_call(supervision, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child)
{
  if (!child || !request) {
    if (child) {
      *child = HANDLE_INVALID;
    }
    return CALL_BAD_REQUEST;
  }
  struct launch_message message = {
    .header = {PROTOCOL_LAUNCHER, LAUNCHER_LAUNCH},
    .body = *request,
  };
  *child = HANDLE_INVALID;
  handle_t reply;
  struct syscall_result result = syscall_call(launcher, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  if (reply == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  *child = reply;
  return CALL_OK;
}

enum call_status launcher_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, handle_t *children, uint64_t *failed_index)
{
  if (failed_index) {
    *failed_index = LAUNCH_NO_STAGE;
  }
  if (children && count > 0 && count <= LAUNCH_BATCH_MAX) {
    for (size_t i = 0; i < count; ++i) {
      children[i] = HANDLE_INVALID;
    }
  }
  if (!requests || !children || !failed_index || !count || count > LAUNCH_BATCH_MAX) {
    return CALL_BAD_REQUEST;
  }

  struct launch_batch_message message = {
    .header = {PROTOCOL_LAUNCHER, LAUNCHER_LAUNCH_BATCH},
    .body = {(uintptr_t)requests, count},
  };
  struct launch_batch_reply reply = {0};
  struct syscall_result result = syscall_call(launcher, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size == 0 && result.status != CALL_OK) {
    return result.status;
  }
  if (result.reply_size != sizeof(reply)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status == CALL_OK) {
    if (reply.failed_index != LAUNCH_NO_STAGE) {
      return CALL_OUTCOME_UNKNOWN;
    }
    for (size_t i = 0; i < LAUNCH_BATCH_MAX; ++i) {
      if ((i < count && reply.children[i] == HANDLE_INVALID) ||
          (i >= count && reply.children[i] != HANDLE_INVALID)) {
        return CALL_OUTCOME_UNKNOWN;
      }
    }
    for (size_t i = 0; i < count; ++i) {
      children[i] = reply.children[i];
    }
    return CALL_OK;
  }

  if (reply.failed_index != LAUNCH_NO_STAGE && reply.failed_index >= count) {
    return CALL_OUTCOME_UNKNOWN;
  }
  for (size_t i = 0; i < LAUNCH_BATCH_MAX; ++i) {
    if (reply.children[i] != HANDLE_INVALID) {
      return CALL_OUTCOME_UNKNOWN;
    }
  }
  *failed_index = reply.failed_index;
  return result.status;
}
