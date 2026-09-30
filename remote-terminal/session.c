#include "session.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/echo.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/namespace.h>
#include <abi/endpoint.h>
#include <abi/pipe.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <console.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>

static enum call_status directory_grant(handle_t directory, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = directory};
  return handle_rights(directory, &grant->rights, &grant->transport);
}

enum call_status remote_shell_launch(size_t columns, size_t rows, unsigned tab_width,
    struct remote_shell *shell)
{
  *shell = (struct remote_shell){0};
  struct terminal_create_reply terminal;
  enum call_status status = terminal_create(startup_resource("terminal"), columns, rows, &terminal);
  if (status != CALL_OK) {
    return status;
  }
  shell->attachment = terminal.attachment;
  handle_t bound = HANDLE_INVALID, image = HANDLE_INVALID;
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
  handle_t app = startup_root("app"), home = startup_root("home");
  status = directory_lookup(app, "shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }

  enum { INPUT, OUTPUT, LAUNCHER, APP, HOME, STDIN, STDOUT, STDERR, FIRST_OPTIONAL };
  struct launch_grant grants[24] = {
    [INPUT] = {terminal.input, CONSOLE_RIGHT_READ, 0},
    [OUTPUT] = {terminal.output, CONSOLE_RIGHT_WRITE, 0},
    [LAUNCHER] = {bound, LAUNCHER_RIGHT_LAUNCH, 0},
    [STDIN] = {terminal.input, CONSOLE_RIGHT_READ, 0},
    [STDOUT] = {terminal.output, CONSOLE_RIGHT_WRITE, 0},
    [STDERR] = {terminal.output, CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_binding resources[16] = {
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"launcher", LAUNCHER},
  };
  size_t grant_count = FIRST_OPTIONAL, resource_count = 3;
  struct launch_binding roots[3] = {{(uintptr_t)"app", APP}, {(uintptr_t)"home", HOME}};
  size_t root_count = 2;
  status = directory_grant(app, &grants[APP]);
  if (status == CALL_OK) {
    status = directory_grant(home, &grants[HOME]);
  }
  if (status != CALL_OK) {
    goto done;
  }
  handle_t host = startup_root("host");
  if (host != HANDLE_INVALID) {
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", grant_count};
    status = directory_grant(host, &grants[grant_count++]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  static const struct {
    const char *name;
    uint64_t rights;
  } allowed[] = {
    {"memory", MEMORY_RIGHT_MANAGE}, {"clock", CLOCK_RIGHTS},
    {"pipe", PIPE_SERVICE_RIGHT_CREATE}, {"service", ENDPOINT_SERVICE_RIGHT_CREATE},
    {"tcp", TCP_SERVICE_RIGHT_CONNECT}, {"random", RANDOM_RIGHT_READ},
    {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_FILE | PROFILE_RIGHT_HOST},
    {"echo", ECHO_RIGHT_SEND}, {"udp", UDP_SERVICE_RIGHT_OPEN},
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
  uint64_t directory = HOME;
  const char *arguments[] = {"app://shell.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = root_count,
    .working_directories = (uintptr_t)&directory, .working_directory_count = 1,
    .working_path = (uintptr_t)"home://",
    .environment = (uintptr_t)startup_environment_variables(),
    .environment_count = startup_environment_count(),
    .argv = (uintptr_t)arguments, .argc = 1,
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
  status = launcher_launch(bound, &request, &shell->process);

done:
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  if (bound != HANDLE_INVALID) {
    handle_close(bound);
  }
  handle_close(terminal.input);
  handle_close(terminal.output);
  /* Keep attachment/supervision even on failure: the loop owns termination and
   * observes cleanup before returning this admission slot to the listener. */
  return status;
}
