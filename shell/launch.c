#include "../common/bootstrap_environment.h"
#include "../common/launch_environment.h"
#include <pyxis/environment.h>
#include <pyxis/working_path.h>
#include "shell.h"
#include "launch_capture.h"
#include "capabilities.h"
#include "../common/provider_setup.h"
#include "../common/session_wait.h"
#include <abi/directory.h>
#include <abi/console.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/thread.h>
#include <abi/display.h>
#include <abi/clock.h>
#include <abi/system_info.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/pipe.h>
#include <abi/retention.h>
#include <abi/http_transaction.h>
#include <abi/power.h>
#include <abi/screen_capture.h>
#include <abi/audio.h>
#include <abi/endpoint.h>
#include <abi/random.h>
#include <abi/net_config.h>
#include <abi/keyboard.h>
#include <abi/pointer.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <abi/terminal.h>
#include <abi/terminal_pointer.h>
#include <abi/clipboard.h>
#include <abi/wait.h>
#include <clock.h>
#include <bundle_launch.h>
#include <job_work.h>
#include <console.h>
#include <handle.h>
#include <endpoint.h>
#include <file.h>
#include <output.h>
#include <launcher.h>
#include <environment.h>
#include <forwarder.h>
#include <network_environment.h>
#include <pipe.h>
#include <directory.h>
#include <http_transaction.h>
#include <provider_http_private.h>
#include <process.h>
#include <startup.h>
#include <wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { CHILD_OUTPUT, CHILD_MEMORY, CHILD_ROOT };

struct prepared_stage {
  handle_t image;
  struct bundle_program *bundle;
  struct bundle_launch *bundle_launch;
  struct pyxis_stdio_binding default_streams[ENVIRONMENT_STREAM_COUNT];
  handle_t redirected[ENVIRONMENT_STREAM_COUNT];
  handle_t observation[ENVIRONMENT_STREAM_COUNT];
  handle_t staging_leaf;
  struct launch_grant *grants;
  uint64_t *directories;
  struct launch_environment_entry resources[37 + BUNDLE_SOURCE_LIMIT];
  char *source_names[BUNDLE_SOURCE_LIMIT];
  struct launch_environment_entry roots[PROFILE_DIRECTORY_LIMIT];
  struct environment_capture captured;
  struct launch_environment_entry *entries;
  struct launch_grant *selected_grants;
  struct launch_environment_entry *selected_entries;
  const struct environment_value **selection;
  size_t selection_count;
  const char *conflict;
  struct launch_request request;
  /* Offered to the shell interpreter only; see program_script_extras. */
  struct program_script_grant extras[2];
  size_t extra_count;
};

struct launch_scope {
  bool capture, append;
  handle_t producer;
  struct execution_group_create_reply group;
  bool evaluated, started, retired, stop_failed;
  enum call_status error;
  struct shell_service_value *service;
  const struct shell_service_root *roots;
  size_t root_count;
  char root_roles[PROVIDER_SETUP_ROOT_MAX * (PROVIDER_SETUP_ROLE_MAX + 1)];
  bool published;
};

static enum call_status publish_service_roots(struct shell *shell,
    struct launch_scope *scope, handle_t replacement_control,
    const struct provider_setup *setup, struct endpoint_packet *packet, bool read_only)
{
  size_t count = scope->roots ? scope->root_count : 1;
  size_t selected[PROVIDER_SETUP_ROOT_MAX];
  for (size_t i = 0; i < PROVIDER_SETUP_ROOT_MAX; ++i) {
    selected[i] = SIZE_MAX;
  }
  for (size_t i = 0; i < setup->root_count; ++i) {
    const char *role = setup->roles[i];
    size_t length = 0;
    while (length <= PROVIDER_SETUP_ROLE_MAX && role[length]) {
      ++length;
    }
    if (!length || length > PROVIDER_SETUP_ROLE_MAX) {
      return CALL_BAD_REQUEST;
    }
    /* Fixed strings are zero padded; reject hidden suffix bytes. */
    for (size_t j = length + 1; j <= PROVIDER_SETUP_ROLE_MAX; ++j) {
      if (role[j]) {
        return CALL_BAD_REQUEST;
      }
    }
    size_t match = count;
    for (size_t j = 0; j < count; ++j) {
      const char *declared = scope->roots ? scope->roots[j].role : PROVIDER_SETUP_DEFAULT_ROLE;
      if (!strcmp(role, declared)) {
        match = j;
      }
    }
    if (match == count || selected[match] != SIZE_MAX) {
      return CALL_BAD_REQUEST;
    }
    struct endpoint_grant *grant = &packet->grants[i];
    struct handle_info info;
    enum call_status status = handle_query(grant->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.kind != HANDLE_KIND_EXPORTED || grant->transport != HANDLE_TRANSPORT_CALL ||
        info.rights != grant->rights || info.transport != grant->transport) {
      return CALL_BAD_REQUEST;
    }
    uint64_t rights = scope->roots ? scope->roots[match].rights : UINT64_MAX;
    uint64_t transport = scope->roots ? scope->roots[match].transport : HANDLE_TRANSPORT_CALL;
    if (read_only && info.protocol == PROTOCOL_PROVIDER) {
      rights &= PROVIDER_RIGHT_OPEN_READ;
    }
    grant->rights &= rights;
    grant->transport &= transport;
    selected[match] = i;
  }
  for (size_t i = 0; i < count; ++i) {
    if (selected[i] == SIZE_MAX && (!scope->roots || !scope->roots[i].optional)) {
      return CALL_UNAVAILABLE;
    }
  }
  if (replacement_control != HANDLE_INVALID) {
    struct endpoint_grant *grant = &packet->grants[selected[0]];
    handle_t target = HANDLE_INVALID;
    enum call_status status = handle_copy_restricted(grant->handle, grant->rights,
        grant->transport, &target);
    if (status == CALL_OK) {
      status = forwarder_retarget(replacement_control, target);
      scope->published = status == CALL_OK;
    }
    if (target != HANDLE_INVALID && handle_close(target) != 0) {
      status = CALL_BAD_HANDLE;
    }
    return status;
  }
  if (!scope->roots) {
    struct endpoint_grant *grant = &packet->grants[selected[0]];
    return forwarder_slot(grant->handle, grant->rights, grant->transport,
        0, &scope->service->slot);
  }
  struct forwarder_reply slots[PROVIDER_SETUP_ROOT_MAX] = {0};
  struct environment_set_entry entries[2 * PROVIDER_SETUP_ROOT_MAX] = {0};
  size_t entry_count = 0;
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < count; ++i) {
    if (selected[i] == SIZE_MAX) {
      continue;
    }
    struct endpoint_grant *grant = &packet->grants[selected[i]];
    status = forwarder_slot(grant->handle, grant->rights, grant->transport, 0, &slots[i]);
    if (status != CALL_OK) {
      break;
    }
    char control[ENVIRONMENT_NAME_MAX + 1];
    snprintf(control, sizeof(control), "%s.control", scope->roots[i].name);
    struct environment_set_entry *holder = &entries[entry_count++];
    struct environment_set_entry *controller = &entries[entry_count++];
    environment_name(holder->name, scope->roots[i].name);
    environment_name(controller->name, control);
    holder->kind = controller->kind = ENVIRONMENT_CAP;
    holder->flags = ENVIRONMENT_EXPORTED;
    holder->set_flags = controller->set_flags = ENVIRONMENT_SET_REQUIRE_ABSENT;
    holder->handle = slots[i].holder;
    holder->rights = grant->rights;
    holder->transport = grant->transport;
    controller->handle = slots[i].controller;
    controller->rights = FORWARDER_RIGHTS;
  }
  if (status == CALL_OK) {
    status = environment_set_batch(shell->environment, entries, entry_count);
    scope->published = status == CALL_OK;
  }
  for (size_t i = 0; i < count; ++i) {
    if (slots[i].holder && handle_close(slots[i].holder)) {
      status = CALL_BAD_HANDLE;
    }
    if (slots[i].controller && handle_close(slots[i].controller)) {
      status = CALL_BAD_HANDLE;
    }
  }
  return status;
}

static bool http_uri(const char *path)
{
  return provider_http_uri(path) ||
      (!strncmp(path, "json+", 5) && provider_http_uri(path + 5));
}

static bool legacy_http_redirect(const struct shell_redirection *redirect)
{
  return !redirect->value && (redirect->capability || http_uri(redirect->path));
}

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

static enum command_result wait_session_successor(struct shell *shell,
    handle_t *child, const char *name)
{
  struct process_result completion;
  enum call_status status = process_wait(*child, &completion);
  bool closed = close_handle(child);
  if (status != CALL_OK || !closed) {
    shell_directory_error(shell, "shell: session wait", name,
        status != CALL_OK ? status : CALL_BAD_HANDLE);
    return COMMAND_FATAL;
  }
  shell->exit_status = completion.kind == PROCESS_EXITED ?
      (int)completion.exit_status : EXIT_FAILURE;
  if (completion.kind != PROCESS_EXITED &&
      shell_error(shell, "shell: session %s: Process %s\n", name,
          completion.kind == PROCESS_FAULTED ? "faulted" : "terminated") == COMMAND_FATAL) {
    return COMMAND_FATAL;
  }
  return COMMAND_EXIT;
}

