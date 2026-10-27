#include "../common/bootstrap_environment.h"
#include <pyxis/working_path.h>
#include <pyxis/environment.h>
#include "shell.h"
#include <abi/console.h>
#include <abi/clock.h>
#include <abi/memory.h>
#include <abi/pipe.h>
#include <abi/file.h>
#include <directory.h>
#include <command_selection.h>
#include "../common/directory.h"
#include "../common/session_wait.h"
#include "../common/pane_cwd.h"
#include <shell_settings.h>
#include <startup.h>
#include <handle.h>
#include <terminal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct shell_cwd_service {
  struct pane_cwd_provider provider;
  struct term_line_service line;
};

static void serve_cwd(void *context)
{
  struct shell_cwd_service *service = context;
  pane_cwd_provider_serve(&service->provider);
  service->line.receiver = service->provider.receiver;
}

static struct term_line_result read_command(struct shell *shell, char *line,
    struct term_line_service *service)
{
  size_t columns, rows;
  enum call_status status = term_size(&shell->terminal, &columns, &rows);
  if (status != CALL_OK) {
    return (struct term_line_result){.status = TERM_LINE_ERROR, .error = status};
  }

  /* Keep at least half the first row available for input. Only the displayed
   * prompt is shortened; child startup metadata keeps the full working path. */
  char prompt[SHELL_LINE_CAPACITY];
  size_t budget = columns / 2;
  if (budget > sizeof(prompt) - 1) {
    budget = sizeof(prompt) - 1;
  }
  size_t available = budget > 2 ? budget - 2 : 0;
  const char *path = pyxis_working_path();
  if (!path) {
    path = "[cwd unavailable]";
  }
  size_t length = strlen(path);
  size_t used = 0;
  if (length > available) {
    if (available >= 3) {
      memcpy(prompt, "...", 3);
      used = 3;
    }
    path += length - (available - used);
    length = available - used;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = path[i];
    prompt[used++] = byte >= ' ' && byte <= '~' ? byte : '?';
  }
  memcpy(prompt + used, "> ", 3);
  const struct term_completion completion = {shell_complete, shell};
  if (service->receiver != HANDLE_INVALID) {
    return term_read_line_servicing(&shell->terminal, prompt,
        shell->quiet_input ? NULL : &shell->history,
        shell->quiet_input ? NULL : &completion, shell->remote_prompt,
        shell->quiet_input, service, line, SHELL_LINE_CAPACITY);
  }
  /* The quiet editor still measures the prompt, preserving the line limit. */
  if (shell->quiet_input) {
    return term_read_line_quiet(&shell->terminal, prompt, line, SHELL_LINE_CAPACITY);
  }
  return term_read_line_completing(&shell->terminal, prompt, &shell->history, &completion,
      shell->remote_prompt, line, SHELL_LINE_CAPACITY);
}

static bool report_completion(struct shell *shell, handle_t events,
    struct shell_outcome outcome)
{
  if (events == HANDLE_INVALID) {
    return true;
  }
  enum call_status result = terminal_command_complete(events, outcome.kind, outcome.status);
  if (result != CALL_OK) {
    shell_directory_error(shell, "shell", "command completion", result);
    return false;
  }
  return true;
}

/* The exit status of a -c line: a failed foreground command's own nonzero
 * status, failure for anything else that stopped the line, otherwise success.
 * Explicit exit status is kept separately. */
static int run_command_line(struct shell *shell, const char *source, char *line)
{
  size_t length = strlen(source);
  if (length > SHELL_SCRIPT_LINE_MAX) {
    shell_error(shell, "shell: Line limit reached; command not executed\n");
    return EXIT_FAILURE;
  }
  if (strpbrk(source, "\r\n")) {
    shell_error(shell, "shell: -c takes a single line\n");
    return EXIT_FAILURE;
  }
  memcpy(line, source, length + 1);
  struct shell_outcome outcome;
  enum command_result result = shell_command(shell, line, &outcome);
  if (result == COMMAND_EXIT) {
    return shell->exit_status;
  }
  if (result == COMMAND_OK) {
    return EXIT_SUCCESS;
  }
  if (result == COMMAND_FAILED && outcome.kind == TERMINAL_COMPLETION_EXITED &&
      outcome.status > 0 && outcome.status <= INT_MAX) {
    return (int)outcome.status;
  }
  return EXIT_FAILURE;
}

