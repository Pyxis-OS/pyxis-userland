#include <pyxis/environment.h>
#include "session.h"
#include "../common/session_wait.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/echo.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/thread.h>
#include <abi/net_config.h>
#include <abi/namespace.h>
#include <abi/endpoint.h>
#include <abi/pipe.h>
#include <abi/power.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/audio.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <console.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <network_environment.h>
#include <startup.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <endpoint.h>
#include <clock.h>
#include <remote/terminal.h>
#include <abi/execution_group.h>

static const struct {
  const char *name;
  uint64_t rights;
} remote_allowed[] = {
  {"memory", MEMORY_RIGHT_MANAGE}, {"threads", THREAD_RIGHT_MANAGE},
  {"clock", CLOCK_RIGHTS},
  {"system_info", SYSTEM_INFO_RIGHT_READ}, {"log", LOG_RIGHT_READ},
  {"screen_capture", SCREEN_CAPTURE_RIGHT_CAPTURE},
  {"audio", AUDIO_RIGHT_PLAYBACK},
  {"pipe", PIPE_SERVICE_RIGHT_CREATE}, {"service", ENDPOINT_SERVICE_RIGHT_CREATE},
  {"tcp", TCP_SERVICE_RIGHT_CONNECT}, {"random", RANDOM_RIGHT_READ},
  {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST},
  {"echo", ECHO_RIGHT_SEND}, {"udp", UDP_SERVICE_RIGHT_OPEN},
  {"net_config", NET_CONFIG_RIGHT_READ},
};

static enum call_status directory_grant(handle_t directory, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = directory};
  return handle_rights(directory, &grant->rights, &grant->transport);
}