static bool release_sources(struct prepared_stage *stages, size_t stage_count,
    struct pipe_create_reply *pipes)
{
  bool closed = true;
  for (size_t i = 0; i < stage_count; ++i) {
    if (!close_handle(&stages[i].staging_leaf)) {
      closed = false;
    }
    bundle_launch_close(stages[i].bundle_launch);
    stages[i].bundle_launch = NULL;
    if (stages[i].bundle) {
      bundle_program_close(stages[i].bundle);
      stages[i].bundle = NULL;
      stages[i].image = HANDLE_INVALID;
    } else if (!close_handle(&stages[i].image)) {
      closed = false;
    }
    for (size_t stream = 0; stream < ENVIRONMENT_STREAM_COUNT; ++stream) {
      if (!close_handle(&stages[i].default_streams[stream].observation)) {
        closed = false;
      }
      if (!close_handle(&stages[i].default_streams[stream].handle)) {
        closed = false;
      }
      if (!close_handle(&stages[i].observation[stream])) {
        closed = false;
      }
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
    for (size_t j = 0; j < BUNDLE_SOURCE_LIMIT; ++j) {
      free(stages[i].source_names[j]);
    }
    free(stages[i].directories);
    free(stages[i].grants);
    free(stages[i].entries);
    free(stages[i].selected_grants);
    free(stages[i].selected_entries);
    free(stages[i].selection);
    environment_capture_close(&stages[i].captured);
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

/* The internal worker's declared profile has only memory and copied streams. */
static enum call_status prepare_http_worker(struct shell *shell,
    struct prepared_stage *prepared, const struct shell_stage *stage,
    const struct pyxis_stdio_binding streams[ENVIRONMENT_STREAM_COUNT])
{
  prepared->grants = calloc(1 + ENVIRONMENT_STREAM_COUNT, sizeof(*prepared->grants));
  if (!prepared->grants) {
    return CALL_NO_MEMORY;
  }
  prepared->grants[0] = (struct launch_grant){shell->memory, MEMORY_RIGHT_MANAGE, 0};
  prepared->resources[0] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"memory", 0);
  prepared->request = (struct launch_request){
    .image = prepared->image, .grants = (uintptr_t)prepared->grants, .grant_count = 1,
    .entries = (uintptr_t)prepared->resources, .entry_count = 1,
    .argv = (uintptr_t)stage->arguments, .argc = stage->count,
  };
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    const struct pyxis_stdio_binding *stream = &streams[i];
    if (stream->protocol == 0) {
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(stream->handle, &info);
    if (status != CALL_OK || info.protocol != stream->protocol) {
      return status != CALL_OK ? status : CALL_WRONG_TYPE;
    }
    uint64_t rights = stream->protocol == PROTOCOL_FILE ? FILE_RIGHT_WRITE :
        stream->protocol == PROTOCOL_PIPE ? (i == ENVIRONMENT_STDIN ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE) :
        stream->protocol == PROTOCOL_CONSOLE ? CONSOLE_RIGHT_WRITE : 0;
    if (!rights || (i == ENVIRONMENT_STDIN && stream->protocol != PROTOCOL_PIPE) ||
        (i == ENVIRONMENT_STDOUT && stream->protocol != PROTOCOL_FILE) ||
        stream->observation || stream->observation_protocol) {
      return CALL_BAD_REQUEST;
    }
    uint64_t index = prepared->request.grant_count++;
    prepared->grants[index] = (struct launch_grant){stream->handle, rights,
      info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0};
    prepared->resources[prepared->request.entry_count++] = profile_stdio_entry(i, index);
  }
  return CALL_OK;
}

static enum call_status capture_selection(const struct shell *shell,
    struct prepared_stage *prepared, const struct shell_stage *stage,
    const struct environment_capture *exported)
{
  size_t operands = stage->operands ? stage->operands->count : 0;
  size_t limit = SIZE_MAX / sizeof(*prepared->selection);
  if (exported->count > limit || operands > limit - exported->count) {
    return CALL_LIMIT;
  }
  size_t capacity = exported->count + operands;
  if (!capacity) {
    return CALL_OK;
  }
  prepared->selection = malloc(capacity * sizeof(*prepared->selection));
  if (!prepared->selection) {
    return CALL_NO_MEMORY;
  }
  for (size_t i = 0; i < exported->count; ++i) {
    const struct environment_value *value = &exported->values[i];
    if (value->kind == ENVIRONMENT_CAP && (value->flags & ENVIRONMENT_EXPORTED) &&
        !shell_environment_initial(shell, value)) {
      prepared->selection[prepared->selection_count++] = value;
    }
  }
  for (size_t i = 0; i < operands; ++i) {
    prepared->selection[prepared->selection_count++] = &stage->operands->values[i];
  }
  return CALL_OK;
}

static enum call_status select_operands(struct prepared_stage *prepared, bool read_only)
{
  size_t count = prepared->selection_count;
  if (!count) {
    return CALL_OK;
  }
  struct launch_request *request = &prepared->request;
  if (request->grant_count > SIZE_MAX / sizeof(struct launch_grant) ||
      count > SIZE_MAX / sizeof(struct launch_grant) - request->grant_count ||
      request->entry_count > SIZE_MAX / sizeof(struct launch_environment_entry) ||
      count > SIZE_MAX / sizeof(struct launch_environment_entry) - request->entry_count) {
    return CALL_LIMIT;
  }
  size_t grant_capacity = request->grant_count + count;
  size_t entry_capacity = request->entry_count + count;
  prepared->selected_grants = malloc(grant_capacity * sizeof(*prepared->selected_grants));
  prepared->selected_entries = malloc(entry_capacity * sizeof(*prepared->selected_entries));
  if (!prepared->selected_grants || !prepared->selected_entries) {
    return CALL_NO_MEMORY;
  }
  memcpy(prepared->selected_grants, (const void *)(uintptr_t)request->grants,
      request->grant_count * sizeof(*prepared->selected_grants));
  memcpy(prepared->selected_entries, (const void *)(uintptr_t)request->entries,
      request->entry_count * sizeof(*prepared->selected_entries));
  request->grants = (uintptr_t)prepared->selected_grants;
  request->entries = (uintptr_t)prepared->selected_entries;
  for (size_t i = 0; i < count; ++i) {
    const struct environment_value *value = prepared->selection[i];
    if (value->kind != ENVIRONMENT_CAP) {
      return CALL_BAD_REQUEST;
    }
    bool repeated = false;
    for (size_t j = 0; j < i; ++j) {
      const struct environment_value *earlier = prepared->selection[j];
      if (strcmp(value->name, earlier->name)) {
        continue;
      }
      if (!value->identity || value->identity != earlier->identity ||
          value->rights != earlier->rights || value->transport != earlier->transport) {
        prepared->conflict = value->name;
        return CALL_ALREADY_EXISTS;
      }
      repeated = true;
      break;
    }
    if (repeated) {
      continue;
    }
    bool selected = false;
    for (size_t j = 0; j < request->entry_count; ++j) {
      const struct launch_environment_entry *entry = &prepared->selected_entries[j];
      if (strcmp(value->name, (const char *)(uintptr_t)entry->name)) {
        continue;
      }
      const struct environment_value *origin =
          environment_capture_find(&prepared->captured, value->name);
      const struct launch_grant *grant = entry->kind == ENVIRONMENT_CAP ?
          &prepared->selected_grants[entry->value] : NULL;
      /* A capture-owned source and publication establish provenance. Handle
       * metadata and independently acquired handle numbers establish no identity. */
      if (!origin || origin->kind != ENVIRONMENT_CAP || !value->identity ||
          origin->identity != value->identity || !grant ||
          grant->source != origin->handle || (grant->rights & ~value->rights) ||
          (grant->transport & ~value->transport)) {
        prepared->conflict = value->name;
        return CALL_ALREADY_EXISTS;
      }
      selected = true;
      break;
    }
    if (selected) {
      continue;
    }
    uint64_t rights = value->rights;
    if (read_only) {
      struct handle_info info;
      enum call_status status = handle_query(value->handle, &info);
      if (status != CALL_OK) {
        return status;
      }
      if (info.protocol == PROTOCOL_DIRECTORY) {
        rights &= DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
            DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
      }
    }
    prepared->selected_entries[request->entry_count++] = (struct launch_environment_entry){
      .name = (uintptr_t)value->name, .kind = ENVIRONMENT_CAP,
      .flags = ENVIRONMENT_EXPORTED, .value = request->grant_count,
    };
    prepared->selected_grants[request->grant_count++] = (struct launch_grant){
      value->handle, rights, value->transport,
    };
  }
  return CALL_OK;
}

static enum call_status validate_scope(const struct launch_request *request,
    const struct launch_scope *scope)
{
  if (!scope || (!scope->capture && !scope->service)) {
    return CALL_OK;
  }
  const struct launch_grant *grants = (const void *)(uintptr_t)request->grants;
  for (size_t i = 0; i < request->grant_count; ++i) {
    struct handle_info info;
    enum call_status status = handle_query(grants[i].source, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.protocol == PROTOCOL_LAUNCHER &&
        (scope->service || grants[i].source != scope->group.launcher)) {
      return CALL_DENIED;
    }
  }
  return CALL_OK;
}

/* The grant an ordinary foreground command receives as launcher. A capture's
 * stages and a script's commands receive one bound to their own subgroup, so
 * stopping that group stops everything they start. A session successor receives
 * the shell's own, which must outlive a script's group. */
static handle_t delegated_launcher(const struct shell *shell, enum shell_launch_mode mode,
    const struct launch_scope *scope)
{
  if (scope && scope->capture && shell->child_launcher != HANDLE_INVALID) {
    return scope->group.launcher;
  }
  if (mode == SHELL_FOREGROUND && shell->script_group.launcher != HANDLE_INVALID) {
    return shell->script_group.launcher;
  }
  return shell->child_launcher;
}

static enum call_status prepare_stage(struct shell *shell, struct prepared_stage *prepared,
    const struct shell_stage *stage, enum shell_launch_mode mode, bool named_input,
    const struct pyxis_stdio_binding streams[ENVIRONMENT_STREAM_COUNT], handle_t publication,
    bool read_only, const struct network_environment *environment,
    const struct environment_capture *exported, bool preflight,
    const struct launch_scope *scope)
{
  if (stage->http_worker) {
    return prepare_http_worker(shell, prepared, stage, streams);
  }
  enum call_status retired = bundle_jobs_reap(&shell->jobs);
  if (retired != CALL_OK) {
    return retired;
  }
  bool session = mode == SHELL_SESSION;
  handle_t child_launcher = delegated_launcher(shell, mode, scope);
  handle_t retention_factory = session ? bootstrap_capability("retention_factory") : HANDLE_INVALID;
  handle_t retention_account = session ? bootstrap_capability("retention_account") : HANDLE_INVALID;
  bool has_retention_factory = retention_factory != HANDLE_INVALID;
  bool has_retention_account = retention_account != HANDLE_INVALID;
  if (has_retention_factory && has_retention_account) {
    return CALL_DENIED;
  }
  /* Only the explicit trusted init/session successor receives this ancestor;
   * ordinary clients receive a newly declared leaf through bundle preparation. */
  handle_t job_provisioner = session ? bootstrap_capability("job_provisioner") : HANDLE_INVALID;
  bool has_job_provisioner = job_provisioner != HANDLE_INVALID;
  handle_t threads = session ? bootstrap_capability("threads") : HANDLE_INVALID;
  bool has_threads = threads != HANDLE_INVALID;
  bool delegate = mode == SHELL_FOREGROUND && !stage->http_producer &&
      child_launcher != HANDLE_INVALID;
  bool pass_child_launcher = session && child_launcher != HANDLE_INVALID;
  bool has_launcher = session || delegate;
  bool has_terminal = session && shell->terminal_service != HANDLE_INVALID;
  handle_t mux_terminal = session ? bootstrap_capability("mux_terminal") : HANDLE_INVALID;
  bool has_mux_terminal = mux_terminal != HANDLE_INVALID;
  handle_t remote_mux_terminal = session ? bootstrap_capability("remote_mux_terminal") : HANDLE_INVALID;
  bool has_remote_mux_terminal = remote_mux_terminal != HANDLE_INVALID;
  handle_t terminal_pointer = has_mux_terminal ? bootstrap_capability("terminal_pointer") : HANDLE_INVALID;
  bool has_terminal_pointer = terminal_pointer != HANDLE_INVALID;
  bool pass_clipboard = session || mode == SHELL_FOREGROUND;
  handle_t clipboard_local = pass_clipboard ? bootstrap_capability("clipboard_local") : HANDLE_INVALID;
  handle_t clipboard_shared = pass_clipboard ? bootstrap_capability("clipboard_shared") : HANDLE_INVALID;
  bool has_clipboard_local = clipboard_local != HANDLE_INVALID;
  bool has_clipboard_shared = clipboard_shared != HANDLE_INVALID;
  /* Only a session successor inherits power; the shell's own programs never do. */
  bool has_power = session && shell->power != HANDLE_INVALID;
  handle_t remote_power = session ? bootstrap_capability("remote_power") : HANDLE_INVALID;
  bool has_remote_power = remote_power != HANDLE_INVALID;
  bool has_screen_capture = shell->screen_capture != HANDLE_INVALID;
  bool has_audio = shell->audio != HANDLE_INVALID;
  bool provider = mode == SHELL_SERVICE;
  bool device_input = named_input &&
      streams[ENVIRONMENT_STDIN].protocol == PROTOCOL_CONSOLE;
  bool has_keyboard = device_input && shell->keyboard != HANDLE_INVALID;
  bool has_pointer = device_input && shell->pointer != HANDLE_INVALID;
  bool has_profile = shell->profile != HANDLE_INVALID;
  bool has_space = session && shell->space != HANDLE_INVALID;
  bool has_net_config = session && shell->net_config != HANDLE_INVALID;
  bool has_random = shell->random != HANDLE_INVALID;
  bool has_tcp = shell->tcp != HANDLE_INVALID;
  bool has_pipe = session && shell->pipe != HANDLE_INVALID;
  /* A script run by the shell interpreter is a shell: it needs the pipe creation
   * this one holds and recognizes delegated child launch by child_launcher. Only
   * the library, seeing the interpreter it actually opens, appends these. */
  prepared->extra_count = 0;
  if (mode == SHELL_FOREGROUND && !prepared->bundle && !stage->http_producer) {
    if (child_launcher != HANDLE_INVALID) {
      prepared->extras[prepared->extra_count++] = (struct program_script_grant){
        "child_launcher", child_launcher, LAUNCHER_RIGHT_LAUNCH, 0};
    }
    if (shell->pipe != HANDLE_INVALID) {
      prepared->extras[prepared->extra_count++] = (struct program_script_grant){
        "pipe", shell->pipe, PIPE_SERVICE_RIGHT_CREATE, 0};
    }
  }
  bool has_service = (session || provider) && shell->service != HANDLE_INVALID;
  bool has_udp = shell->udp != HANDLE_INVALID;
  bool has_udp_beacons = session && shell->udp_beacons != HANDLE_INVALID;
  bool has_echo = shell->echo != HANDLE_INVALID;
  bool has_clock = shell->clock != HANDLE_INVALID;
  bool has_system_info = !provider && shell->system_info != HANDLE_INVALID;
  bool has_log = !provider && shell->log != HANDLE_INVALID;
  bool has_display = shell->display != HANDLE_INVALID;
  enum call_status captured = environment_capture_init(shell->environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &prepared->captured);
  if (captured != CALL_OK) {
    return captured;
  }
  captured = capture_selection(shell, prepared, stage, exported);
  if (captured != CALL_OK) {
    return captured;
  }
  struct profile_directory selected_roots[PROFILE_DIRECTORY_LIMIT];
  size_t root_count;
  captured = profile_directories(&prepared->captured, selected_roots,
      PROFILE_DIRECTORY_LIMIT, &root_count);
  if (captured != CALL_OK) {
    return captured;
  }
  size_t retained = 0;
  for (size_t i = 0; i < root_count; ++i) {
    if (strcmp(selected_roots[i].name, "app")) {
      selected_roots[retained++] = selected_roots[i];
    }
  }
  root_count = retained;
  size_t directory_index = CHILD_ROOT + root_count;
  size_t depth = shell->directory->count;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - directory_index - 37 -
      BUNDLE_SOURCE_LIMIT - ENVIRONMENT_STREAM_COUNT) {
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
  size_t publication_index = input_index + (named_input ? 1 : 0);
  size_t terminal_index = publication_index + (provider ? 1 : 0);
  size_t mux_terminal_index = terminal_index + (has_terminal ? 1 : 0);
  size_t remote_mux_terminal_index = mux_terminal_index + (has_mux_terminal ? 1 : 0);
  size_t terminal_pointer_index = remote_mux_terminal_index + (has_remote_mux_terminal ? 1 : 0);
  size_t power_index = terminal_pointer_index + (has_terminal_pointer ? 1 : 0);
  size_t remote_power_index = power_index + (has_power ? 1 : 0);
  size_t screen_capture_index = remote_power_index + (has_remote_power ? 1 : 0);
  size_t audio_index = screen_capture_index + (has_screen_capture ? 1 : 0);
  size_t clipboard_local_index = audio_index + (has_audio ? 1 : 0);
  size_t clipboard_shared_index = clipboard_local_index + (has_clipboard_local ? 1 : 0);
  size_t threads_index = clipboard_shared_index + (has_clipboard_shared ? 1 : 0);
  size_t job_provisioner_index = threads_index + (has_threads ? 1 : 0);
  size_t retention_factory_index = job_provisioner_index + (has_job_provisioner ? 1 : 0);
  size_t retention_account_index = retention_factory_index + (has_retention_factory ? 1 : 0);
  size_t grant_count = retention_account_index + (has_retention_account ? 1 : 0);
  size_t grant_capacity = grant_count + ENVIRONMENT_STREAM_COUNT + 1 + BUNDLE_SOURCE_LIMIT + prepared->captured.count;
  prepared->grants = malloc(grant_capacity * sizeof(*prepared->grants));
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
    grants[CHILD_ROOT + i] = (struct launch_grant){selected_roots[i].handle, 0, 0};
    prepared->roots[i] = (struct launch_environment_entry)
        PROFILE_CAP(selected_roots[i].name, CHILD_ROOT + i);
  }
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = directory_index + i;
    grants[directory_index + i] = (struct launch_grant){shell->directory->directories[i], 0, 0};
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
        rights & LAUNCHER_RIGHT_LAUNCH, 0};
  } else if (has_launcher) {
    grants[launcher_index] = (struct launch_grant){child_launcher,
        LAUNCHER_RIGHT_LAUNCH, 0};
  }
  if (pass_child_launcher) {
    grants[child_launcher_index] = (struct launch_grant){child_launcher,
        LAUNCHER_RIGHT_LAUNCH, 0};
  }
  if (has_terminal) {
    grants[terminal_index] = (struct launch_grant){shell->terminal_service, TERMINAL_SERVICE_RIGHT_CREATE, 0};
  }
  if (has_mux_terminal) {
    uint64_t rights, transport;
    enum call_status status = handle_rights(mux_terminal, &rights, &transport);
    if (status != CALL_OK) {
      return status;
    }
    grants[mux_terminal_index] = (struct launch_grant){mux_terminal,
        rights & TERMINAL_SERVICE_RIGHT_CREATE, transport};
  }
  if (has_remote_mux_terminal) {
    grants[remote_mux_terminal_index] = (struct launch_grant){remote_mux_terminal,
        TERMINAL_SERVICE_RIGHT_CREATE, 0};
  }
  if (has_terminal_pointer) {
    grants[terminal_pointer_index] = (struct launch_grant){terminal_pointer,
        TERMINAL_POINTER_RIGHT_CONTROL, 0};
  }
  if (has_power) {
    grants[power_index] = (struct launch_grant){shell->power, POWER_RIGHTS, 0};
  }
  if (has_remote_power) {
    grants[remote_power_index] = (struct launch_grant){remote_power, POWER_RIGHTS, 0};
  }
  if (has_screen_capture) {
    grants[screen_capture_index] = (struct launch_grant){shell->screen_capture,
        SCREEN_CAPTURE_RIGHT_CAPTURE, 0};
  }
  if (has_audio) {
    grants[audio_index] = (struct launch_grant){shell->audio, AUDIO_RIGHT_PLAYBACK, 0};
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
        rights & (PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST), 0};
  }
  if (provider) {
    grants[publication_index] = (struct launch_grant){publication,
        0, HANDLE_TRANSPORT_CALL};
  }
  if (has_clipboard_local) {
    grants[clipboard_local_index] = (struct launch_grant){clipboard_local, CLIPBOARD_RIGHTS, 0};
  }
  if (has_clipboard_shared) {
    grants[clipboard_shared_index] = (struct launch_grant){clipboard_shared, CLIPBOARD_RIGHTS, 0};
  }

  if (has_threads) {
    grants[threads_index] = (struct launch_grant){threads, THREAD_RIGHT_MANAGE, 0};
  }

  if (has_job_provisioner) {
    grants[job_provisioner_index] = (struct launch_grant){
      job_provisioner, JOB_PROVISIONER_RIGHTS, 0,
    };
  }
  if (has_retention_factory) {
    grants[retention_factory_index] = (struct launch_grant){
      retention_factory, RETENTION_FACTORY_RIGHT_CREATE, 0,
    };
  }
  if (has_retention_account) {
    grants[retention_account_index] = (struct launch_grant){
      retention_account, RETENTION_RIGHT_CAPTURE, 0,
    };
  }
  struct launch_environment_entry *resources = prepared->resources;
  resources[0] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"output", CHILD_OUTPUT);
  resources[1] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"memory", CHILD_MEMORY);
  size_t resource_count = 2;
  if (has_job_provisioner) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"job_provisioner", job_provisioner_index);
  }
  if (has_threads) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"threads", threads_index);
  }
  if (named_input) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"input", input_index);
  }
  if (has_display) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"display", display_index);
  }
  if (has_clock) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"clock", clock_index);
  }
  if (has_system_info) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"system_info", system_info_index);
  }
  if (has_log) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"log", log_index);
  }
  if (has_echo) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"echo", echo_index);
  }
  if (has_udp) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"udp", udp_index);
  }
  if (has_udp_beacons) {
    resources[resource_count++] =
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"udp_beacons", udp_beacons_index);
  }
  if (has_tcp) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"tcp", tcp_index);
  }
  if (has_random) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"random", random_index);
  }
  if (has_keyboard) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"keyboard", keyboard_index);
  }
  if (has_pointer) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"pointer", pointer_index);
  }
  if (has_launcher) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"launcher", launcher_index);
  }
  if (pass_child_launcher) {
    resources[resource_count++] =
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"child_launcher", child_launcher_index);
  }
  if (has_net_config) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"net_config", net_config_index);
  }
  if (has_pipe) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"pipe", pipe_index);
  }
  if (has_service) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"service", service_index);
  }
  if (has_space) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"space", space_index);
  }
  if (has_profile) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"profile", profile_index);
  }
  if (provider) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"publication", publication_index);
    resources[resource_count++] = (struct launch_environment_entry){
      .name = (uintptr_t)PROVIDER_SETUP_ROOTS_ENTRY, .kind = ENVIRONMENT_TEXT,
      .value = (uintptr_t)scope->root_roles, .length = strlen(scope->root_roles),
    };
  }
  if (has_terminal) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"terminal", terminal_index);
  }
  if (has_mux_terminal) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"mux_terminal", mux_terminal_index);
  }
  if (has_remote_mux_terminal) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"remote_mux_terminal", remote_mux_terminal_index);
  }
  if (has_terminal_pointer) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"terminal_pointer", terminal_pointer_index);
  }
  if (has_power) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"power", power_index);
  }
  if (has_remote_power) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"remote_power", remote_power_index);
  }
  if (has_screen_capture) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"screen_capture", screen_capture_index);
  }
  if (has_audio) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"audio", audio_index);
  }
  if (has_clipboard_local) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"clipboard_local", clipboard_local_index);
  }
  if (has_clipboard_shared) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"clipboard_shared", clipboard_shared_index);
  }
  if (has_retention_factory) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("retention_factory", retention_factory_index);
  }
  if (has_retention_account) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("retention_account", retention_account_index);
  }
  if (session) {
    struct bundle_authority sources[BUNDLE_SOURCE_LIMIT];
    size_t source_count;
    enum call_status status = bundle_sources_from_capture(&prepared->captured, sources, BUNDLE_SOURCE_LIMIT,
        &source_count);
    if (status != CALL_OK) {
      return status;
    }
    if (source_count > grant_capacity - grant_count ||
        source_count > sizeof(prepared->resources) / sizeof(*resources) - resource_count) {
      return CALL_LIMIT;
    }
    for (size_t i = 0; i < source_count; ++i) {
      struct handle_info info;
      status = handle_query(sources[i].source, &info);
      if (status != CALL_OK) {
        return status;
      }
      if (info.protocol != sources[i].protocol ||
          (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED)) {
        return CALL_WRONG_TYPE;
      }
      uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
      if ((transport & info.transport) != transport) {
        return CALL_DENIED;
      }
      size_t prefix = sizeof(BUNDLE_SOURCE_PREFIX) - 1;
      size_t length = strlen(sources[i].resource);
      if (length > SIZE_MAX - prefix - 1) {
        return CALL_LIMIT;
      }
      char *name = malloc(prefix + length + 1);
      if (!name) {
        return CALL_NO_MEMORY;
      }
      prepared->source_names[i] = name;
      memcpy(name, BUNDLE_SOURCE_PREFIX, prefix);
      memcpy(name + prefix, sources[i].resource, length + 1);
      for (size_t j = 0; j < resource_count; ++j) {
        if (!strcmp(name, (const char *)(uintptr_t)resources[j].name)) {
          return CALL_BAD_REQUEST;
        }
      }
      resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)name, grant_count);
      grants[grant_count++] = (struct launch_grant){sources[i].source, info.rights, transport};
    }
  }
  struct launch_environment_entry *roots = prepared->roots;
  /* Root names and display paths do not determine delegated authority. */
  for (size_t i = 0; i < root_count; ++i) {
    struct launch_grant *grant = &grants[roots[i].value];
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

  size_t entry_capacity = resource_count + root_count + environment->count +
      prepared->captured.count + 2 * ENVIRONMENT_STREAM_COUNT + 1;
  prepared->entries = calloc(entry_capacity, sizeof(*prepared->entries));
  if (!prepared->entries) {
    return CALL_NO_MEMORY;
  }
  size_t entry_count = resource_count;
  memcpy(prepared->entries, resources, resource_count * sizeof(*prepared->entries));
  memcpy(prepared->entries + entry_count, roots, root_count * sizeof(*prepared->entries));
  entry_count += root_count;
  for (size_t i = 0; i < environment->count; ++i) {
    const struct launch_environment_entry *entry = &environment->variables[i];
    bool stream_policy = false;
    for (size_t stream = 0; stream < ENVIRONMENT_STREAM_COUNT; ++stream) {
      stream_policy |= !strcmp((const char *)(uintptr_t)entry->name,
          environment_stdio_append_name(stream));
    }
    if (!stream_policy) {
      prepared->entries[entry_count++] = *entry;
    }
  }
  if (!provider) {
    for (size_t i = 0; i < prepared->captured.count; ++i) {
      const struct environment_value *entry = &prepared->captured.values[i];
      if (entry->kind != ENVIRONMENT_CAP ||
          !strncmp(entry->name, BUNDLE_SOURCE_PREFIX, sizeof(BUNDLE_SOURCE_PREFIX) - 1) ||
          !strcmp(entry->name, "script") ||
          !strcmp(entry->name, "job_provisioner") || !strcmp(entry->name, "job_work") ||
          !strcmp(entry->name, "staging") ||
          !strcmp(entry->name, "stdin") || !strcmp(entry->name, "stdin.status") ||
          !strcmp(entry->name, "stdout") || !strcmp(entry->name, "stderr")) {
        continue;
      }
      bool selected = false;
      for (size_t j = 0; j < entry_count; ++j) {
        selected |= !strcmp(entry->name, (const char *)(uintptr_t)prepared->entries[j].name);
      }
      if (selected) {
        continue;
      }
      size_t length = strlen(entry->name);
      bool controller = session && !(entry->flags & ENVIRONMENT_EXPORTED) &&
          length > sizeof(".control") - 1 &&
          !strcmp(entry->name + length - (sizeof(".control") - 1), ".control");
      /* An unrelated private revoked holder must not poison launch. Query only
       * actual inheritance candidates; selected operands are checked separately. */
      if (!(entry->flags & ENVIRONMENT_EXPORTED) && !controller) {
        continue;
      }
      struct handle_info info;
      enum call_status status = handle_query(entry->handle, &info);
      if (status != CALL_OK) {
        return status;
      }
      controller = controller && info.protocol == PROTOCOL_FORWARDER;
      bool value = info.protocol == PROTOCOL_FILE || info.protocol == PROTOCOL_FORWARDER;
      if (!controller && (!(entry->flags & ENVIRONMENT_EXPORTED) ||
          (info.kind != HANDLE_KIND_EXPORTED && !value))) {
        continue;
      }
      prepared->entries[entry_count++] = (struct launch_environment_entry){
        .name = (uintptr_t)entry->name, .kind = ENVIRONMENT_CAP,
        .flags = controller ? 0 : ENVIRONMENT_EXPORTED, .value = grant_count,
      };
      grants[grant_count++] = (struct launch_grant){entry->handle, info.rights, info.transport};
    }
  }
  struct launch_request *request = &prepared->request;
  *request = (struct launch_request){
    .image = prepared->image,
    .grants = (uintptr_t)grants,
    .grant_count = grant_count,
    .entries = (uintptr_t)prepared->entries, .entry_count = entry_count,
    .working_directories = (uintptr_t)directories,
    .working_directory_count = depth,
    .working_path = (uintptr_t)pyxis_working_path(),
    .argv = (uintptr_t)stage->arguments,
    .argc = stage->count,
  };
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    struct pyxis_stdio_binding stream = streams[i];
    if (stream.protocol == 0) {
      continue;
    }
    bool input = i == ENVIRONMENT_STDIN;
    bool response = stream.protocol == PROTOCOL_STREAM;
    bool observation = stream.observation_protocol || stream.observation;
    if ((response && !observation) || (observation && (!input ||
        (stream.protocol != PROTOCOL_FILE && !response) ||
        stream.observation_protocol != PROTOCOL_HTTP_TRANSACTION ||
        stream.observation == HANDLE_INVALID || stream.observation == stream.handle))) {
      return CALL_BAD_REQUEST;
    }
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        stream.protocol == PROTOCOL_PIPE ?
        (input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE) :
        response ? STREAM_RIGHT_READ :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    prepared->entries[request->entry_count++] = profile_stdio_entry(i, request->grant_count);
    uint64_t transport = 0;
    if (stream.protocol == PROTOCOL_FILE || response) {
      struct handle_info info;
      enum call_status status = handle_query(stream.handle, &info);
      if (status != CALL_OK) {
        return status;
      }
      if (info.protocol != stream.protocol ||
          (info.kind != HANDLE_KIND_EXPORTED && (response || info.kind != HANDLE_KIND_NATIVE))) {
        return CALL_WRONG_TYPE;
      }
      if (info.kind == HANDLE_KIND_EXPORTED) {
        transport = HANDLE_TRANSPORT_CALL;
      }
    }
    grants[request->grant_count++] = (struct launch_grant){stream.handle, rights, transport};
    if (stream.append) {
      if (input || stream.protocol != PROTOCOL_FILE) {
        return CALL_BAD_REQUEST;
      }
      prepared->entries[request->entry_count++] = environment_stdio_append_entry(i);
    }
    if (observation) {
      prepared->entries[request->entry_count++] = (struct launch_environment_entry)
          PROFILE_CAP("stdin.status", request->grant_count);
      grants[request->grant_count++] = (struct launch_grant){
        stream.observation, HTTP_TRANSACTION_RIGHT_STATUS, HANDLE_TRANSPORT_CALL,
      };
    }
  }
  if (prepared->bundle) {
    const struct bundle_authority ordinary[] = {
      {"input", shell->terminal.input, PROTOCOL_CONSOLE},
      {"output", shell->terminal.output, PROTOCOL_CONSOLE},
      {"memory", shell->memory, PROTOCOL_MEMORY},
      {"threads", bootstrap_capability("threads"), PROTOCOL_THREAD_SERVICE},
      {"keyboard", has_keyboard ? shell->keyboard : HANDLE_INVALID, PROTOCOL_KEYBOARD},
      {"pointer", has_pointer ? shell->pointer : HANDLE_INVALID, PROTOCOL_POINTER},
      {"clock", shell->clock, PROTOCOL_CLOCK},
      {"display", shell->display, PROTOCOL_DISPLAY},
      {"audio", shell->audio, PROTOCOL_AUDIO},
      {"screen_capture", shell->screen_capture, PROTOCOL_SCREEN_CAPTURE},
      {"clipboard_local", clipboard_local, PROTOCOL_CLIPBOARD},
      {"clipboard_shared", clipboard_shared, PROTOCOL_CLIPBOARD},
      {"system_info", shell->system_info, PROTOCOL_SYSTEM_INFO},
      {"echo", shell->echo, PROTOCOL_ECHO},
      {"udp", shell->udp, PROTOCOL_UDP_SERVICE},
      {"tcp", shell->tcp, PROTOCOL_TCP_SERVICE},
      {"pipe", mode == SHELL_FOREGROUND ? shell->pipe : HANDLE_INVALID, PROTOCOL_PIPE_SERVICE},
      {"random", shell->random, PROTOCOL_RANDOM},
      {"profile", shell->profile, PROTOCOL_PROFILE},
      {"launcher", stage->http_producer ? HANDLE_INVALID : child_launcher, PROTOCOL_LAUNCHER},
      {"service", provider ? shell->service : HANDLE_INVALID, PROTOCOL_ENDPOINT_SERVICE},
      {"publication", provider ? publication : HANDLE_INVALID, PROTOCOL_ENDPOINT},
    };
    size_t ordinary_count = sizeof(ordinary) / sizeof(*ordinary);
    struct bundle_authority authority[sizeof(ordinary) / sizeof(*ordinary) + BUNDLE_SOURCE_LIMIT];
    memcpy(authority, ordinary, sizeof(ordinary));
    size_t source_count;
    enum call_status status = bundle_sources_from_capture(&prepared->captured, authority + ordinary_count,
        BUNDLE_SOURCE_LIMIT, &source_count);
    if (status != CALL_OK) {
      return status;
    }

    for (size_t i = ordinary_count; i < ordinary_count + source_count; ++i) {
      if (strcmp(authority[i].resource, "http_staging")) {
        continue;
      }
      if (!provider) {
        authority[i].source = HANDLE_INVALID;
        continue;
      }
      if (preflight) {
        continue;
      }
      uint64_t now;
      status = clock_now(shell->clock, &now);
      if (status != CALL_OK) {
        return status;
      }
      static uint64_t sequence;
      char name[64];
      snprintf(name, sizeof(name), "service-%jx-%jx", (uintmax_t)now, (uintmax_t)++sequence);
      /* Client read-only policy does not remove private backing writes needed
       * to assemble complete response snapshots. The leaf is never exported. */
      status = directory_create(authority[i].source, name, DIRECTORY_KIND_DIRECTORY,
          DIRECTORY_CONTENT_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO, &prepared->staging_leaf);
      if (status != CALL_OK) {
        /* Optional provider backing can be absent; required requests still refuse. */
        authority[i].source = HANDLE_INVALID;
      } else {
        authority[i].source = prepared->staging_leaf;
      }
    }
    status = bundle_launch_prepare_jobs(prepared->bundle, request, authority,
        ordinary_count + source_count, bootstrap_capability("job_provisioner"),
        &shell->jobs, &prepared->bundle_launch);
    if (status != CALL_OK) {
      return status;
    }
    *request = *bundle_launch_request(prepared->bundle_launch);
  }
  enum call_status status = select_operands(prepared, read_only);
  return status == CALL_OK ? validate_scope(request, scope) : status;
}

