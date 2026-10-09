#include <profile.h>
#include <syscall.h>

static enum call_status profile_command(handle_t profile, uint64_t operation,
    struct profile_snapshot *snapshot)
{
  if (snapshot) {
    *snapshot = (struct profile_snapshot){0};
  }
  struct message_header message = {PROTOCOL_PROFILE, operation};
  struct profile_snapshot reply;
  size_t size = snapshot ? sizeof(reply) : 0;
  struct syscall_result result = syscall_call(profile, &message, sizeof(message), &reply, size);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (snapshot && result.status == CALL_OK) {
    *snapshot = reply;
  }
  return result.status;
}

enum call_status profile_begin(handle_t profile)
{
  return profile_command(profile, PROFILE_BEGIN, NULL);
}

enum call_status profile_snapshot(handle_t profile, struct profile_snapshot *snapshot)
{
  return snapshot ? profile_command(profile, PROFILE_SNAPSHOT, snapshot) : CALL_BAD_REQUEST;
}

enum call_status profile_end(handle_t profile, struct profile_snapshot *snapshot)
{
  return snapshot ? profile_command(profile, PROFILE_END, snapshot) : CALL_BAD_REQUEST;
}

static enum call_status profile_host_command(handle_t profile, uint64_t operation,
    struct profile_host_snapshot *snapshot)
{
  if (snapshot) {
    *snapshot = (struct profile_host_snapshot){0};
  }
  struct message_header message = {PROTOCOL_PROFILE, operation};
  struct profile_host_snapshot reply;
  size_t size = snapshot ? sizeof(reply) : 0;
  struct syscall_result result = syscall_call(profile, &message, sizeof(message), &reply, size);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (snapshot && result.status == CALL_OK) {
    *snapshot = reply;
  }
  return result.status;
}

enum call_status profile_host_begin(handle_t profile)
{
  return profile_host_command(profile, PROFILE_HOST_BEGIN, NULL);
}

enum call_status profile_host_snapshot(handle_t profile, struct profile_host_snapshot *snapshot)
{
  return snapshot ? profile_host_command(profile, PROFILE_HOST_SNAPSHOT, snapshot) : CALL_BAD_REQUEST;
}

enum call_status profile_host_end(handle_t profile, struct profile_host_snapshot *snapshot)
{
  return snapshot ? profile_host_command(profile, PROFILE_HOST_END, snapshot) : CALL_BAD_REQUEST;
}
