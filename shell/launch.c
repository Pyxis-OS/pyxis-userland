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
#include <abi/endpoint.h>
#include <abi/namespace.h>
#include <abi/random.h>
#include <abi/net_config.h>
#include <abi/keyboard.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <handle.h>
#include <endpoint.h>
#include <file.h>
#include <launcher.h>
#include <namespace.h>
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
  struct launch_binding resources[18];
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
    const struct startup_stream streams[STARTUP_STREAM_COUNT], handle_t publication)
{
  bool session = mode == SHELL_SESSION;
  bool provider = mode == SHELL_SERVICE;
  bool has_host = shell->host != HANDLE_INVALID;
  bool has_keyboard = named_input && shell->keyboard != HANDLE_INVALID;
  bool has_profile = shell->profile != HANDLE_INVALID;
  bool has_space = session && shell->space != HANDLE_INVALID;
  bool has_net_config = session && shell->net_config != HANDLE_INVALID;
  bool has_random = shell->random != HANDLE_INVALID;
  bool has_tcp = shell->tcp != HANDLE_INVALID;
  bool has_pipe = session && shell->pipe != HANDLE_INVALID;
  bool has_service = (session || provider) && shell->service != HANDLE_INVALID;
  bool has_namespace_service = session && shell->namespace_service != HANDLE_INVALID;
  bool has_namespace = !provider && shell->namespace != HANDLE_INVALID;
  bool has_udp = shell->udp != HANDLE_INVALID;
  bool has_echo = shell->echo != HANDLE_INVALID;
  bool has_clock = shell->clock != HANDLE_INVALID;
  bool has_display = shell->display != HANDLE_INVALID;
  size_t depth = shell->directory.count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - CHILD_DIRECTORY - 18 - STARTUP_STREAM_COUNT) {
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
  size_t service_index = pipe_index + (has_pipe ? 1 : 0);
  size_t space_index = service_index + (has_service ? 1 : 0);
  size_t profile_index = space_index + (has_space ? 1 : 0);
  size_t input_index = profile_index + (has_profile ? 1 : 0);
  size_t namespace_index = input_index + (named_input ? 1 : 0);
  size_t publication_index = namespace_index + (has_namespace ? 1 : 0);
  size_t namespace_service_index = publication_index + (provider ? 1 : 0);
  size_t grant_count = namespace_service_index + (has_namespace_service ? 1 : 0);
  prepared->grants = malloc((grant_count + STARTUP_STREAM_COUNT) * sizeof(*prepared->grants));
  prepared->directories = depth ? malloc(depth * sizeof(*prepared->directories)) : NULL;
  if (!prepared->grants || (depth && !prepared->directories)) {
    return CALL_NO_MEMORY;
  }
  struct launch_grant *grants = prepared->grants;
  uint64_t *directories = prepared->directories;
  if (named_input) {
    grants[input_index] = (struct launch_grant){shell->terminal.input, CONSOLE_RIGHT_READ, 0};
  }
  grants[CHILD_OUTPUT] = (struct launch_grant){shell->terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[CHILD_MEMORY] = (struct launch_grant){shell->memory, MEMORY_RIGHT_MANAGE, 0};
  grants[CHILD_APP] = (struct launch_grant){shell->app, 0, 0};
  grants[CHILD_HOME] = (struct launch_grant){shell->home, 0, 0};
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = CHILD_DIRECTORY + i;
    grants[CHILD_DIRECTORY + i] = (struct launch_grant){shell->directory.directories[i], 0, 0};
  }
  if (has_display) {
    grants[display_index] = (struct launch_grant){shell->display, DISPLAY_RIGHT_DRAW, 0};
  }
  if (has_clock) {
    grants[clock_index] = (struct launch_grant){shell->clock, CLOCK_RIGHTS, 0};
  }
  if (has_echo) {
    grants[echo_index] = (struct launch_grant){shell->echo, ECHO_RIGHT_SEND, 0};
  }
  if (has_udp) {
    grants[udp_index] = (struct launch_grant){shell->udp, UDP_SERVICE_RIGHT_OPEN, 0};
  }
  if (has_tcp) {
    grants[tcp_index] = (struct launch_grant){shell->tcp, TCP_SERVICE_RIGHT_CONNECT, 0};
  }
  if (has_random) {
    grants[random_index] = (struct launch_grant){shell->random, RANDOM_RIGHT_READ, 0};
  }
  if (has_keyboard) {
    grants[keyboard_index] = (struct launch_grant){shell->keyboard, KEYBOARD_RIGHT_INPUT, 0};
  }
  if (has_host) {
    grants[host_index] = (struct launch_grant){shell->host, 0, 0};
  }
  if (session) {
    grants[launcher_index] = (struct launch_grant){shell->launcher, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  if (has_net_config) {
    grants[net_config_index] = (struct launch_grant){shell->net_config, NET_CONFIG_RIGHTS, 0};
  }
  if (has_pipe) {
    grants[pipe_index] = (struct launch_grant){shell->pipe, PIPE_SERVICE_RIGHT_CREATE, 0};
  }
  if (has_service) {
    grants[service_index] = (struct launch_grant){shell->service, ENDPOINT_SERVICE_RIGHT_CREATE, 0};
  }
  if (has_space) {
    grants[space_index] = (struct launch_grant){shell->space, SPACE_RIGHT_SET_TITLE, 0};
  }
  if (has_profile) {
    grants[profile_index] = (struct launch_grant){shell->profile, PROFILE_RIGHT_MEMORY, 0};
  }
  if (has_namespace) {
    uint64_t rights, transport;
    enum call_status status = handle_rights(shell->namespace, &rights, &transport);
    if (status != CALL_OK) {
      return status;
    }
    grants[namespace_index] = (struct launch_grant){shell->namespace,
        session ? rights : NAMESPACE_RIGHT_LOOKUP, transport};
  }
  if (provider) {
    grants[publication_index] = (struct launch_grant){publication,
        0, HANDLE_TRANSPORT_CALL};
  }
  if (has_namespace_service) {
    grants[namespace_service_index] = (struct launch_grant){shell->namespace_service,
        NAMESPACE_SERVICE_RIGHT_CREATE, 0};
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
  if (has_service) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"service", service_index};
  }
  if (has_space) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"space", space_index};
  }
  if (has_profile) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"profile", profile_index};
  }
  if (provider) {
    resources[resource_count++] = (struct launch_binding){
        (uintptr_t)"publication", publication_index};
  }
  if (has_namespace_service) {
    resources[resource_count++] = (struct launch_binding){
        (uintptr_t)"namespace_service", namespace_service_index};
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
    enum call_status status = handle_rights(grant->source, &grant->rights,
        &grant->transport);
    if (status != CALL_OK) {
      return status;
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    struct launch_grant *grant = &grants[directories[i]];
    enum call_status status = handle_rights(grant->source, &grant->rights,
        &grant->transport);
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
    .namespace_grant = has_namespace ? namespace_index + 1 : 0,
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
    grants[request->grant_count++] = (struct launch_grant){stream.handle, rights, 0};
  }
  return CALL_OK;
}

