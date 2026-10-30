#include <wait.h>
#include <syscall.h>
#include <string.h>

enum call_status wait_many(const struct wait_interest *interests, size_t count,
    uint64_t deadline_ns, uint64_t *events)
{
  if (!interests || !events || !count || count > WAIT_MAX_INTERESTS) {
    return CALL_BAD_REQUEST;
  }
  uint64_t response[WAIT_MAX_INTERESTS];
  struct syscall_result result = syscall_wait_many(interests, count, deadline_ns, response);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  size_t size = count * sizeof(*events);
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    memcpy(events, response, size);
  }
  return result.status;
}

static enum call_status readiness_reply(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status readiness_set_create(handle_t *set, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (!set) {
    return CALL_BAD_REQUEST;
  }
  *set = HANDLE_INVALID;
  struct readiness_set_create_reply reply = {0};
  enum call_status status = readiness_reply(
      syscall_readiness_set_create_with_limit(&reply, sizeof(reply), limit), sizeof(reply));
  if (status == CALL_OK) {
    if (reply.handle == HANDLE_INVALID) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *set = reply.handle;
  }
  return status;
}

enum call_status readiness_set_register(handle_t set, handle_t target,
    uint64_t events, uint64_t tag, struct readiness_set_registration *registration,
    struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (!registration) {
    return CALL_BAD_REQUEST;
  }
  struct readiness_set_register_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_REGISTER},
    .target = target, .allowed_events = events, .active_events = events, .tag = tag,
  };
  struct readiness_set_registration reply = {0};
  enum call_status status = readiness_reply(syscall_call_with_limit(set, &request,
      sizeof(request), &reply, sizeof(reply), limit), sizeof(reply));
  if (status == CALL_OK) {
    *registration = reply;
  }
  return status;
}

enum call_status readiness_set_update(handle_t set,
    struct readiness_set_registration *registration, uint64_t events,
    struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (!registration) {
    return CALL_BAD_REQUEST;
  }
  struct readiness_set_update_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_UPDATE},
    .registration = *registration, .active_events = events,
  };
  struct readiness_set_registration reply = {0};
  enum call_status status = readiness_reply(syscall_call_with_limit(set, &request,
      sizeof(request), &reply, sizeof(reply), limit), sizeof(reply));
  if (status == CALL_OK) {
    *registration = reply;
  }
  return status;
}

enum call_status readiness_set_remove(handle_t set,
    struct readiness_set_registration registration)
{
  struct readiness_set_remove_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_REMOVE},
    .registration = registration,
  };
  return readiness_reply(syscall_call(set, &request, sizeof(request), NULL, 0), 0);
}

enum call_status readiness_set_wait(handle_t set, uint64_t deadline,
    struct readiness_set_event *events, size_t capacity, size_t *count)
{
  if (!events || !count || !capacity || capacity > READINESS_SET_BATCH_MAX) {
    return CALL_BAD_REQUEST;
  }
  *count = 0;
  struct readiness_set_wait_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_WAIT}, .deadline = deadline,
    .events_address = (uintptr_t)events, .capacity = capacity,
  };
  struct readiness_set_wait_reply reply = {0};
  enum call_status status = readiness_reply(syscall_call(set, &request, sizeof(request),
      &reply, sizeof(reply)), sizeof(reply));
  if (status == CALL_OK) {
    if (reply.count > capacity || (reply.flags & ~READINESS_SET_WOKEN)) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *count = reply.count;
  }
  return status;
}
