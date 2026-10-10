#include <abi/console.h>
#include <console.h>
#include <syscall.h>

static enum call_status write_console(handle_t output, uint64_t operation,
    const void *bytes, size_t size, size_t *written)
{
  if (!written) {
    return CALL_BAD_REQUEST;
  }
  *written = 0;
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, operation},
    .body.write = {
      .address = (uintptr_t)bytes,
      .length = size,
    },
  };
  struct console_write_reply reply;
  struct syscall_result result = syscall_call(output, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.written > size ||
      (size && !reply.written)) {
    return CALL_BAD_REQUEST;
  }
  *written = reply.written;
  return CALL_OK;
}

enum call_status console_write(handle_t output, const void *bytes, size_t size, size_t *written)
{
  return write_console(output, CONSOLE_WRITE, bytes, size, written);
}

enum call_status console_try_write(handle_t output, const void *bytes, size_t size,
    size_t *written)
{
  return write_console(output, CONSOLE_TRY_WRITE, bytes, size, written);
}

enum call_status console_write_all(handle_t output, const void *bytes, size_t size)
{
  const char *cursor = bytes;
  while (size) {
    size_t written;
    enum call_status status = console_write(output, cursor, size, &written);
    if (status != CALL_OK) {
      return status;
    }
    cursor += written;
    size -= written;
  }
  return CALL_OK;
}

enum call_status console_print(handle_t output, const char *text)
{
  size_t size = 0;
  while (text[size]) {
    ++size;
  }
  return console_write_all(output, text, size);
}

static enum call_status read_console(handle_t input, void *bytes, size_t capacity,
                                    uint64_t timeout_ms, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_READ},
    .body.read = {.address = (uintptr_t)bytes, .capacity = capacity, .timeout_ms = timeout_ms},
  };
  struct console_read_reply reply;
  struct syscall_result result = syscall_call(input, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.read > capacity) {
    return CALL_BAD_REQUEST;
  }
  *read = reply.read;
  return CALL_OK;
}

enum call_status console_read(handle_t input, void *bytes, size_t capacity, size_t *read)
{
  return read_console(input, bytes, capacity, CONSOLE_WAIT_FOREVER, read);
}

enum call_status console_read_timeout(handle_t input, void *bytes, size_t capacity,
                                     uint32_t timeout_ms, size_t *read)
{
  return read_console(input, bytes, capacity, timeout_ms, read);
}

enum call_status console_size(handle_t console, struct console_size_reply *size)
{
  if (!size) {
    return CALL_BAD_REQUEST;
  }
  *size = (struct console_size_reply){0};
  struct console_message message = {.header = {PROTOCOL_CONSOLE, CONSOLE_SIZE}};
  struct console_size_reply reply;
  struct syscall_result result = syscall_call(console, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || !reply.columns || !reply.rows ||
      !reply.generation) {
    return CALL_BAD_REQUEST;
  }
  *size = reply;
  return CALL_OK;
}

enum call_status console_fresh_line(handle_t output)
{
  struct console_message message = {.header = {PROTOCOL_CONSOLE, CONSOLE_FRESH_LINE}};
  struct syscall_result result = syscall_call(output, &message, sizeof(message), NULL, 0);
  return result.status < CALL_STATUS_COUNT && !result.reply_size ?
         (enum call_status)result.status : CALL_BAD_REQUEST;
}

enum call_status console_set_tab_width(handle_t output, size_t columns)
{
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_SET_TAB_WIDTH},
    .body.tab_width = {.columns = columns},
  };
  struct syscall_result result = syscall_call(output, &message, sizeof(message), NULL, 0);
  return result.status < CALL_STATUS_COUNT && !result.reply_size ?
         (enum call_status)result.status : CALL_BAD_REQUEST;
}

static enum call_status console_handle(handle_t input, uint64_t operation, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  struct console_message message = {.header = {PROTOCOL_CONSOLE, operation}};
  struct console_handle_reply reply;
  struct syscall_result result = syscall_call(input, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.handle == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  *handle = reply.handle;
  return CALL_OK;
}

enum call_status console_arm_interrupt(handle_t input, handle_t *armed)
{
  return console_handle(input, CONSOLE_ARM_INTERRUPT, armed);
}

enum call_status console_passthrough(handle_t input, handle_t *passthrough)
{
  return console_handle(input, CONSOLE_PASSTHROUGH, passthrough);
}

static enum call_status paste_call(handle_t input, const void *request, size_t size,
    void *reply, size_t reply_size)
{
  struct syscall_result result = syscall_call(input, request, size, reply, reply_size);
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status console_paste_register(handle_t input, handle_t output, uint64_t *epoch)
{
  if (!epoch) {
    return CALL_BAD_REQUEST;
  }
  *epoch = 0;
  struct console_paste_register_request request = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_PASTE_REGISTER}, .output = output,
  };
  struct console_paste_register_reply reply;
  enum call_status status = paste_call(input, &request, sizeof(request), &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (!reply.epoch) {
      return CALL_BAD_REQUEST;
    }
    *epoch = reply.epoch;
  }
  return status;
}

enum call_status console_paste_read(handle_t input, uint64_t epoch, void *bytes,
    size_t capacity, uint64_t timeout_ms, bool boundary,
    struct console_paste_read_reply *record)
{
  if (!record) {
    return CALL_BAD_REQUEST;
  }
  *record = (struct console_paste_read_reply){0};
  struct console_paste_read_request request = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_PASTE_READ},
    .epoch = epoch, .address = (uintptr_t)bytes, .capacity = capacity,
    .timeout_ms = timeout_ms, .flags = boundary ? CONSOLE_PASTE_BOUNDARY : 0,
  };
  struct console_paste_read_reply reply;
  enum call_status status = paste_call(input, &request, sizeof(request), &reply, sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.epoch != epoch || reply.status >= CALL_STATUS_COUNT || reply.length > capacity ||
      reply.kind < CONSOLE_PASTE_INPUT || reply.kind > CONSOLE_PASTE_CANCEL ||
      (reply.kind == CONSOLE_PASTE_INPUT && (reply.length > 1 || reply.transaction_id)) ||
      (reply.kind != CONSOLE_PASTE_INPUT && !reply.transaction_id) ||
      (reply.kind == CONSOLE_PASTE_DATA && !reply.length) ||
      (reply.kind != CONSOLE_PASTE_INPUT && reply.kind != CONSOLE_PASTE_DATA && reply.length)) {
    return CALL_BAD_REQUEST;
  }
  *record = reply;
  return CALL_OK;
}

enum call_status console_paste_ack(handle_t input, uint64_t epoch, uint64_t transaction_id)
{
  struct console_paste_ack_request request = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_PASTE_ACK},
    .epoch = epoch, .transaction_id = transaction_id,
  };
  return paste_call(input, &request, sizeof(request), NULL, 0);
}

enum call_status console_paste_release(handle_t input, uint64_t epoch)
{
  struct console_paste_release_request request = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_PASTE_RELEASE}, .epoch = epoch,
  };
  return paste_call(input, &request, sizeof(request), NULL, 0);
}
