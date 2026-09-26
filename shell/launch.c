#include "shell.h"
#include <abi/console.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/display.h>
#include <abi/clock.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/pipe.h>
#include <abi/random.h>
#include <abi/net_config.h>
#include <abi/keyboard.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <handle.h>
#include <file.h>
#include <launcher.h>
#include <pipe.h>
#include <process.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>

enum { CHILD_OUTPUT, CHILD_MEMORY, CHILD_APP, CHILD_HOME, CHILD_DIRECTORY };

struct prepared_stage {
  handle_t image;
  handle_t redirected[STARTUP_STREAM_COUNT];
  struct launch_grant *grants;
  uint64_t *directories;
  struct launch_binding resources[15];
  struct launch_binding roots[3];
  struct launch_request request;
};

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool closed = handle_close(*handle) == 0;
  if (closed) {
    *handle = HANDLE_INVALID;
  }
  return closed;
}

static bool release_sources(struct prepared_stage *stages, size_t stage_count,
    struct pipe_create_reply *pipes)
{
  bool closed = true;
  for (size_t i = 0; i < stage_count; ++i) {
    if (!close_handle(&stages[i].image)) {
      closed = false;
    }
    for (size_t stream = 0; stream < STARTUP_STREAM_COUNT; ++stream) {
      if (!close_handle(&stages[i].redirected[stream])) {
        closed = false;
      }
    }
  }
  for (size_t i = 0; i + 1 < stage_count; ++i) {
    if (!close_handle(&pipes[i].reader)) {
      closed = false;
    }
    if (!close_handle(&pipes[i].writer)) {
      closed = false;
    }
  }
  return closed;
}

static void free_preparation(struct prepared_stage *stages, size_t stage_count)
{
  for (size_t i = 0; i < stage_count; ++i) {
    free(stages[i].directories);
    free(stages[i].grants);
  }
  free(stages);
}

static enum command_result launch_error(struct shell *shell, const struct shell_stage *stages,
    size_t stage_count, size_t stage, const char *operation, const char *path,
    enum call_status status)
{
  if (stage_count > 1 && stage < stage_count) {
    return shell_error(shell, "shell: stage %zu (%s): %s %s failed (status %u)\n",
        stage + 1, stages[stage].arguments[0], operation, path, status);
  }
  if (stage_count == 1 && !strcmp(operation, "shell: open") &&
      status == CALL_WRONG_TYPE) {
    return shell_error(shell, "shell: %s: Not a file\n", path);
  }
  if (stage_count == 1 && (!strcmp(operation, "shell: open") ||
      !strcmp(operation, "shell: prepare") || !strcmp(operation, "shell: launch"))) {
    operation = "shell";
  }
  return shell_directory_error(shell, operation, path, status);
}

