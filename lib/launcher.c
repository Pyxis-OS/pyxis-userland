#include <launcher.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>

enum call_status launcher_create_group(handle_t launcher,
    struct execution_group_create_reply *reply, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_LAUNCHER, LAUNCHER_CREATE_GROUP};
  struct execution_group_create_reply response = {0};
  struct syscall_result result = syscall_call_with_limit(launcher, &request, sizeof(request),
      &response, sizeof(response), limit);
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

enum call_status execution_group_terminate(handle_t supervision)
{
  struct message_header request = {PROTOCOL_EXECUTION_GROUP, EXECUTION_GROUP_TERMINATE};
  struct syscall_result result = syscall_call(supervision, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status execution_group_wait(handle_t observer)
{
  struct message_header request = {PROTOCOL_EXECUTION_GROUP, EXECUTION_GROUP_WAIT};
  struct syscall_result result = syscall_call(observer, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status execution_group_wait_result(handle_t observer,
    struct execution_group_result *completion)
{
  if (!completion) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_EXECUTION_GROUP, EXECUTION_GROUP_WAIT};
  struct execution_group_result reply = {0};
  struct syscall_result result = syscall_call(observer, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0) ||
      (result.status == CALL_OK && (reply.flags & ~EXECUTION_GROUP_RESULT_FLAGS))) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status == CALL_OK) {
    *completion = reply;
  }
  return result.status;
}

enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
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
  struct syscall_result result = syscall_call_with_limit(launcher, &message, sizeof(message),
      &reply, sizeof(reply), limit);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    /* The kernel can publish a child, then fail to copy its observer reply. */
    if (result.status == CALL_BAD_BUFFER) {
      return CALL_OUTCOME_UNKNOWN;
    }
    return result.status;
  }
  if (reply == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *child = reply;
  return CALL_OK;
}

enum call_status launcher_launch_observed(handle_t launcher, const struct launch_request *request,
    struct launch_observation *observation, handle_t *child, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (observation) {
    observation->registration = (struct readiness_set_registration){0};
  }
  if (child) {
    *child = HANDLE_INVALID;
  }
  if (!child || !request || !observation) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct launch_observed_request body;
  } message = {
    .header = {PROTOCOL_LAUNCHER, LAUNCHER_LAUNCH_OBSERVED},
    .body = {.request = *request, .set = observation->set, .tag = observation->tag},
  };
  struct launch_observed_reply reply = {0};
  struct syscall_result result = syscall_call_with_limit(launcher, &message, sizeof(message),
      &reply, sizeof(reply), limit);
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    return result.status == CALL_BAD_BUFFER ? CALL_OUTCOME_UNKNOWN : result.status;
  }
  if (reply.child == HANDLE_INVALID || !reply.registration.identity ||
      !reply.registration.generation) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *child = reply.child;
  observation->registration = reply.registration;
  return CALL_OK;
}

enum call_status launcher_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, handle_t *children, uint64_t *failed_index, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (failed_index) {
    *failed_index = LAUNCH_NO_STAGE;
  }
  if (!requests || !children || !failed_index || !count) {
    return CALL_BAD_REQUEST;
  }
  if (count > (SIZE_MAX - sizeof(struct launch_batch_reply)) / sizeof(handle_t) ||
      count > SIZE_MAX / sizeof(*requests)) {
    if (limit) {
      *limit = (struct call_limit){.reason = CALL_LIMIT_REQUEST_ENCODING,
        .requested = UINT64_MAX, .limit = SIZE_MAX,
        .flags = CALL_LIMIT_HAS_REQUESTED | CALL_LIMIT_HAS_LIMIT |
            CALL_LIMIT_REQUESTED_SATURATED};
    }
    return CALL_LIMIT;
  }
  memset(children, 0, count * sizeof(*children));
  size_t size = sizeof(struct launch_batch_reply) + count * sizeof(handle_t);
  struct launch_batch_reply *reply = calloc(1, size);
  if (!reply) {
    return CALL_NO_MEMORY;
  }
  struct launch_batch_message message = {
    .header = {PROTOCOL_LAUNCHER, LAUNCHER_LAUNCH_BATCH},
    .body = {(uintptr_t)requests, count},
  };
  struct syscall_result result = syscall_call_with_limit(launcher, &message, sizeof(message),
      reply, size, limit);
  enum call_status status = CALL_OUTCOME_UNKNOWN;
  if (result.status >= CALL_STATUS_COUNT) {
    goto done;
  }
  if (result.reply_size == 0 && result.status != CALL_OK) {
    status = result.status == CALL_BAD_BUFFER ? CALL_OUTCOME_UNKNOWN : result.status;
    goto done;
  }
  if (result.reply_size != size) {
    goto done;
  }
  if (result.status == CALL_OK) {
    if (reply->failed_index != LAUNCH_NO_STAGE || reply->child_count != count) {
      goto done;
    }
    for (size_t i = 0; i < count; ++i) {
      if (reply->children[i] == HANDLE_INVALID) {
        goto done;
      }
    }
    memcpy(children, reply->children, count * sizeof(*children));
    status = CALL_OK;
    goto done;
  }
  if (reply->child_count ||
      (reply->failed_index != LAUNCH_NO_STAGE && reply->failed_index >= count)) {
    goto done;
  }
  for (size_t i = 0; i < count; ++i) {
    if (reply->children[i] != HANDLE_INVALID) {
      goto done;
    }
  }
  *failed_index = reply->failed_index;
  status = result.status;
done:
  free(reply);
  return status;
}
