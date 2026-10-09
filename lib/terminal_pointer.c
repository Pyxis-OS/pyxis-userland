#include <terminal_pointer.h>
#include <syscall.h>

static enum call_status pointer_call(handle_t pointer, const void *request, size_t size,
    void *reply, size_t reply_size)
{
  struct syscall_result result = syscall_call(pointer, request, size, reply, reply_size);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static enum call_status pointer_command(handle_t pointer, uint64_t operation)
{
  struct message_header request = {PROTOCOL_TERMINAL_POINTER, operation};
  return pointer_call(pointer, &request, sizeof(request), NULL, 0);
}

enum call_status terminal_pointer_acquire(handle_t pointer)
{
  return pointer_command(pointer, TERMINAL_POINTER_ACQUIRE);
}

enum call_status terminal_pointer_release(handle_t pointer)
{
  return pointer_command(pointer, TERMINAL_POINTER_RELEASE);
}

enum call_status terminal_pointer_cancel_clipboard(handle_t pointer)
{
  return pointer_command(pointer, TERMINAL_POINTER_CANCEL_CLIPBOARD);
}

enum call_status terminal_pointer_read(handle_t pointer, uint64_t flags,
    struct pointer_event *event)
{
  if (!event) {
    return CALL_BAD_REQUEST;
  }
  *event = (struct pointer_event){0};
  struct pointer_read_request request = {
    .header = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_READ},
    .flags = flags,
  };
  struct pointer_event reply;
  enum call_status status = pointer_call(pointer, &request, sizeof(request), &reply, sizeof(reply));
  if (status == CALL_OK) {
    *event = reply;
  }
  return status;
}

enum call_status terminal_pointer_get_geometry(handle_t pointer,
    struct terminal_pointer_geometry *geometry)
{
  if (!geometry) {
    return CALL_BAD_REQUEST;
  }
  *geometry = (struct terminal_pointer_geometry){0};
  struct message_header request = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_GEOMETRY};
  struct terminal_pointer_geometry reply;
  enum call_status status = pointer_call(pointer, &request, sizeof(request), &reply, sizeof(reply));
  if (status == CALL_OK) {
    *geometry = reply;
  }
  return status;
}

enum call_status terminal_pointer_view_changed(handle_t pointer, uint64_t generation,
    uint64_t mapping_identity, struct terminal_pointer_geometry *geometry)
{
  if (!geometry) {
    return CALL_BAD_REQUEST;
  }
  *geometry = (struct terminal_pointer_geometry){0};
  struct terminal_pointer_view_request request = {
    .header = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_VIEW_CHANGED},
    .generation = generation,
    .mapping_identity = mapping_identity,
  };
  struct terminal_pointer_geometry reply;
  enum call_status status = pointer_call(pointer, &request, sizeof(request), &reply, sizeof(reply));
  if (status == CALL_OK) {
    *geometry = reply;
  }
  return status;
}

enum call_status terminal_pointer_set_image(handle_t pointer, const void *pixels,
    uint32_t width, uint32_t height, uint32_t hotspot_x, uint32_t hotspot_y)
{
  struct pointer_image_request request = {
    .header = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_SET_IMAGE},
    .address = (uintptr_t)pixels,
    .width = width, .height = height,
    .hotspot_x = hotspot_x, .hotspot_y = hotspot_y,
  };
  return pointer_call(pointer, &request, sizeof(request), NULL, 0);
}

enum call_status terminal_pointer_set_visible(handle_t pointer, bool visible)
{
  struct pointer_visibility_request request = {
    .header = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_VISIBILITY},
    .visible = visible,
  };
  return pointer_call(pointer, &request, sizeof(request), NULL, 0);
}

enum call_status terminal_pointer_default_image(handle_t pointer)
{
  return pointer_command(pointer, TERMINAL_POINTER_DEFAULT_IMAGE);
}

enum call_status terminal_pointer_state(handle_t pointer, uint64_t *flags)
{
  if (!flags) {
    return CALL_BAD_REQUEST;
  }
  *flags = 0;
  struct message_header request = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_STATE};
  uint64_t reply;
  enum call_status status = pointer_call(pointer, &request, sizeof(request), &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (reply & ~(uint64_t)POINTER_EVENT_FOCUSED) {
      return CALL_BAD_REQUEST;
    }
    *flags = reply;
  }
  return status;
}

enum call_status terminal_pointer_clipboard_refuse(handle_t pointer, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, uint64_t operation)
{
  struct terminal_pointer_clipboard_refuse_request request = {
    .header = {PROTOCOL_TERMINAL_POINTER, TERMINAL_POINTER_CLIPBOARD_REFUSE},
    .action_id = action_id, .generation = generation, .mapping_identity = mapping_identity,
    .operation = operation,
  };
  return pointer_call(pointer, &request, sizeof(request), NULL, 0);
}