static enum call_status prepare_stage(struct shell *shell, struct prepared_stage *prepared,
    const struct shell_stage *stage, enum shell_launch_mode mode, bool named_input,
    const struct startup_stream streams[STARTUP_STREAM_COUNT])
{
  bool session = mode == SHELL_SESSION;
  bool has_host = shell->host != HANDLE_INVALID;
  bool has_keyboard = named_input && shell->keyboard != HANDLE_INVALID;
  bool has_profile = shell->profile != HANDLE_INVALID;
  bool has_space = session && shell->space != HANDLE_INVALID;
  bool has_net_config = session && shell->net_config != HANDLE_INVALID;
  bool has_random = shell->random != HANDLE_INVALID;
  bool has_tcp = shell->tcp != HANDLE_INVALID;
  bool has_pipe = session && shell->pipe != HANDLE_INVALID;
  bool has_udp = shell->udp != HANDLE_INVALID;
  bool has_echo = shell->echo != HANDLE_INVALID;
  bool has_clock = shell->clock != HANDLE_INVALID;
  bool has_display = shell->display != HANDLE_INVALID;
  size_t depth = shell->directory.count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - CHILD_DIRECTORY - 14 - STARTUP_STREAM_COUNT) {
    return CALL_LIMIT;
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
  size_t pipe_index = net_config_index + (has_net_config ? 1 : 0);
  size_t space_index = pipe_index + (has_pipe ? 1 : 0);
  size_t profile_index = space_index + (has_space ? 1 : 0);
  size_t input_index = profile_index + (has_profile ? 1 : 0);
  size_t grant_count = input_index + (named_input ? 1 : 0);
  prepared->grants = malloc((grant_count + STARTUP_STREAM_COUNT) * sizeof(*prepared->grants));
  prepared->directories = depth ? malloc(depth * sizeof(*prepared->directories)) : NULL;
  if (!prepared->grants || (depth && !prepared->directories)) {
    return CALL_NO_MEMORY;
  }
  struct launch_grant *grants = prepared->grants;
  uint64_t *directories = prepared->directories;
  if (named_input) {
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
  if (has_pipe) {
    grants[pipe_index] = (struct launch_grant){shell->pipe, PIPE_SERVICE_RIGHT_CREATE};
  }
  if (has_space) {
    grants[space_index] = (struct launch_grant){shell->space, SPACE_RIGHT_SET_TITLE};
  }
  if (has_profile) {
    grants[profile_index] = (struct launch_grant){shell->profile, PROFILE_RIGHT_MEMORY};
  }

  struct launch_binding *resources = prepared->resources;
  resources[0] = (struct launch_binding){(uintptr_t)"output", CHILD_OUTPUT};
  resources[1] = (struct launch_binding){(uintptr_t)"memory", CHILD_MEMORY};
  size_t resource_count = 2;
  if (named_input) {
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
  if (has_pipe) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"pipe", pipe_index};
  }
  if (has_space) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"space", space_index};
  }
  if (has_profile) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"profile", profile_index};
  }
  struct launch_binding *roots = prepared->roots;
  roots[0] = (struct launch_binding){(uintptr_t)"app", CHILD_APP};
  roots[1] = (struct launch_binding){(uintptr_t)"home", CHILD_HOME};
  size_t root_count = 2;
  if (has_host) {
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", host_index};
  }
  /* Root names and display paths do not determine delegated authority. */
  for (size_t i = 0; i < root_count; ++i) {
    struct launch_grant *grant = &grants[roots[i].grant];
    enum call_status status = handle_rights(grant->source, &grant->rights);
    if (status != CALL_OK) {
      return status;
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    struct launch_grant *grant = &grants[directories[i]];
    enum call_status status = handle_rights(grant->source, &grant->rights);
    if (status != CALL_OK) {
      return status;
    }
  }

  struct launch_request *request = &prepared->request;
  *request = (struct launch_request){
    .image = prepared->image,
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
    .argv = (uintptr_t)stage->arguments,
    .argc = stage->count,
  };
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    struct startup_stream stream = streams[i];
    if (stream.protocol == STARTUP_STREAM_NONE) {
      continue;
    }
    bool input = i == STARTUP_STDIN;
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        stream.protocol == PROTOCOL_PIPE ?
        (input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE) :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    request->streams[i] = (struct launch_stream){stream.protocol, request->grant_count};
    grants[request->grant_count++] = (struct launch_grant){stream.handle, rights};
  }
  return CALL_OK;
}