enum call_status remote_shell_launch(size_t columns, size_t rows, unsigned tab_width,
    bool no_echo, struct remote_shell *shell)
{
  *shell = (struct remote_shell){0};
  struct terminal_create_reply terminal;
  enum call_status status = terminal_create(startup_resource("terminal"), columns, rows, &terminal);
  if (status != CALL_OK) {
    return status;
  }
  shell->attachment = terminal.attachment;
  handle_t bound = HANDLE_INVALID, image = HANDLE_INVALID;
  struct network_environment environment = {0};
  struct pyxis_environment_snapshot inherited = {0};
  struct execution_group_create_reply group;
  status = launcher_create_group(startup_resource("launcher"), &group);
  if (status != CALL_OK) {
    goto done;
  }
  shell->group = group.supervision;
  bound = group.launcher;
  status = console_set_tab_width(terminal.output, tab_width);
  if (status != CALL_OK) {
    goto done;
  }
  handle_t boot = startup_root("boot"), tmp = startup_root("tmp");
  if (boot == HANDLE_INVALID || tmp == HANDLE_INVALID) {
    status = CALL_NOT_FOUND;
    goto done;
  }
  status = directory_lookup(boot, "shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }

  const struct startup_binding *selected_roots = startup_roots();
  size_t root_count = startup_root_count();
  if (root_count > STARTUP_ROOT_LIMIT) {
    status = CALL_LIMIT;
    goto done;
  }
  enum { INPUT, OUTPUT, LAUNCHER, STDIN, STDOUT, STDERR, EVENTS, FIRST_OPTIONAL };
  enum { OPTIONAL_RESOURCE_COUNT = 17, NAMESPACE_GRANT_COUNT = 1 };
  struct launch_grant grants[FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT +
      NAMESPACE_GRANT_COUNT + STARTUP_ROOT_LIMIT] = {
    /* Only the root shell may arm Ctrl+C; its commands receive READ alone. */
    [INPUT] = {terminal.input, CONSOLE_RIGHT_READ | CONSOLE_RIGHT_INTERRUPT, 0},
    [OUTPUT] = {terminal.output, CONSOLE_RIGHT_WRITE, 0},
    [LAUNCHER] = {bound, LAUNCHER_RIGHT_LAUNCH, 0},
    [STDIN] = {terminal.input, CONSOLE_RIGHT_READ, 0},
    [STDOUT] = {terminal.output, CONSOLE_RIGHT_WRITE, 0},
    [STDERR] = {terminal.output, CONSOLE_RIGHT_WRITE, 0},
    [EVENTS] = {terminal.events, TERMINAL_EVENTS_RIGHT_EMIT, 0},
  };
  struct launch_binding resources[4 + OPTIONAL_RESOURCE_COUNT] = {
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"launcher", LAUNCHER}, {(uintptr_t)"terminal_events", EVENTS},
  };
  size_t grant_count = FIRST_OPTIONAL, resource_count = 4;
  handle_t child_launcher = startup_resource("child_launcher");
  if (child_launcher != HANDLE_INVALID) {
    struct handle_info info;
    status = handle_query(child_launcher, &info);
    if (status != CALL_OK) {
      goto done;
    }
    if (info.protocol != PROTOCOL_LAUNCHER) {
      status = CALL_WRONG_TYPE;
      goto done;
    }
    if (!(info.rights & LAUNCHER_RIGHT_LAUNCH)) {
      status = CALL_DENIED;
      goto done;
    }
    /* Session members can receive only launchers bound to this group. */
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"child_launcher", grant_count};
    grants[grant_count++] = (struct launch_grant){bound, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  struct launch_binding roots[STARTUP_ROOT_LIMIT];
  /* Reuse the selected tmp grant for cwd, including any withheld rights. */
  uint64_t directory = SIZE_MAX;
  for (size_t i = 0; i < root_count; ++i) {
    roots[i] = (struct launch_binding){selected_roots[i].name, grant_count};
    status = directory_grant(selected_roots[i].handle, &grants[grant_count]);
    if (status != CALL_OK) {
      goto done;
    }
    if (!strcmp((const char *)selected_roots[i].name, "tmp")) {
      directory = grant_count;
    }
    ++grant_count;
  }
  if (directory == SIZE_MAX) {
    status = CALL_NOT_FOUND;
    goto done;
  }
  for (size_t i = 0; i < sizeof(remote_allowed) / sizeof(remote_allowed[0]); ++i) {
    handle_t source = startup_resource(remote_allowed[i].name);
    if (source == HANDLE_INVALID) {
      continue;
    }
    uint64_t rights;
    status = handle_rights(source, &rights, NULL);
    if (status != CALL_OK) {
      goto done;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)remote_allowed[i].name, grant_count};
    grants[grant_count++] = (struct launch_grant){source, rights & remote_allowed[i].rights, 0};
  }
  /* Only the explicit remote opt-in becomes power in a remote root shell. */
  handle_t remote_power = startup_resource("remote_power");
  if (remote_power != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"power", grant_count};
    grants[grant_count++] = (struct launch_grant){remote_power, POWER_RIGHTS, 0};
  }
  const char *arguments[] = {"boot://shell.pxe", no_echo ? "--no-echo" : "--remote-prompt"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = root_count,
    .working_directories = (uintptr_t)&directory, .working_directory_count = 1,
    .working_path = (uintptr_t)"tmp://",
    .argv = (uintptr_t)arguments, .argc = 2,
    .streams = {{PROTOCOL_CONSOLE, STDIN}, {PROTOCOL_CONSOLE, STDOUT}, {PROTOCOL_CONSOLE, STDERR}},
  };
  handle_t namespace = startup_namespace();
  if (namespace != HANDLE_INVALID) {
    uint64_t transport;
    status = handle_rights(namespace, NULL, &transport);
    if (status != CALL_OK) {
      goto done;
    }
    request.namespace_grant = grant_count + 1;
    grants[grant_count++] = (struct launch_grant){namespace, NAMESPACE_RIGHT_LOOKUP, transport};
  }
  request.grant_count = grant_count;
  status = pyxis_environment_snapshot_init(&inherited);
  if (status != CALL_OK) {
    goto done;
  }
  status = network_environment_read(&environment, startup_resource("net_config"),
      inherited.variables, inherited.count, NULL);
  if (status != CALL_OK) {
    goto done;
  }
  /* Discovery configuration stays with the trusted bootstrap and daemon. */
  size_t forwarded = 0;
  for (size_t i = 0; i < environment.count; ++i) {
    if (strcmp((const char *)environment.variables[i].name, "PYXIS_REMOTE_BEACON")) {
      environment.variables[forwarded++] = environment.variables[i];
    }
  }
  environment.count = forwarded;
  status = session_wait_environment(&environment, true);
  if (status != CALL_OK) {
    goto done;
  }
  request.environment = (uintptr_t)environment.variables;
  request.environment_count = environment.count;
  status = launcher_launch(bound, &request, &shell->process);

