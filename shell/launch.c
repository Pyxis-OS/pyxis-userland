#include "shell.h"
#include "../common/directory.h"
#include <abi/console.h>
#include <abi/memory.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>

bool shell_launch(struct shell *shell, char **arguments, size_t count)
{
  handle_t image;
  enum call_status status = shell_open_image(shell, arguments[0], &image);
  if (status != CALL_OK) {
    if (status == CALL_WRONG_TYPE) {
      return fprintf(stderr, "shell: %s: Not a file\n", arguments[0]) >= 0;
    }
    return report_directory_error("shell", arguments[0], status) >= 0;
  }

  enum { CHILD_INPUT, CHILD_OUTPUT, CHILD_MEMORY, CHILD_APP, CHILD_HOME, CHILD_DIRECTORY };
  size_t depth = shell->directory.count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - CHILD_DIRECTORY) {
    handle_close(image);
    return report_directory_error("shell", arguments[0], CALL_LIMIT) >= 0;
  }
  size_t grant_count = CHILD_DIRECTORY + depth;
  struct launch_grant *grants = malloc(grant_count * sizeof(*grants));
  uint64_t *directories = malloc(depth * sizeof(*directories));
  if (!grants || (depth && !directories)) {
    free(directories);
    free(grants);
    handle_close(image);
    return report_directory_error("shell", arguments[0], CALL_NO_MEMORY) >= 0;
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
  struct launch_binding resources[] = {
    {(uintptr_t)"input", CHILD_INPUT},
    {(uintptr_t)"output", CHILD_OUTPUT},
    {(uintptr_t)"memory", CHILD_MEMORY},
  };
  struct launch_binding roots[] = {
    {(uintptr_t)"app", CHILD_APP},
    {(uintptr_t)"home", CHILD_HOME},
  };
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = grant_count,
    .resources = (uintptr_t)resources,
    .resource_count = sizeof(resources) / sizeof(resources[0]),
    .roots = (uintptr_t)roots,
    .root_count = sizeof(roots) / sizeof(roots[0]),
    .working_directories = (uintptr_t)directories,
    .working_directory_count = depth,
    /* The current chain is authoritative. Do not forward the stale startup
     * display path after cd; a displayed current path is not maintained yet. */
    .environment = (uintptr_t)startup_environment_variables(),
    .environment_count = startup_environment_count(),
    .argv = (uintptr_t)arguments,
    .argc = count,
  };
  handle_t child;
  status = launcher_launch(shell->launcher, &request, &child);
  free(directories);
  free(grants);
  bool closed_image = handle_close(image) == 0;
  if (status != CALL_OK) {
    return report_directory_error("shell", arguments[0], status) >= 0 && closed_image;
  }

  /* No terminal reads until the child has stopped and its resources are gone. */
  struct process_result completion;
  status = process_wait(child, &completion);
  bool closed_child = handle_close(child) == 0;
  if (status != CALL_OK) {
    report_directory_error("shell: wait", arguments[0], status);
    return false;
  }
  if (!closed_image || !closed_child) {
    report_directory_error("shell: close", arguments[0], CALL_BAD_HANDLE);
    return false;
  }
  /* Preserve partial child output before any completion diagnostic. */
  if (term_fresh_line(&shell->terminal) != CALL_OK) {
    return false;
  }
  if (completion.kind == PROCESS_FAULTED) {
    return fprintf(stderr, "shell: %s: Process faulted\n", arguments[0]) >= 0;
  }
  if (completion.exit_status != 0) {
    return fprintf(stderr, "shell: %s: Exited with status %jd\n", arguments[0],
        (intmax_t)completion.exit_status) >= 0;
  }
  return true;
}
