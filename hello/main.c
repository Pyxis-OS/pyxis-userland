#include <file.h>
#include <stdlib.h>
#include <stdio.h>
#include <directory.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <abi/console.h>
#include <abi/memory.h>
#include <console.h>
#include <term.h>
#include <process.h>
#include <launcher.h>
#include <handle.h>
#include <path.h>
#include <startup.h>

static int print_content(handle_t output, handle_t content)
{
  uint64_t size;
  if (file_size(content, &size) != CALL_OK) {
    return -1;
  }
  const size_t buffer_size = 4096;
  char *buffer = malloc(buffer_size);
  if (!buffer) {
    return -1;
  }

  int result = -1;
  uint64_t offset = 0;
  for (;;) {
    size_t read;
    if (file_read(content, offset, buffer, buffer_size, &read) != CALL_OK) {
      break;
    }
    if (!read) {
      result = offset == size ? 0 : -1;
      break;
    }
    /* These examples do not modify file contents while reading. */
    if (read > size - offset ||
        console_write_all(output, buffer, read) != 0) {
      break;
    }
    offset += read;
  }
  free(buffer);
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

static int read_application_file(handle_t output, handle_t root, char *scratch,
                                   size_t scratch_size)
{
  handle_t directories[8];
  handle_t temporary[8];
  struct path_workspace workspace = {temporary, 8, scratch, scratch_size};
  struct path_context context;
  if (path_context_init(&context, directories, 8, startup_working_directories(),
        startup_working_directory_count()) != CALL_OK) {
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

static int create_home_content(handle_t home)
{
  handle_t directory;
  uint64_t rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE |
                    DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES;
  if (directory_create(home, "notes", DIRECTORY_KIND_DIRECTORY, rights, &directory) != CALL_OK) {
    return -1;
  }
  if (handle_close(directory) != 0) {
    return -1;
  }

  FILE *file = fopen("home://notes/greeting.txt", "w+");
  if (!file) {
    perror("open greeting");
    return -1;
  }
  int result = -1;
  if (fprintf(file, "Hello from %s!\n", "stdio") < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    perror("write greeting");
    goto done;
  }

  char buffer[128];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
    if (fwrite(buffer, 1, count, stdout) != count) {
      perror("print greeting");
      goto done;
    }
  }
  if (ferror(file)) {
    perror("read greeting");
    goto done;
  }
  result = 0;

done:
  if (fclose(file) != 0) {
    perror("close greeting");
    result = -1;
  }
  return result;
}

static int run_utility(handle_t launcher, handle_t output, handle_t memory,
                       handle_t app, handle_t home, const char **arguments, size_t count,
                       uint64_t app_rights, uint64_t home_rights)
{
  handle_t image;
  if (directory_lookup(app, arguments[0], DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image) != CALL_OK) {
    return -1;
  }

  enum { UTILITY_OUTPUT, UTILITY_MEMORY, UTILITY_APP, UTILITY_HOME, UTILITY_GRANTS };
  struct launch_grant grants[UTILITY_GRANTS + STARTUP_STREAM_COUNT] = {
    {output, CONSOLE_RIGHT_WRITE, 0},
    {memory, MEMORY_RIGHT_MANAGE, 0},
    {app, app_rights, 0},
    {home, home_rights, 0},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"output", UTILITY_OUTPUT},
    {(uintptr_t)"memory", UTILITY_MEMORY},
  };
  struct launch_binding roots[] = {
    {(uintptr_t)"app", UTILITY_APP},
    {(uintptr_t)"home", UTILITY_HOME},
  };
  uint64_t working_directory = UTILITY_HOME;
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = UTILITY_GRANTS,
    .resources = (uintptr_t)resources,
    .resource_count = sizeof(resources) / sizeof(resources[0]),
    .roots = (uintptr_t)roots,
    .root_count = sizeof(roots) / sizeof(roots[0]),
    .working_directories = (uintptr_t)&working_directory,
    .working_directory_count = 1,
    .working_path = (uintptr_t)"home://",
    .argv = (uintptr_t)arguments,
    .argc = count,
  };
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    struct startup_stream stream = startup_stream(i);
    if (stream.protocol == STARTUP_STREAM_NONE || i == STARTUP_STDIN) {
      continue;
    }
    bool input = i == STARTUP_STDIN;
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        stream.protocol == PROTOCOL_PIPE ?
        (input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE) :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    request.streams[i] = (struct launch_stream){stream.protocol, request.grant_count};
    grants[request.grant_count++] = (struct launch_grant){stream.handle, rights, 0};
  }
  handle_t child;
  int result = launcher_launch(launcher, &request, &child) == CALL_OK ? 0 : -1;
  if (handle_close(image) != 0) {
    result = -1;
  }

  /* Finish all child output before starting the interactive line editor. */
  if (child != HANDLE_INVALID) {
    struct process_result completion;
    if (process_wait(child, &completion) != CALL_OK ||
        completion.kind != PROCESS_EXITED || completion.exit_status != 0) {
      result = -1;
    }
    if (handle_close(child) != 0) {
      result = -1;
    }
  }
  return result;
}