static void retain_interpreter_image(struct shell *shell)
{
  if (shell->interpreter_root != HANDLE_INVALID) {
    directory_lookup(shell->interpreter_root, "init-shell.pxe", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &shell->interpreter_image);
    return;
  }
  handle_t inherited = bootstrap_capability("interpreter_image");
  if (inherited != HANDLE_INVALID) {
    struct handle_info info;
    if (handle_query(inherited, &info) == CALL_OK &&
        info.kind == HANDLE_KIND_NATIVE && info.protocol == PROTOCOL_FILE) {
      handle_copy_restricted(inherited, FILE_RIGHT_READ, 0, &shell->interpreter_image);
    }
    return;
  }
  handle_t app = bootstrap_capability("app");
  handle_t bin = HANDLE_INVALID;
  if (app != HANDLE_INVALID && directory_lookup(app, "bin", DIRECTORY_KIND_DIRECTORY,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &bin) == CALL_OK) {
    directory_lookup(bin, "shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &shell->interpreter_image);
    handle_close(bin);
  }
  /* Failure leaves re-entry unavailable; ordinary native commands still run. */
}

int main(int argc, char **argv)
{
  handle_t script = bootstrap_capability("script");
  if (script != HANDLE_INVALID && argc < 2) {
    fputs("shell: Missing script diagnostic name\n", stderr);
    return EXIT_FAILURE;
  }
  bool option = argc >= 2 && (!strcmp(argv[1], "--no-echo") ||
      !strcmp(argv[1], "--remote-prompt") || !strcmp(argv[1], "--remote-records") ||
      !strcmp(argv[1], "-c"));
  bool quiet_input = script == HANDLE_INVALID && argc == 2 && !strcmp(argv[1], "--no-echo");
  bool record_output = script == HANDLE_INVALID && argc == 2 && !strcmp(argv[1], "--remote-records");
  bool remote_prompt = record_output ||
      (script == HANDLE_INVALID && argc == 2 && !strcmp(argv[1], "--remote-prompt"));
  bool command_line = script == HANDLE_INVALID && argc >= 3 && !strcmp(argv[1], "-c");
  bool positional = script == HANDLE_INVALID && argc >= 2 && !option;
  if (script == HANDLE_INVALID && argc > 1 && !quiet_input && !remote_prompt && !positional &&
      !command_line) {
    fputs("usage: shell [script.pxs [argument...] | -c LINE [argument...] | --no-echo | "
        "--remote-prompt | --remote-records]\n", stderr);
    return EXIT_FAILURE;
  }
  /* A -c line runs as a one-line script: no prompt, script launch rules, and
   * its arguments follow the line. */
  bool interactive = script == HANDLE_INVALID && !positional && !command_line;
  /* Taken in every mode so the private grant never reaches a successor;
   * only an initial interactive prompt applies it. */
  handle_t settings = bootstrap_take_capability(SHELL_SETTINGS_RESOURCE);
  /* Script shells never claim their launcher's interactive completion channel. */
  handle_t events = interactive ? bootstrap_capability("terminal_events") : HANDLE_INVALID;
  struct shell shell = {
    .script_name = command_line ? "-c" : interactive ? NULL : argv[1],
    .positional_script = positional || command_line,
    .script_line = 1,
    /* After the interpreter or command name and the script name or line. */
    .arguments = command_line ? argv + 3 : interactive ? NULL : argv + 2,
    .argument_count = command_line ? (size_t)argc - 3 : interactive ? 0 : (size_t)argc - 2,
    .quiet_input = quiet_input,
    .record_output = record_output,
    .remote_prompt = remote_prompt,
    .terminal = {bootstrap_capability("input"), bootstrap_capability("output")},
    .launcher = bootstrap_capability("launcher"),
    .child_launcher = bootstrap_capability("child_launcher"),
    .terminal_service = bootstrap_capability("terminal"),
    .memory = bootstrap_capability("memory"),
    .threads = bootstrap_capability("threads"),
    .display = bootstrap_capability("display"),
    .clock = bootstrap_capability("clock"),
    .system_info = bootstrap_capability("system_info"),
    .log = bootstrap_capability("log"),
    .echo = bootstrap_capability("echo"),
    .udp = bootstrap_capability("udp"),
    .udp_beacons = bootstrap_capability("udp_beacons"),
    .tcp = bootstrap_capability("tcp"),
    .pipe = bootstrap_capability("pipe"),
    .service = bootstrap_capability("service"),
    .retention_account = bootstrap_capability("retention_account"),
    .job_provisioner = bootstrap_capability("job_provisioner"),
    .tcp_listen = bootstrap_capability(BUNDLE_TCP_LISTEN_SOURCE),
    .environment = startup_environment_handle(),
    .random = bootstrap_capability("random"),
    .net_registry = bootstrap_capability("net_registry"),
    .keyboard = bootstrap_capability("keyboard"),
    .gamepad = bootstrap_capability("gamepad"),
    .pointer = bootstrap_capability("pointer"),
    .space = bootstrap_capability("space"),
    .profile = bootstrap_capability("profile"),
    .boot = bootstrap_capability("boot"),
    .tmp = bootstrap_capability("tmp"),
    .host_mount = bootstrap_capability("host_mount"),
    .native_mount = bootstrap_capability("native_mount"),
    .power = bootstrap_capability("power"),
    .screen_capture = bootstrap_capability("screen_capture"),
    .audio = bootstrap_capability("audio"),
  };
  /* Only a trusted launcher supplies authority to extend interpreter trust. */
  shell.interpreter_root = bootstrap_capability("interpreter_root");
  if (shell.positional_script) {
    struct {
      handle_t handle;
      uint64_t protocol, rights;
    } required[] = {
      {shell.memory, PROTOCOL_MEMORY, MEMORY_RIGHT_MANAGE},
      {shell.clock, PROTOCOL_CLOCK, CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE | CLOCK_RIGHT_SLEEP},
      {shell.launcher, PROTOCOL_LAUNCHER, LAUNCHER_RIGHT_LAUNCH},
      {shell.pipe, PROTOCOL_PIPE_SERVICE, PIPE_SERVICE_RIGHT_CREATE},
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(*required); ++i) {
      struct handle_info info;
      if (handle_query(required[i].handle, &info) != CALL_OK ||
          info.kind != HANDLE_KIND_NATIVE || info.protocol != required[i].protocol ||
          (info.rights & required[i].rights) != required[i].rights) {
        fputs("shell: Missing declared script authority\n", stderr);
        return EXIT_FAILURE;
      }
    }
  }
  /* Only an interactive shell needs the console; scripts and -c lines use the
   * standard streams they were given. */
  if ((interactive && (shell.terminal.input == HANDLE_INVALID ||
      shell.terminal.output == HANDLE_INVALID)) || shell.launcher == HANDLE_INVALID || shell.memory == HANDLE_INVALID ||
      shell.boot == HANDLE_INVALID || shell.tmp == HANDLE_INVALID) {
    shell_error(&shell, "shell: Missing startup resource or filesystem root\n");
    return EXIT_FAILURE;
  }

  const char *session_wait = NULL;
  enum call_status policy_status = pyxis_environment_get(SESSION_WAIT_ENV, &session_wait);
  if (policy_status != CALL_OK && policy_status != CALL_NOT_FOUND) {
    shell_directory_error(&shell, "shell", "session policy", policy_status);
    return EXIT_FAILURE;
  }
  shell.session_wait = session_wait && !strcmp(session_wait, "1");

  uint64_t input_rights = 0;
  shell.interrupts = shell.clock != HANDLE_INVALID &&
      handle_rights(shell.terminal.input, &input_rights, NULL) == CALL_OK &&
      (input_rights & CONSOLE_RIGHT_INTERRUPT);

  int result = EXIT_FAILURE;
  struct shell_cwd_service cwd_service = {0};
  bool own_script = false;
  char *line = malloc(interactive ? SHELL_LINE_CAPACITY : SHELL_SCRIPT_LINE_MAX + 1);
  if (!line) {
    shell_directory_error(&shell, "shell", "command storage", CALL_NO_MEMORY);
    goto done;
  }
  retain_interpreter_image(&shell);
  enum call_status status = shell_environment_init(&shell);
  if (status != CALL_OK) {
    shell_directory_error(&shell, "shell", "initial environment", status);
    goto done;
  }
  status = shell_directory_init(&shell);
  if (status != CALL_OK) {
    shell_directory_error(&shell, "shell", "working directory", status);
    goto done;
  }

  /* Pin the startup command selection for containment only. A later shadowing
   * name or a manifest ID is not proof of the same held executable. */
  struct command_selection *command_shell = NULL;
  if (shell_open_image(&shell, "shell", &command_shell) == CALL_OK) {
    if (command_shell->bundle) {
      handle_copy_restricted(command_shell->image, FILE_RIGHT_READ, 0,
          &shell.command_shell_image);
    }
    command_selection_close(command_shell);
  }

  if (positional) {
    struct resource_result opened = {0};
    status = resource_open_path(shell.directory, argv[1], PROVIDER_INTENT_READ, 0, 0, &opened);
    if (status == CALL_OK) {
      status = resource_outcome_status(opened.info.description.outcome);
    }
    if (status == CALL_OK && opened.info.protocol != PROTOCOL_FILE) {
      status = CALL_WRONG_TYPE;
    }
    if (status == CALL_OK) {
      script = opened.resource;
      opened.resource = HANDLE_INVALID;
      own_script = true;
    }
    enum call_status closed = resource_result_close(&opened);
    if (status != CALL_OK || closed != CALL_OK) {
      shell_directory_error(&shell, "shell", argv[1], status != CALL_OK ? status : closed);
      goto done;
    }
  }
  if (command_line) {
    if (shell_script_group_open(&shell) != CALL_OK) {
      goto done;
    }
    shell.child_launcher = shell.script_group.launcher;
    result = run_command_line(&shell, argv[2], line);
    if (!shell_script_group_close(&shell)) {
      result = EXIT_FAILURE;
    }
    goto done;
  }
  if (script != HANDLE_INVALID) {
    if (shell_script_group_open(&shell) != CALL_OK) {
      goto done;
    }
    if (positional) {
      shell.child_launcher = shell.script_group.launcher;
    }
    result = shell_script(&shell, script, line);
    if (!shell_script_group_close(&shell)) {
      result = EXIT_FAILURE;
    }
    goto done;
  }
  if (!quiet_input && settings != HANDLE_INVALID) {
    shell_settings_load(&shell, settings);
    settings = HANDLE_INVALID;
  }
  if (!quiet_input) {
    shell_history_load(&shell);
  }
  handle_t cwd_bootstrap = bootstrap_take_capability("pane_cwd");
  if (cwd_bootstrap != HANDLE_INVALID) {
    pane_cwd_provider_init(&cwd_service.provider, shell.service, cwd_bootstrap, shell.clock);
    handle_close(cwd_bootstrap);
    cwd_service.line = (struct term_line_service){
      .receiver = cwd_service.provider.receiver, .ready = serve_cwd, .context = &cwd_service,
    };
  }

  for (;;) {
    status = bundle_jobs_reap(&shell.jobs, true);
    if (status != CALL_OK) {
      report_directory_error("shell", "job context cleanup", status);
      break;
    }
    struct term_line_result read = read_command(&shell, line, &cwd_service.line);
    if (read.status == TERM_LINE_EOF) {
      result = EXIT_SUCCESS;
      break;
    }
    if (read.status == TERM_LINE_CANCELLED) {
      continue;
    }
    if (read.status == TERM_LINE_INPUT_LOST) {
      if (fputs("shell: Input lost; please re-enter the command\n", stderr) == EOF) {
        break;
      }
      continue;
    }
    if (read.status == TERM_LINE_ERROR) {
      report_directory_error("shell", "terminal", read.error);
      break;
    }
    /* Preserve the submitted spelling in history. */
    if (read.recorded) {
      shell_history_save(&shell, line);
    }
    if (read.limit_reached) {
      if (fputs("shell: Line limit reached; command not executed\n", stderr) == EOF) {
        break;
      }
      if (!report_completion(&shell, events,
          (struct shell_outcome){TERMINAL_COMPLETION_REJECTED, 0, false})) {
        break;
      }
      continue;
    }
    const char *start = line;
    while (*start == ' ' || *start == '\t') {
      ++start;
    }
    if (!*start) {
      continue;
    }
    struct shell_outcome outcome;
    enum command_result command = shell_command(&shell, line, &outcome);
    if (command == COMMAND_FATAL) {
      break;
    }
    if (!report_completion(&shell, events, outcome)) {
      break;
    }
    if (command == COMMAND_EXIT) {
      result = shell.exit_status;
      break;
    }
  }

done:
  if (settings != HANDLE_INVALID) {
    handle_close(settings);
  }
  if (shell.command_shell_image != HANDLE_INVALID && handle_close(shell.command_shell_image) != 0) {
    shell_directory_error(&shell, "shell: cleanup", "shell command image", CALL_BAD_HANDLE);
    result = EXIT_FAILURE;
  }
  if (shell.interpreter_image != HANDLE_INVALID && handle_close(shell.interpreter_image) != 0) {
    shell_directory_error(&shell, "shell: cleanup", "interpreter image", CALL_BAD_HANDLE);
    result = EXIT_FAILURE;
  }
  if (own_script && handle_close(script) != 0) {
    shell_directory_error(&shell, "shell: cleanup", "script input", CALL_BAD_HANDLE);
    result = EXIT_FAILURE;
  }
  shell_environment_close(&shell);
  enum call_status jobs_closed = bundle_jobs_close(&shell.jobs);
  if (jobs_closed != CALL_OK) {
    shell_directory_error(&shell, "shell: cleanup", "job contexts", jobs_closed);
    result = EXIT_FAILURE;
  }
  pane_cwd_provider_close(&cwd_service.provider);
  shell_fat_close(&shell);
  shell_directory_close(&shell);
  shell_aliases_close(&shell);
  term_history_free(&shell.history);
  free(line);
  /* Cleanup reports failure, but cannot replace an explicitly requested nonzero
   * native status. A requested zero still becomes failure when cleanup fails. */
  if (shell.exit_requested && shell.exit_status != 0) {
    result = shell.exit_status;
  }
  /* Runtime exit releases the original startup grants, which may alias. */
  return result;
}
