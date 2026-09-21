#include <blob.h>
#include <console.h>
#include <handle.h>
#include <abi/startup.h>

static int print_content(handle_t output, handle_t content)
{
  uint64_t size;
  if (blob_size(content, &size) != 0) {
    return -1;
  }

  char buffer[512];
  uint64_t offset = 0;
  for (;;) {
    size_t read;
    if (blob_read(content, offset, buffer, sizeof(buffer), &read) != 0) {
      return -1;
    }
    if (!read) {
      return offset == size ? 0 : -1;
    }
    /* The blob is immutable: counts must agree with its advertised size. */
    if (read > size - offset) {
      return -1;
    }
    if (console_write_all(output, buffer, read) != 0) {
      return -1;
    }
    offset += read;
  }
}

int main(const struct startup_info *startup)
{
  if (!startup || startup->version != STARTUP_VERSION ||
      startup->size < sizeof(*startup)) {
    return 1;
  }

  int result = console_print(startup->output, "Hello from C!\n");
  if (result == 0) {
    result = print_content(startup->output, startup->content);
  }
  if (handle_close(startup->content) != 0) {
    result = 1;
  }
  if (handle_close(startup->output) != 0) {
    result = 1;
  }
  return result != 0;
}
