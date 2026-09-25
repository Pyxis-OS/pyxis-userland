#ifndef USERSPACE_SHELL_H
#define USERSPACE_SHELL_H

#include <path.h>
#include <term.h>

#define SHELL_LINE_CAPACITY 1024
/* Bytes before LF, including CR in CRLF; storage also needs a trailing NUL. */
#define SHELL_SCRIPT_LINE_MAX 1024
enum command_result {
  COMMAND_OK,
  COMMAND_FAILED,
  COMMAND_EXIT,
  COMMAND_FATAL, /* Terminal, wait or cleanup failed; input cannot resume. */
};

enum shell_launch_mode { SHELL_FOREGROUND, SHELL_BACKGROUND, SHELL_SESSION };

struct shell {
  const char *script_name; /* Borrowed diagnostic name, NULL for interactive input. */
  size_t script_line;
  struct terminal terminal;
  handle_t profile, space, launcher, memory, display, clock, echo, udp, tcp, random, net_config, keyboard, app, home, host, host_mount;
  bool owns_host;
  struct path_root roots[3];
  struct path_context directory;
  struct path_workspace workspace;
  char *working_path; /* Owned display metadata; directory handles authorize lookup. */
};

/* Compacts in place. Argument pointers borrow line; capacity includes final NULL.
 * An unquoted trailing & selects background launch; no other operators.
 * Returns a static diagnostic on malformed input, NULL on success. */
const char *parse_line(char *line, char **arguments, size_t capacity, size_t *count, bool *background);
enum call_status shell_directory_init(struct shell *shell);
void shell_directory_close(struct shell *shell);
enum call_status shell_change_directory(struct shell *shell, const char *path);
enum call_status shell_open_image(struct shell *shell, const char *command, handle_t *image);
enum command_result shell_error(struct shell *shell, const char *format, ...);
enum command_result shell_directory_error(struct shell *shell, const char *operation,
    const char *path, enum call_status status);
enum command_result shell_command(struct shell *shell, char *line, char **arguments);
enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode);
/* Borrows script; line has SHELL_SCRIPT_LINE_MAX + 1 bytes. */
int shell_script(struct shell *shell, handle_t script, char *line, char **arguments);

#endif