static const char *selected_stream_conflict(const struct prepared_stage *prepared,
    const struct shell_stage *stage,
    const struct pyxis_stdio_binding streams[ENVIRONMENT_STREAM_COUNT],
    size_t index, size_t count)
{
  for (size_t i = 0; i < prepared->selection_count; ++i) {
    const char *name = prepared->selection[i]->name;
    for (size_t stream = 0; stream < ENVIRONMENT_STREAM_COUNT; ++stream) {
      bool bound = streams[stream].protocol ||
          (stream == ENVIRONMENT_STDIN && index) ||
          (stream == ENVIRONMENT_STDOUT && index + 1 < count);
      bool append = streams[stream].append;
      bool response = streams[stream].observation != HANDLE_INVALID;
      for (size_t j = 0; j < stage->redirection_count; ++j) {
        const struct shell_redirection *redirect = &stage->redirections[j];
        if (redirect->stream == stream) {
          bound = true;
          append = redirect->append;
          response = redirect->observation || legacy_http_redirect(redirect);
        }
      }
      struct launch_environment_entry entry = profile_stdio_entry(stream, 0);
      if ((bound && !strcmp(name, (const char *)(uintptr_t)entry.name)) ||
          (append && !strcmp(name, environment_stdio_append_name(stream))) ||
          (stream == ENVIRONMENT_STDIN && response && !strcmp(name, "stdin.status"))) {
        return name;
      }
    }
  }
  return NULL;
}

