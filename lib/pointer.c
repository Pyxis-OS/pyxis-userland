#include <pointer.h>
#include <syscall.h>

static enum call_status pointer_request(handle_t pointer, const void *request,
    size_t size)
{
  struct syscall_result result = syscall_call(pointer, request, size, NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static enum call_status pointer_command(handle_t pointer, uint64_t operation)
{
  struct message_header message = {PROTOCOL_POINTER, operation};
  return pointer_request(pointer, &message, sizeof(message));
}

enum call_status pointer_acquire(handle_t pointer)
{
  return pointer_command(pointer, POINTER_ACQUIRE);
}

enum call_status pointer_release(handle_t pointer)
{
  return pointer_command(pointer, POINTER_RELEASE);
}

enum call_status pointer_read(handle_t pointer, uint64_t flags, struct pointer_event *event)
{
  if (!event) {
    return CALL_BAD_REQUEST;
  }
  *event = (struct pointer_event){0};
  struct pointer_read_request message = {
    .header = {PROTOCOL_POINTER, POINTER_READ},
    .flags = flags,
  };
  struct pointer_event reply;
  struct syscall_result result = syscall_call(pointer, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *event = reply;
  }
  return result.status;
}

enum call_status pointer_geometry(handle_t pointer, struct pointer_geometry *geometry)
{
  if (!geometry) {
    return CALL_BAD_REQUEST;
  }
  *geometry = (struct pointer_geometry){0};
  struct message_header message = {PROTOCOL_POINTER, POINTER_GEOMETRY};
  struct pointer_geometry reply;
  struct syscall_result result = syscall_call(pointer, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *geometry = reply;
  }
  return result.status;
}

enum call_status pointer_set_image(handle_t pointer, const void *pixels,
    uint32_t width, uint32_t height, uint32_t hotspot_x, uint32_t hotspot_y)
{
  struct pointer_image_request request = {
    .header = {PROTOCOL_POINTER, POINTER_SET_IMAGE},
    .address = (uintptr_t)pixels,
    .width = width,
    .height = height,
    .hotspot_x = hotspot_x,
    .hotspot_y = hotspot_y,
  };
  return pointer_request(pointer, &request, sizeof(request));
}

enum call_status pointer_default_image(handle_t pointer)
{
  return pointer_command(pointer, POINTER_DEFAULT_IMAGE);
}

enum call_status pointer_set_visible(handle_t pointer, bool visible)
{
  struct pointer_visibility_request request = {
    .header = {PROTOCOL_POINTER, POINTER_VISIBILITY},
    .visible = visible,
  };
  return pointer_request(pointer, &request, sizeof(request));
}

enum call_status pointer_warp(handle_t pointer, int64_t x, int64_t y,
    uint64_t generation, uint64_t mapping_identity)
{
  struct pointer_warp_request request = {
    .header = {PROTOCOL_POINTER, POINTER_WARP},
    .x = x,
    .y = y,
    .generation = generation,
    .mapping_identity = mapping_identity,
  };
  return pointer_request(pointer, &request, sizeof(request));
}

enum call_status pointer_lock(handle_t pointer)
{
  return pointer_command(pointer, POINTER_LOCK);
}

enum call_status pointer_unlock(handle_t pointer)
{
  return pointer_command(pointer, POINTER_UNLOCK);
}

enum call_status pointer_state(handle_t pointer, uint64_t *flags)
{
  if (!flags) {
    return CALL_BAD_REQUEST;
  }
  *flags = 0;
  struct message_header message = {PROTOCOL_POINTER, POINTER_STATE};
  uint64_t reply;
  struct syscall_result result = syscall_call(pointer, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply & ~(uint64_t)(POINTER_EVENT_FOCUSED | POINTER_EVENT_LOCKED)) {
      return CALL_BAD_REQUEST;
    }
    *flags = reply;
  }
  return result.status;
}
