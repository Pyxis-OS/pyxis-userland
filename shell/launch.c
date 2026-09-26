#include "shell.h"
#include <abi/console.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/display.h>
#include <abi/clock.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/random.h>
#include <abi/net_config.h>
#include <abi/keyboard.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <handle.h>
#include <file.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdlib.h>

enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode, const struct shell_redirection *redirections,
    size_t redirection_count)
{
  bool session = mode == SHELL_SESSION;
  bool background = mode == SHELL_BACKGROUND;
  bool terminal_input = !background;
  for (size_t i = 0; i < redirection_count; ++i) {
    if (redirections[i].stream == STARTUP_STDIN) {
      terminal_input = false;
    }
  }
  handle_t redirected[STARTUP_STREAM_COUNT] = {0};
  const char *failure_path = arguments[0];
  const char *failure_operation = "shell";
  handle_t image;
  enum call_status status = shell_open_image(shell, arguments[0], &image);
  if (status != CALL_OK) {
    if (status == CALL_WRONG_TYPE) {
      return shell_error(shell, "shell: %s: Not a file\n", arguments[0]);
    }
    return shell_directory_error(shell, "shell", arguments[0], status);
  }

  enum { CHILD_OUTPUT, CHILD_MEMORY, CHILD_APP, CHILD_HOME, CHILD_DIRECTORY };
  bool has_host = shell->host != HANDLE_INVALID;
  bool has_keyboard = terminal_input && shell->keyboard != HANDLE_INVALID;
  bool has_profile = shell->profile != HANDLE_INVALID;
  bool has_space = session && shell->space != HANDLE_INVALID;
  bool has_net_config = session && shell->net_config != HANDLE_INVALID;
  bool has_random = shell->random != HANDLE_INVALID;
  bool has_tcp = shell->tcp != HANDLE_INVALID;
  bool has_udp = shell->udp != HANDLE_INVALID;
  bool has_echo = shell->echo != HANDLE_INVALID;
  bool has_clock = shell->clock != HANDLE_INVALID;
  bool has_display = shell->display != HANDLE_INVALID;
  size_t depth = shell->directory.count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - CHILD_DIRECTORY - 13 - STARTUP_STREAM_COUNT) {
    handle_close(image);
    return shell_directory_error(shell, "shell", arguments[0], CALL_LIMIT);
  }
  size_t display_index = CHILD_DIRECTORY + depth;
  size_t clock_index = display_index + (has_display ? 1 : 0);
  size_t echo_index = clock_index + (has_clock ? 1 : 0);
  size_t udp_index = echo_index + (has_echo ? 1 : 0);
  size_t tcp_index = udp_index + (has_udp ? 1 : 0);
  size_t random_index = tcp_index + (has_tcp ? 1 : 0);
  size_t keyboard_index = random_index + (has_random ? 1 : 0);
  size_t host_index = keyboard_index + (has_keyboard ? 1 : 0);
  size_t launcher_index = host_index + (has_host ? 1 : 0);
  size_t net_config_index = launcher_index + (session ? 1 : 0);
  size_t space_index = net_config_index + (has_net_config ? 1 : 0);
  size_t profile_index = space_index + (has_space ? 1 : 0);
  size_t input_index = profile_index + (has_profile ? 1 : 0);
  size_t grant_count = input_index + (terminal_input ? 1 : 0);
  struct launch_grant *grants = malloc((grant_count + STARTUP_STREAM_COUNT) * sizeof(*grants));
  uint64_t *directories = malloc(depth * sizeof(*directories));
  if (!grants || (depth && !directories)) {
    free(directories);
    free(grants);
    handle_close(image);
    return shell_directory_error(shell, "shell", arguments[0], CALL_NO_MEMORY);
  }
  if (terminal_input) {
    grants[input_index] = (struct launch_grant){shell->terminal.input, CONSOLE_RIGHT_READ};
  }
  grants[CHILD_OUTPUT] = (struct launch_grant){shell->terminal.output, CONSOLE_RIGHT_WRITE};
  grants[CHILD_MEMORY] = (struct launch_grant){shell->memory, MEMORY_RIGHT_MANAGE};
  grants[CHILD_APP] = (struct launch_grant){shell->app, 0};
  grants[CHILD_HOME] = (struct launch_grant){shell->home, 0};
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = CHILD_DIRECTORY + i;
    grants[CHILD_DIRECTORY + i] = (struct launch_grant){shell->directory.directories[i], 0};
  }
  if (has_display) {
    grants[display_index] = (struct launch_grant){shell->display, DISPLAY_RIGHT_DRAW};
  }
  if (has_clock) {
    grants[clock_index] = (struct launch_grant){shell->clock, CLOCK_RIGHTS};
  }
  if (has_echo) {
    grants[echo_index] = (struct launch_grant){shell->echo, ECHO_RIGHT_SEND};
  }
  if (has_udp) {
    grants[udp_index] = (struct launch_grant){shell->udp, UDP_SERVICE_RIGHT_OPEN};
  }
  if (has_tcp) {
    grants[tcp_index] = (struct launch_grant){shell->tcp, TCP_SERVICE_RIGHT_CONNECT};
  }
  if (has_random) {
    grants[random_index] = (struct launch_grant){shell->random, RANDOM_RIGHT_READ};
  }
  if (has_keyboard) {
    grants[keyboard_index] = (struct launch_grant){shell->keyboard, KEYBOARD_RIGHT_INPUT};
  }
  if (has_host) {
    grants[host_index] = (struct launch_grant){shell->host, 0};
  }
  if (session) {
    grants[launcher_index] = (struct launch_grant){shell->launcher, LAUNCHER_RIGHT_LAUNCH};
  }
  if (has_net_config) {
    grants[net_config_index] = (struct launch_grant){shell->net_config, NET_CONFIG_RIGHTS};
  }
  if (has_space) {
    grants[space_index] = (struct launch_grant){shell->space, SPACE_RIGHT_SET_TITLE};
  }
  if (has_profile) {
    grants[profile_index] = (struct launch_grant){shell->profile, PROFILE_RIGHT_MEMORY};
  }
  struct launch_binding resources[14] = {
    {(uintptr_t)"output", CHILD_OUTPUT},
    {(uintptr_t)"memory", CHILD_MEMORY},
  };
  size_t resource_count = 2;
  if (terminal_input) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"input", input_index};
  }
  if (has_display) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"display", display_index};
  }
  if (has_clock) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"clock", clock_index};
  }
  if (has_echo) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"echo", echo_index};
  }
  if (has_udp) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"udp", udp_index};
  }
  if (has_tcp) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"tcp", tcp_index};
  }
  if (has_random) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"random", random_index};
  }
  if (has_keyboard) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"keyboard", keyboard_index};
  }
  if (session) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"launcher", launcher_index};
  }
  if (has_net_config) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"net_config", net_config_index};
  }
  if (has_space) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"space", space_index};
  }
  if (has_profile) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"profile", profile_index};
  }
  struct launch_binding roots[3] = {
    {(uintptr_t)"app", CHILD_APP},
    {(uintptr_t)"home", CHILD_HOME},
  };
  size_t root_count = 2;
  if (has_host) {
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", host_index};
  }

  /* Root names and display paths do not determine delegated authority. */
  for (size_t i = 0; i < root_count; ++i) {
    struct launch_grant *grant = &grants[roots[i].grant];
    status = handle_rights(grant->source, &grant->rights);
    if (status != CALL_OK) {
      goto release_launch;
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    struct launch_grant *grant = &grants[directories[i]];
    status = handle_rights(grant->source, &grant->rights);
    if (status != CALL_OK) {
      goto release_launch;
    }
  }

  for (size_t i = 0; i < redirection_count; ++i) {
    const struct shell_redirection *redirect = &redirections[i];
    status = shell_open_redirect(shell, redirect->path, redirect->stream == STARTUP_STDIN,
        &redirected[redirect->stream]);
    if (status != CALL_OK) {
      failure_path = redirect->path;
      failure_operation = "shell: redirect";
      goto release_launch;
    }
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
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    struct startup_stream stream = redirected[i] != HANDLE_INVALID ?
        (struct startup_stream){PROTOCOL_FILE, redirected[i]} : startup_stream(i);
    if (stream.protocol == STARTUP_STREAM_NONE || (background && i == STARTUP_STDIN)) {
      continue;
    }
    bool input = i == STARTUP_STDIN;
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    request.streams[i] = (struct launch_stream){stream.protocol, request.grant_count};
    grants[request.grant_count++] = (struct launch_grant){stream.handle, rights};
  }
  /* All target opens succeeded. From here, failure may leave truncated files;
   * no path reopening or rollback can restore their previous contents. */
  for (size_t i = 0; i < redirection_count; ++i) {
    const struct shell_redirection *redirect = &redirections[i];
    if (redirect->stream == STARTUP_STDIN) {
      continue;
    }
    status = file_resize(redirected[redirect->stream], 0);
    if (status != CALL_OK) {
      failure_path = redirect->path;
      failure_operation = "shell: truncate";
      goto release_launch;
    }
  }
  handle_t child;
  status = program_launch(shell->launcher, &request, &child);

release_launch:
  free(directories);
  free(grants);
  bool closed_sources = handle_close(image) == 0;
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    if (redirected[i] != HANDLE_INVALID && handle_close(redirected[i]) != 0) {
      closed_sources = false;
    }
  }
  if (status != CALL_OK) {
    enum command_result result = shell_directory_error(shell, failure_operation, failure_path, status);
    return closed_sources ? result : COMMAND_FATAL;
  }

  if (session || background) {
    /* The child owns copies of its grants. Closing the observer does not stop
     * it. Background children have no input grants; session handoff ends us. */
    bool closed_child = handle_close(child) == 0;
    if (!closed_sources || !closed_child) {
      shell_directory_error(shell, "shell: close", arguments[0], CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
    return session ? COMMAND_EXIT : COMMAND_OK;
  }

  /* No terminal reads until the child has stopped and its resources are gone. */
  struct process_result completion;
  status = process_wait(child, &completion);
  bool closed_child = handle_close(child) == 0;
  if (status != CALL_OK) {
    shell_directory_error(shell, "shell: wait", arguments[0], status);
    return COMMAND_FATAL;
  }
  if (!closed_sources || !closed_child) {
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
