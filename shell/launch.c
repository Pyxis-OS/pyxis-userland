#include "shell.h"
#include "../common/provider_setup.h"
#include <abi/directory.h>
#include <abi/console.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/display.h>
#include <abi/clock.h>
#include <abi/system_info.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/pipe.h>
#include <abi/power.h>
#include <abi/endpoint.h>
#include <abi/namespace.h>
#include <abi/random.h>
#include <abi/net_config.h>
#include <abi/keyboard.h>
#include <abi/pointer.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <abi/terminal.h>
#include <abi/wait.h>
#include <clock.h>
#include <console.h>
#include <handle.h>
#include <endpoint.h>
#include <file.h>
#include <launcher.h>
#include <namespace.h>
#include <network_environment.h>
#include <pipe.h>
#include <process.h>
#include <startup.h>
#include <wait.h>
#include <stdlib.h>
#include <string.h>

enum { CHILD_OUTPUT, CHILD_MEMORY, CHILD_ROOT };

struct prepared_stage {
  handle_t image;
  handle_t redirected[STARTUP_STREAM_COUNT];
  struct launch_grant *grants;
  uint64_t *directories;
  struct launch_binding resources[25];
  struct launch_binding roots[STARTUP_ROOT_LIMIT];
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

static void set_outcome(struct shell_outcome *outcome, uint64_t kind, int64_t status)
{
  if (outcome) {
    *outcome = (struct shell_outcome){kind, status};
  }
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
    const struct startup_stream streams[STARTUP_STREAM_COUNT], handle_t publication,
    bool read_only, const struct network_environment *environment)
{
  bool session = mode == SHELL_SESSION;
  bool pass_child_launcher = session && shell->child_launcher != HANDLE_INVALID;
  bool has_launcher = session ||
      (mode == SHELL_FOREGROUND && shell->child_launcher != HANDLE_INVALID);
  bool has_terminal = session && shell->terminal_service != HANDLE_INVALID;
  /* Only a session successor inherits power; the shell's own programs never do. */
  bool has_power = session && shell->power != HANDLE_INVALID;
  bool provider = mode == SHELL_SERVICE;
  bool device_input = named_input &&
      streams[STARTUP_STDIN].protocol == PROTOCOL_CONSOLE;
  bool has_keyboard = device_input && shell->keyboard != HANDLE_INVALID;
  bool has_pointer = device_input && shell->pointer != HANDLE_INVALID;
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
  bool has_udp_beacons = session && shell->udp_beacons != HANDLE_INVALID;
  bool has_echo = shell->echo != HANDLE_INVALID;
  bool has_clock = shell->clock != HANDLE_INVALID;
  bool has_system_info = !provider && shell->system_info != HANDLE_INVALID;
  bool has_log = !provider && shell->log != HANDLE_INVALID;
  bool has_display = shell->display != HANDLE_INVALID;
  size_t root_count = shell->directory.root_count;
  if (root_count > STARTUP_ROOT_LIMIT) {
    return CALL_LIMIT;
  }
  size_t directory_index = CHILD_ROOT + root_count;
  size_t depth = shell->directory.count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - directory_index - 25 - STARTUP_STREAM_COUNT) {
    return CALL_LIMIT;
  }
  size_t display_index = directory_index + depth;
  size_t clock_index = display_index + (has_display ? 1 : 0);
  size_t system_info_index = clock_index + (has_clock ? 1 : 0);
  size_t log_index = system_info_index + (has_system_info ? 1 : 0);
  size_t echo_index = log_index + (has_log ? 1 : 0);
  size_t udp_index = echo_index + (has_echo ? 1 : 0);
  size_t udp_beacons_index = udp_index + (has_udp ? 1 : 0);
  size_t tcp_index = udp_beacons_index + (has_udp_beacons ? 1 : 0);
  size_t random_index = tcp_index + (has_tcp ? 1 : 0);
  size_t keyboard_index = random_index + (has_random ? 1 : 0);
  size_t pointer_index = keyboard_index + (has_keyboard ? 1 : 0);
  size_t launcher_index = pointer_index + (has_pointer ? 1 : 0);
  size_t child_launcher_index = launcher_index + (has_launcher ? 1 : 0);
  size_t net_config_index = child_launcher_index + (pass_child_launcher ? 1 : 0);
  size_t pipe_index = net_config_index + (has_net_config ? 1 : 0);
  size_t service_index = pipe_index + (has_pipe ? 1 : 0);
  size_t space_index = service_index + (has_service ? 1 : 0);
  size_t profile_index = space_index + (has_space ? 1 : 0);
  size_t input_index = profile_index + (has_profile ? 1 : 0);
  size_t namespace_index = input_index + (named_input ? 1 : 0);
  size_t publication_index = namespace_index + (has_namespace ? 1 : 0);
  size_t namespace_service_index = publication_index + (provider ? 1 : 0);
  size_t terminal_index = namespace_service_index + (has_namespace_service ? 1 : 0);
  size_t power_index = terminal_index + (has_terminal ? 1 : 0);
  size_t grant_count = power_index + (has_power ? 1 : 0);
  prepared->grants = malloc((grant_count + STARTUP_STREAM_COUNT) * sizeof(*prepared->grants));
  prepared->directories = depth ? malloc(depth * sizeof(*prepared->directories)) : NULL;
  if (!prepared->grants || (depth && !prepared->directories)) {
    return CALL_NO_MEMORY;
  }
  struct launch_grant *grants = prepared->grants;
  uint64_t *directories = prepared->directories;
  if (named_input) {
    /* A session successor replaces this shell and inherits Ctrl+C arming;
     * ordinary commands receive READ alone. */
    uint64_t input_rights = CONSOLE_RIGHT_READ;
    if (session) {
      enum call_status status = handle_rights(shell->terminal.input, &input_rights, NULL);
      if (status != CALL_OK) {
        return status;
      }
      input_rights &= CONSOLE_RIGHT_READ | CONSOLE_RIGHT_INTERRUPT;
    }
    grants[input_index] = (struct launch_grant){shell->terminal.input, input_rights, 0};
  }
  grants[CHILD_OUTPUT] = (struct launch_grant){shell->terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[CHILD_MEMORY] = (struct launch_grant){shell->memory, MEMORY_RIGHT_MANAGE, 0};
  for (size_t i = 0; i < root_count; ++i) {
    grants[CHILD_ROOT + i] = (struct launch_grant){shell->roots[i].handle, 0, 0};
    prepared->roots[i] = (struct launch_binding){(uintptr_t)shell->roots[i].name,
        CHILD_ROOT + i};
  }
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = directory_index + i;
    grants[directory_index + i] = (struct launch_grant){shell->directory.directories[i], 0, 0};
  }
  if (has_display) {
    grants[display_index] = (struct launch_grant){shell->display, DISPLAY_RIGHT_DRAW, 0};
  }
  if (has_clock) {
    grants[clock_index] = (struct launch_grant){shell->clock, CLOCK_RIGHTS, 0};
  }
  if (has_system_info) {
    grants[system_info_index] = (struct launch_grant){shell->system_info, SYSTEM_INFO_RIGHT_READ, 0};
  }
  if (has_log) {
    grants[log_index] = (struct launch_grant){shell->log, LOG_RIGHT_READ, 0};
  }
  if (has_echo) {
    grants[echo_index] = (struct launch_grant){shell->echo, ECHO_RIGHT_SEND, 0};
  }
  if (has_udp) {
    uint64_t rights = UDP_SERVICE_RIGHT_OPEN;
    if (session) {
      enum call_status status = handle_rights(shell->udp, &rights, NULL);
      if (status != CALL_OK) {
        return status;
      }
      rights &= UDP_SERVICE_RIGHT_OPEN | UDP_SERVICE_RIGHT_BROADCAST;
    }
    grants[udp_index] = (struct launch_grant){shell->udp, rights, 0};
  }
  if (has_udp_beacons) {
    grants[udp_beacons_index] = (struct launch_grant){shell->udp_beacons,
        UDP_SERVICE_RIGHT_BROADCAST, 0};
  }
  if (has_tcp) {
    uint64_t rights = TCP_SERVICE_RIGHT_CONNECT;
    if (session) {
      enum call_status status = handle_rights(shell->tcp, &rights, NULL);
      if (status != CALL_OK) {
        return status;
      }
      rights &= TCP_SERVICE_RIGHTS;
    }
    grants[tcp_index] = (struct launch_grant){shell->tcp, rights, 0};
  }
  if (has_random) {
    grants[random_index] = (struct launch_grant){shell->random, RANDOM_RIGHT_READ, 0};
  }
  if (has_keyboard) {
    grants[keyboard_index] = (struct launch_grant){shell->keyboard, KEYBOARD_RIGHT_INPUT, 0};
  }
  if (has_pointer) {
    grants[pointer_index] = (struct launch_grant){shell->pointer, POINTER_RIGHT_INPUT, 0};
  }
  if (session) {
    uint64_t rights;
    enum call_status status = handle_rights(shell->launcher, &rights, NULL);
    if (status != CALL_OK) {
      return status;
    }
    grants[launcher_index] = (struct launch_grant){shell->launcher,
        rights & (LAUNCHER_RIGHT_LAUNCH | LAUNCHER_RIGHT_CREATE_GROUP), 0};
  } else if (has_launcher) {
    grants[launcher_index] = (struct launch_grant){shell->child_launcher,
        LAUNCHER_RIGHT_LAUNCH, 0};
  }
  if (pass_child_launcher) {
    grants[child_launcher_index] = (struct launch_grant){shell->child_launcher,
        LAUNCHER_RIGHT_LAUNCH, 0};
  }
  if (has_terminal) {
    grants[terminal_index] = (struct launch_grant){shell->terminal_service, TERMINAL_SERVICE_RIGHT_CREATE, 0};
  }
  if (has_power) {
    grants[power_index] = (struct launch_grant){shell->power, POWER_RIGHTS, 0};
  }
  if (has_net_config) {
    uint64_t rights;
    enum call_status status = handle_rights(shell->net_config, &rights, NULL);
    if (status != CALL_OK) {
      return status;
    }
    grants[net_config_index] = (struct launch_grant){shell->net_config,
        rights & NET_CONFIG_RIGHTS, 0};
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
    uint64_t rights;
    enum call_status status = handle_rights(shell->profile, &rights, NULL);
    if (status != CALL_OK) {
      return status;
    }
    grants[profile_index] = (struct launch_grant){shell->profile,
        rights & (PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_FILE | PROFILE_RIGHT_HOST), 0};
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
  if (has_system_info) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"system_info", system_info_index};
  }
  if (has_log) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"log", log_index};
  }
  if (has_echo) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"echo", echo_index};
  }
  if (has_udp) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"udp", udp_index};
  }
  if (has_udp_beacons) {
    resources[resource_count++] =
        (struct launch_binding){(uintptr_t)"udp_beacons", udp_beacons_index};
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
  if (has_pointer) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"pointer", pointer_index};
  }
  if (has_launcher) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"launcher", launcher_index};
  }
  if (pass_child_launcher) {
    resources[resource_count++] =
        (struct launch_binding){(uintptr_t)"child_launcher", child_launcher_index};
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
  if (has_terminal) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"terminal", terminal_index};
  }
  if (has_power) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"power", power_index};
  }
  struct launch_binding *roots = prepared->roots;
  /* Root names and display paths do not determine delegated authority. */
  for (size_t i = 0; i < root_count; ++i) {
    struct launch_grant *grant = &grants[roots[i].grant];
    enum call_status status = handle_rights(grant->source, &grant->rights,
        &grant->transport);
    if (status != CALL_OK) {
      return status;
    }
    if (read_only) {
      grant->rights &= DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
          DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
    }
  }
  /* The cwd is explicitly selected alongside the complete root list. Apply
   * profile attenuation to both; subset launchers must select their own cwd. */
  for (size_t i = 0; i < depth; ++i) {
    struct launch_grant *grant = &grants[directories[i]];
    enum call_status status = handle_rights(grant->source, &grant->rights,
        &grant->transport);
    if (status != CALL_OK) {
      return status;
    }
    if (read_only) {
      grant->rights &= DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
          DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
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
    .environment = (uintptr_t)environment->variables,
    .environment_count = environment->count,
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
    uint64_t transport = 0;
    if (stream.protocol == PROTOCOL_FILE) {
      struct handle_info info;
      enum call_status status = handle_query(stream.handle, &info);
      if (status != CALL_OK) {
        return status;
      }
      if (info.protocol != PROTOCOL_FILE ||
          (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED)) {
        return CALL_WRONG_TYPE;
      }
      if (info.kind == HANDLE_KIND_EXPORTED) {
        transport = HANDLE_TRANSPORT_CALL;
      }
    }
    grants[request->grant_count++] = (struct launch_grant){stream.handle, rights, transport};
  }
  return CALL_OK;
}

