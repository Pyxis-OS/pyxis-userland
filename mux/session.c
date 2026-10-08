#include "session.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/keyboard.h>
#include <abi/net_config.h>
#include <abi/namespace.h>
#include <abi/pipe.h>
#include <abi/pointer.h>
#include <abi/power.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <console.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <stdlib.h>

static enum call_status copy_grant(handle_t source, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = source};
  return handle_rights(source, &grant->rights, &grant->transport);
}

void mux_session_release(struct mux_session *session)
{
  if (session->group != HANDLE_INVALID) {
    execution_group_terminate(session->group);
  }
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
    struct mux_session *session)
{
  *session = (struct mux_session){0};
  size_t root_count = startup_root_count();
  size_t depth = startup_working_directory_count();
  enum { INPUT, OUTPUT, LAUNCHER, STDIN, STDOUT, STDERR, EVENTS, FIRST_OPTIONAL };
  enum { OPTIONAL_RESOURCE_COUNT = 19, NAMESPACE_GRANT_COUNT = 1 };
  size_t fixed_grants = FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT +
      NAMESPACE_GRANT_COUNT + STARTUP_ROOT_LIMIT;
  if (root_count > STARTUP_ROOT_LIMIT ||
      depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants ||
      depth > SIZE_MAX / sizeof(uint64_t)) {
    return CALL_LIMIT;
  }
  struct launch_grant *grants = malloc((fixed_grants + depth) * sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  if (!grants || (depth && !directories)) {
    free(grants);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct terminal_create_reply terminal = {0};
  handle_t bound = HANDLE_INVALID, image = HANDLE_INVALID;
  enum call_status status = terminal_create(startup_resource("terminal"), columns, rows,
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
  status = launcher_create_group(startup_resource("launcher"), &group);
  if (status != CALL_OK) {
    goto done;
  }
  session->group = group.supervision;
  bound = group.launcher;
  handle_t boot = startup_root("boot");
  if (boot == HANDLE_INVALID) {
    status = CALL_NOT_FOUND;
    goto done;
  }
  status = directory_lookup(boot, "shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
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
    /* The opt-in controls delegation; only this group's launcher may enter it. */
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"child_launcher", grant_count};
    grants[grant_count++] = (struct launch_grant){bound, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  static const struct {
    const char *name;
    uint64_t rights;
  } allowed[] = {
    {"memory", MEMORY_RIGHT_MANAGE}, {"clock", CLOCK_RIGHTS},
    {"system_info", SYSTEM_INFO_RIGHT_READ}, {"log", LOG_RIGHT_READ},
    {"screen_capture", SCREEN_CAPTURE_RIGHT_CAPTURE},
    /* Graphics remain a shared space layer, never a pane surface. */
    {"display", DISPLAY_RIGHT_DRAW}, {"keyboard", KEYBOARD_RIGHT_INPUT},
    {"pointer", POINTER_RIGHT_INPUT}, {"power", POWER_RIGHTS},
    {"pipe", PIPE_SERVICE_RIGHT_CREATE}, {"service", ENDPOINT_SERVICE_RIGHT_CREATE},
    {"namespace_service", NAMESPACE_SERVICE_RIGHT_CREATE},
    {"tcp", TCP_SERVICE_RIGHTS}, {"random", RANDOM_RIGHT_READ},
    {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_FILE | PROFILE_RIGHT_HOST},
    {"echo", ECHO_RIGHT_SEND}, {"udp", UDP_SERVICE_RIGHT_OPEN},
    {"net_config", NET_CONFIG_RIGHT_READ},
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
    handle_t source = startup_resource(allowed[i].name);
    if (source == HANDLE_INVALID) {
      continue;
    }
    status = copy_grant(source, &grants[grant_count]);
    if (status != CALL_OK) {
      goto done;
    }
    grants[grant_count].rights &= allowed[i].rights;
    resources[resource_count++] = (struct launch_binding){(uintptr_t)allowed[i].name, grant_count++};
  }
  struct launch_binding roots[STARTUP_ROOT_LIMIT];
  const struct startup_binding *selected_roots = startup_roots();
  for (size_t i = 0; i < root_count; ++i) {
    roots[i] = (struct launch_binding){selected_roots[i].name, grant_count};
    status = copy_grant(selected_roots[i].handle, &grants[grant_count++]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = grant_count;
    status = copy_grant(startup_working_directory(i), &grants[grant_count++]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  const char *arguments[] = {"boot://shell.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = root_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)startup_working_path(),
    .environment = (uintptr_t)startup_environment_variables(),
    .environment_count = startup_environment_count(),
    .argv = (uintptr_t)arguments, .argc = 1,
    .streams = {{PROTOCOL_CONSOLE, STDIN}, {PROTOCOL_CONSOLE, STDOUT}, {PROTOCOL_CONSOLE, STDERR}},
  };
  handle_t namespace = startup_namespace();
  if (namespace != HANDLE_INVALID) {
    request.namespace_grant = grant_count + 1;
    status = copy_grant(namespace, &grants[grant_count++]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  request.grant_count = grant_count;
  status = launcher_launch(bound, &request, &session->process);

done:
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
  if (status != CALL_OK) {
    mux_session_release(session);
  }
  return status;
}
