#include <pyxis/environment.h>
#include "session.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/echo.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
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
  enum { OPTIONAL_RESOURCE_COUNT = 16, NAMESPACE_GRANT_COUNT = 1 };
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
  static const struct {
    const char *name;
    uint64_t rights;
  } allowed[] = {
    {"memory", MEMORY_RIGHT_MANAGE}, {"clock", CLOCK_RIGHTS},
    {"system_info", SYSTEM_INFO_RIGHT_READ}, {"log", LOG_RIGHT_READ},
    {"screen_capture", SCREEN_CAPTURE_RIGHT_CAPTURE},
    {"audio", AUDIO_RIGHT_PLAYBACK},
    {"pipe", PIPE_SERVICE_RIGHT_CREATE}, {"service", ENDPOINT_SERVICE_RIGHT_CREATE},
    {"tcp", TCP_SERVICE_RIGHT_CONNECT}, {"random", RANDOM_RIGHT_READ},
    {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST},
    {"echo", ECHO_RIGHT_SEND}, {"udp", UDP_SERVICE_RIGHT_OPEN},
    {"net_config", NET_CONFIG_RIGHT_READ},
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
    handle_t source = startup_resource(allowed[i].name);
    if (source == HANDLE_INVALID) {
      continue;
    }
    uint64_t rights;
    status = handle_rights(source, &rights, NULL);
    if (status != CALL_OK) {
      goto done;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)allowed[i].name, grant_count};
    grants[grant_count++] = (struct launch_grant){source, rights & allowed[i].rights, 0};
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