done:
  network_environment_free(&environment);
  pyxis_environment_snapshot_close(&inherited);
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  if (bound != HANDLE_INVALID) {
    handle_close(bound);
  }
  handle_close(terminal.input);
  handle_close(terminal.output);
  handle_close(terminal.events);
  /* Keep attachment/supervision even on failure: the loop owns termination and
   * observes cleanup before returning this admission slot to the listener. */
  return status;
}

static void owned_close(handle_t *handle)
{
  if (*handle != HANDLE_INVALID) {
    handle_close(*handle);
    *handle = HANDLE_INVALID;
  }
}

static void context_close(struct remote_context *context)
{
  for (size_t i = 0; i < context->grant_count; ++i) {
    owned_close(&context->grants[i].source);
  }
  network_environment_free(&context->environment);
  pyxis_environment_snapshot_close(&context->inherited);
  *context = (struct remote_context){0};
}

static enum call_status context_add(struct remote_context *context, handle_t source,
    uint64_t rights, uint64_t transport, size_t *index)
{
  if (context->grant_count == REMOTE_POLICY_GRANTS) {
    return CALL_LIMIT;
  }
  struct launch_grant *grant = &context->grants[context->grant_count];
  enum call_status status = handle_copy_restricted(source, rights, transport, &grant->source);
  if (status != CALL_OK) {
    return status;
  }
  grant->rights = rights;
  grant->transport = transport;
  *index = context->grant_count++;
  return CALL_OK;
}

static enum call_status context_init(struct remote_context *context)
{
  *context = (struct remote_context){.directory = SIZE_MAX, .namespace_index = SIZE_MAX};
  const struct startup_binding *roots = startup_roots();
  size_t root_count = startup_root_count();
  enum call_status status;
  if (root_count > STARTUP_ROOT_LIMIT) {
    return CALL_LIMIT;
  }
  for (size_t i = 0; i < root_count; ++i) {
    uint64_t rights, transport;
    status = handle_rights(roots[i].handle, &rights, &transport);
    size_t index;
    if (status == CALL_OK) {
      status = context_add(context, roots[i].handle, rights, transport, &index);
    }
    if (status != CALL_OK) {
      goto failed;
    }
    context->roots[context->root_count++] = (struct launch_binding){roots[i].name, index};
    if (!strcmp((const char *)roots[i].name, "tmp")) {
      context->directory = index;
    }
  }
  if (context->directory == SIZE_MAX) {
    status = CALL_NOT_FOUND;
    goto failed;
  }
  for (size_t i = 0; i < sizeof(remote_allowed) / sizeof(remote_allowed[0]); ++i) {
    handle_t source = startup_resource(remote_allowed[i].name);
    if (source == HANDLE_INVALID) {
      continue;
    }
    uint64_t rights;
    status = handle_rights(source, &rights, NULL);
    size_t index;
    if (status == CALL_OK) {
      status = context_add(context, source, rights & remote_allowed[i].rights, 0, &index);
    }
    if (status != CALL_OK) {
      goto failed;
    }
    context->resources[context->resource_count++] =
        (struct launch_binding){(uintptr_t)remote_allowed[i].name, index};
  }
  handle_t power = startup_resource("remote_power");
  if (power != HANDLE_INVALID) {
    size_t index;
    status = context_add(context, power, POWER_RIGHTS, 0, &index);
    if (status != CALL_OK) {
      goto failed;
    }
    context->resources[context->resource_count++] = (struct launch_binding){(uintptr_t)"power", index};
  }
  handle_t namespace = startup_namespace();
  if (namespace != HANDLE_INVALID) {
    uint64_t transport;
    status = handle_rights(namespace, NULL, &transport);
    if (status == CALL_OK) {
      status = context_add(context, namespace, NAMESPACE_RIGHT_LOOKUP, transport,
          &context->namespace_index);
    }
    if (status != CALL_OK) {
      goto failed;
    }
  }
  handle_t child_launcher = startup_resource("child_launcher");
  if (child_launcher != HANDLE_INVALID) {
    struct handle_info info;
    status = handle_query(child_launcher, &info);
    if (status != CALL_OK) {
      goto failed;
    }
    if (info.protocol != PROTOCOL_LAUNCHER || !(info.rights & LAUNCHER_RIGHT_LAUNCH)) {
      status = CALL_DENIED;
      goto failed;
    }
    context->allow_launch = true;
  }
  status = pyxis_environment_snapshot_init(&context->inherited);
  if (status == CALL_OK) {
    status = network_environment_read(&context->environment, startup_resource("net_config"),
        context->inherited.variables, context->inherited.count, NULL);
  }
  if (status != CALL_OK) {
    goto failed;
  }
  size_t count = 0;
  for (size_t i = 0; i < context->environment.count; ++i) {
    const char *name = (const char *)context->environment.variables[i].name;
    if (strcmp(name, "PYXIS_REMOTE_BEACON") && strcmp(name, "PYXIS_REMOTE_MUX")) {
      context->environment.variables[count++] = context->environment.variables[i];
    }
  }
  context->environment.count = count;
  status = session_wait_environment(&context->environment, true);
  if (status != CALL_OK) {
    goto failed;
  }
  struct startup_variable *variables = realloc(context->environment.variables,
      (context->environment.count + 1) * sizeof(*variables));
  if (!variables) {
    status = CALL_NO_MEMORY;
    goto failed;
  }
  context->environment.variables = variables;
  variables[context->environment.count++] =
      (struct startup_variable){(uintptr_t)"PYXIS_REMOTE_MUX", (uintptr_t)"1"};
  return CALL_OK;

failed:
  context_close(context);
  return status;
}

