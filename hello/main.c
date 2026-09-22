#include <file.h>
#include <console.h>
#include <handle.h>
#include <startup.h>

static int print_content(handle_t output, handle_t content)
{
  uint64_t size;
  if (file_size(content, &size) != 0) {
    return -1;
  }

  char buffer[512];
  uint64_t offset = 0;
  for (;;) {
    size_t read;
    if (file_read(content, offset, buffer, sizeof(buffer), &read) != 0) {
      return -1;
    }
    if (!read) {
      return offset == size ? 0 : -1;
    }
    /* This initrd file is immutable: counts must agree with its advertised size. */
    if (read > size - offset) {
      return -1;
    }
    if (console_write_all(output, buffer, read) != 0) {
      return -1;
    }
    offset += read;
  }
}

int main(int argc, char **argv)
{
  handle_t output = startup_resource("output");
  handle_t content = startup_resource("content");
  if (output == HANDLE_INVALID || content == HANDLE_INVALID) {
    return 1;
  }

  const char *os_name = startup_environment("OS_NAME");
  int result = console_print(output, "Hello from C!\n");
  if (result == 0 && argc > 0 && os_name) {
    result = console_print(output, argv[0]);
    if (result == 0) {
      result = console_print(output, " running on ");
    }
    if (result == 0) {
      result = console_print(output, os_name);
    }
    if (result == 0) {
      result = console_print(output, "\n");
    }
  }
  if (result == 0) {
    result = print_content(output, content);
  }
  if (handle_close(content) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
