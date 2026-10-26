#include <gamepad.h>
#include <syscall.h>
#include <string.h>

static enum call_status gamepad_request(handle_t gamepad, const void *request,
    size_t size, void *output, size_t output_size)
{
  if (output_size) {
    memset(output, 0, output_size);
  }
  struct syscall_result result = syscall_call(gamepad, request, size, output, output_size);
  if (result.status >= CALL_STATUS_COUNT) {
    if (output_size) {
      memset(output, 0, output_size);
    }
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? output_size : 0)) {
    if (output_size) {
      memset(output, 0, output_size);
    }
    return CALL_BAD_REQUEST;
  }
  if (result.status != CALL_OK && output_size) {
    memset(output, 0, output_size);
  }
  return result.status;
}

static enum call_status gamepad_command(handle_t gamepad, uint64_t operation)
{
  struct message_header request = {PROTOCOL_GAMEPAD, operation};
  return gamepad_request(gamepad, &request, sizeof(request), NULL, 0);
}

enum call_status gamepad_acquire(handle_t gamepad)
{
  return gamepad_command(gamepad, GAMEPAD_ACQUIRE);
}

enum call_status gamepad_release(handle_t gamepad)
{
  return gamepad_command(gamepad, GAMEPAD_RELEASE);
}

enum call_status gamepad_read(handle_t gamepad, uint64_t flags,
    struct gamepad_event *event)
{
  if (!event) {
    return CALL_BAD_REQUEST;
  }
  struct gamepad_read_request request = {
    .header = {PROTOCOL_GAMEPAD, GAMEPAD_READ}, .flags = flags,
  };
  return gamepad_request(gamepad, &request, sizeof(request), event, sizeof(*event));
}

enum call_status gamepad_catalog(handle_t gamepad, struct gamepad_catalog *catalog)
{
  if (!catalog) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_GAMEPAD, GAMEPAD_CATALOG};
  return gamepad_request(gamepad, &request, sizeof(request), catalog, sizeof(*catalog));
}

enum call_status gamepad_info(handle_t gamepad, uint64_t instance,
    struct gamepad_info *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  struct gamepad_device_request request = {
    .header = {PROTOCOL_GAMEPAD, GAMEPAD_INFO}, .instance = instance,
  };
  return gamepad_request(gamepad, &request, sizeof(request), info, sizeof(*info));
}

enum call_status gamepad_state(handle_t gamepad, uint64_t instance,
    struct gamepad_event *state)
{
  if (!state) {
    return CALL_BAD_REQUEST;
  }
  struct gamepad_device_request request = {
    .header = {PROTOCOL_GAMEPAD, GAMEPAD_STATE}, .instance = instance,
  };
  return gamepad_request(gamepad, &request, sizeof(request), state, sizeof(*state));
}