static enum call_status broker_launch(struct remote_context *context,
    size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, handle_t bootstrap, handle_t broker_caller,
    uint64_t deadline, struct remote_shell *shell)
{
  *shell = (struct remote_shell){0};
  size_t depth = cwd ? cwd->context.count : 1;
  enum { INPUT, OUTPUT, LAUNCHER, STDIN, STDOUT, STDERR, EVENTS, FIRST_OPTIONAL };
  size_t fixed_grants = FIRST_OPTIONAL + context->grant_count + 3;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants ||
      depth > SIZE_MAX / sizeof(uint64_t)) {
    return CALL_LIMIT;
  }
  struct launch_grant *grants = calloc(fixed_grants + depth, sizeof(*grants));
  uint64_t *directories = malloc(depth * sizeof(*directories));
  if (!grants || !directories) {
    free(grants);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct terminal_create_reply terminal = {0};
  handle_t bound = HANDLE_INVALID, image = HANDLE_INVALID;
  enum call_status status = terminal_create(startup_resource("remote_mux_terminal"),
      columns, rows, &terminal);
  if (status != CALL_OK) {
    goto done;
  }
  shell->attachment = terminal.attachment;
  struct execution_group_create_reply group;
  status = launcher_create_group(startup_resource("launcher"), &group);
  if (status != CALL_OK) {
    goto done;
  }
  shell->group = group.supervision;
  bound = group.launcher;
  status = console_set_tab_width(terminal.output, tab_width);
  if (status != CALL_OK) {
    goto done;
  }
  bool mux = broker_caller != HANDLE_INVALID;
  handle_t image_root = HANDLE_INVALID;
  for (size_t i = 0; i < context->root_count; ++i) {
    if (!strcmp((const char *)context->roots[i].name, mux ? "bin" : "boot")) {
      image_root = context->grants[context->roots[i].grant].source;
    }
  }
  status = directory_lookup(image_root, mux ? "mux.pxe" : "shell.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }
  grants[INPUT] = (struct launch_grant){terminal.input,
      CONSOLE_RIGHT_READ | (mux ? 0 : CONSOLE_RIGHT_INTERRUPT), 0};
  grants[OUTPUT] = (struct launch_grant){terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[LAUNCHER] = (struct launch_grant){bound, LAUNCHER_RIGHT_LAUNCH, 0};
  grants[STDIN] = (struct launch_grant){terminal.input, CONSOLE_RIGHT_READ, 0};
  grants[STDOUT] = (struct launch_grant){terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[STDERR] = (struct launch_grant){terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[EVENTS] = (struct launch_grant){terminal.events, TERMINAL_EVENTS_RIGHT_EMIT, 0};
  struct launch_binding resources[24] = {
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"launcher", LAUNCHER}, {(uintptr_t)"terminal_events", EVENTS},
  };
  size_t resource_count = 4, grant_count = FIRST_OPTIONAL;
  for (size_t i = 0; i < context->grant_count; ++i) {
    grants[grant_count++] = context->grants[i];
  }
  for (size_t i = 0; i < context->resource_count; ++i) {
    resources[resource_count++] = (struct launch_binding){context->resources[i].name,
        context->resources[i].grant + FIRST_OPTIONAL};
  }
  struct launch_binding roots[STARTUP_ROOT_LIMIT];
  for (size_t i = 0; i < context->root_count; ++i) {
    roots[i] = (struct launch_binding){context->roots[i].name,
        context->roots[i].grant + FIRST_OPTIONAL};
  }
  if (context->allow_launch) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"child_launcher", grant_count};
    grants[grant_count++] = (struct launch_grant){bound, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  if (mux || bootstrap != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)(mux ? "pane_broker" : "pane_cwd"), grant_count};
    grants[grant_count++] = (struct launch_grant){mux ? broker_caller : bootstrap, 0, HANDLE_TRANSPORT_SEND};
  }
  for (size_t i = 0; i < depth; ++i) {
    if (cwd) {
      directories[i] = grant_count;
      status = directory_grant(cwd->context.directories[i], &grants[grant_count++]);
      if (status != CALL_OK) {
        goto done;
      }
    } else {
      directories[i] = context->directory + FIRST_OPTIONAL;
    }
  }
  char width[sizeof("32")];
  snprintf(width, sizeof(width), "%u", tab_width);
  const char *arguments[] = {mux ? "bin://mux.pxe" : "boot://shell.pxe", "--tab-width", width};
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants, .grant_count = grant_count,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = context->root_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)(cwd ? cwd->path : "tmp://"),
    .environment = (uintptr_t)context->environment.variables,
    .environment_count = context->environment.count,
    .argv = (uintptr_t)arguments, .argc = mux ? 3 : 1,
    .streams = {{PROTOCOL_CONSOLE, STDIN}, {PROTOCOL_CONSOLE, STDOUT}, {PROTOCOL_CONSOLE, STDERR}},
    .namespace_grant = context->namespace_index == SIZE_MAX ? 0 : context->namespace_index + FIRST_OPTIONAL + 1,
  };
  if (deadline) {
    uint64_t now;
    status = clock_now(startup_resource("clock"), &now);
    if (status != CALL_OK) {
      goto done;
    }
    if (now >= deadline) {
      status = CALL_TIMED_OUT;
      goto done;
    }
  }
  status = launcher_launch(bound, &request, &shell->process);
  /* The launcher helper folds malformed replies into ordinary errors, and
   * BAD_BUFFER can follow publication. Retain group cleanup ownership and
   * prevent a new split when this reply cannot prove that no child ran. */
  switch (status) {
  case CALL_BAD_REQUEST:
  case CALL_BAD_BUFFER:
  case CALL_UNAVAILABLE:
  case CALL_ABANDONED:
    status = CALL_OUTCOME_UNKNOWN;
    break;
  default:
    break;
  }

