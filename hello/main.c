#include <file.h>
#include <memory.h>
#include <directory.h>
#include <abi/file.h>
#include <console.h>
#include <handle.h>
#include <path.h>
#include <startup.h>

static int print_content(handle_t output, handle_t content, handle_t memory)
{
  uint64_t size;
  if (file_size(content, &size) != CALL_OK) {
    return -1;
  }
  struct memory_region buffer;
  if (memory_allocate(memory, 512, &buffer) != CALL_OK) {
    return -1;
  }

  int result = -1;
  uint64_t offset = 0;
  for (;;) {
    size_t read;
    if (file_read(content, offset, (void *)buffer.address, buffer.size, &read) != CALL_OK) {
      break;
    }
    if (!read) {
      result = offset == size ? 0 : -1;
      break;
    }
    /* These examples do not modify file contents while reading. */
    if (read > size - offset ||
        console_write_all(output, (const char *)buffer.address, read) != 0) {
      break;
    }
    offset += read;
  }
  if (memory_release(memory, buffer) != CALL_OK) {
    result = -1;
  }
  return result;
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

static int read_path(handle_t output, handle_t memory, const struct path_context *context,
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
    result = print_content(output, file, memory);
  }
  if (handle_close(file) != 0) {
    result = -1;
  }
  return result;
}

static int read_application_file(handle_t output, handle_t root, handle_t memory,
                                   struct memory_region scratch)
{
  handle_t directories[8];
  handle_t temporary[8];
  struct path_workspace workspace = {temporary, 8, (char *)scratch.address, scratch.size};
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
      read_path(output, memory, &context, "app://share/hello.txt", &workspace) != 0 ||
      path_change(&context, "app://share", &workspace) != CALL_OK) {
    goto done;
  }
  result = read_path(output, memory, &context, "hello.txt", &workspace);

done:
  if (directory != HANDLE_INVALID && handle_close(directory) != 0) {
    result = -1;
  }
  path_context_close(&context);
  return result;
}

static int create_home_content(handle_t output, handle_t home, handle_t memory)
{
  handle_t created_directory = HANDLE_INVALID, created_file = HANDLE_INVALID;
  handle_t directory = HANDLE_INVALID, file = HANDLE_INVALID;
  uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
                              DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_CREATE |
                              DIRECTORY_RIGHT_WRITE_FILES;
  int result = -1;
  if (directory_create(home, "notes", DIRECTORY_KIND_DIRECTORY, directory_rights,
        &created_directory) != CALL_OK ||
      directory_create(created_directory, "greeting.txt", DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE,
        &created_file) != CALL_OK) {
    goto done;
  }

  /* Rediscover the published names through independent grants. The tree owns
   * their lifetime; closing creation handles does not remove the entries. */
  if (directory_lookup(home, "notes", DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES,
        &directory) != CALL_OK ||
      directory_lookup(directory, "greeting.txt", DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &file) != CALL_OK) {
    goto done;
  }
  if (console_print(output, "home://\n") != 0 || list_directory(output, home) != 0 ||
      console_print(output, "home://notes/\n") != 0 || list_directory(output, directory) != 0) {
    goto done;
  }
  static const char greeting[] = "Hello from a RAM file!\n";
  static const char temporary[] = "This tail will be removed.\n";
  size_t written;
  if (file_write(created_file, 0, greeting, sizeof(greeting) - 1, &written) != CALL_OK ||
      file_write(created_file, sizeof(greeting) - 1, temporary, sizeof(temporary) - 1,
        &written) != CALL_OK ||
      file_resize(created_file, sizeof(greeting) - 1) != CALL_OK) {
    goto done;
  }
  result = print_content(output, file, memory);

done:
  handle_t owned[] = {file, directory, created_file, created_directory};
  for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i) {
    if (owned[i] != HANDLE_INVALID && handle_close(owned[i]) != 0) {
      result = -1;
    }
  }
  return result;
}

int main(int argc, char **argv)
{
  handle_t output = startup_resource("output");
  handle_t root = startup_root("app");
  handle_t home = startup_root("home");
  handle_t memory = startup_resource("memory");
  if (output == HANDLE_INVALID || root == HANDLE_INVALID || home == HANDLE_INVALID ||
      memory == HANDLE_INVALID) {
    return 1;
  }

  /* This path workspace lives until process exit; individual file buffers
   * above are released after use. Neither lifetime needs a userspace heap yet. */
  struct memory_region scratch;
  if (memory_allocate(memory, 256, &scratch) != CALL_OK) {
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
    result = read_application_file(output, root, memory, scratch);
  }
  if (result == 0) {
    result = create_home_content(output, home, memory);
  }
  if (handle_close(memory) != 0) {
    result = 1;
  }
  if (handle_close(home) != 0) {
    result = 1;
  }
  if (handle_close(root) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
