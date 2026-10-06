#include <space.h>
#include <string.h>
#include <syscall.h>

enum call_status space_set_title(handle_t space, const char *title)
{
  if (!title) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strnlen(title, SPACE_TITLE_MAX + 1);
  if (!length || length > SPACE_TITLE_MAX) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 0; i < length; ++i) {
    if ((unsigned char)title[i] < 0x20 || (unsigned char)title[i] > 0x7e) {
      return CALL_BAD_REQUEST;
    }
  }
  struct space_title_request request = {
    .header = {PROTOCOL_SPACE, SPACE_SET_TITLE},
    .title = (uintptr_t)title,
    .length = length,
  };
  struct syscall_result result = syscall_call(space, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status space_set_affinity(handle_t space, const uint64_t *cpus, uint64_t cpu_count)
{
  if (!cpus || !cpu_count) {
    return CALL_BAD_REQUEST;
  }
  struct space_affinity_request request = {
    .header = {PROTOCOL_SPACE, SPACE_SET_AFFINITY},
    .cpus = (uintptr_t)cpus,
    .cpu_count = cpu_count,
  };
  struct syscall_result result = syscall_call(space, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static struct space_create_request create_request(const struct space_definition *space)
{
  return (struct space_create_request){
    .header = {PROTOCOL_SPACE_FACTORY, SPACE_FACTORY_CREATE},
    .name = (uintptr_t)space->name,
    .name_length = strlen(space->name),
    .title = (uintptr_t)space->title,
    .title_length = strlen(space->title),
  };
}

enum call_status space_create_unstarted(handle_t factory, const struct space_definition *space,
    const char *reason)
{
  if (!space || !space->name || !space->title || !reason || !*reason) {
    return CALL_BAD_REQUEST;
  }
  struct space_create_request request = create_request(space);
  request.reason = (uintptr_t)reason;
  request.reason_length = strlen(reason);
  struct syscall_result result = syscall_call(factory, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status space_create_started(handle_t factory, const struct space_definition *space,
    const uint64_t *cpus, uint64_t cpu_count, const struct launch_request *launch,
    handle_t *child)
{
  if (!child) {
    return CALL_BAD_REQUEST;
  }
  *child = HANDLE_INVALID;
  if (!space || !space->name || !space->title || !cpus || !cpu_count || !launch) {
    return CALL_BAD_REQUEST;
  }
  struct space_create_request request = create_request(space);
  request.cpus = (uintptr_t)cpus;
  request.cpu_count = cpu_count;
  request.launch = (uintptr_t)launch;
  handle_t reply = HANDLE_INVALID;
  struct syscall_result result = syscall_call(factory, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status == CALL_OK) {
    if (reply == HANDLE_INVALID) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *child = reply;
  }
  return result.status;
}