done:
  owned_close(&image);
  owned_close(&bound);
  owned_close(&terminal.input);
  owned_close(&terminal.output);
  owned_close(&terminal.events);
  free(grants);
  free(directories);
  return status;
}

static void pane_dispose(struct remote_pane *pane)
{
  owned_close(&pane->reply);
  owned_close(&pane->bootstrap);
  pyxis_working_snapshot_close(&pane->cwd);
  owned_close(&pane->shell.attachment);
  owned_close(&pane->shell.process);
  owned_close(&pane->shell.group);
  *pane = (struct remote_pane){0};
}

static void pane_cancel(struct remote_broker *broker, struct remote_pane *pane)
{
  owned_close(&pane->reply);
  owned_close(&pane->bootstrap);
  pyxis_working_snapshot_close(&pane->cwd);
  pane->request.deadline = 0;
  if (pane->shell.group != HANDLE_INVALID) {
    enum call_status status = execution_group_terminate(pane->shell.group);
    if (status != CALL_OK) {
      broker->error = status;
    }
    pane->closing = true;
    /* Keep WAIT and the admission slot, even if result delivery failed. */
  } else {
    pane_dispose(pane);
  }
}

enum call_status remote_mux_launch(size_t columns, size_t rows, unsigned tab_width,
    struct remote_broker *broker, struct remote_shell *shell)
{
  *broker = (struct remote_broker){0};
  *shell = (struct remote_shell){0};
  enum call_status status = context_init(&broker->context);
  if (status != CALL_OK) {
    return status;
  }
  struct endpoint_create_reply endpoint = {0};
  status = endpoint_create(startup_resource("service"), &endpoint);
  if (status != CALL_OK) {
    return status;
  }
  broker->receiver = endpoint.receiver;
  status = broker_launch(&broker->context, columns, rows, tab_width, NULL,
      HANDLE_INVALID, endpoint.caller, 0, shell);
  owned_close(&endpoint.caller);
  return status;
}

