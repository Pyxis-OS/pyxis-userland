#include "config.h"
#include "network.h"
#include <abi/clock.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/random.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <abi/keyboard.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <abi/memory.h>
#include <abi/endpoint.h>
#include <abi/namespace.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

static int launch_shell(const struct session_config *config, const struct network_config *network,
    bool configure_network)
{
  struct terminal terminal = {startup_resource("input"), startup_resource("output")};
  handle_t launcher = startup_resource("launcher");
  handle_t memory = startup_resource("memory");
  handle_t app = startup_root("app"), home = startup_root("home");
  if (terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID ||
      launcher == HANDLE_INVALID || memory == HANDLE_INVALID ||
      app == HANDLE_INVALID || home == HANDLE_INVALID) {
    fputs("session: missing startup resource or filesystem root\n", stderr);
    return EXIT_FAILURE;
  }

  handle_t image;
  enum call_status status = directory_lookup(app, "shell.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open shell (status %u)\n", status);
    return EXIT_FAILURE;
  }

  enum { INPUT, OUTPUT, MEMORY, LAUNCHER, APP, HOME, FIRST_OPTIONAL };
  size_t depth = startup_working_directory_count();
  size_t inherited = startup_environment_count();
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - FIRST_OPTIONAL - 14 - STARTUP_STREAM_COUNT ||
      inherited > SIZE_MAX / sizeof(struct startup_variable) - 2) {
    handle_close(image);
    fputs("session: startup metadata too large\n", stderr);
    return EXIT_FAILURE;
  }
  struct launch_grant *grants = malloc((FIRST_OPTIONAL + 14 + depth + STARTUP_STREAM_COUNT) * sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  struct startup_variable *environment = malloc((inherited + 2) * sizeof(*environment));
  int result = EXIT_FAILURE;
  if (!grants || (depth && !directories) || !environment) {
    fputs("session: cannot allocate launch metadata\n", stderr);
    goto done;
  }

  grants[INPUT] = (struct launch_grant){terminal.input, CONSOLE_RIGHT_READ, 0};
  grants[OUTPUT] = (struct launch_grant){terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[MEMORY] = (struct launch_grant){memory, MEMORY_RIGHT_MANAGE, 0};
  grants[LAUNCHER] = (struct launch_grant){launcher, LAUNCHER_RIGHT_LAUNCH, 0};
  grants[APP] = (struct launch_grant){app, 0, 0};
  grants[HOME] = (struct launch_grant){home, 0, 0};
  struct launch_binding resources[16] = {
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"launcher", LAUNCHER},
  };
  size_t resource_count = 4, grant_count = FIRST_OPTIONAL;
  handle_t display = startup_resource("display");
  if (display != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"display", grant_count};
    grants[grant_count++] = (struct launch_grant){display, DISPLAY_RIGHT_DRAW, 0};
  }
  handle_t clock = startup_resource("clock");
  if (clock != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"clock", grant_count};
    grants[grant_count++] = (struct launch_grant){clock, CLOCK_RIGHTS, 0};
  }
  handle_t echo = startup_resource("echo");
  if (echo != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"echo", grant_count};
    grants[grant_count++] = (struct launch_grant){echo, ECHO_RIGHT_SEND, 0};
  }
  handle_t udp = startup_resource("udp");
  if (udp != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"udp", grant_count};
    grants[grant_count++] = (struct launch_grant){udp, UDP_SERVICE_RIGHT_OPEN, 0};
  }
  handle_t tcp = startup_resource("tcp");
  if (tcp != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"tcp", grant_count};
    grants[grant_count++] = (struct launch_grant){tcp, TCP_SERVICE_RIGHT_CONNECT, 0};
  }
  handle_t pipe = startup_resource("pipe");
  if (pipe != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"pipe", grant_count};
    grants[grant_count++] = (struct launch_grant){pipe, PIPE_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t service = startup_resource("service");
  if (service != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"service", grant_count};
    grants[grant_count++] = (struct launch_grant){service, ENDPOINT_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t namespace_service = startup_resource("namespace_service");
  if (namespace_service != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){
        (uintptr_t)"namespace_service", grant_count};
    grants[grant_count++] = (struct launch_grant){namespace_service,
        NAMESPACE_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t random = startup_resource("random");
  if (random != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"random", grant_count};
    grants[grant_count++] = (struct launch_grant){random, RANDOM_RIGHT_READ, 0};
  }
  handle_t keyboard = startup_resource("keyboard");
  if (keyboard != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"keyboard", grant_count};
    grants[grant_count++] = (struct launch_grant){keyboard, KEYBOARD_RIGHT_INPUT, 0};
  }

  handle_t space = startup_resource("space");
  if (space != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"space", grant_count};
    grants[grant_count++] = (struct launch_grant){space, SPACE_RIGHT_SET_TITLE, 0};
  }

  handle_t profile = startup_resource("profile");
  if (profile != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"profile", grant_count};
    grants[grant_count++] = (struct launch_grant){profile, PROFILE_RIGHT_MEMORY, 0};
  }

  struct launch_binding roots[3] = {{(uintptr_t)"app", APP}, {(uintptr_t)"home", HOME}};
  size_t root_count = 2;
  handle_t host = startup_root("host");
  if (host != HANDLE_INVALID) {
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", grant_count};
    grants[grant_count++] = (struct launch_grant){host, 0, 0};
  }

  const char *working_path = startup_working_path();
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = grant_count;
    grants[grant_count++] = (struct launch_grant){startup_working_directory(i), 0, 0};
  }

  /* Root names and display paths do not determine delegated authority. */
  for (size_t i = 0; i < root_count; ++i) {
    struct launch_grant *grant = &grants[roots[i].grant];
    status = handle_rights(grant->source, &grant->rights, &grant->transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query directory rights (status %u)\n", status);
      goto done;
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    struct launch_grant *grant = &grants[directories[i]];
    status = handle_rights(grant->source, &grant->rights, &grant->transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query directory rights (status %u)\n", status);
      goto done;
    }
  }

  size_t environment_count = 0;
  const struct startup_variable *source = startup_environment_variables();
  for (size_t i = 0; i < inherited; ++i) {
    const char *name = (const char *)source[i].name;
    if (strcmp(name, "TZ") && strcmp(name, "DNS_SERVER")) {
      environment[environment_count++] = source[i];
    }
  }
  environment[environment_count++] = (struct startup_variable){
    (uintptr_t)"TZ", (uintptr_t)config->timezone,
  };
  environment[environment_count++] = (struct startup_variable){
    (uintptr_t)"DNS_SERVER", (uintptr_t)network->dns_server,
  };
  const char *arguments[] = {"app://shell.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = grant_count,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = root_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)working_path,
    .environment = (uintptr_t)environment, .environment_count = environment_count,
    .argv = (uintptr_t)arguments, .argc = 1,
  };
  handle_t namespace_handle = startup_namespace();
  if (namespace_handle != HANDLE_INVALID) {
    uint64_t rights, transport;
    status = handle_rights(namespace_handle, &rights, &transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query namespace rights (status %u)\n", status);
      goto done;
    }
    request.namespace_grant = grant_count + 1;
    grants[grant_count++] = (struct launch_grant){namespace_handle,
        rights, transport};
    request.grant_count = grant_count;
  }

  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    struct startup_stream stream = startup_stream(i);
    if (stream.protocol == STARTUP_STREAM_NONE) {
      continue;
    }
    bool input = i == STARTUP_STDIN;
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        stream.protocol == PROTOCOL_PIPE ?
        (input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE) :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    request.streams[i] = (struct launch_stream){stream.protocol, request.grant_count};
    grants[request.grant_count++] = (struct launch_grant){stream.handle, rights, 0};
  }

  if (configure_network && !network_config_apply(network)) {
    goto done;
  }
  status = term_set_tab_width(&terminal, config->tab_width);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot set tab width (status %u)\n", status);
    goto done;
  }
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch shell (status %u)\n", status);
    goto done;
  }
  /* The child owns its copied grants and strings. Never read terminal input
   * or wait after handing off; closing this observer leaves the shell alive. */
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  free(environment);
  free(directories);
  free(grants);
  if (handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}

int main(int argc, char **argv)
{
  bool configure_network = argc == 2 && !strcmp(argv[1], "--configure-network");
  if ((argc != 1 && !configure_network) || startup_resource("script") != HANDLE_INVALID) {
    fputs("Usage: session.pxe [--configure-network] (native init or session handoff)\n", stderr);
    return EXIT_FAILURE;
  }
  struct session_config config;
  if (!session_config_read(&config)) {
    return EXIT_FAILURE;
  }
  struct network_config network;
  if (!network_config_read(&network)) {
    free(config.timezone);
    return EXIT_FAILURE;
  }
  int result = launch_shell(&config, &network, configure_network);
  free(config.timezone);
  return result;
}