static enum call_status preflight_operands(struct shell *shell,
    const struct shell_stage *stage, struct prepared_stage *image,
    enum shell_launch_mode mode, bool named_input,
    const struct pyxis_stdio_binding streams[ENVIRONMENT_STREAM_COUNT],
    handle_t publication, bool read_only, const struct network_environment *environment,
    const struct environment_capture *exported, size_t index, size_t count,
    const char **conflict, const struct launch_scope *scope)
{
  *conflict = NULL;
  struct prepared_stage *prepared = calloc(1, sizeof(*prepared));
  if (!prepared) {
    return CALL_NO_MEMORY;
  }
  prepared->image = image->image;
  prepared->bundle = image->bundle;
  enum call_status status = prepare_stage(shell, prepared, stage, mode, named_input,
      streams, publication, read_only, environment, exported, true, scope);
  *conflict = prepared->conflict;
  if (status == CALL_OK) {
    *conflict = selected_stream_conflict(prepared, stage, streams, index, count);
    if (*conflict) {
      status = CALL_ALREADY_EXISTS;
    }
  }
  /* The image/bundle belong to the real launch, not this temporary selection. */
  prepared->image = HANDLE_INVALID;
  prepared->bundle = NULL;
  if (!release_sources(prepared, 1, NULL)) {
    status = CALL_BAD_HANDLE;
  }
  free_preparation(prepared, 1);
  return status;
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
    size_t stage_count, handle_t armed, uint64_t deadline, bool *cancelled, struct launch_scope *scope)
{
  handle_t group = scope && scope->capture ? scope->group.supervision : HANDLE_INVALID;
  bool complete[LAUNCH_BATCH_MAX] = {0};
  size_t remaining = stage_count;
  while (remaining) {
    struct wait_interest interests[LAUNCH_BATCH_MAX + 1];
    size_t stage_of[LAUNCH_BATCH_MAX];
    size_t count = 0;
    for (size_t i = 0; i < stage_count; ++i) {
      if (!complete[i]) {
        stage_of[count] = i;
        interests[count++] = (struct wait_interest){.handle = children[i], .events = WAIT_COMPLETE};
      }
    }
    if (armed != HANDLE_INVALID) {
      interests[count] = (struct wait_interest){.handle = armed, .events = WAIT_INTERRUPT};
    }
    uint64_t now;
    enum call_status status = clock_now(shell->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    if (deadline && now >= deadline) {
      *cancelled = true;
      for (size_t i = 0; i < stage_count; ++i) {
        process_terminate(children[i]);
      }
      return CALL_TIMED_OUT;
    }
    uint64_t until = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX : now + WAIT_MAX_WAIT_NS;
    if (deadline && deadline < until) {
      until = deadline;
    }
    uint64_t events[LAUNCH_BATCH_MAX + 1] = {0};
    status = wait_many(interests, count + (armed != HANDLE_INVALID), until, events);
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
        if (deadline) {
          struct process_result result;
          status = process_wait(children[stage_of[j]], &result);
          if (status != CALL_OK || result.kind != PROCESS_EXITED || result.exit_status) {
            *cancelled = true;
            for (size_t i = 0; i < stage_count; ++i) {
              process_terminate(children[i]);
            }
            return status;
          }
        }
      }
    }
    if (armed != HANDLE_INVALID && (events[count] & WAIT_INTERRUPT)) {
      *cancelled = true;
      if (group != HANDLE_INVALID) {
        status = execution_group_terminate(group);
        scope->stop_failed = status != CALL_OK;
        return status;
      }
      for (size_t i = 0; i < stage_count; ++i) {
        status = process_terminate(children[i]);
        if (status != CALL_OK) {
          shell_error(shell, "shell: stage %zu: cannot terminate (status %u)\n", i + 1, status);
        }
      }
      return CALL_OK;
    }
    if (armed != HANDLE_INVALID && (events[count] & WAIT_ERROR)) {
      *cancelled = true;
      if (group != HANDLE_INVALID) {
        status = execution_group_terminate(group);
        scope->stop_failed = status != CALL_OK;
        return status;
      }
      for (size_t i = 0; i < stage_count; ++i) {
        process_terminate(children[i]);
      }
      return CALL_OK;
    }
  }
  return CALL_OK;
}