static int run_utilities(handle_t launcher, handle_t output, handle_t memory,
                         handle_t app, handle_t home)
{
  uint64_t read_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES;
  const char *cat_arguments[] = {"cat.pxe", "app://share/hello.txt", "home://notes/greeting.txt"};
  if (run_utility(launcher, output, memory, app, home, cat_arguments, 3,
        read_rights, read_rights) != 0) {
    return -1;
  }

  const char *mkdir_arguments[] = {"mkdir.pxe", "home://documents"};
  if (run_utility(launcher, output, memory, app, home, mkdir_arguments, 2,
        0, DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE) != 0) {
    return -1;
  }

  uint64_t list_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE;
  const char *ls_arguments[] = {"ls.pxe", "app://", "home://", "home://documents"};
  return run_utility(launcher, output, memory, app, home, ls_arguments, 4,
      list_rights, list_rights);
}

static int receive_input(handle_t input, handle_t output)
{
  struct terminal term = {.input = input, .output = output};
  size_t columns, rows;
  if (term_size(&term, &columns, &rows) != CALL_OK) {
    return -1;
  }
  char message[128];
  int length = snprintf(message, sizeof(message),
      "Terminal: %zu columns, %zu rows. Enter a line; Ctrl+C cancels.\n", columns, rows);
  if (length < 0 || (size_t)length >= sizeof(message) ||
      term_write_all(&term, message, length) != CALL_OK) {
    return -1;
  }

  /* Heap storage leaves room for wrapped input without enlarging the initial
   * one-page user stack. Libterm itself never allocates. */
  size_t capacity = 1024;
  char *line = malloc(capacity);
  if (!line) {
    return -1;
  }
  int result = -1;
  for (;;) {
    struct term_line_result read = term_read_line(&term, "> ", line, capacity);
    if (read.status == TERM_LINE_EOF) {
      result = 0;
      break;
    }
    if (read.status == TERM_LINE_CANCELLED || read.status == TERM_LINE_INPUT_LOST) {
      const char *notice = read.status == TERM_LINE_CANCELLED ?
          "Line cancelled.\n" : "Input lost; please try again.\n";
      if (term_print(&term, notice) != CALL_OK) {
        break;
      }
      continue;
    }
    if (read.status == TERM_LINE_ERROR) {
      if (read.error == CALL_UNAVAILABLE) {
        result = term_print(&term, "Keyboard input unavailable.\n") == CALL_OK ? 0 : -1;
      }
      break;
    }
    if (read.limit_reached && term_print(&term, "Line limit reached during editing.\n") != CALL_OK) {
      break;
    }
    if (term_print(&term, "You entered: ") == CALL_OK &&
        term_write_all(&term, line, read.length) == CALL_OK &&
        term_print(&term, "\n") == CALL_OK) {
      result = 0;
    }
    break;
  }
  free(line);
  return result;
}

int main(int argc, char **argv)
{
  handle_t input = startup_resource("input");
  handle_t output = startup_resource("output");
  handle_t root = startup_root("app");
  handle_t home = startup_root("home");
  handle_t memory = startup_resource("memory");
  handle_t client = startup_resource("client_process");
  handle_t launcher = startup_resource("launcher");
  if (input == HANDLE_INVALID || output == HANDLE_INVALID || root == HANDLE_INVALID ||
      home == HANDLE_INVALID || memory == HANDLE_INVALID || client == HANDLE_INVALID ||
      launcher == HANDLE_INVALID) {
    return 1;
  }

  /* The path workspace lives until exit; file buffers are freed after use. */
  const size_t scratch_size = 256;
  char *scratch = malloc(scratch_size);
  if (!scratch) {
    return 1;
  }

  const char *os_name = getenv("OS_NAME");
  int result = puts("Hello from C!") == EOF ? -1 : 0;
  if (result == 0 && argc > 0 && os_name) {
    result = printf("%s running on %s\n", argv[0], os_name) < 0 ? -1 : 0;
  }
  if (result == 0) {
    result = read_application_file(output, root, scratch, scratch_size);
  }
  if (result == 0) {
    result = create_home_content(home);
  }
  if (result == 0) {
    struct process_result completion;
    if (process_wait(client, &completion) != CALL_OK ||
        completion.kind != PROCESS_EXITED || completion.exit_status != 0) {
      result = 1;
    } else {
      result = run_utilities(launcher, output, memory, root, home);
    }
  }
  if (handle_close(launcher) != 0) {
    result = 1;
  }
  if (result == 0) {
    result = receive_input(input, output);
  }
  if (handle_close(client) != 0) {
    result = 1;
  }
  if (handle_close(input) != 0) {
    result = 1;
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
