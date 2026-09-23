#ifndef USERSPACE_SHELL_H
#define USERSPACE_SHELL_H

#include <path.h>
#include <term.h>

#define SHELL_LINE_CAPACITY 1024
#define APP_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define HOME_DIRECTORY_RIGHTS (APP_DIRECTORY_RIGHTS | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)

enum command_result {
  COMMAND_OK,
  COMMAND_FAILED,
  COMMAND_EXIT,
  COMMAND_FATAL, /* Terminal, wait or cleanup failed; input cannot resume. */
};

struct shell {
  const char *script_name; /* Borrowed diagnostic name, NULL for interactive input. */
  size_t script_line;
  struct terminal terminal;
  handle_t launcher, memory, app, home;
  struct path_context directory;
  struct path_workspace workspace;
  char *working_path; /* Owned display metadata; directory handles authorize lookup. */
};

/* Compacts in place. Argument pointers borrow line; capacity includes final NULL.
 * Returns a static diagnostic on malformed input, NULL on success. */
const char *parse_line(char *line, char **arguments, size_t capacity, size_t *count);
enum call_status shell_directory_init(struct shell *shell);
void shell_directory_close(struct shell *shell);
enum call_status shell_change_directory(struct shell *shell, const char *path);
enum call_status shell_open_image(struct shell *shell, const char *command, handle_t *image);
enum command_result shell_error(struct shell *shell, const char *format, ...);
enum command_result shell_directory_error(struct shell *shell, const char *operation,
    const char *path, enum call_status status);
enum command_result shell_command(struct shell *shell, char *line, char **arguments);
enum command_result shell_launch(struct shell *shell, char **arguments, size_t count);

#endif
