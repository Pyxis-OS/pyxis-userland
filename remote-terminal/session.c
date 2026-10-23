#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <environment.h>
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
#include <abi/endpoint.h>
#include <abi/pipe.h>
#include <abi/power.h>
#include <abi/wifi.h>
#include <abi/forwarder.h>
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
#include <package_maintenance.h>
#include <program.h>
#include <shell_settings.h>
#include <startup.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <endpoint.h>
#include <clock.h>
#include <remote/terminal.h>
#include <abi/execution_group.h>
#include <abi/retention.h>

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
  {"net_registry", NET_REGISTRY_RIGHT_READ},
  {"wifi", WIFI_RIGHT_READ | WIFI_RIGHT_SCAN | WIFI_RIGHT_CONNECT},
  {"wifi_session_control", FORWARDER_RIGHT_CUT},
  /* Directory rights are copied as held; the entry is private (see below). */
  {"interpreter_root", UINT64_MAX},
};

static enum call_status directory_grant(handle_t directory, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = directory};
  return handle_rights(directory, &grant->rights, &grant->transport);
}

static enum call_status context_init(struct remote_context *context, bool multiplexer);
static void context_close(struct remote_context *context);
static enum call_status broker_launch(struct remote_context *context,
    size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, handle_t bootstrap, handle_t broker_caller,
    uint64_t deadline, bool legacy_root, bool no_echo, struct remote_shell *shell);

enum call_status remote_shell_launch(size_t columns, size_t rows, unsigned tab_width,
    bool no_echo, struct remote_shell *shell)
{
  *shell = (struct remote_shell){0};
  struct remote_context context = {0};
  enum call_status status = context_init(&context, false);
  if (status == CALL_OK) {
    status = broker_launch(&context, columns, rows, tab_width, NULL, HANDLE_INVALID,
        HANDLE_INVALID, 0, true, no_echo, shell);
  }
  context_close(&context);
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
  for (size_t i = 0; i < context->entry_count; ++i) {
    free((void *)(uintptr_t)context->entries[i].name);
  }
  free(context->grants);
  free(context->entries);
  owned_close(&context->job_provisioner);
  network_environment_free(&context->environment);
  environment_capture_close(&context->inherited);
  *context = (struct remote_context){0};
}

static enum call_status context_add(struct remote_context *context, const char *name,
    handle_t source, uint64_t rights, uint64_t transport, uint64_t flags, size_t *index)
{
  if (context->grant_count == context->capacity || context->entry_count == context->capacity) {
    return CALL_LIMIT;
  }
  char *owned_name = strdup(name);
  if (!owned_name) {
    return CALL_NO_MEMORY;
  }
  struct launch_grant *grant = &context->grants[context->grant_count];
  enum call_status status = handle_copy_restricted(source, rights, transport, &grant->source);
  if (status != CALL_OK) {
    free(owned_name);
    return status;
  }
  grant->rights = rights;
  grant->transport = transport;
  *index = context->grant_count++;
  context->entries[context->entry_count++] = (struct launch_environment_entry){
    .name = (uintptr_t)owned_name, .kind = ENVIRONMENT_CAP, .flags = flags, .value = *index,
  };
  return CALL_OK;
}

static const struct environment_value *context_capability(
    const struct environment_capture *capture, const char *name)
{
  const struct environment_value *value = environment_capture_find(capture, name);
  return value && value->kind == ENVIRONMENT_CAP ? value : NULL;
}

