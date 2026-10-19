#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <environment.h>
#include <pyxis/working_path.h>
#include "session.h"
#include "../common/pane_broker.h"
#include <abi/clock.h>
#include <abi/clipboard.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/thread.h>
#include <abi/keyboard.h>
#include <abi/net_config.h>
#include <abi/pipe.h>
#include <abi/retention.h>
#include <abi/pointer.h>
#include <abi/power.h>
#include <abi/wifi.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/audio.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <console.h>
#include <bundle_launch.h>
#include <directory.h>
#include <endpoint.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <wait.h>
#include <stdlib.h>

static enum call_status copy_grant(handle_t source, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = source};
  return handle_rights(source, &grant->rights, &grant->transport);
}

void mux_session_release(struct mux_session *session)
{
  if (session->cwd_caller != HANDLE_INVALID) {
    handle_close(session->cwd_caller);
  }
  if (session->cwd_bootstrap != HANDLE_INVALID) {
    handle_close(session->cwd_bootstrap);
  }
  if (session->group != HANDLE_INVALID) {
    execution_group_terminate(session->group);
  }
  bool retired = false;
  if (session->group != HANDLE_INVALID) {
    struct wait_interest interest = {.handle = session->group, .events = WAIT_COMPLETE};
    uint64_t events;
    retired = wait_many(&interest, 1, 0, &events) == CALL_OK &&
        (events & WAIT_COMPLETE) && execution_group_wait(session->group) == CALL_OK;
  }
  job_work_close(session->job_provisioner, retired);
  if (session->process != HANDLE_INVALID) {
    handle_close(session->process);
  }
  if (session->attachment != HANDLE_INVALID) {
    handle_close(session->attachment);
  }
  if (session->group != HANDLE_INVALID) {
    handle_close(session->group);
  }
  *session = (struct mux_session){0};
}

