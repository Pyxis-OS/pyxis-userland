#include <file.h>
#include <directory.h>
#include <abi/file.h>
#include <console.h>
#include <handle.h>
#include <path.h>
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

static int list_directory(handle_t output, handle_t directory)
{
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply entry;
    if (directory_enumerate(directory, &cursor, name, sizeof(name), &entry) != CALL_OK) {
      return -1;
    }
    if (entry.outcome == DIRECTORY_END) {
      return 0;
    }
    if (entry.outcome != DIRECTORY_ENTRY) {
      return -1; /* This fixed-buffer example reports oversized names as an error. */
    }
    if (console_print(output, name) != 0 ||
        (entry.kind == DIRECTORY_KIND_DIRECTORY && console_print(output, "/") != 0) ||
        console_print(output, "\n") != 0) {
      return -1;
    }
    cursor = entry.cursor;
  }
}

static int read_path(handle_t output, const struct path_context *context,
                       const char *path, struct path_workspace *workspace)
{
  handle_t file;
  if (path_resolve(context, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        workspace, &file) != CALL_OK) {
    return -1;
  }
  int result = console_print(output, path);
  if (!result) {
    result = console_print(output, "\n");
  }
  if (!result) {
    result = print_content(output, file);
  }
  if (handle_close(file) != 0) {
    result = -1;
  }
  return result;
}

static int read_application_file(handle_t output, handle_t root)
{
  handle_t directories[8];
  handle_t temporary[8];
  char component[256];
  struct path_workspace workspace = {temporary, 8, component, sizeof(component)};
  struct path_context context;
  uint64_t rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
                    DIRECTORY_RIGHT_READ_FILES;
  if (path_context_init(&context, directories, 8, startup_working_directories(),
        startup_working_directory_count(), rights) != CALL_OK) {
    return -1;
  }

  handle_t directory = HANDLE_INVALID;
  int result = -1;
  if (console_print(output, "app://\n") != 0 || list_directory(output, root) != 0 ||
      path_resolve(&context, "app://share", DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_ENUMERATE, &workspace, &directory) != CALL_OK) {
    goto done;
  }
  if (console_print(output, "app://share/\n") != 0 || list_directory(output, directory) != 0 ||
      read_path(output, &context, "app://share/hello.txt", &workspace) != 0 ||
      path_change(&context, "app://share", &workspace) != CALL_OK) {
    goto done;
  }
  result = read_path(output, &context, "hello.txt", &workspace);

done:
  if (directory != HANDLE_INVALID && handle_close(directory) != 0) {
    result = -1;
  }
  path_context_close(&context);
  return result;
}

int main(int argc, char **argv)
{
  handle_t output = startup_resource("output");
  handle_t root = startup_root("app");
  if (output == HANDLE_INVALID || root == HANDLE_INVALID) {
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
    result = read_application_file(output, root);
  }
  if (handle_close(root) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
