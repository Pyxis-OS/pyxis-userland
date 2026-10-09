#include <clipboard.h>
#include <syscall.h>

static enum call_status clipboard_call(handle_t clipboard, const void *request,
    size_t size, void *reply, size_t reply_size)
{
  struct syscall_result result = syscall_call(clipboard, request, size, reply, reply_size);
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status clipboard_publish(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, const void *bytes, size_t length)
{
  struct clipboard_publish_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_PUBLISH},
    .action_id = action_id, .generation = generation, .mapping_identity = mapping_identity,
    .address = (uintptr_t)bytes, .length = length,
  };
  return clipboard_call(clipboard, &request, sizeof(request), NULL, 0);
}

enum call_status clipboard_paste(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, handle_t attachment,
    uint64_t *transaction_id)
{
  if (!transaction_id) {
    return CALL_BAD_REQUEST;
  }
  *transaction_id = 0;
  struct clipboard_paste_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_PASTE},
    .action_id = action_id, .generation = generation, .mapping_identity = mapping_identity,
    .attachment = attachment,
  };
  struct clipboard_paste_reply reply;
  enum call_status status = clipboard_call(clipboard, &request, sizeof(request),
      &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (!reply.transaction_id) {
      return CALL_BAD_REQUEST;
    }
    *transaction_id = reply.transaction_id;
  }
  return status;
}

enum call_status clipboard_clear(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity)
{
  struct clipboard_clear_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_CLEAR},
    .action_id = action_id, .generation = generation, .mapping_identity = mapping_identity,
  };
  return clipboard_call(clipboard, &request, sizeof(request), NULL, 0);
}

enum call_status clipboard_refuse(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, uint64_t operation)
{
  struct clipboard_refuse_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_REFUSE},
    .action_id = action_id, .generation = generation, .mapping_identity = mapping_identity,
    .operation = operation,
  };
  return clipboard_call(clipboard, &request, sizeof(request), NULL, 0);
}

enum call_status clipboard_graphics_publish(handle_t clipboard, uint64_t action_id,
    const void *bytes, size_t length)
{
  struct clipboard_graphics_publish_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_GRAPHICS_PUBLISH},
    .action_id = action_id, .address = (uintptr_t)bytes, .length = length,
  };
  return clipboard_call(clipboard, &request, sizeof(request), NULL, 0);
}

enum call_status clipboard_graphics_read(handle_t clipboard, uint64_t action_id,
    void *bytes, size_t capacity, size_t *length)
{
  if (!length) {
    return CALL_BAD_REQUEST;
  }
  *length = 0;
  struct clipboard_graphics_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_GRAPHICS_READ},
    .action_id = action_id,
  };
  struct syscall_result result = syscall_call(clipboard, &request, sizeof(request),
      bytes, capacity);
  if (result.status >= CALL_STATUS_COUNT ||
      (result.status != CALL_OK && result.reply_size) ||
      result.reply_size > capacity || result.reply_size > CLIPBOARD_TEXT_MAX) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *length = result.reply_size;
  }
  return result.status;
}

enum call_status clipboard_graphics_has(handle_t clipboard, uint64_t action_id,
    bool *has_text)
{
  if (!has_text) {
    return CALL_BAD_REQUEST;
  }
  *has_text = false;
  struct clipboard_graphics_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_GRAPHICS_HAS},
    .action_id = action_id,
  };
  struct clipboard_graphics_has_reply reply;
  enum call_status status = clipboard_call(clipboard, &request, sizeof(request),
      &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (reply.has_text > 1) {
      return CALL_BAD_REQUEST;
    }
    *has_text = reply.has_text != 0;
  }
  return status;
}

enum call_status clipboard_graphics_refuse(handle_t clipboard, uint64_t action_id,
    uint64_t operation)
{
  struct clipboard_graphics_refuse_request request = {
    .header = {PROTOCOL_CLIPBOARD, CLIPBOARD_GRAPHICS_REFUSE},
    .action_id = action_id, .operation = operation,
  };
  return clipboard_call(clipboard, &request, sizeof(request), NULL, 0);
}