static enum call_status wait_capture_group(struct shell *shell, struct launch_scope *scope,
    handle_t armed, bool *cancelled)
{
  handle_t group = scope->group.supervision;
  enum call_status status = execution_group_seal(group);
  if (status != CALL_OK) {
    return status;
  }
  for (;;) {
    struct wait_interest interests[2] = {
      {.handle = group, .events = WAIT_COMPLETE},
      {.handle = armed, .events = WAIT_INTERRUPT},
    };
    uint64_t now;
    status = clock_now(shell->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    uint64_t until = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX : now + WAIT_MAX_WAIT_NS;
    uint64_t events[2] = {0};
    status = wait_many(interests, armed != HANDLE_INVALID ? 2 : 1, until, events);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (events[0] & WAIT_COMPLETE) {
      return execution_group_wait(group);
    }
    if (armed != HANDLE_INVALID && (events[1] & (WAIT_INTERRUPT | WAIT_ERROR))) {
      *cancelled = true;
      status = execution_group_terminate(group);
      if (status != CALL_OK) {
        scope->stop_failed = true;
        return status;
      }
      armed = HANDLE_INVALID;
    }
  }
}

static enum call_status receive_service_setup(struct shell *shell, handle_t process,
    handle_t receiver, handle_t armed, struct endpoint_packet *packet)
{
  uint64_t now;
  enum call_status status = clock_now(shell->clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - PROVIDER_SETUP_TIMEOUT_NS) {
    return status == CALL_OK ? CALL_LIMIT : status;
  }
  uint64_t deadline = now + PROVIDER_SETUP_TIMEOUT_NS;
  for (;;) {
    struct wait_interest interests[] = {
      {.handle = receiver, .events = WAIT_READABLE},
      {.handle = process, .events = WAIT_COMPLETE},
      {.handle = armed, .events = WAIT_INTERRUPT},
    };
    uint64_t events[3] = {0};
    status = wait_many(interests, armed != HANDLE_INVALID ? 3 : 2, deadline, events);
    if (status != CALL_OK) {
      return status;
    }
    if (events[0]) {
      status = endpoint_try_receive(receiver, packet);
      if (status != CALL_WOULD_BLOCK) {
        return status;
      }
    }
    if (events[1]) {
      return CALL_IO;
    }
    if (armed != HANDLE_INVALID && (events[2] & (WAIT_INTERRUPT | WAIT_ERROR))) {
      return CALL_ABANDONED;
    }
  }
}

static enum call_status check_redirect_inputs(const struct shell_stage *stages,
    const struct prepared_stage *prepared,
    const struct pyxis_stdio_binding streams[LAUNCH_BATCH_MAX][ENVIRONMENT_STREAM_COUNT],
    size_t stage_count, size_t *failed_stage, const char **operation, const char **path)
{
  bool has_input = false, has_output = false;
  for (size_t i = 0; i < stage_count; ++i) {
    has_input |= streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_FILE;
    has_output |= prepared[i].redirected[ENVIRONMENT_STDOUT] != HANDLE_INVALID ||
                  prepared[i].redirected[ENVIRONMENT_STDERR] != HANDLE_INVALID;
  }
  if (!has_input || !has_output) {
    return CALL_OK;
  }

  struct file_info_reply inputs[LAUNCH_BATCH_MAX];
  for (size_t i = 0; i < stage_count; ++i) {
    if (streams[i][ENVIRONMENT_STDIN].protocol != PROTOCOL_FILE) {
      continue;
    }
    *failed_stage = i;
    *operation = "shell: query stdin";
    *path = stages[i].arguments[0];
    enum call_status status = file_info(streams[i][ENVIRONMENT_STDIN].handle, &inputs[i]);
    if (status != CALL_OK) {
      return status;
    }
  }

  for (size_t i = 0; i < stage_count; ++i) {
    for (size_t j = 0; j < stages[i].redirection_count; ++j) {
      const struct shell_redirection *redirect = &stages[i].redirections[j];
      if (redirect->stream == ENVIRONMENT_STDIN) {
        continue;
      }
      if (stages[i].http_producer && legacy_http_redirect(redirect)) {
        continue;
      }
      *failed_stage = i;
      *operation = "shell: query output";
      *path = redirect->path;
      struct file_info_reply output;
      enum call_status status = file_info(prepared[i].redirected[redirect->stream], &output);
      if (status != CALL_OK) {
        return status;
      }
      for (size_t input = 0; input < stage_count; ++input) {
        if (streams[input][ENVIRONMENT_STDIN].protocol != PROTOCOL_FILE) {
          continue;
        }
        const struct file_info_reply *source = &inputs[input];
        if ((source->valid & FILE_INFO_DOMAIN_VALID) && (output.valid & FILE_INFO_DOMAIN_VALID)) {
          if (source->domain != output.domain) {
            continue;
          }
          if ((source->valid & FILE_INFO_OBJECT_VALID) && (output.valid & FILE_INFO_OBJECT_VALID)) {
            if (source->object != output.object) {
              continue;
            }
            *operation = "shell: output aliases stdin";
            return CALL_DENIED;
          }
        }
        *operation = "shell: cannot prove output differs from stdin";
        return CALL_UNAVAILABLE;
      }
    }
  }
  return CALL_OK;
}

static enum command_result launch_stages(struct shell *shell, const struct shell_stage *stages,
    size_t stage_count, enum shell_launch_mode mode, const char *service_name,
    handle_t replacement_control,
    bool optional, bool read_only, struct shell_outcome *outcome,
    struct shell_capability *body, bool automatic, struct launch_scope *scope)
{
  struct prepared_stage *prepared = calloc(stage_count, sizeof(*prepared));
  struct launch_request *requests = malloc(stage_count * sizeof(*requests));
  if (!prepared || !requests) {
    free(requests);
    free(prepared);
    if (body) {
      shell_capability_fail(shell, body, CALL_NO_MEMORY);
    }
    set_outcome(outcome, TERMINAL_COMPLETION_LAUNCH_FAILED, 0);
    if (scope) {
      scope->error = CALL_NO_MEMORY;
    }
    return shell_directory_error(shell, "shell", stages[0].arguments[0], CALL_NO_MEMORY);
  }
  struct pipe_create_reply pipes[LAUNCH_BATCH_MAX - 1] = {0};
  struct pyxis_stdio_binding streams[LAUNCH_BATCH_MAX][ENVIRONMENT_STREAM_COUNT] = {0};
  const char *operation = "shell: open";
  const char *path = stages[0].arguments[0];
  char conflicting_entry[ENVIRONMENT_NAME_MAX + 1];
  size_t failed_stage = 0;
  enum call_status status = CALL_OK;
  handle_t children[LAUNCH_BATCH_MAX] = {0};
  handle_t armed = HANDLE_INVALID;
  handle_t default_environment = HANDLE_INVALID;
  uint64_t failed_index = LAUNCH_NO_STAGE;
  bool cleanup_failure = false;
  bool cancelled = false, response_input = false;
  struct endpoint_create_reply publication = {0};
  struct network_environment environment = {0};
  struct environment_capture inherited = {0};

  if (mode == SHELL_SERVICE) {
    size_t offset = 0;
    size_t count = scope->roots ? scope->root_count : 1;
    for (size_t i = 0; i < count; ++i) {
      const char *role = scope->roots ? scope->roots[i].role : PROVIDER_SETUP_DEFAULT_ROLE;
      if (i) {
        scope->root_roles[offset++] = ',';
      }
      size_t length = strlen(role);
      memcpy(scope->root_roles + offset, role, length + 1);
      offset += length;
    }
    status = endpoint_create(shell->service, &publication);
    if (status != CALL_OK) {
      operation = "shell: publication endpoint";
      goto failed;
    }
  }

  operation = "shell: snapshot stdio";
  status = environment_snapshot(shell->environment, 0, &default_environment, NULL);
  if (status != CALL_OK) {
    goto failed;
  }

  /* Open every image before opening or creating any redirect target. */
  operation = "shell: open image";
  for (size_t i = 0; i < stage_count; ++i) {
    failed_stage = i;
    path = stages[i].arguments[0];
    status = shell_open_image(shell, path, &prepared[i].image, &prepared[i].bundle);
    if (status != CALL_OK) {
      if (scope) {
        scope->evaluated = true;
      }
      goto failed;
    }
  }
  for (size_t i = 0; i < stage_count; ++i) {
    for (size_t stream = 0; stream < ENVIRONMENT_STREAM_COUNT; ++stream) {
      bool explicit_stream = false;
      for (size_t j = 0; j < stages[i].redirection_count; ++j) {
        explicit_stream |= stages[i].redirections[j].stream == stream;
      }
      if (scope && scope->capture && i + 1 == stage_count &&
          stream == ENVIRONMENT_STDOUT && !explicit_stream) {
        streams[i][stream] = (struct pyxis_stdio_binding){
          .protocol = PROTOCOL_FILE, .handle = scope->producer, .append = scope->append,
        };
        continue;
      }
      bool override = (stream == ENVIRONMENT_STDIN &&
          (i || mode == SHELL_BACKGROUND || mode == SHELL_SERVICE)) ||
          (stream == ENVIRONMENT_STDOUT && (i + 1 < stage_count || stages[i].http_worker));
      for (size_t j = 0; j < stages[i].redirection_count; ++j) {
        override |= stages[i].redirections[j].stream == stream;
      }
      if (override) {
        continue;
      }
      failed_stage = i;
      operation = "shell: capture stdio";
      status = environment_stdio(default_environment, stream,
          &prepared[i].default_streams[stream], NULL, NULL);
      if (status == CALL_NOT_FOUND) {
        status = CALL_OK;
      }
      if (status != CALL_OK) {
        goto failed;
      }
      streams[i][stream] = prepared[i].default_streams[stream];
    }
    if (mode == SHELL_BACKGROUND || mode == SHELL_SERVICE) {
      streams[i][ENVIRONMENT_STDIN] = (struct pyxis_stdio_binding){0};
    }
  }

  operation = "shell: network environment";
  path = stages[0].arguments[0];
  status = environment_capture_init(shell->environment, 0, &inherited);
  if (status != CALL_OK) {
    goto failed;
  }
  status = network_environment_from_capture(&environment, shell->net_config,
      &inherited, NULL);
  if (status != CALL_OK) {
    goto failed;
  }
  status = session_wait_environment(&environment, mode == SHELL_SESSION && shell->session_wait);
  if (status != CALL_OK) {
    goto failed;
  }

  if (mode == SHELL_SERVICE) {
    const struct environment_value *conflict =
        environment_capture_find(&inherited, PROVIDER_SETUP_ROOTS_ENTRY);
    if (conflict) {
      operation = "service: automatic startup entry conflicts:";
      path = PROVIDER_SETUP_ROOTS_ENTRY;
      status = CALL_ALREADY_EXISTS;
      goto failed;
    }
    for (size_t i = 0; stages[0].operands && i < stages[0].operands->count; ++i) {
      if (!strcmp(stages[0].operands->values[i].name, PROVIDER_SETUP_ROOTS_ENTRY)) {
        operation = "service: automatic startup entry conflicts:";
        path = PROVIDER_SETUP_ROOTS_ENTRY;
        status = CALL_ALREADY_EXISTS;
        goto failed;
      }
    }
  }

  bool has_redirects = false;
  for (size_t i = 0; i < stage_count; ++i) {
    has_redirects |= stages[i].redirection_count != 0;
  }
  bool selected_exports = false;
  for (size_t i = 0; i < inherited.count; ++i) {
    const struct environment_value *value = &inherited.values[i];
    selected_exports |= value->kind == ENVIRONMENT_CAP &&
        !shell_environment_initial(shell, value);
  }
  /* Validate every selected CAP before any redirect target is opened. */
  for (size_t i = 0; has_redirects && i < stage_count; ++i) {
    if (!scope && !selected_exports && (!stages[i].operands || !stages[i].operands->count)) {
      continue;
    }
    failed_stage = i;
    operation = "shell: prepare";
    path = stages[i].arguments[0];
    const char *conflict = NULL;
    bool input_redirect = false;
    for (size_t j = 0; j < stages[i].redirection_count; ++j) {
      input_redirect |= stages[i].redirections[j].stream == ENVIRONMENT_STDIN;
    }
    bool console_stdin = streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_CONSOLE;
    bool pager_input = mode == SHELL_FOREGROUND && i + 1 == stage_count &&
        streams[i][ENVIRONMENT_STDOUT].protocol == PROTOCOL_CONSOLE &&
        (i || input_redirect || streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_FILE ||
         streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_PIPE ||
         streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_STREAM);
    bool named_input = mode != SHELL_BACKGROUND && mode != SHELL_SERVICE &&
        ((i == 0 && console_stdin) || pager_input);
    status = preflight_operands(shell, &stages[i], &prepared[i], mode, named_input,
        streams[i], publication.caller, read_only, &environment, &inherited,
        i, stage_count, &conflict, scope);
    if (conflict) {
      operation = "shell: CAP entry conflicts; copy the value to a different name:";
      memcpy(conflicting_entry, conflict, strlen(conflict) + 1);
      path = conflicting_entry;
    }
    if (status != CALL_OK) {
      goto failed;
    }
  }

  for (size_t i = 0; i < stage_count; ++i) {
    for (size_t j = 0; j < stages[i].redirection_count; ++j) {
      const struct shell_redirection *redirect = &stages[i].redirections[j];
      failed_stage = i;
      operation = "shell: redirect";
      path = redirect->path;
      bool input = redirect->stream == ENVIRONMENT_STDIN;
      if (!input && stages[i].http_producer &&
          legacy_http_redirect(redirect)) {
        if (automatic) {
          status = shell_capability_prepare(shell, body, path, HTTP_PREPARE_POST, NULL, 0, 30);
          if (status != CALL_OK) {
            goto failed;
          }
        }
        continue;
      }
      if (input && legacy_http_redirect(redirect)) {
        struct http_prepared opened = {0};
        struct shell_capability *entry = NULL;
        if (redirect->capability) {
          status = shell_capability_lookup(shell, path, &entry);
          if (status == CALL_OK && (!entry->value.response || !entry->value.observation)) {
            status = CALL_BAD_OPERATION;
          }
          if (status == CALL_OK) {
            status = handle_copy_restricted(entry->value.response, FILE_RIGHT_READ,
                HANDLE_TRANSPORT_CALL, &opened.response);
          }
          if (status == CALL_OK) {
            status = handle_copy_restricted(entry->value.observation, HTTP_TRANSACTION_RIGHT_STATUS,
                HANDLE_TRANSPORT_CALL, &opened.observation);
          }
        } else {
          uint64_t now;
          status = clock_now(shell->clock, &now);
          struct pyxis_response_info info;
          if (status == CALL_OK && now <= UINT64_MAX - UINT64_C(30000000000)) {
            status = http_transaction_open_uri(shell->directory, path,
                now + UINT64_C(30000000000), &opened, &info);
          } else if (status == CALL_OK) {
            status = CALL_LIMIT;
          }
        }
        if (status != CALL_OK) {
          http_prepared_close(&opened);
          goto failed;
        }
        prepared[i].redirected[redirect->stream] = opened.response;
        prepared[i].observation[redirect->stream] = opened.observation;
        streams[i][redirect->stream] = (struct pyxis_stdio_binding){.protocol = PROTOCOL_FILE,
          .handle = opened.response, .observation_protocol = PROTOCOL_HTTP_TRANSACTION,
          .observation = opened.observation};
        opened.response = opened.observation = HANDLE_INVALID;
        status = http_prepared_close(&opened);
        response_input = true;
      } else {
        struct resource_result opened = {0};
        status = shell_open_redirect_value(shell, redirect, &opened);
        if (status == CALL_OK) {
          status = resource_outcome_status(opened.info.description.outcome);
        }
        if (status != CALL_OK && scope && scope->capture) {
          scope->evaluated = true;
          if (opened.resource) {
            struct output_handoff_request owned = {.value = opened.resource};
            enum call_status handed = output_handoff(scope->producer, &owned);
            opened.resource = owned.value;
            if (handed != CALL_OK) {
              status = handed;
            }
          }
        }
        bool response = input && (opened.info.protocol == PROTOCOL_STREAM ||
            opened.info.protocol == PROTOCOL_FILE) && redirect->observation;
        if (status == CALL_OK && (response || redirect->observation)) {
          struct handle_info info;
          const struct environment_value *observation = redirect->observation;
          status = !response || observation->kind != ENVIRONMENT_CAP ? CALL_BAD_OPERATION :
              handle_query(observation->handle, &info);
          if (status == CALL_OK && (info.kind != HANDLE_KIND_EXPORTED ||
              info.protocol != PROTOCOL_HTTP_TRANSACTION)) {
            status = CALL_WRONG_TYPE;
          }
          if (status == CALL_OK && (!(info.rights & HTTP_TRANSACTION_RIGHT_STATUS) ||
              (info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL)) {
            status = CALL_DENIED;
          }
          if (status == CALL_OK) {
            status = handle_copy_restricted(observation->handle, HTTP_TRANSACTION_RIGHT_STATUS,
                HANDLE_TRANSPORT_CALL, &prepared[i].observation[redirect->stream]);
          }
        }
        if (status == CALL_OK && opened.info.protocol != PROTOCOL_FILE && !response) {
          status = CALL_BAD_OPERATION;
          operation = "shell: typed redirect requires a FILE";
        }
        if (status == CALL_OK) {
          prepared[i].redirected[redirect->stream] = opened.resource;
          opened.resource = HANDLE_INVALID;
          streams[i][redirect->stream] = (struct pyxis_stdio_binding){
            .protocol = opened.info.protocol,
            .handle = prepared[i].redirected[redirect->stream], .append = redirect->append,
            .observation_protocol = response ? PROTOCOL_HTTP_TRANSACTION : 0,
            .observation = prepared[i].observation[redirect->stream],
          };
          response_input |= response;
        }
        enum call_status closed = resource_result_close(&opened);
        if (closed != CALL_OK) {
          cleanup_failure = true;
          if (status == CALL_OK) {
            status = closed;
          }
        }
      }
      if (status != CALL_OK) {
        goto failed;
      }
    }
  }

  if (!close_handle(&default_environment)) {
    status = CALL_BAD_HANDLE;
    cleanup_failure = true;
    goto failed;
  }

  if (body) {
    size_t worker = stage_count - 1;
    status = handle_copy_restricted(shell_capability_body(body), FILE_RIGHT_WRITE,
        HANDLE_TRANSPORT_CALL, &prepared[worker].redirected[ENVIRONMENT_STDOUT]);
    if (status != CALL_OK) {
      goto failed;
    }
    streams[worker][ENVIRONMENT_STDIN] = (struct pyxis_stdio_binding){0};
    streams[worker][ENVIRONMENT_STDOUT] = (struct pyxis_stdio_binding){.protocol = PROTOCOL_FILE,
      .handle = prepared[worker].redirected[ENVIRONMENT_STDOUT]};
  }
  for (size_t i = 0; i + 1 < stage_count; ++i) {
    bool writer = prepared[i].redirected[ENVIRONMENT_STDOUT] == HANDLE_INVALID;
    bool reader = prepared[i + 1].redirected[ENVIRONMENT_STDIN] == HANDLE_INVALID;
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
      streams[i][ENVIRONMENT_STDOUT] = (struct pyxis_stdio_binding){.protocol = PROTOCOL_PIPE, .handle = pipes[i].writer};
    } else if (!close_handle(&pipes[i].writer)) {
      status = CALL_BAD_HANDLE;
      cleanup_failure = true;
      goto failed;
    }
    if (reader) {
      streams[i + 1][ENVIRONMENT_STDIN] =
          (struct pyxis_stdio_binding){.protocol = PROTOCOL_PIPE, .handle = pipes[i].reader};
    } else if (!close_handle(&pipes[i].reader)) {
      status = CALL_BAD_HANDLE;
      cleanup_failure = true;
      goto failed;
    }
  }

  for (size_t i = 0; i < stage_count; ++i) {
    response_input |= streams[i][ENVIRONMENT_STDIN].observation != HANDLE_INVALID;
  }
  /* Compare held objects after pipe wiring, before the first output resize. */
  status = check_redirect_inputs(stages, prepared, streams, stage_count,
      &failed_stage, &operation, &path);
  if (status != CALL_OK) {
    goto failed;
  }

  /* A redirected pipe side is absent from every grant, so its peer sees
   * EOF or a broken reader once the remaining owned endpoint closes. */
  for (size_t i = 0; i < stage_count; ++i) {
    failed_stage = i;
    operation = "shell: prepare";
    path = stages[i].arguments[0];
    bool console_stdin = streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_CONSOLE;
    bool pager_input = mode == SHELL_FOREGROUND && i + 1 == stage_count &&
        streams[i][ENVIRONMENT_STDOUT].protocol == PROTOCOL_CONSOLE &&
        (streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_PIPE ||
         streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_FILE ||
         streams[i][ENVIRONMENT_STDIN].protocol == PROTOCOL_STREAM);
    bool named_input = mode != SHELL_BACKGROUND && mode != SHELL_SERVICE &&
        ((i == 0 && console_stdin) || pager_input);
    status = prepare_stage(shell, &prepared[i], &stages[i], mode, named_input,
        streams[i], publication.caller, read_only, &environment, &inherited, false, scope);
    if (status != CALL_OK) {
      if (prepared[i].conflict) {
        operation = "shell: CAP entry conflicts; copy the value to a different name:";
        memcpy(conflicting_entry, prepared[i].conflict, strlen(prepared[i].conflict) + 1);
        path = conflicting_entry;
      }
      goto failed;
    }
    requests[i] = prepared[i].request;
  }

  /* All opens, pipe creation and grant preparation precede any truncation.
   * Created files and truncated contents cannot be rolled back. */
  for (size_t i = 0; i < stage_count; ++i) {
    for (size_t j = 0; j < stages[i].redirection_count; ++j) {
      const struct shell_redirection *redirect = &stages[i].redirections[j];
      if (redirect->stream == ENVIRONMENT_STDIN) {
        continue;
      }
      if (stages[i].http_producer && legacy_http_redirect(redirect)) {
        continue;
      }
      failed_stage = i;
      if (redirect->append) {
        continue;
      }
      operation = "shell: truncate";
      path = redirect->path;
      status = file_resize(prepared[i].redirected[redirect->stream], 0);
      if (status != CALL_OK) {
        goto failed;
      }
    }
  }

  if (body) {
    status = file_resize(shell_capability_body(body), 0);
    if (status != CALL_OK) {
      operation = "shell: truncate staging";
      goto failed;
    }
  }
  operation = "shell: launch";
  path = stages[0].arguments[0];
  /* Arm before launch so Ctrl+C during startup cannot reach a stage as data. */
  if (mode == SHELL_FOREGROUND || (scope && scope->service)) {
    armed = arm_interrupt(shell);
  }
  struct path_context interpreter_context = {0};
  if (scope) {
    scope->evaluated = true;
  }
  handle_t launcher = scope && scope->capture ? scope->group.launcher :
      mode == SHELL_FOREGROUND && shell->script_group.launcher != HANDLE_INVALID ?
      shell->script_group.launcher : shell->launcher;
  struct program_script_extras extras[LAUNCH_BATCH_MAX];
  for (size_t i = 0; i < stage_count; ++i) {
    extras[i] = (struct program_script_extras){
      .interpreter = SHELL_INTERPRETER, .grants = prepared[i].extras,
      .count = prepared[i].extra_count,
    };
  }
  if (stage_count == 1) {
    status = program_launch_extras(launcher, &requests[0], &interpreter_context, &extras[0],
        &children[0]);
  } else {
    status = program_launch_batch_extras(launcher, requests, stage_count,
        &interpreter_context, extras, children, &failed_index);
  }
  for (size_t i = 0; i < stage_count; ++i) {
    bundle_launch_admitted(prepared[i].bundle_launch, status, children[i]);
  }
  if (status != CALL_OK) {
    if (scope && status == CALL_OUTCOME_UNKNOWN) {
      scope->started = true;
    }
    failed_stage = failed_index == LAUNCH_NO_STAGE ? stage_count : failed_index;
    if (failed_stage < stage_count) {
      path = stages[failed_stage].arguments[0];
    }
    goto failed;
  }
  if (scope) {
    scope->started = true;
  }

  /* Release all source endpoints before waiting. Holding one writer here
   * could prevent EOF, and holding one reader could prevent EPIPE. */
  bool closed_sources = release_sources(prepared, stage_count, pipes);
  network_environment_free(&environment);
  environment_capture_close(&inherited);
  free(requests);
  free_preparation(prepared, stage_count);
  if (!closed_sources) {
    if (scope) {
      scope->error = CALL_BAD_HANDLE;
      if (scope->service) {
        scope->service->process = children[0];
        children[0] = HANDLE_INVALID;
      }
    }
    for (size_t i = 0; i < stage_count; ++i) {
      close_handle(&children[i]);
    }
    close_handle(&armed);
    if (scope && scope->service) {
      close_handle(&publication.caller);
      close_handle(&publication.receiver);
    }
    if (body) {
      shell_capability_fail(shell, body, CALL_BAD_HANDLE);
    }
    shell_directory_error(shell, "shell: close", stages[0].arguments[0], CALL_BAD_HANDLE);
    return COMMAND_FATAL;
  }
  if (mode == SHELL_SERVICE) {
    if (scope && scope->service) {
      scope->service->process = children[0];
      children[0] = HANDLE_INVALID;
    }
    struct endpoint_packet packet = {0};
    status = scope && scope->service ?
        receive_service_setup(shell, scope->service->process, publication.receiver, armed, &packet) :
        endpoint_receive(publication.receiver, &packet);
    if (status != CALL_OK) {
      if (scope) {
        scope->error = status;
      }
      bool closed = close_handle(&children[0]);
      closed = close_handle(&publication.caller) && closed;
      closed = close_handle(&publication.receiver) && closed;
      closed = close_handle(&armed) && closed;
      if (!closed) {
        if (scope) {
          scope->error = CALL_BAD_HANDLE;
        }
        return COMMAND_FATAL;
      }
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
            setup.root_count && setup.root_count <= PROVIDER_SETUP_ROOT_MAX &&
            packet.grant_count == setup.root_count) {
          published = publish_service_roots(shell, scope, replacement_control,
              &setup, &packet, read_only);
        } else if (setup.status != CALL_OK && !setup.root_count && packet.grant_count == 0) {
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
    closed = close_handle(&armed) && closed;
    if (published != CALL_OK || reply_status != CALL_OK || !closed || !finished) {
      if (scope) {
        scope->error = published != CALL_OK ? published :
            reply_status != CALL_OK ? reply_status : CALL_BAD_HANDLE;
      }
      enum command_result diagnostic = shell_error(shell,
          "service: %s %s failed (status %u, reply %u)\n",
          replacement_control != HANDLE_INVALID ? "replace" : "start",
          service_name, published, reply_status);
      return !closed || !closed_grants || !finished ? COMMAND_FATAL : diagnostic;
    }
    if (reported_failure) {
      if (scope) {
        scope->error = setup.status;
      }
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
  if (mode == SHELL_SESSION && shell->session_wait) {
    /* No prompt/input reader or Ctrl+C arming while the successor owns them. */
    return wait_session_successor(shell, &children[0], stages[0].arguments[0]);
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

  if (armed != HANDLE_INVALID || body || (scope && scope->capture)) {
    status = wait_or_interrupt(shell, children, stage_count, armed,
        body ? body->state->deadline_ns : 0, &cancelled,
        scope);
    if (status != CALL_OK) {
      /* Nothing watches the interrupt now; disarm so Ctrl+C is data again
       * rather than silently swallowed. */
      shell_error(shell, "shell: foreground wait failed (status %u)\n", status);
      if (scope && scope->capture) {
        scope->error = status;
        close_handle(&armed);
        for (size_t i = 0; i < stage_count; ++i) {
          close_handle(&children[i]);
        }
        return COMMAND_FATAL;
      }
      if (body) {
        cancelled = true;
        shell_capability_fail(shell, body, status);
        for (size_t i = 0; i < stage_count; ++i) {
          process_terminate(children[i]);
        }
      }
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
      if (scope) {
        scope->error = status != CALL_OK ? status : CALL_BAD_HANDLE;
      }
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
  if (scope && scope->capture) {
    status = wait_capture_group(shell, scope,
        cancelled ? HANDLE_INVALID : armed, &cancelled);
    if (status != CALL_OK) {
      scope->error = status;
      close_handle(&armed);
      shell_directory_error(shell, "shell: capture subtree wait", "", status);
      return COMMAND_FATAL;
    }
    scope->retired = true;
  }
  status = bundle_jobs_reap(&shell->jobs);
  if (status != CALL_OK) {
    close_handle(&armed);
    if (body) {
      shell_capability_fail(shell, body, status);
    }
    return shell_directory_error(shell, "shell: job context cleanup", "", status);
  }
  /* Disarm only after every stage finished, so no press reaches the editor. */
  if (!close_handle(&armed)) {
    shell_directory_error(shell, "shell: close", "Ctrl+C arming", CALL_BAD_HANDLE);
    return COMMAND_FATAL;
  }
  status = scope && scope->capture ? CALL_OK : term_fresh_line(&shell->terminal);
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
    if ((body || response_input || (scope && scope->capture)) && diagnostic != COMMAND_OK) {
      result = diagnostic;
    } else if (!body && !response_input && !(scope && scope->capture) && i + 1 == stage_count) {
      result = diagnostic;
    }
  }
  if (scope && scope->capture && cancelled) {
    scope->error = CALL_ABANDONED;
    set_outcome(outcome, TERMINAL_COMPLETION_TERMINATED, 0);
    return COMMAND_FAILED;
  }
  if (body && (result != COMMAND_OK || cancelled)) {
    shell_capability_fail(shell, body, cancelled ? CALL_ABANDONED : CALL_IO);
    set_outcome(outcome, TERMINAL_COMPLETION_EXITED, 1);
    return COMMAND_FAILED;
  }
  if (body && automatic) {
    result = shell_capability_commit(shell, body);
    set_outcome(outcome, TERMINAL_COMPLETION_EXITED, result == COMMAND_OK ? 0 : 1);
    return result;
  }
  const struct process_result *last = &completion[stage_count - 1];
  if (scope && scope->capture) {
    for (size_t i = 0; i < stage_count; ++i) {
      if (completion[i].kind != PROCESS_EXITED) {
        last = &completion[i];
        break;
      }
    }
  }
  switch (last->kind) {
  case PROCESS_EXITED:
    set_outcome(outcome, TERMINAL_COMPLETION_EXITED,
        result == COMMAND_FAILED && (body || response_input || (scope && scope->capture)) ?
            1 : last->exit_status);
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
  if (scope) {
    scope->error = status;
  }
  if (!close_handle(&default_environment)) {
    cleanup_failure = true;
  }
  if (body) {
    shell_capability_fail(shell, body, status);
  }
  if (!close_handle(&armed)) {
    cleanup_failure = true;
  }
  closed_sources = release_sources(prepared, stage_count, pipes);
  close_handle(&publication.caller);
  close_handle(&publication.receiver);
  network_environment_free(&environment);
  environment_capture_close(&inherited);
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
  return shell_launch_selected(shell, &stage, mode, outcome);
}

enum command_result shell_launch_selected(struct shell *shell,
    const struct shell_stage *stage, enum shell_launch_mode mode, struct shell_outcome *outcome)
{
  return launch_stages(shell, stage, 1, mode, NULL, HANDLE_INVALID, false, false, outcome, NULL, false, NULL);
}

enum command_result shell_launch_pipeline(struct shell *shell,
    const struct shell_command_line *command, struct shell_outcome *outcome)
{
  if (!command->stage_count || command->stage_count > LAUNCH_BATCH_MAX ||
      (command->background && command->stage_count != 1)) {
    return shell_error(shell, "shell: Invalid foreground pipeline\n");
  }
  const struct shell_redirection *sink = NULL;
  for (size_t i = 0; i < command->stage_count; ++i) {
    const struct shell_stage *stage = &command->stages[i];
    for (size_t j = 0; j < stage->redirection_count; ++j) {
      const struct shell_redirection *redirect = &stage->redirections[j];
      if (redirect->stream == ENVIRONMENT_STDIN || !legacy_http_redirect(redirect)) {
        continue;
      }
      if (sink || redirect->stream != ENVIRONMENT_STDOUT || i + 1 != command->stage_count) {
        return shell_error(shell, "shell: HTTP body sink must be the final stdout\n");
      }
      sink = redirect;
    }
  }
  if (command->capture && (!sink || sink->capability)) {
    return shell_error(shell, "shell: => name requires a final HTTP URI stdout\n");
  }
  if (!sink) {
    return launch_stages(shell, command->stages, command->stage_count,
        command->background ? SHELL_BACKGROUND : SHELL_FOREGROUND,
        NULL, HANDLE_INVALID, false, false, outcome, NULL, false, NULL);
  }
  if (command->background) {
    return shell_error(shell, "shell: HTTP redirection is unsupported in background commands\n");
  }
  if (sink->append) {
    return shell_error(shell, "shell: HTTP staging does not support append\n");
  }
  if (command->stage_count == LAUNCH_BATCH_MAX) {
    return shell_error(shell, "shell: HTTP staging supports at most seven producer stages\n");
  }
  if (!sink->capability && !command->capture) {
    return shell_error(shell, "shell: HTTP stdout requires => name to retain its response\n");
  }
  struct shell_capability *body;
  enum call_status status = sink->capability ?
      shell_capability_lookup(shell, sink->path, &body) :
      shell_capability_reserve(shell, command->capture, &body);
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: HTTP binding", sink->path, status);
  }
  if (sink->capability && (!shell_capability_body(body) || body->state->failure != CALL_OK)) {
    return shell_error(shell, "shell: %s is not a writable prepared request\n", sink->path);
  }
  struct shell_stage stages[LAUNCH_BATCH_MAX];
  memcpy(stages, command->stages, command->stage_count * sizeof(*stages));
  for (size_t i = 0; i < command->stage_count; ++i) {
    stages[i].http_producer = true;
  }
  char *worker_arguments[] = {"boot://http-stage.pxe", NULL};
  stages[command->stage_count] = (struct shell_stage){.arguments = worker_arguments,
      .count = 1, .http_worker = true};
  return launch_stages(shell, stages, command->stage_count + 1,
      SHELL_FOREGROUND, NULL, HANDLE_INVALID, false, false, outcome, body, !sink->capability, NULL);
}

static enum command_result launch_named_service(struct shell *shell,
    const struct shell_stage *stage, const struct shell_service_root *roots,
    size_t root_count, handle_t replacement_control, bool optional, bool read_only)
{
  struct shell_service_value owned = {0};
  struct launch_scope scope = {.service = root_count > 1 ? &owned : NULL,
    .roots = roots, .root_count = root_count};
  enum command_result result = launch_stages(shell, stage, 1, SHELL_SERVICE,
      roots[0].name, replacement_control, optional, read_only, NULL, NULL, false, &scope);
  if (!scope.published && owned.process != HANDLE_INVALID) {
    enum call_status status = process_terminate(owned.process);
    if (status == CALL_OK) {
      struct process_result completion;
      status = process_wait(owned.process, &completion);
    }
    if (status != CALL_OK) {
      shell_directory_error(shell, "service: startup cleanup", roots[0].name, status);
      result = COMMAND_FATAL;
    }
  }
  if (!close_handle(&owned.process)) {
    shell_directory_error(shell, "service: close startup", roots[0].name, CALL_BAD_HANDLE);
    result = COMMAND_FATAL;
  }
  return result;
}

enum command_result shell_launch_service(struct shell *shell, const char *name,
    bool replace, bool optional, bool read_only, char **arguments, size_t count,
    const struct environment_capture *operands,
    const struct shell_service_root *additional_roots, size_t additional_count)
{
  if (additional_count >= PROVIDER_SETUP_ROOT_MAX || (replace && additional_count)) {
    return shell_error(shell, "service: invalid root declarations\n");
  }
  struct shell_service_root roots[PROVIDER_SETUP_ROOT_MAX] = {0};
  memcpy(roots[0].role, PROVIDER_SETUP_DEFAULT_ROLE, sizeof(PROVIDER_SETUP_DEFAULT_ROLE));
  roots[0].rights = UINT64_MAX;
  roots[0].transport = HANDLE_TRANSPORT_CALL;
  if (!environment_name(roots[0].name, name)) {
    return shell_error(shell, "service: invalid name %s\n", name);
  }
  if (additional_count) {
    memcpy(roots + 1, additional_roots, additional_count * sizeof(*additional_roots));
  }
  size_t root_count = additional_count + 1;
  if (shell->service == HANDLE_INVALID || shell->clock == HANDLE_INVALID) {
    return shell_error(shell, "service: endpoint or clock authority unavailable\n");
  }
  char control_name[ENVIRONMENT_NAME_MAX + 1];
  size_t length = strlen(name);
  if (!length || length > ENVIRONMENT_NAME_MAX - (sizeof(".control") - 1)) {
    return shell_error(shell, "service: invalid name %s\n", name);
  }
  snprintf(control_name, sizeof(control_name), "%s.control", name);
  const char *names[] = {name, control_name};
  struct shell_stage stage = {.arguments = arguments, .count = count, .operands = operands};
  if (replace) {
    handle_t snapshot = HANDLE_INVALID;
    handle_t held[2] = {HANDLE_INVALID, HANDLE_INVALID};
    const char *failed_name = name;
    enum call_status status = environment_snapshot(shell->environment,
        ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &snapshot, NULL);
    for (size_t i = 0; status == CALL_OK && i < 2; ++i) {
      failed_name = names[i];
      struct environment_entry_reply entry;
      status = environment_lookup(snapshot, names[i], &entry, NULL, 0);
      if (status == CALL_BUFFER_TOO_SMALL ||
          (status == CALL_OK && entry.kind != ENVIRONMENT_CAP)) {
        status = CALL_WRONG_TYPE;
      }
      if (status == CALL_OK) {
        held[i] = entry.handle;
      }
    }
    if (status == CALL_OK) {
      struct handle_info info;
      status = handle_query(held[1], &info);
      if (status == CALL_OK && (info.protocol != PROTOCOL_FORWARDER ||
          info.kind != HANDLE_KIND_NATIVE || info.transport)) {
        status = CALL_WRONG_TYPE;
      } else if (status == CALL_OK && !(info.rights & FORWARDER_RIGHT_RETARGET)) {
        status = CALL_DENIED;
      }
      if (status == CALL_OK) {
        status = forwarder_match(held[1], held[0]);
      }
    }
    bool closed_snapshot = close_handle(&snapshot);
    if (!closed_snapshot && status == CALL_OK) {
      status = CALL_BAD_HANDLE;
    }
    /* Both grants stay owned through startup and its publication reply. A
     * later environment replacement cannot redirect this management operation. */
    enum command_result result = status == CALL_OK ?
        launch_named_service(shell, &stage, roots, root_count, held[1], optional, read_only) :
        shell_directory_error(shell, "service: replace", failed_name, status);
    bool closed = close_handle(&held[0]);
    closed = close_handle(&held[1]) && closed;
    if (!closed || !closed_snapshot) {
      shell_directory_error(shell, "service: close replacement", name, CALL_BAD_HANDLE);
      return COMMAND_FATAL;
    }
    return result;
  }
  /* Check the complete declaration before image preparation, redirects or child
   * launch. The atomic SET repeats absence checks against concurrent mutation. */
  char destinations[2 * PROVIDER_SETUP_ROOT_MAX][ENVIRONMENT_NAME_MAX + 1];
  for (size_t i = 0; i < root_count; ++i) {
    char checked_role[ENVIRONMENT_NAME_MAX + 1];
    if (!environment_name(checked_role, roots[i].role) ||
        strlen(roots[i].role) > PROVIDER_SETUP_ROLE_MAX ||
        strlen(roots[i].name) > ENVIRONMENT_NAME_MAX - (sizeof(".control") - 1)) {
      return shell_error(shell, "service: invalid root declaration\n");
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(roots[i].role, roots[j].role)) {
        return shell_error(shell, "service: duplicate root role %s\n", roots[i].role);
      }
    }
    environment_name(destinations[2 * i], roots[i].name);
    snprintf(destinations[2 * i + 1], sizeof(destinations[0]), "%s.control", roots[i].name);
  }
  for (size_t i = 0; i < 2 * root_count; ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(destinations[i], destinations[j])) {
        return shell_error(shell, "service: duplicate destination %s\n", destinations[i]);
      }
    }
    struct environment_entry_reply entry;
    enum call_status status = environment_lookup(shell->environment, destinations[i], &entry, NULL, 0);
    bool present = status == CALL_OK || status == CALL_BUFFER_TOO_SMALL;
    if (status == CALL_OK && entry.kind == ENVIRONMENT_CAP) {
      if (handle_close(entry.handle) != 0) {
        shell_directory_error(shell, "service: close", destinations[i], CALL_BAD_HANDLE);
        return COMMAND_FATAL;
      }
    }
    if (present) {
      return shell_directory_error(shell, "service", destinations[i], CALL_ALREADY_EXISTS);
    }
    if (status != CALL_NOT_FOUND) {
      return shell_directory_error(shell, "service", destinations[i], status);
    }
  }
  return launch_named_service(shell, &stage, roots, root_count, HANDLE_INVALID, optional, read_only);
}


enum command_result shell_launch_capture(struct shell *shell,
    const struct shell_command_line *command, handle_t stdout_producer,
    struct shell_outcome *outcome, bool *evaluated, bool *started,
    enum call_status *error_status)
{
  *evaluated = false;
  *started = false;
  *error_status = CALL_BAD_REQUEST;
  set_outcome(outcome, TERMINAL_COMPLETION_REJECTED, 0);
  if (!command || !command->stage_count || command->stage_count > LAUNCH_BATCH_MAX ||
      command->background || command->capture) {
    return shell_error(shell, "shell: invalid captured pipeline\n");
  }
  for (size_t i = 0; i < command->stage_count; ++i) {
    const struct shell_stage *stage = &command->stages[i];
    if (!stage->count || !stage->arguments || !stage->arguments[0][0] ||
        stage->http_worker || stage->http_producer) {
      return shell_error(shell, "shell: invalid capture stage\n");
    }
    for (size_t j = 0; j < stage->redirection_count; ++j) {
      const struct shell_redirection *redirect = &stage->redirections[j];
      if (redirect->stream != ENVIRONMENT_STDIN && legacy_http_redirect(redirect)) {
        *error_status = CALL_BAD_OPERATION;
        return shell_error(shell, "shell: legacy HTTP output is unsupported in capture\n");
      }
    }
  }
  struct output_status_reply producer_status;
  enum call_status status = output_producer_status(stdout_producer, &producer_status);
  if (status != CALL_OK) {
    *error_status = status;
    return shell_directory_error(shell, "shell: capture output", "", status);
  }
  struct launch_scope scope = {.capture = true, .producer = stdout_producer,
      /* Child FILE descriptors do not share a userspace cursor. The append
       * policy also follows descendant launch, avoiding offset-zero overwrite. */
      .append = producer_status.mode != OUTPUT_MODE_VALUE};
  handle_t launcher = shell->script_group.launcher != HANDLE_INVALID ?
      shell->script_group.launcher : shell->child_launcher != HANDLE_INVALID ?
      shell->child_launcher : shell->launcher;
  status = launcher_create_group(launcher, &scope.group);
  if (status != CALL_OK) {
    *error_status = status;
    return status == CALL_OUTCOME_UNKNOWN ? COMMAND_FATAL :
        shell_directory_error(shell, "shell: capture group", "", status);
  }
  enum command_result result = launch_stages(shell, command->stages, command->stage_count,
      SHELL_FOREGROUND, NULL, HANDLE_INVALID, false, false, outcome, NULL, false, &scope);
  if (!scope.retired) {
    status = scope.stop_failed ? CALL_OUTCOME_UNKNOWN :
        execution_group_terminate(scope.group.supervision);
    if (status == CALL_OK) {
      status = execution_group_wait(scope.group.supervision);
    }
    if (status != CALL_OK) {
      scope.error = status;
      result = COMMAND_FATAL;
      shell_directory_error(shell, "shell: capture group cleanup", "", status);
    }
  }
  bool closed = close_handle(&scope.group.launcher);
  closed = close_handle(&scope.group.supervision) && closed;
  if (!closed) {
    scope.error = CALL_BAD_HANDLE;
    result = COMMAND_FATAL;
    shell_directory_error(shell, "shell: close capture group", "", CALL_BAD_HANDLE);
  }
  *evaluated = scope.evaluated;
  *started = scope.started;
  *error_status = scope.error;
  return result;
}

enum command_result shell_launch_service_value(struct shell *shell,
    const struct shell_stage *stage, bool read_only, struct shell_service_value *owned,
    bool *started, enum call_status *error_status)
{
  *owned = (struct shell_service_value){0};
  *started = false;
  *error_status = CALL_BAD_REQUEST;
  if (!stage || !stage->count || !stage->arguments || !stage->arguments[0][0]) {
    return shell_error(shell, "service: missing provider image\n");
  }
  if (shell->service == HANDLE_INVALID || shell->clock == HANDLE_INVALID) {
    *error_status = CALL_UNAVAILABLE;
    return shell_error(shell, "service: endpoint or clock authority unavailable\n");
  }
  struct launch_scope scope = {.service = owned};
  enum command_result result = launch_stages(shell, stage, 1, SHELL_SERVICE,
      "captured service", HANDLE_INVALID, false, read_only, NULL, NULL, false, &scope);
  owned->evaluated = scope.evaluated;
  *started = scope.started;
  *error_status = scope.error;
  if (result == COMMAND_OK) {
    return result;
  }
  if (owned->process != HANDLE_INVALID) {
    enum call_status status = process_terminate(owned->process);
    if (status == CALL_OK) {
      struct process_result completion;
      status = process_wait(owned->process, &completion);
    }
    if (status != CALL_OK) {
      *error_status = status;
      result = COMMAND_FATAL;
      shell_directory_error(shell, "service: startup cleanup", stage->arguments[0], status);
    }
  }
  bool closed = close_handle(&owned->process);
  closed = close_handle(&owned->slot.holder) && closed;
  closed = close_handle(&owned->slot.controller) && closed;
  if (!closed) {
    *error_status = CALL_BAD_HANDLE;
    result = COMMAND_FATAL;
    shell_directory_error(shell, "service: close startup", stage->arguments[0], CALL_BAD_HANDLE);
  }
  return result;
}

enum call_status shell_script_group_open(struct shell *shell)
{
  /* Without a child_launcher nothing is delegated, so there is nothing to contain. */
  if (shell->child_launcher == HANDLE_INVALID) {
    return CALL_OK;
  }
  enum call_status status = launcher_create_group(shell->child_launcher, &shell->script_group);
  if (status != CALL_OK) {
    shell_directory_error(shell, "shell", "script group", status);
  }
  return status;
}

bool shell_script_group_close(struct shell *shell)
{
  struct execution_group_create_reply *group = &shell->script_group;
  if (group->supervision == HANDLE_INVALID) {
    return true;
  }
  enum call_status status = execution_group_terminate(group->supervision);
  if (status == CALL_OK) {
    status = execution_group_wait(group->supervision);
  }
  bool closed = close_handle(&group->launcher);
  closed = close_handle(&group->supervision) && closed;
  if (status != CALL_OK) {
    shell_directory_error(shell, "shell", "script subtree cleanup", status);
    return false;
  }
  if (!closed) {
    shell_directory_error(shell, "shell", "close script group", CALL_BAD_HANDLE);
    return false;
  }
  return true;
}