/* Arms Ctrl+C for one foreground job. Without interrupt authority the job
 * runs as before, interruptible only by ending the session. */
static handle_t arm_interrupt(struct shell *shell)
{
  if (!shell->interrupts) {
    return HANDLE_INVALID;
  }
  handle_t armed;
  enum call_status status = console_arm_interrupt(shell->terminal.input, &armed);
  if (status != CALL_OK) {
    shell_error(shell, "shell: Ctrl+C unavailable for this command (status %u)\n", status);
    return HANDLE_INVALID;
  }
  return armed;
}

static_assert(LAUNCH_BATCH_MAX + 1 <= WAIT_MAX_INTERESTS,
    "a foreground wait watches every stage and the interrupt");

/* Waits until every stage completes or Ctrl+C arrives. On Ctrl+C, requests
 * termination of every stage and stops watching the interrupt; the caller
 * stays armed, so later presses are discarded, and collects results as usual.
 * Terminating a stage that already finished keeps its real result. */
static enum call_status wait_or_interrupt(struct shell *shell, const handle_t *children,
    size_t stage_count, handle_t armed)
{
  bool complete[LAUNCH_BATCH_MAX] = {0};
  size_t remaining = stage_count;
  while (remaining) {
    struct wait_interest interests[LAUNCH_BATCH_MAX + 1];
    size_t stage_of[LAUNCH_BATCH_MAX];
    size_t count = 0;
    for (size_t i = 0; i < stage_count; ++i) {
      if (!complete[i]) {
        stage_of[count] = i;
        interests[count++] = (struct wait_interest){children[i], WAIT_COMPLETE};
      }
    }
    interests[count] = (struct wait_interest){armed, WAIT_INTERRUPT};
    uint64_t now;
    enum call_status status = clock_now(shell->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    uint64_t events[LAUNCH_BATCH_MAX + 1];
    status = wait_many(interests, count + 1, now + WAIT_MAX_WAIT_NS, events);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    for (size_t j = 0; j < count; ++j) {
      if (events[j]) {
        complete[stage_of[j]] = true;
        --remaining;
      }
    }
    if (events[count] & WAIT_INTERRUPT) {
      for (size_t i = 0; i < stage_count; ++i) {
        status = process_terminate(children[i]);
        if (status != CALL_OK) {
          shell_error(shell, "shell: stage %zu: cannot terminate (status %u)\n", i + 1, status);
        }
      }
      return CALL_OK;
    }
    if (events[count] & WAIT_ERROR) {
      /* The terminal hung up; its readers fail on their own. */
      return CALL_OK;
    }
  }
  return CALL_OK;
}

static enum command_result launch_stages(struct shell *shell, const struct shell_stage *stages,
    size_t stage_count, enum shell_launch_mode mode, const char *service_name, bool replace,
    bool optional, bool read_only, struct shell_outcome *outcome)
{
  struct prepared_stage *prepared = calloc(stage_count, sizeof(*prepared));
  struct launch_request *requests = malloc(stage_count * sizeof(*requests));
  if (!prepared || !requests) {
    free(requests);
    free(prepared);
    set_outcome(outcome, TERMINAL_COMPLETION_LAUNCH_FAILED, 0);
    return shell_directory_error(shell, "shell", stages[0].arguments[0], CALL_NO_MEMORY);
  }
  struct pipe_create_reply pipes[LAUNCH_BATCH_MAX - 1] = {0};
  struct startup_stream streams[LAUNCH_BATCH_MAX][STARTUP_STREAM_COUNT] = {0};
  const char *operation = "shell: open";
  const char *path = stages[0].arguments[0];
  size_t failed_stage = 0;
  enum call_status status = CALL_OK;
  handle_t children[LAUNCH_BATCH_MAX] = {0};
  handle_t armed = HANDLE_INVALID;
  uint64_t failed_index = LAUNCH_NO_STAGE;
  bool cleanup_failure = false;
  struct endpoint_create_reply publication = {0};
  struct network_environment environment = {0};

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

  operation = "shell: network environment";
  path = stages[0].arguments[0];
  status = network_environment_read(&environment, shell->net_config,
      startup_environment_variables(), startup_environment_count(), NULL);
  if (status != CALL_OK) {
    goto failed;
  }

  /* A redirected pipe side is absent from every grant, so its peer sees
   * EOF or a broken reader once the remaining owned endpoint closes. */
  for (size_t i = 0; i < stage_count; ++i) {
    failed_stage = i;
    operation = "shell: prepare";
    path = stages[i].arguments[0];
    bool console_stdin = streams[i][STARTUP_STDIN].protocol == PROTOCOL_CONSOLE;
    bool pager_input = mode == SHELL_FOREGROUND && i + 1 == stage_count &&
        streams[i][STARTUP_STDOUT].protocol == PROTOCOL_CONSOLE &&
        (streams[i][STARTUP_STDIN].protocol == PROTOCOL_PIPE ||
         streams[i][STARTUP_STDIN].protocol == PROTOCOL_FILE);
    bool named_input = mode != SHELL_BACKGROUND && mode != SHELL_SERVICE &&
        ((i == 0 && console_stdin) || pager_input);
    status = prepare_stage(shell, &prepared[i], &stages[i], mode, named_input,
        streams[i], publication.caller, read_only, &environment);
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
  /* Arm before launch so Ctrl+C during startup cannot reach a stage as data. */
  if (mode == SHELL_FOREGROUND) {
    armed = arm_interrupt(shell);
  }
  struct path_context interpreter_context = {.namespace = shell->namespace};
  if (stage_count == 1) {
    status = program_launch(shell->launcher, &requests[0], &interpreter_context,
        &children[0]);
  } else {
    status = program_launch_batch(shell->launcher, requests, stage_count,
        &interpreter_context, children, &failed_index);
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
  network_environment_free(&environment);
  free(requests);
  free_preparation(prepared, stage_count);
  if (!closed_sources) {
    for (size_t i = 0; i < stage_count; ++i) {
      close_handle(&children[i]);
    }
    close_handle(&armed);
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
    struct provider_setup setup = {0};
    bool reported_failure = false;
    if (packet.kind == ENDPOINT_MESSAGE_CALL && packet.protocol == 0 &&
        packet.operation == 0 && packet.object_id == 0 && packet.reason == 0 &&
        packet.size == sizeof(setup)) {
      memcpy(&setup, packet.data, sizeof(setup));
      if (setup.status < CALL_STATUS_COUNT && setup.cleanup_status < CALL_STATUS_COUNT) {
        if (setup.status == CALL_OK && setup.cleanup_status == CALL_OK &&
            packet.grant_count == 1) {
          struct endpoint_grant *grant = &packet.grants[0];
          struct handle_info info;
          enum call_status queried = handle_query(grant->handle, &info);
          if (queried == CALL_OK && info.kind == HANDLE_KIND_EXPORTED &&
              grant->transport == HANDLE_TRANSPORT_CALL &&
              info.rights == grant->rights && info.transport == grant->transport) {
            published = replace ? namespace_replace(shell->namespace, service_name,
                grant->handle, grant->rights, grant->transport) :
                namespace_publish(shell->namespace, service_name, grant->handle,
                grant->rights, grant->transport);
          }
        } else if (setup.status != CALL_OK && packet.grant_count == 0) {
          reported_failure = true;
          published = CALL_OK;
        }
      }
    }
    bool closed_grants = true;
    for (size_t i = 0; i < packet.grant_count; ++i) {
      closed_grants = close_handle(&packet.grants[i].handle) && closed_grants;
    }
    if (!closed_grants) {
      published = CALL_BAD_HANDLE;
    }
    enum call_status reply_status = packet.kind == ENDPOINT_MESSAGE_CALL ?
        endpoint_reply(packet.receipt, published, NULL, 0, NULL, 0) :
        endpoint_finish(packet.receipt);
    bool finished = true;
    if (packet.kind == ENDPOINT_MESSAGE_CALL && reply_status != CALL_OK) {
      finished = endpoint_finish(packet.receipt) == CALL_OK;
    }
    bool closed = close_handle(&children[0]);
    closed = close_handle(&publication.caller) && closed;
    closed = close_handle(&publication.receiver) && closed;
    if (published != CALL_OK || reply_status != CALL_OK || !closed || !finished) {
      enum command_result diagnostic = shell_error(shell,
          "service: %s %s failed (status %u, reply %u)\n",
          replace ? "replace" : "start", service_name, published, reply_status);
      return !closed || !closed_grants || !finished ? COMMAND_FATAL : diagnostic;
    }
    if (reported_failure) {
      enum command_result diagnostic = shell_error(shell,
          "service: %s setup failed (status %ju, cleanup %ju)\n",
          service_name, setup.status, setup.cleanup_status);
      if (diagnostic == COMMAND_FATAL || setup.cleanup_status != CALL_OK) {
        return diagnostic;
      }
      return optional ? COMMAND_OK : diagnostic;
    }
    return COMMAND_OK;
  }
  if (mode == SHELL_SESSION || mode == SHELL_BACKGROUND) {
    bool closed = close_handle(&children[0]);
    if (!closed) {
      shell_directory_error(shell, "shell: close", stages[0].arguments[0], CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
    set_outcome(outcome, TERMINAL_COMPLETION_LAUNCHED, 0);
    return mode == SHELL_SESSION ? COMMAND_EXIT : COMMAND_OK;
  }

  if (armed != HANDLE_INVALID) {
    status = wait_or_interrupt(shell, children, stage_count, armed);
    if (status != CALL_OK) {
      /* Nothing watches the interrupt now; disarm so Ctrl+C is data again
       * rather than silently swallowed. */
      shell_error(shell, "shell: Ctrl+C wait failed (status %u)\n", status);
      if (!close_handle(&armed)) {
        for (size_t i = 0; i < stage_count; ++i) {
          close_handle(&children[i]);
        }
        shell_directory_error(shell, "shell: close", "Ctrl+C arming", CALL_BAD_HANDLE);
        return COMMAND_FATAL;
      }
    }
  }
  struct process_result completion[LAUNCH_BATCH_MAX] = {0};
  for (size_t i = 0; i < stage_count; ++i) {
    status = process_wait(children[i], &completion[i]);
    bool closed = close_handle(&children[i]);
    if (status != CALL_OK || !closed) {
      for (size_t j = i + 1; j < stage_count; ++j) {
        close_handle(&children[j]);
      }
      close_handle(&armed);
      launch_error(shell, stages, stage_count, i,
          status != CALL_OK ? "shell: wait" : "shell: close",
          stages[i].arguments[0], status != CALL_OK ? status : CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
  }
  /* Disarm only after every stage finished, so no press reaches the editor. */
  if (!close_handle(&armed)) {
    shell_directory_error(shell, "shell: close", "Ctrl+C arming", CALL_BAD_HANDLE);
    return COMMAND_FATAL;
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
    } else if (completion[i].kind == PROCESS_TERMINATED) {
      diagnostic = stage_count == 1 ?
          shell_error(shell, "shell: %s: Process terminated\n", stages[i].arguments[0]) :
          shell_error(shell, "shell: stage %zu (%s): Process terminated\n",
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
  const struct process_result *last = &completion[stage_count - 1];
  switch (last->kind) {
  case PROCESS_EXITED:
    set_outcome(outcome, TERMINAL_COMPLETION_EXITED, last->exit_status);
    break;
  case PROCESS_FAULTED:
    set_outcome(outcome, TERMINAL_COMPLETION_FAULTED, 0);
    break;
  default:
    set_outcome(outcome, TERMINAL_COMPLETION_TERMINATED, 0);
    break;
  }
  return result;

failed:
  if (!close_handle(&armed)) {
    cleanup_failure = true;
  }
  closed_sources = release_sources(prepared, stage_count, pipes);
  close_handle(&publication.caller);
  close_handle(&publication.receiver);
  network_environment_free(&environment);
  free(requests);
  free_preparation(prepared, stage_count);
  enum command_result error = launch_error(shell, stages, stage_count, failed_stage,
      operation, path, status);
  set_outcome(outcome, TERMINAL_COMPLETION_LAUNCH_FAILED, 0);
  if (!closed_sources || cleanup_failure || status == CALL_OUTCOME_UNKNOWN) {
    return COMMAND_FATAL;
  }
  return error;
}

enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode, const struct shell_redirection *redirections,
    size_t redirection_count, struct shell_outcome *outcome)
{
  struct shell_stage stage = {.arguments = arguments, .count = count,
      .redirection_count = redirection_count};
  for (size_t i = 0; i < redirection_count; ++i) {
    stage.redirections[i] = redirections[i];
  }
  return launch_stages(shell, &stage, 1, mode, NULL, false, false, false, outcome);
}

enum command_result shell_launch_pipeline(struct shell *shell,
    const struct shell_command_line *command, struct shell_outcome *outcome)
{
  if (command->stage_count < 2 || command->stage_count > LAUNCH_BATCH_MAX ||
      command->background) {
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_REJECTED, 0};
    return shell_error(shell, "shell: Invalid foreground pipeline\n");
  }
  return launch_stages(shell, command->stages, command->stage_count,
      SHELL_FOREGROUND, NULL, false, false, false, outcome);
}

enum command_result shell_launch_service(struct shell *shell, const char *name,
    bool replace, bool optional, bool read_only, char **arguments, size_t count)
{
  if (shell->namespace == HANDLE_INVALID || shell->service == HANDLE_INVALID ||
      shell->clock == HANDLE_INVALID) {
    return shell_error(shell,
        "service: namespace, endpoint or clock authority unavailable\n");
  }
  uint64_t rights, transport;
  enum call_status status = handle_rights(shell->namespace, &rights, &transport);
  if (status != CALL_OK || (rights & NAMESPACE_RIGHTS) != NAMESPACE_RIGHTS) {
    return shell_error(shell, "service: namespace lookup or management denied\n");
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
  return launch_stages(shell, &stage, 1, SHELL_SERVICE, name, replace, optional, read_only,
      NULL);
}