static enum call_status context_init(struct remote_context *context, bool multiplexer)
{
  *context = (struct remote_context){.directory = SIZE_MAX, .multiplexer = multiplexer};
  struct environment_capture selection = {0};
  enum call_status status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &selection);
  if (status != CALL_OK) {
    return status;
  }
  context->capacity = selection.count + sizeof(remote_allowed) / sizeof(remote_allowed[0]) + 1;
  context->grants = calloc(context->capacity, sizeof(*context->grants));
  context->entries = calloc(context->capacity, sizeof(*context->entries));
  if (!context->grants || !context->entries) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  struct profile_directory directories[PROFILE_DIRECTORY_LIMIT];
  size_t directory_count;
  status = profile_directories(&selection, directories, PROFILE_DIRECTORY_LIMIT, &directory_count);
  if (status != CALL_OK) {
    goto done;
  }
  for (size_t i = 0; i < directory_count; ++i) {
    uint64_t rights, transport;
    status = handle_rights(directories[i].handle, &rights, &transport);
    size_t index;
    if (status == CALL_OK) {
      status = context_add(context, directories[i].name, directories[i].handle,
          rights, transport, ENVIRONMENT_EXPORTED, &index);
    }
    if (status != CALL_OK) {
      goto done;
    }
    if (!strcmp(directories[i].name, "tmp")) {
      context->directory = index;
    }
  }
  if (context->directory == SIZE_MAX) {
    status = CALL_NOT_FOUND;
    goto done;
  }
  for (size_t i = 0; i < selection.count; ++i) {
    const struct environment_value *value = &selection.values[i];
    if (value->kind != ENVIRONMENT_CAP || !package_launch_source(value->name)) continue;
    size_t index;
    status = context_add(context, value->name, value->handle,
        value->rights, value->transport, 0, &index);
    if (status != CALL_OK) goto done;
  }
  for (size_t i = 0; i < sizeof(remote_allowed) / sizeof(remote_allowed[0]); ++i) {
    const struct environment_value *value = environment_capture_find(&selection, remote_allowed[i].name);
    bool wifi_control = !strcmp(remote_allowed[i].name, "wifi_session_control");
    if (value && wifi_control && value->kind != ENVIRONMENT_CAP) {
      status = CALL_WRONG_TYPE;
      goto done;
    }
    if (!value || value->kind != ENVIRONMENT_CAP) {
      continue;
    }
    if (wifi_control) {
      struct handle_info info;
      status = handle_query(value->handle, &info);
      if (status != CALL_OK) {
        goto done;
      }
      if (value->flags || info.kind != HANDLE_KIND_NATIVE ||
          info.protocol != PROTOCOL_FORWARDER || info.transport ||
          !(info.rights & FORWARDER_RIGHT_CUT)) {
        status = CALL_DENIED;
        goto done;
      }
    }
    size_t index;
    status = context_add(context, remote_allowed[i].name, value->handle,
        value->rights & remote_allowed[i].rights, 0,
        wifi_control || !strcmp(remote_allowed[i].name, "service") ||
            !strcmp(remote_allowed[i].name, "interpreter_root") ? 0 : ENVIRONMENT_EXPORTED, &index);
    if (status != CALL_OK) {
      goto done;
    }
  }
  const struct environment_value *value = context_capability(&selection, "retention_account");
  if (!value || value->flags) {
    status = value ? CALL_DENIED : CALL_NOT_FOUND;
    goto done;
  }
  size_t account_index;
  status = context_add(context, "retention_account", value->handle,
      RETENTION_RIGHT_CAPTURE, 0, 0, &account_index);
  if (status != CALL_OK) {
    goto done;
  }
  value = context_capability(&selection, "job_provisioner");
  if (value) {
    status = handle_copy_restricted(value->handle, JOB_PROVISIONER_RIGHTS, 0,
        &context->job_provisioner);
    if (status != CALL_OK) {
      goto done;
    }
  }
  value = context_capability(&selection, "remote_power");
  if (value) {
    size_t index;
    status = context_add(context, "power", value->handle, POWER_RIGHTS, 0,
        ENVIRONMENT_EXPORTED, &index);
    if (status != CALL_OK) {
      goto done;
    }
  }
  value = context_capability(&selection, "child_launcher");
  if (value) {
    struct handle_info info;
    status = handle_query(value->handle, &info);
    if (status != CALL_OK) {
      goto done;
    }
    if (info.protocol != PROTOCOL_LAUNCHER || !(info.rights & LAUNCHER_RIGHT_LAUNCH)) {
      status = CALL_DENIED;
      goto done;
    }
    context->allow_launch = true;
  }
  struct launch_grant *services = calloc(selection.count, sizeof(*services));
  struct launch_environment_entry *service_entries = calloc(selection.count, sizeof(*service_entries));
  if (selection.count && (!services || !service_entries)) {
    free(services);
    free(service_entries);
    status = CALL_NO_MEMORY;
    goto done;
  }
  size_t service_count = 0, service_entry_count = 0;
  status = profile_inherit_services(&selection, false, services, selection.count,
      &service_count, service_entries, selection.count, &service_entry_count);
  for (size_t i = 0; status == CALL_OK && i < service_entry_count; ++i) {
    const struct launch_environment_entry *entry = &service_entries[i];
    bool selected = false;
    for (size_t j = 0; j < context->entry_count; ++j) {
      selected |= !strcmp((const char *)(uintptr_t)entry->name,
          (const char *)(uintptr_t)context->entries[j].name);
    }
    if (selected) {
      continue;
    }
    const struct launch_grant *grant = &services[entry->value];
    size_t index;
    status = context_add(context, (const char *)(uintptr_t)entry->name, grant->source,
        grant->rights, grant->transport, ENVIRONMENT_EXPORTED, &index);
  }
  free(services);
  free(service_entries);
  if (status != CALL_OK) {
    goto done;
  }
  status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_TEXT_ONLY, &context->inherited);
  if (status == CALL_OK) {
    status = network_environment_from_capture(&context->environment,
        bootstrap_capability("net_registry"),
        bootstrap_capability(bootstrap_capability("net_registry") != HANDLE_INVALID ? "udp" : "dns_udp"),
        &context->inherited);
  }
  if (status != CALL_OK) {
    goto done;
  }
  if (context->environment.dns_udp != HANDLE_INVALID) {
    size_t index;
    status = context_add(context, "dns_udp", context->environment.dns_udp,
        UDP_SERVICE_RIGHT_OPEN, 0, ENVIRONMENT_EXPORTED, &index);
    if (status != CALL_OK) {
      goto done;
    }
  }
  size_t count = 0;
  for (size_t i = 0; i < context->environment.count; ++i) {
    const char *name = (const char *)(uintptr_t)context->environment.variables[i].name;
    if (strcmp(name, "PYXIS_REMOTE_BEACON") && strcmp(name, "PYXIS_REMOTE_MUX")) {
      context->environment.variables[count++] = context->environment.variables[i];
    }
  }
  context->environment.count = count;
  status = session_wait_environment(&context->environment, true);
  if (status != CALL_OK || !multiplexer) {
    goto done;
  }
  struct launch_environment_entry *variables = realloc(context->environment.variables,
      (context->environment.count + 1) * sizeof(*variables));
  if (!variables) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  context->environment.variables = variables;
  variables[context->environment.count++] =
      (struct launch_environment_entry)PROFILE_TEXT("PYXIS_REMOTE_MUX", "1");

