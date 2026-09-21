#include <abi/console.h>
#include <console.h>
#include <syscall.h>

int console_write(handle_t output, const void *bytes, size_t size, size_t *written)
{
  if (!written) {
    return -1;
  }
  *written = 0;
  struct console_write_request request = {
    .address = (uintptr_t)bytes,
    .length = size,
  };
  struct console_write_reply reply;
  struct syscall_result result = syscall_call(output, CONSOLE_WRITE,
      &request, sizeof(request), &reply, sizeof(reply));
  if (result.status != CALL_OK || result.reply_size != sizeof(reply) ||
      reply.written > size || (size && !reply.written)) {
    return -1;
  }
  *written = reply.written;
  return 0;
}

int console_print(handle_t output, const char *text)
{
  size_t remaining = 0;
  while (text[remaining]) {
    ++remaining;
  }

  while (remaining) {
    size_t written;
    if (console_write(output, text, remaining, &written) != 0) {
      return -1;
    }
    text += written;
    remaining -= written;
  }
  return 0;
}