static enum call_status broker_reply(handle_t caller, uint64_t operation,
    uint64_t id, enum call_status status, const struct endpoint_grant *grants, size_t count)
{
  struct pane_broker_reply reply = {PANE_BROKER_MAGIC, operation, id, status};
  return endpoint_send(caller, &reply, sizeof(reply), grants, count);
}

static bool begin_request(struct remote_broker *broker, struct pane_broker_message *request,
    struct endpoint_packet *packet, uint64_t now)
{
  if (packet->size != sizeof(*request) || packet->grant_count != 2 ||
      !pane_broker_grant(&packet->grants[0], PROTOCOL_ENDPOINT, HANDLE_TRANSPORT_SEND) ||
      !pane_broker_grant(&packet->grants[1], PROTOCOL_ENDPOINT, HANDLE_TRANSPORT_SEND)) {
    return false;
  }
  handle_t reply = packet->grants[0].handle;
  enum call_status status = CALL_OK;
  if (!request->id || request->id <= broker->last_id || request->directory_offset ||
      request->path_offset || request->directories || request->bytes ||
      !request->columns || request->columns > REMOTE_COLUMNS_MAX || !request->rows ||
      request->rows > REMOTE_ROWS_MAX || request->tab_width < 1 || request->tab_width > 32 ||
      (!request->directory_count && request->path_size) ||
      !pane_broker_cwd_fits(request->directory_count, request->path_size)) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK && (request->deadline <= now ||
      request->deadline - now > PANE_BROKER_TIMEOUT_NS)) {
    status = CALL_TIMED_OUT;
  }
  if (request->id > broker->last_id) {
    broker->last_id = request->id;
  }
  struct remote_pane *pane = NULL;
  if (status == CALL_OK) {
    for (unsigned i = 0; i < PANE_BROKER_PANES; ++i) {
      if (!broker->panes[i].used) {
        pane = &broker->panes[i];
        break;
      }
    }
    if (!pane) {
      status = CALL_LIMIT;
    }
  }
  if (status != CALL_OK) {
    broker_reply(reply, request->operation, request->id, status, NULL, 0);
    return true;
  }
  *pane = (struct remote_pane){.used = true, .request = *request,
      .reply = reply, .bootstrap = packet->grants[1].handle};
  packet->grants[0].handle = packet->grants[1].handle = HANDLE_INVALID;
  if (request->directory_count) {
    pane->cwd.context.directories = calloc(request->directory_count, sizeof(handle_t));
    pane->cwd.context.capacity = request->directory_count;
    pane->cwd.path = request->path_size ? malloc(request->path_size) : NULL;
    if (!pane->cwd.context.directories || (request->path_size && !pane->cwd.path)) {
      status = CALL_NO_MEMORY;
    }
  }
  enum call_status sent = broker_reply(pane->reply, request->operation, request->id, status, NULL, 0);
  if (status != CALL_OK || sent != CALL_OK) {
    pane_cancel(broker, pane);
  }
  return true;
}