enum call_status mux_session_start(size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, struct mux_session *session)
{
  *session = (struct mux_session){0};
  if (bootstrap_capability("pane_broker") != HANDLE_INVALID) {
    struct endpoint_create_reply bootstrap = {0};
    enum call_status status = endpoint_create(bootstrap_capability("service"), &bootstrap);
    if (status != CALL_OK) {
      return status;
    }
    struct pane_broker_result result = {0};
    status = pane_broker_start(columns, rows, tab_width, cwd, bootstrap.caller, &result);
    handle_close(bootstrap.caller);
    if (status != CALL_OK) {
      handle_close(bootstrap.receiver);
      return status;
    }
    session->attachment = result.attachment;
    session->process = result.process;
    session->group = result.group;
    session->cwd_bootstrap = bootstrap.receiver;
    return CALL_OK;
  }
  const struct path_context *context;
  enum call_status status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    return status;
  }
  if (cwd) {
    context = &cwd->context;
  }
  size_t depth = context->count;
  enum { INPUT, OUTPUT, LAUNCHER, STDIN, STDOUT, STDERR, EVENTS, FIRST_OPTIONAL };
  enum { OPTIONAL_RESOURCE_COUNT = 27 };
  struct environment_capture capture = {0};
  status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
  if (status != CALL_OK) {
    return status;
  }
  size_t fixed_grants = FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT +
      capture.count + BUNDLE_SOURCE_LIMIT;
  size_t entry_capacity = FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT +
      capture.count + BUNDLE_SOURCE_LIMIT;
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants ||
      depth > SIZE_MAX / sizeof(uint64_t)) {
    environment_capture_close(&capture);
    return CALL_LIMIT;
  }
  struct launch_grant *grants = malloc((fixed_grants + depth) * sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  struct launch_environment_entry *entries = calloc(entry_capacity, sizeof(*entries));
  if (!grants || (depth && !directories) || !entries) {
    environment_capture_close(&capture);
    free(entries);
    free(grants);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct terminal_create_reply terminal = {0};
  handle_t bound = HANDLE_INVALID, image = HANDLE_INVALID;
  bool admission_attempted = false;
  struct endpoint_create_reply bootstrap = {0};
  status = terminal_create(bootstrap_capability("terminal"), columns, rows,
      &terminal);
  if (status != CALL_OK) {
    goto done;
  }
  session->attachment = terminal.attachment;
  status = console_set_tab_width(terminal.output, tab_width);
  if (status != CALL_OK) {
    goto done;
  }
  struct execution_group_create_reply group;
  status = launcher_create_group(bootstrap_capability("launcher"), &group);
  if (status != CALL_OK) {
    goto done;
  }
  session->group = group.supervision;
  bound = group.launcher;
  const struct environment_value *boot_entry = environment_capture_find(&capture, "boot");
  handle_t boot = boot_entry && boot_entry->kind == ENVIRONMENT_CAP ?
      boot_entry->handle : HANDLE_INVALID;
  if (boot == HANDLE_INVALID) {
    status = CALL_NOT_FOUND;
    goto done;
  }
  status = directory_lookup(boot, "init-shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }
  grants[INPUT] = (struct launch_grant){terminal.input,
      CONSOLE_RIGHT_READ | CONSOLE_RIGHT_INTERRUPT, 0};
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
  size_t grant_count = FIRST_OPTIONAL, entry_count = 7;
  if (bootstrap_capability("service") != HANDLE_INVALID &&
      endpoint_create(bootstrap_capability("service"), &bootstrap) == CALL_OK) {
    session->cwd_bootstrap = bootstrap.receiver;
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("pane_cwd", grant_count);
    grants[grant_count++] = (struct launch_grant){bootstrap.caller, 0, HANDLE_TRANSPORT_SEND};
  }
  handle_t provisioner = bootstrap_capability("job_provisioner");
  if (provisioner != HANDLE_INVALID) {
    status = job_work_create(provisioner, bootstrap_capability("clock"), &session->job_provisioner);
    if (status != CALL_OK) {
      goto done;
    }
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("job_provisioner", grant_count);
    grants[grant_count++] = (struct launch_grant){
      job_work_directory(session->job_provisioner), JOB_PROVISIONER_RIGHTS, 0,
    };
  }
  handle_t child_launcher = bootstrap_capability("child_launcher");
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
    /* The opt-in controls delegation; only this group's launcher may enter it. */
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP("child_launcher", grant_count);
    grants[grant_count++] = (struct launch_grant){bound, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  handle_t interpreter_root = bootstrap_capability("interpreter_root");
  if (interpreter_root != HANDLE_INVALID) {
    struct launch_grant root = {.source = interpreter_root};
    if (handle_rights(root.source, &root.rights, &root.transport) == CALL_OK) {
      entries[entry_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("interpreter_root", grant_count);
      grants[grant_count++] = root;
    }
  }
  static const struct {
    const char *name;
    uint64_t rights;
  } allowed[] = {
    {"memory", MEMORY_RIGHT_MANAGE}, {"threads", THREAD_RIGHT_MANAGE},
    {"clock", CLOCK_RIGHTS},
    {"system_info", SYSTEM_INFO_RIGHT_READ}, {"log", LOG_RIGHT_READ},
    {"screen_capture", SCREEN_CAPTURE_RIGHT_CAPTURE},
    {"audio", AUDIO_RIGHT_PLAYBACK},
    /* Graphics remain a shared space layer, never a pane surface. */
    {"display", DISPLAY_RIGHT_DRAW}, {"keyboard", KEYBOARD_RIGHT_INPUT},
    {"pointer", POINTER_RIGHT_INPUT}, {"power", POWER_RIGHTS},
    /* Store grants permit owned graphics actions, never terminal CONTROL. */
    {"clipboard_local", CLIPBOARD_RIGHTS}, {"clipboard_shared", CLIPBOARD_RIGHTS},
    {"pipe", PIPE_SERVICE_RIGHT_CREATE}, {"service", ENDPOINT_SERVICE_RIGHT_CREATE},
    {"retention_account", RETENTION_RIGHT_CAPTURE},
    {"tcp", TCP_SERVICE_RIGHTS}, {"random", RANDOM_RIGHT_READ},
    {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST},
    {"echo", ECHO_RIGHT_SEND}, {"udp", UDP_SERVICE_RIGHT_OPEN},
    {"dns_udp", UDP_SERVICE_RIGHT_OPEN},
    {"net_registry", NET_REGISTRY_RIGHT_READ},
    {"wifi", WIFI_RIGHT_READ | WIFI_RIGHT_SCAN},
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
    const struct environment_value *value = environment_capture_find(&capture, allowed[i].name);
    handle_t source = value && value->kind == ENVIRONMENT_CAP ? value->handle : HANDLE_INVALID;
    if (source == HANDLE_INVALID) {
      continue;
    }
    status = copy_grant(source, &grants[grant_count]);
    if (status != CALL_OK) {
      goto done;
    }
    grants[grant_count].rights &= allowed[i].rights;
    entries[entry_count++] = (struct launch_environment_entry){
      .name = (uintptr_t)allowed[i].name, .kind = ENVIRONMENT_CAP,
      .flags = !strcmp(allowed[i].name, "service") || !strcmp(allowed[i].name, "retention_account") ?
          0 : ENVIRONMENT_EXPORTED,
      .value = grant_count++,
    };
  }
  status = bundle_sources_forward_capture(&capture, grants, fixed_grants + depth, &grant_count,
      entries, entry_capacity, &entry_count);
  if (status != CALL_OK) {
    goto done;
  }
  struct profile_directory selected_directories[PROFILE_DIRECTORY_LIMIT];
  size_t directory_count;
  status = profile_directories(&capture, selected_directories,
      PROFILE_DIRECTORY_LIMIT, &directory_count);
  if (status != CALL_OK) {
    goto done;
  }
  for (size_t i = 0; i < directory_count; ++i) {
    bool selected = false;
    for (size_t j = 0; j < entry_count; ++j) {
      selected |= !strcmp(selected_directories[i].name, (const char *)(uintptr_t)entries[j].name);
    }
    if (selected) {
      continue;
    }
    entries[entry_count++] = (struct launch_environment_entry)
        PROFILE_CAP(selected_directories[i].name, grant_count);
    status = copy_grant(selected_directories[i].handle, &grants[grant_count++]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  status = profile_inherit_services(&capture, true, grants, fixed_grants + depth, &grant_count,
      entries, entry_capacity, &entry_count);
  if (status != CALL_OK) {
    goto done;
  }
  for (size_t i = 0; i < capture.count; ++i) {
    const struct environment_value *value = &capture.values[i];
    if (value->kind != ENVIRONMENT_TEXT) {
      continue;
    }
    bool selected = false;
    for (size_t j = 0; j < entry_count; ++j) {
      selected |= !strcmp(value->name, (const char *)(uintptr_t)entries[j].name);
    }
    if (!selected) {
      entries[entry_count++] = (struct launch_environment_entry){
        .name = (uintptr_t)value->name, .kind = ENVIRONMENT_TEXT, .flags = value->flags,
        .value = (uintptr_t)value->text, .length = value->length,
      };
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = grant_count;
    status = copy_grant(context->directories[i], &grants[grant_count++]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  const char *arguments[] = {"boot://init-shell.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = grant_count,
    .entries = (uintptr_t)entries, .entry_count = entry_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)(cwd ? cwd->path : pyxis_working_path()),
    .argv = (uintptr_t)arguments, .argc = 1,
  };
  admission_attempted = true;
  status = launcher_launch(bound, &request, &session->process);

done:
  if (bootstrap.caller != HANDLE_INVALID) {
    handle_close(bootstrap.caller);
  }
  environment_capture_close(&capture);
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  if (bound != HANDLE_INVALID) {
    handle_close(bound);
  }
  if (terminal.input != HANDLE_INVALID) {
    handle_close(terminal.input);
    handle_close(terminal.output);
    handle_close(terminal.events);
  }
  free(grants);
  free(directories);
  free(entries);
  if (status != CALL_OK) {
    bool uncertain = admission_attempted && status == CALL_OUTCOME_UNKNOWN;
    if (!uncertain) {
      job_work_close(session->job_provisioner, true);
      session->job_provisioner = NULL;
    }
    mux_session_release(session);
  }
  return status;
}
