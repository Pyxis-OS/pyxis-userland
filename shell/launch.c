#include "shell.h"
#include <abi/console.h>
#include <abi/memory.h>
#include <abi/display.h>
#include <abi/clock.h>
#include <abi/keyboard.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdlib.h>

enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode)
{
  bool session = mode == SHELL_SESSION;
  handle_t image;
  enum call_status status = shell_open_image(shell, arguments[0], &image);
  if (status != CALL_OK) {
    if (status == CALL_WRONG_TYPE) {
      return shell_error(shell, "shell: %s: Not a file\n", arguments[0]);
    }
    return shell_directory_error(shell, "shell", arguments[0], status);
  }

  enum { CHILD_INPUT, CHILD_OUTPUT, CHILD_MEMORY, CHILD_APP, CHILD_HOME, CHILD_DIRECTORY };
  bool has_host = shell->host != HANDLE_INVALID;
  bool has_keyboard = shell->keyboard != HANDLE_INVALID;
  bool has_clock = shell->clock != HANDLE_INVALID;
  bool has_display = shell->display != HANDLE_INVALID;
  size_t depth = shell->directory.count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - CHILD_DIRECTORY - 5) {
    handle_close(image);
    return shell_directory_error(shell, "shell", arguments[0], CALL_LIMIT);
  }
  size_t display_index = CHILD_DIRECTORY + depth;
  size_t clock_index = display_index + (has_display ? 1 : 0);
  size_t keyboard_index = clock_index + (has_clock ? 1 : 0);
  size_t host_index = keyboard_index + (has_keyboard ? 1 : 0);
  size_t launcher_index = host_index + (has_host ? 1 : 0);
  size_t grant_count = launcher_index + (session ? 1 : 0);
  struct launch_grant *grants = malloc(grant_count * sizeof(*grants));
  uint64_t *directories = malloc(depth * sizeof(*directories));
  if (!grants || (depth && !directories)) {
    free(directories);
    free(grants);
    handle_close(image);
    return shell_directory_error(shell, "shell", arguments[0], CALL_NO_MEMORY);
  }
  grants[CHILD_INPUT] = (struct launch_grant){shell->terminal.input, CONSOLE_RIGHT_READ};
  grants[CHILD_OUTPUT] = (struct launch_grant){shell->terminal.output, CONSOLE_RIGHT_WRITE};
  grants[CHILD_MEMORY] = (struct launch_grant){shell->memory, MEMORY_RIGHT_MANAGE};
  grants[CHILD_APP] = (struct launch_grant){shell->app, APP_DIRECTORY_RIGHTS};
  grants[CHILD_HOME] = (struct launch_grant){shell->home, HOME_DIRECTORY_RIGHTS};
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = CHILD_DIRECTORY + i;
    grants[CHILD_DIRECTORY + i] = (struct launch_grant){shell->directory.directories[i],
        shell->directory.directory_rights};
  }
  if (has_display) {
    grants[display_index] = (struct launch_grant){shell->display, DISPLAY_RIGHT_DRAW};
  }
  if (has_clock) {
    grants[clock_index] = (struct launch_grant){shell->clock, CLOCK_RIGHTS};
  }
  if (has_keyboard) {
    grants[keyboard_index] = (struct launch_grant){shell->keyboard, KEYBOARD_RIGHT_INPUT};
  }
  if (has_host) {
    grants[host_index] = (struct launch_grant){shell->host, APP_DIRECTORY_RIGHTS};
  }
  if (session) {
    grants[launcher_index] = (struct launch_grant){shell->launcher, LAUNCHER_RIGHT_LAUNCH};
  }
  struct launch_binding resources[7] = {
    {(uintptr_t)"input", CHILD_INPUT},
    {(uintptr_t)"output", CHILD_OUTPUT},
    {(uintptr_t)"memory", CHILD_MEMORY},
  };
  size_t resource_count = 3;
  if (has_display) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"display", display_index};
  }
  if (has_clock) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"clock", clock_index};
  }
  if (has_keyboard) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"keyboard", keyboard_index};
  }
  if (session) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"launcher", launcher_index};
  }
  struct launch_binding roots[3] = {
    {(uintptr_t)"app", CHILD_APP},
    {(uintptr_t)"home", CHILD_HOME},
  };
  size_t root_count = 2;
  if (has_host) {
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", host_index};
  }
  /* Mount authority belongs to init; only directory access crosses handoff. */
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = grant_count,
    .resources = (uintptr_t)resources,
    .resource_count = resource_count,
    .roots = (uintptr_t)roots,
    .root_count = root_count,
    .working_directories = (uintptr_t)directories,
    .working_directory_count = depth,
    .working_path = (uintptr_t)shell->working_path,
    .environment = (uintptr_t)startup_environment_variables(),
    .environment_count = startup_environment_count(),
    .argv = (uintptr_t)arguments,
    .argc = count,
  };
  handle_t child;
  status = program_launch(shell->launcher, &request, &child);
  free(directories);
  free(grants);
  bool closed_image = handle_close(image) == 0;
  if (status != CALL_OK) {
    enum command_result result = shell_directory_error(shell, "shell", arguments[0], status);
    return closed_image ? result : COMMAND_FATAL;
  }

  if (session) {
    /* The child owns copies of its grants. Closing the observer does not stop
     * it; never read input or resume the script after a successful handoff. */
    bool closed_child = handle_close(child) == 0;
    if (!closed_image || !closed_child) {
      shell_directory_error(shell, "shell: close", arguments[0], CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
    return COMMAND_EXIT;
  }

  /* No terminal reads until the child has stopped and its resources are gone. */
  struct process_result completion;
  status = process_wait(child, &completion);
  bool closed_child = handle_close(child) == 0;
  if (status != CALL_OK) {
    shell_directory_error(shell, "shell: wait", arguments[0], status);
    return COMMAND_FATAL;
  }
  if (!closed_image || !closed_child) {
    shell_directory_error(shell, "shell: close", arguments[0], CALL_BAD_HANDLE);
    return COMMAND_FATAL;
  }
  /* Preserve partial child output before any completion diagnostic. */
  status = term_fresh_line(&shell->terminal);
  if (status != CALL_OK) {
    shell_directory_error(shell, "shell", "terminal", status);
    return COMMAND_FATAL;
  }
  if (completion.kind == PROCESS_FAULTED) {
    return shell_error(shell, "shell: %s: Process faulted\n", arguments[0]);
  }
  if (completion.exit_status != 0) {
    return shell_error(shell, "shell: %s: Exited with status %jd\n", arguments[0],
        (intmax_t)completion.exit_status);
  }
  return COMMAND_OK;
}