static bool same_transaction(const struct pane_broker_message *request,
    const struct remote_pane *pane)
{
  const struct pane_broker_message *begin = &pane->request;
  return request->deadline == begin->deadline && request->columns == begin->columns &&
      request->rows == begin->rows && request->tab_width == begin->tab_width &&
      request->directory_count == begin->directory_count && request->path_size == begin->path_size &&
      request->directory_offset == pane->directories && request->path_offset == pane->bytes;
}

static void broker_record(struct remote_broker *broker, struct endpoint_packet *packet, uint64_t now)
{
  struct pane_broker_message request;
  if (!pane_broker_envelope(packet) || packet->size < sizeof(request)) {
    return;
  }
  memcpy(&request, packet->data, sizeof(request));
  if (request.magic != PANE_BROKER_MAGIC) {
    return;
  }
  if (request.operation == PANE_BROKER_BEGIN) {
    begin_request(broker, &request, packet, now);
    return;
  }
  struct remote_pane *pane = NULL;
  for (unsigned i = 0; i < PANE_BROKER_PANES; ++i) {
    if (broker->panes[i].used && broker->panes[i].request.id == request.id) {
      pane = &broker->panes[i];
      break;
    }
  }
  if (!pane) {
    return;
  }
  if (request.operation == PANE_BROKER_CANCEL && packet->size == sizeof(request) &&
      !packet->grant_count && !request.directories && !request.bytes) {
    pane_cancel(broker, pane);
    return;
  }
  if (pane->closing || !same_transaction(&request, pane) ||
      !pane->request.deadline || now >= pane->request.deadline) {
    pane_cancel(broker, pane);
    return;
  }
  enum call_status status = CALL_OK;
  if (request.operation == PANE_BROKER_CHUNK && !pane->published) {
    if (request.directories != packet->grant_count ||
        request.directories > pane->request.directory_count - pane->directories ||
        request.bytes > pane->request.path_size - pane->bytes ||
        request.bytes > sizeof(((struct pane_broker_chunk *)0)->path) ||
        packet->size != sizeof(request) + request.bytes ||
        (!request.directories && !request.bytes)) {
      status = CALL_BAD_REQUEST;
    }
    for (size_t i = 0; i < packet->grant_count && status == CALL_OK; ++i) {
      if (!pane_broker_grant(&packet->grants[i], PROTOCOL_DIRECTORY, 0)) {
        status = CALL_WRONG_TYPE;
      }
    }
    if (status == CALL_OK) {
      for (size_t i = 0; i < packet->grant_count; ++i) {
        pane->cwd.context.directories[pane->cwd.context.count++] = packet->grants[i].handle;
        packet->grants[i].handle = HANDLE_INVALID;
      }
      if (request.bytes) {
        memcpy(pane->cwd.path + pane->bytes, packet->data + sizeof(request), request.bytes);
      }
      pane->directories += request.directories;
      pane->bytes += request.bytes;
    }
  } else if (request.operation == PANE_BROKER_COMMIT && !pane->published &&
      packet->size == sizeof(request) && !packet->grant_count && !request.directories && !request.bytes &&
      pane->directories == pane->request.directory_count && pane->bytes == pane->request.path_size) {
    if (pane->bytes && (pane->cwd.path[pane->bytes - 1] ||
        memchr(pane->cwd.path, 0, pane->bytes - 1))) {
      status = CALL_BAD_REQUEST;
    } else {
      status = broker_launch(&broker->context, request.columns, request.rows, request.tab_width,
          pane->directories ? &pane->cwd : NULL, pane->bootstrap, HANDLE_INVALID,
          pane->request.deadline, &pane->shell);
      pane->published = status == CALL_OK;
      pyxis_working_snapshot_close(&pane->cwd);
      owned_close(&pane->bootstrap);
      /* Publication cannot become a retryable timeout or clock failure. An
       * untrustworthy launch response likewise remains OUTCOME_UNKNOWN. */
      if (status == CALL_OK) {
        uint64_t completed_at;
        enum call_status clock_status = clock_now(startup_resource("clock"), &completed_at);
        if (clock_status != CALL_OK || completed_at >= pane->request.deadline) {
          status = CALL_OUTCOME_UNKNOWN;
        }
      }
    }
  } else if (request.operation == PANE_BROKER_ACCEPT && pane->published &&
      packet->size == sizeof(request) && !packet->grant_count && !request.directories && !request.bytes) {
    enum call_status accepted = broker_reply(pane->reply, request.operation, request.id, CALL_OK, NULL, 0);
    if (accepted != CALL_OK) {
      pane_cancel(broker, pane);
      return;
    }
    pane->request.deadline = 0;
    owned_close(&pane->reply);
    owned_close(&pane->shell.attachment);
    owned_close(&pane->shell.process);
    return;
  } else {
    status = CALL_BAD_REQUEST;
  }
  struct endpoint_grant grants[3];
  size_t count = 0;
  if (status == CALL_OK && request.operation == PANE_BROKER_COMMIT) {
    grants[0] = (struct endpoint_grant){pane->shell.attachment, TERMINAL_RIGHTS, 0};
    grants[1] = (struct endpoint_grant){pane->shell.process, PROCESS_RIGHT_WAIT, 0};
    grants[2] = (struct endpoint_grant){pane->shell.group,
        EXECUTION_GROUP_RIGHT_CONTROL | EXECUTION_GROUP_RIGHT_WAIT, 0};
    count = 3;
  }
  enum call_status sent = broker_reply(pane->reply, request.operation, request.id, status, grants, count);
  if (status != CALL_OK || sent != CALL_OK) {
    pane_cancel(broker, pane);
  }
}