done:
  environment_capture_close(&selection);
  if (status != CALL_OK) {
    context_close(context);
  }
  return status;
}

static enum call_status broker_launch(struct remote_context *context,
    size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, handle_t bootstrap, handle_t broker_caller,
    uint64_t deadline, bool legacy_root, bool no_echo, struct remote_shell *shell)
{
  *shell = (struct remote_shell){0};
  size_t depth = cwd ? cwd->context.count : 1;
  enum { INPUT, OUTPUT, LAUNCHER, STDIN, STDOUT, STDERR, EVENTS, FIRST_OPTIONAL };
  size_t fixed_grants = FIRST_OPTIONAL + context->grant_count + 5;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants ||
      depth > SIZE_MAX / sizeof(uint64_t)) {
    return CALL_LIMIT;
  }
  struct launch_grant *grants = calloc(fixed_grants + depth, sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  size_t entry_capacity = context->entry_count + context->environment.count + 11;
  struct launch_environment_entry *entries = calloc(entry_capacity, sizeof(*entries));
  if (!grants || (depth && !directories) || !entries) {
    free(entries);
    free(grants);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct terminal_create_reply terminal = {0};
  handle_t bound = HANDLE_INVALID, image = HANDLE_INVALID, settings = HANDLE_INVALID;
  enum call_status status = terminal_create(bootstrap_capability(
      context->multiplexer ? "remote_mux_terminal" : "terminal"),
      columns, rows, &terminal);
  if (status != CALL_OK) {
    goto done;
  }
  shell->attachment = terminal.attachment;
  struct execution_group_create_reply group;
  status = launcher_create_group(bootstrap_capability("launcher"), &group);
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
  if (mux) {
    /* mux is a package; the core archive keeps the rescue shell. */
    status = program_image("mux", &image);
  } else {
    handle_t boot = HANDLE_INVALID;
    for (size_t i = 0; i < context->entry_count; ++i) {
      if (!strcmp((const char *)(uintptr_t)context->entries[i].name, "boot")) {
        boot = context->grants[context->entries[i].value].source;
      }
    }
    status = directory_lookup(boot, "init-shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  }
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
  entries[0] = (struct launch_environment_entry)PROFILE_CAP("input", INPUT);
  entries[1] = (struct launch_environment_entry)PROFILE_CAP("output", OUTPUT);
  entries[2] = (struct launch_environment_entry)PROFILE_CAP("launcher", LAUNCHER);
  entries[3] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("terminal_events", EVENTS);
  entries[4] = profile_stdio_entry(ENVIRONMENT_STDIN, STDIN);
  entries[5] = profile_stdio_entry(ENVIRONMENT_STDOUT, STDOUT);
  entries[6] = profile_stdio_entry(ENVIRONMENT_STDERR, STDERR);
  size_t entry_count = 7, grant_count = FIRST_OPTIONAL;
  for (size_t i = 0; i < context->grant_count; ++i) {
    grants[grant_count++] = context->grants[i];
  }
  for (size_t i = 0; i < context->entry_count; ++i) {
    bool selected = false;
    for (size_t j = 0; j < entry_count; ++j) {
      selected |= !strcmp((const char *)(uintptr_t)context->entries[i].name,
          (const char *)(uintptr_t)entries[j].name);
    }
    if (!selected) {
      entries[entry_count] = context->entries[i];
      entries[entry_count++].value += FIRST_OPTIONAL;
    }
  }
  if (context->job_provisioner != HANDLE_INVALID) {
    status = job_work_create(context->job_provisioner, bootstrap_capability("clock"),
        &shell->job_provisioner);
    if (status != CALL_OK) {
      goto done;
    }
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("job_provisioner", grant_count);
    grants[grant_count++] = (struct launch_grant){
      job_work_directory(shell->job_provisioner), JOB_PROVISIONER_RIGHTS, 0,
    };
  }
  if (context->allow_launch) {
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP("child_launcher", grant_count);
    grants[grant_count++] = (struct launch_grant){bound, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  /* A remote mux opens the file for each of its panes; --no-echo machine
   * sessions never apply it. */
  if (!mux && !no_echo) {
    handle_t home = HANDLE_INVALID;
    for (size_t i = 0; i < context->entry_count; ++i) {
      if (context->entries[i].kind == ENVIRONMENT_CAP &&
          !strcmp((const char *)(uintptr_t)context->entries[i].name, "home")) {
        home = context->grants[context->entries[i].value].source;
      }
    }
    enum call_status opened = shell_settings_open(home, &settings);
    if (opened == CALL_OK) {
      entries[entry_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP(SHELL_SETTINGS_RESOURCE, grant_count);
      grants[grant_count++] = (struct launch_grant){settings, FILE_RIGHT_READ, 0};
    } else if (opened != CALL_NOT_FOUND) {
      fprintf(stderr, "remote: cannot open %s (status %u); shell settings skipped\n",
          SHELL_SETTINGS_PATH, opened);
    }
  }
  if (mux || bootstrap != HANDLE_INVALID) {
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP(mux ? "pane_broker" : "pane_cwd", grant_count);
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
  const char *arguments[] = {mux ? "mux" : "boot://init-shell.pxe",
    legacy_root ? (no_echo ? "--no-echo" : "--remote-prompt") : "--tab-width", width};
  for (size_t i = 0; i < context->environment.count; ++i) {
    bool selected = false;
    for (size_t j = 0; j < entry_count; ++j) {
      selected |= !strcmp((const char *)(uintptr_t)context->environment.variables[i].name,
          (const char *)(uintptr_t)entries[j].name);
    }
    if (!selected) {
      entries[entry_count++] = context->environment.variables[i];
    }
  }
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants, .grant_count = grant_count,
    .entries = (uintptr_t)entries, .entry_count = entry_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)(cwd ? cwd->path : "tmp://"),
    .argv = (uintptr_t)arguments, .argc = mux ? 3 : legacy_root ? 2 : 1,
  };
  if (deadline) {
    uint64_t now;
    status = clock_now(bootstrap_capability("clock"), &now);
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
  if (status != CALL_OK && status != CALL_OUTCOME_UNKNOWN) {
    job_work_close(shell->job_provisioner, true);
    shell->job_provisioner = NULL;
  }
  owned_close(&settings);
  owned_close(&image);
  owned_close(&bound);
  owned_close(&terminal.input);
  owned_close(&terminal.output);
  owned_close(&terminal.events);
  free(grants);
  free(directories);
  free(entries);
  return status;
}

static void pane_dispose(struct remote_pane *pane)
{
  job_work_close(pane->shell.job_provisioner, false);
  pane->shell.job_provisioner = NULL;
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
  enum call_status status = context_init(&broker->context, true);
  if (status != CALL_OK) {
    return status;
  }
  struct endpoint_create_reply endpoint = {0};
  status = endpoint_create(bootstrap_capability("service"), &endpoint);
  if (status != CALL_OK) {
    return status;
  }
  broker->receiver = endpoint.receiver;
  status = broker_launch(&broker->context, columns, rows, tab_width, NULL,
      HANDLE_INVALID, endpoint.caller, 0, false, false, shell);
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
          pane->request.deadline, false, false, &pane->shell);
      pane->published = status == CALL_OK;
      pyxis_working_snapshot_close(&pane->cwd);
      owned_close(&pane->bootstrap);
      /* Publication cannot become a retryable timeout or clock failure. An
       * untrustworthy launch response likewise remains OUTCOME_UNKNOWN. */
      if (status == CALL_OK) {
        uint64_t completed_at;
        enum call_status clock_status = clock_now(bootstrap_capability("clock"), &completed_at);
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
        status = job_work_close(pane->shell.job_provisioner, true);
        pane->shell.job_provisioner = NULL;
        if (status != CALL_OK) {
          broker->error = status;
        }
        pane_dispose(pane);
        progress = true;
      } else {
        broker->error = status;
      }
      continue;
    }
    if (pane->request.deadline) {
      uint64_t now;
      enum call_status status = clock_now(bootstrap_capability("clock"), &now);
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
    status = clock_now(bootstrap_capability("clock"), &now);
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
