#ifndef USERSPACE_SHELL_H
#define USERSPACE_SHELL_H

#include <abi/launcher.h>
#include <abi/startup.h>
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

struct shell_redirection {
  enum startup_stream_index stream;
  const char *path;
};

struct shell_stage {
  char **arguments;
  size_t count;
  size_t redirection_count;
  struct shell_redirection redirections[STARTUP_STREAM_COUNT];
};

struct shell_command_line {
  size_t stage_count;
  bool background;
  struct shell_stage stages[LAUNCH_BATCH_MAX];
};

struct shell {
  const char *script_name; /* Borrowed diagnostic name, NULL for interactive input. */
  size_t script_line;
  struct terminal terminal;
  handle_t profile, space, launcher, memory, display, clock, echo, udp, tcp, pipe, random, net_config, keyboard, app, home, host, host_mount;
  bool owns_host;
  struct path_root roots[3];
  struct path_context directory;
  struct path_workspace workspace;
  char *working_path; /* Owned display metadata; directory handles authorize lookup. */
};

/* Compacts in place. Arguments and redirection paths borrow line; capacity
 * includes a NULL after each stage's arguments. Redirections remain in source order.
 * An unquoted trailing & selects background launch.
 * Returns a static diagnostic on malformed input, NULL on success. */
const char *parse_line(char *line, char **arguments, size_t capacity,
    struct shell_command_line *command);
enum call_status shell_directory_init(struct shell *shell);
void shell_directory_close(struct shell *shell);
enum call_status shell_change_directory(struct shell *shell, const char *path);
enum call_status shell_open_image(struct shell *shell, const char *command, handle_t *image);
enum call_status shell_open_redirect(struct shell *shell, const char *path,
    bool input, handle_t *file);
enum command_result shell_error(struct shell *shell, const char *format, ...);
enum command_result shell_directory_error(struct shell *shell, const char *operation,
    const char *path, enum call_status status);
enum command_result shell_command(struct shell *shell, char *line, char **arguments);
enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode, const struct shell_redirection *redirections,
    size_t redirection_count);
enum command_result shell_launch_pipeline(struct shell *shell,
    const struct shell_command_line *command);
/* Borrows script; line has SHELL_SCRIPT_LINE_MAX + 1 bytes. */
int shell_script(struct shell *shell, handle_t script, char *line, char **arguments);

#endif