static enum command_result launch_stages(struct shell *shell, const struct shell_stage *stages,
    size_t stage_count, enum shell_launch_mode mode)
{
  struct prepared_stage *prepared = calloc(stage_count, sizeof(*prepared));
  struct launch_request *requests = malloc(stage_count * sizeof(*requests));
  if (!prepared || !requests) {
    free(requests);
    free(prepared);
    return shell_directory_error(shell, "shell", stages[0].arguments[0], CALL_NO_MEMORY);
  }
  struct pipe_create_reply pipes[LAUNCH_BATCH_MAX - 1] = {0};
  struct startup_stream streams[LAUNCH_BATCH_MAX][STARTUP_STREAM_COUNT] = {0};
  const char *operation = "shell: open";
  const char *path = stages[0].arguments[0];
  size_t failed_stage = 0;
  enum call_status status = CALL_OK;
  handle_t children[LAUNCH_BATCH_MAX] = {0};
  uint64_t failed_index = LAUNCH_NO_STAGE;
  bool cleanup_failure = false;

  /* Open every image before opening or creating any redirect target. */
  for (size_t i = 0; i < stage_count; ++i) {
    failed_stage = i;
    path = stages[i].arguments[0];
    status = shell_open_image(shell, path, &prepared[i].image);
    if (status != CALL_OK) {
      goto failed;
    }
  }
  for (size_t i = 0; i < stage_count; ++i) {
    for (size_t stream = 0; stream < STARTUP_STREAM_COUNT; ++stream) {
      streams[i][stream] = startup_stream(stream);
    }
    if (mode == SHELL_BACKGROUND) {
      streams[i][STARTUP_STDIN] = (struct startup_stream){0};
    }
    for (size_t j = 0; j < stages[i].redirection_count; ++j) {
      const struct shell_redirection *redirect = &stages[i].redirections[j];
      failed_stage = i;
      operation = "shell: redirect";
      path = redirect->path;
      status = shell_open_redirect(shell, path, redirect->stream == STARTUP_STDIN,
          &prepared[i].redirected[redirect->stream]);
      if (status != CALL_OK) {
        goto failed;
      }
      streams[i][redirect->stream] =
          (struct startup_stream){PROTOCOL_FILE, prepared[i].redirected[redirect->stream]};
    }
  }

  for (size_t i = 0; i + 1 < stage_count; ++i) {
    bool writer = prepared[i].redirected[STARTUP_STDOUT] == HANDLE_INVALID;
    bool reader = prepared[i + 1].redirected[STARTUP_STDIN] == HANDLE_INVALID;
    if (!writer && !reader) {
      continue;
    }
    failed_stage = i;
    operation = "shell: pipe";
    path = stages[i].arguments[0];
    status = pipe_create(shell->pipe, &pipes[i]);
    if (status != CALL_OK) {
      goto failed;
    }
    if (writer) {
      streams[i][STARTUP_STDOUT] = (struct startup_stream){PROTOCOL_PIPE, pipes[i].writer};
    } else if (!close_handle(&pipes[i].writer)) {
      status = CALL_BAD_HANDLE;
      cleanup_failure = true;
      goto failed;
    }
    if (reader) {
      streams[i + 1][STARTUP_STDIN] =
          (struct startup_stream){PROTOCOL_PIPE, pipes[i].reader};
    } else if (!close_handle(&pipes[i].reader)) {
      status = CALL_BAD_HANDLE;
      cleanup_failure = true;
      goto failed;
    }
  }

  /* A redirected pipe side is absent from every grant, so its peer sees
   * EOF or a broken reader once the remaining owned endpoint closes. */
  for (size_t i = 0; i < stage_count; ++i) {
    failed_stage = i;
    operation = "shell: prepare";
    path = stages[i].arguments[0];
    bool named_input = mode != SHELL_BACKGROUND && i == 0 &&
        streams[i][STARTUP_STDIN].protocol == PROTOCOL_CONSOLE;
    status = prepare_stage(shell, &prepared[i], &stages[i], mode, named_input, streams[i]);
    if (status != CALL_OK) {
      goto failed;
    }
    requests[i] = prepared[i].request;
  }

  /* All opens, pipe creation and grant preparation precede any truncation.
   * Created files and truncated contents cannot be rolled back. */
  for (size_t i = 0; i < stage_count; ++i) {
    for (size_t j = 0; j < stages[i].redirection_count; ++j) {
      const struct shell_redirection *redirect = &stages[i].redirections[j];
      if (redirect->stream == STARTUP_STDIN) {
        continue;
      }
      failed_stage = i;
      operation = "shell: truncate";
      path = redirect->path;
      status = file_resize(prepared[i].redirected[redirect->stream], 0);
      if (status != CALL_OK) {
        goto failed;
      }
    }
  }

  operation = "shell: launch";
  path = stages[0].arguments[0];
  if (stage_count == 1) {
    status = program_launch(shell->launcher, &requests[0], &children[0]);
  } else {
    status = program_launch_batch(shell->launcher, requests, stage_count, children,
        &failed_index);
  }
  if (status != CALL_OK) {
    failed_stage = failed_index == LAUNCH_NO_STAGE ? stage_count : failed_index;
    if (failed_stage < stage_count) {
      path = stages[failed_stage].arguments[0];
    }
    goto failed;
  }

  /* Release all source endpoints before waiting. Holding one writer here
   * could prevent EOF, and holding one reader could prevent EPIPE. */
  bool closed_sources = release_sources(prepared, stage_count, pipes);
  free(requests);
  free_preparation(prepared, stage_count);
  if (!closed_sources) {
    for (size_t i = 0; i < stage_count; ++i) {
      close_handle(&children[i]);
    }
    shell_directory_error(shell, "shell: close", stages[0].arguments[0], CALL_BAD_HANDLE);
    return COMMAND_FATAL;
  }
  if (mode == SHELL_SESSION || mode == SHELL_BACKGROUND) {
    bool closed = close_handle(&children[0]);
    if (!closed) {
      shell_directory_error(shell, "shell: close", stages[0].arguments[0], CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
    return mode == SHELL_SESSION ? COMMAND_EXIT : COMMAND_OK;
  }

  struct process_result completion[LAUNCH_BATCH_MAX] = {0};
  for (size_t i = 0; i < stage_count; ++i) {
    status = process_wait(children[i], &completion[i]);
    bool closed = close_handle(&children[i]);
    if (status != CALL_OK || !closed) {
      for (size_t j = i + 1; j < stage_count; ++j) {
        close_handle(&children[j]);
      }
      launch_error(shell, stages, stage_count, i,
          status != CALL_OK ? "shell: wait" : "shell: close",
          stages[i].arguments[0], status != CALL_OK ? status : CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
  }
  status = term_fresh_line(&shell->terminal);
  if (status != CALL_OK) {
    shell_directory_error(shell, "shell", "terminal", status);
    return COMMAND_FATAL;
  }
  enum command_result result = COMMAND_OK;
  for (size_t i = 0; i < stage_count; ++i) {
    enum command_result diagnostic = COMMAND_OK;
    if (completion[i].kind == PROCESS_FAULTED) {
      diagnostic = stage_count == 1 ?
          shell_error(shell, "shell: %s: Process faulted\n", stages[i].arguments[0]) :
          shell_error(shell, "shell: stage %zu (%s): Process faulted\n",
              i + 1, stages[i].arguments[0]);
    } else if (completion[i].exit_status != 0) {
      diagnostic = stage_count == 1 ?
          shell_error(shell, "shell: %s: Exited with status %jd\n",
              stages[i].arguments[0], (intmax_t)completion[i].exit_status) :
          shell_error(shell, "shell: stage %zu (%s): Exited with status %jd\n",
              i + 1, stages[i].arguments[0], (intmax_t)completion[i].exit_status);
    }
    if (diagnostic == COMMAND_FATAL) {
      return COMMAND_FATAL;
    }
    if (i + 1 == stage_count) {
      result = diagnostic;
    }
  }
  return result;

failed:
  closed_sources = release_sources(prepared, stage_count, pipes);
  free(requests);
  free_preparation(prepared, stage_count);
  enum command_result error = launch_error(shell, stages, stage_count, failed_stage,
      operation, path, status);
  if (!closed_sources || cleanup_failure || status == CALL_OUTCOME_UNKNOWN) {
    return COMMAND_FATAL;
  }
  return error;
}

enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode, const struct shell_redirection *redirections,
    size_t redirection_count)
{
  struct shell_stage stage = {.arguments = arguments, .count = count,
      .redirection_count = redirection_count};
  for (size_t i = 0; i < redirection_count; ++i) {
    stage.redirections[i] = redirections[i];
  }
  return launch_stages(shell, &stage, 1, mode);
}

enum command_result shell_launch_pipeline(struct shell *shell,
    const struct shell_command_line *command)
{
  if (command->stage_count < 2 || command->stage_count > LAUNCH_BATCH_MAX ||
      command->background) {
    return shell_error(shell, "shell: Invalid foreground pipeline\n");
  }
  return launch_stages(shell, command->stages, command->stage_count, SHELL_FOREGROUND);
}