bool remote_broker_service(struct remote_broker *broker)
{
  bool progress = false;
  for (unsigned i = 0; i < PANE_BROKER_PANES; ++i) {
    struct remote_pane *pane = &broker->panes[i];
    if (!pane->used) {
      continue;
    }
    if (pane->group_ready) {
      enum call_status status = execution_group_wait(pane->shell.group);
      if (status == CALL_OK) {
        pane_dispose(pane);
        progress = true;
      } else {
        broker->error = status;
      }
      continue;
    }
    if (pane->request.deadline) {
      uint64_t now;
      enum call_status status = clock_now(startup_resource("clock"), &now);
      if (status != CALL_OK) {
        broker->error = status;
      } else if (now >= pane->request.deadline) {
        pane_cancel(broker, pane);
        progress = true;
      }
    }
  }
  if (broker->error != CALL_OK) {
    remote_broker_seal(broker);
  }
  if (broker->sealed || broker->receiver == HANDLE_INVALID) {
    return progress;
  }
  for (unsigned i = 0; i < 4; ++i) {
    struct endpoint_packet packet = {0};
    enum call_status status = pane_broker_receive(broker->receiver, 0, &packet);
    if (status != CALL_OK) {
      if (status != CALL_WOULD_BLOCK && status != CALL_TIMED_OUT) {
        broker->error = status;
        remote_broker_seal(broker);
      }
      break;
    }
    uint64_t now;
    status = clock_now(startup_resource("clock"), &now);
    if (status != CALL_OK) {
      pane_broker_packet_close(&packet);
      broker->error = status;
      remote_broker_seal(broker);
      break;
    }
    broker_record(broker, &packet, now);
    pane_broker_packet_close(&packet);
    progress = true;
  }
  return progress;
}

void remote_broker_seal(struct remote_broker *broker)
{
  if (broker->sealed) {
    return;
  }
  broker->sealed = true;
  /* Receiver closure discards queued SENDs and all their authority first. */
  owned_close(&broker->receiver);
  for (unsigned i = 0; i < PANE_BROKER_PANES; ++i) {
    if (broker->panes[i].used) {
      pane_cancel(broker, &broker->panes[i]);
    }
  }
}

bool remote_broker_complete(const struct remote_broker *broker)
{
  for (unsigned i = 0; i < PANE_BROKER_PANES; ++i) {
    if (broker->panes[i].used) {
      return false;
    }
  }
  return true;
}

uint64_t remote_broker_deadline(const struct remote_broker *broker, uint64_t deadline)
{
  for (unsigned i = 0; i < PANE_BROKER_PANES; ++i) {
    if (broker->panes[i].used && broker->panes[i].request.deadline &&
        broker->panes[i].request.deadline < deadline) {
      deadline = broker->panes[i].request.deadline;
    }
  }
  return deadline;
}

void remote_broker_release(struct remote_broker *broker)
{
  owned_close(&broker->receiver);
  context_close(&broker->context);
}