static enum command_result launch_stages(struct shell *shell, const struct shell_stage *stages,
    size_t stage_count, enum shell_launch_mode mode, const char *service_name, bool replace)
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
  struct endpoint_create_reply publication = {0};

  if (mode == SHELL_SERVICE) {
    status = endpoint_create(shell->service, &publication);
    if (status != CALL_OK) {
      operation = "shell: publication endpoint";
      goto failed;
    }
  }

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
    if (mode == SHELL_BACKGROUND || mode == SHELL_SERVICE) {
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
    bool named_input = mode != SHELL_BACKGROUND && mode != SHELL_SERVICE && i == 0 &&
        streams[i][STARTUP_STDIN].protocol == PROTOCOL_CONSOLE;
    status = prepare_stage(shell, &prepared[i], &stages[i], mode, named_input,
        streams[i], publication.caller);
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
  if (mode == SHELL_SERVICE) {
    struct endpoint_packet packet = {0};
    status = endpoint_receive(publication.receiver, &packet);
    if (status != CALL_OK) {
      close_handle(&children[0]);
      close_handle(&publication.caller);
      close_handle(&publication.receiver);
      return shell_error(shell, "service: publication receive failed (status %u)\n", status);
    }
    enum call_status published = CALL_BAD_REQUEST;
    if (packet.kind == ENDPOINT_MESSAGE_CALL && packet.protocol == 0 &&
        packet.operation == 0 && packet.object_id == 0 && packet.size == 0 &&
        packet.grant_count == 1) {
      struct endpoint_grant *grant = &packet.grants[0];
      published = replace ? namespace_replace(shell->namespace, service_name,
          grant->handle, grant->rights, grant->transport) :
          namespace_publish(shell->namespace, service_name, grant->handle,
          grant->rights, grant->transport);
    }
    for (size_t i = 0; i < packet.grant_count; ++i) {
      close_handle(&packet.grants[i].handle);
    }
    enum call_status reply_status = packet.kind == ENDPOINT_MESSAGE_CALL ?
        endpoint_reply(packet.receipt, published, NULL, 0, NULL, 0) :
        endpoint_finish(packet.receipt);
    if (packet.kind == ENDPOINT_MESSAGE_CALL && reply_status != CALL_OK) {
      endpoint_finish(packet.receipt);
    }
    bool closed = close_handle(&children[0]);
    closed = close_handle(&publication.caller) && closed;
    closed = close_handle(&publication.receiver) && closed;
    if (published != CALL_OK || reply_status != CALL_OK || !closed) {
      return shell_error(shell,
          "service: %s %s failed (status %u, reply %u)\n",
          replace ? "replace" : "start", service_name, published, reply_status);
    }
    return COMMAND_OK;
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
  close_handle(&publication.caller);
  close_handle(&publication.receiver);
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
  return launch_stages(shell, &stage, 1, mode, NULL, false);
}

enum command_result shell_launch_pipeline(struct shell *shell,
    const struct shell_command_line *command)
{
  if (command->stage_count < 2 || command->stage_count > LAUNCH_BATCH_MAX ||
      command->background) {
    return shell_error(shell, "shell: Invalid foreground pipeline\n");
  }
  return launch_stages(shell, command->stages, command->stage_count,
      SHELL_FOREGROUND, NULL, false);
}

enum command_result shell_launch_service(struct shell *shell, const char *name,
    bool replace, char **arguments, size_t count)
{
  if (shell->namespace == HANDLE_INVALID || shell->service == HANDLE_INVALID ||
      shell->clock == HANDLE_INVALID) {
    return shell_error(shell,
        "service: namespace, endpoint or clock authority unavailable\n");
  }
  uint64_t rights, transport;
  enum call_status status = handle_rights(shell->namespace, &rights, &transport);
  if (status != CALL_OK || !(rights & NAMESPACE_RIGHT_MANAGE)) {
    return shell_error(shell, "service: namespace management denied\n");
  }
  for (size_t i = 0; i < shell->directory.root_count; ++i) {
    if (!strcmp(shell->directory.roots[i].name, name)) {
      return shell_error(shell, "service: %s conflicts with a filesystem root\n", name);
    }
  }
  handle_t existing = HANDLE_INVALID;
  status = namespace_lookup(shell->namespace, name, &existing);
  if (existing != HANDLE_INVALID) {
    handle_close(existing);
  }
  if (status == CALL_BAD_REQUEST) {
    return shell_error(shell, "service: invalid name %s\n", name);
  }
  bool present = status == CALL_OK || status == CALL_ENDPOINT_CLOSED;
  if (present != replace) {
    return shell_error(shell, "service: %s %s failed (status %u)\n",
        replace ? "replace" : "start", name,
        replace ? CALL_NOT_FOUND : CALL_ALREADY_EXISTS);
  }
  if (!present && status != CALL_NOT_FOUND) {
    return shell_error(shell, "service: lookup %s failed (status %u)\n", name, status);
  }
  struct shell_stage stage = {.arguments = arguments, .count = count};
  return launch_stages(shell, &stage, 1, SHELL_SERVICE, name, replace);
}
