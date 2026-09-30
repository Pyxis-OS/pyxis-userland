#include "remote_server.h"
#include <abi/console.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/namespace.h>
#include <abi/pipe.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/system_info.h>
#include <abi/terminal.h>
#include <abi/udp.h>
#include <clock.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <net_config.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>

#define NETWORK_ASSIGNMENT_SLEEP_NS UINT64_C(100000000)
#define READ_ONLY_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | \
    DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)

static bool wait_for_address(handle_t authority, handle_t clock, uint32_t *address)
{
  bool announced = false;
  for (;;) {
    struct net_config_reply snapshot;
    enum call_status status = net_config_query(authority, &snapshot);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query remote network (status %u)\n", status);
      return false;
    }
    if (!(snapshot.flags & NET_CONFIG_PRESENT) || !(snapshot.flags & NET_CONFIG_READY)) {
      fputs("session: remote network unavailable\n", stderr);
      return false;
    }
    if (snapshot.flags & NET_CONFIG_ASSIGNED) {
      *address = snapshot.address;
      return true;
    }
    if (!announced) {
      fputs("session: waiting for remote network configuration\n", stdout);
      announced = true;
    }
    status = clock_sleep_for(clock, NETWORK_ASSIGNMENT_SLEEP_NS);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot wait for remote network (status %u)\n", status);
      return false;
    }
  }
}

static bool directory_grant(handle_t source, uint64_t allowed, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = source};
  enum call_status status = handle_rights(source, &grant->rights, &grant->transport);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot query remote directory rights (status %u)\n", status);
    return false;
  }
  grant->rights &= allowed;
  return true;
}

int launch_remote_server(const struct session_config *config,
    const struct network_config *network, uint16_t port)
{
  handle_t memory = startup_resource("memory"), clock = startup_resource("clock");
  handle_t launcher = startup_resource("launcher"), tcp = startup_resource("tcp");
  handle_t terminal = startup_resource("terminal"), output = startup_resource("output");
  handle_t authority = startup_resource("net_config");
  handle_t app = startup_root("app"), home = startup_root("home");
  if (memory == HANDLE_INVALID || clock == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      tcp == HANDLE_INVALID || terminal == HANDLE_INVALID || output == HANDLE_INVALID ||
      authority == HANDLE_INVALID || app == HANDLE_INVALID || home == HANDLE_INVALID) {
    fputs("session: missing remote server launch authority\n", stderr);
    return EXIT_FAILURE;
  }

  enum { MEMORY, CLOCK, LAUNCHER, TERMINAL, STDOUT, STDERR, APP, HOME, LISTENER, FIRST_OPTIONAL };
  enum { OPTIONAL_COUNT = 10, RESOURCE_CAPACITY = 13 };
  struct launch_grant grants[FIRST_OPTIONAL + OPTIONAL_COUNT] = {
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [CLOCK] = {clock, CLOCK_RIGHT_READ | CLOCK_RIGHT_SLEEP, 0},
    [LAUNCHER] = {launcher, LAUNCHER_RIGHT_LAUNCH | LAUNCHER_RIGHT_CREATE_GROUP, 0},
    [TERMINAL] = {terminal, TERMINAL_SERVICE_RIGHT_CREATE, 0},
    [STDOUT] = {output, CONSOLE_RIGHT_WRITE, 0},
    [STDERR] = {output, CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_binding resources[RESOURCE_CAPACITY] = {
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"clock", CLOCK},
    {(uintptr_t)"launcher", LAUNCHER}, {(uintptr_t)"terminal", TERMINAL},
    {(uintptr_t)"tcp_listener", LISTENER},
  };
  size_t resource_count = 5, grant_count = FIRST_OPTIONAL;
  struct launch_binding roots[3] = {{(uintptr_t)"app", APP}, {(uintptr_t)"home", HOME}};
  size_t root_count = 2;
  handle_t image = HANDLE_INVALID, listener = HANDLE_INVALID;
  struct startup_variable *environment = NULL;
  int result = EXIT_FAILURE;
  if (!directory_grant(app, READ_ONLY_DIRECTORY_RIGHTS, &grants[APP]) ||
      !directory_grant(home, DIRECTORY_RIGHTS, &grants[HOME])) {
    goto done;
  }
  handle_t host = startup_root("host");
  if (host != HANDLE_INVALID) {
    if (!directory_grant(host, DIRECTORY_RIGHTS, &grants[grant_count])) {
      goto done;
    }
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", grant_count++};
  }

  handle_t system_info = startup_resource("system_info");
  if (system_info != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"system_info", grant_count};
    grants[grant_count++] = (struct launch_grant){system_info, SYSTEM_INFO_RIGHT_READ, 0};
  }
  handle_t pipe = startup_resource("pipe");
  if (pipe != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"pipe", grant_count};
    grants[grant_count++] = (struct launch_grant){pipe, PIPE_SERVICE_RIGHT_CREATE, 0};
  }
  resources[resource_count++] = (struct launch_binding){(uintptr_t)"tcp", grant_count};
  grants[grant_count++] = (struct launch_grant){tcp, TCP_SERVICE_RIGHT_CONNECT, 0};
  handle_t random = startup_resource("random");
  if (random != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"random", grant_count};
    grants[grant_count++] = (struct launch_grant){random, RANDOM_RIGHT_READ, 0};
  }
  handle_t service = startup_resource("service");
  if (service != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"service", grant_count};
    grants[grant_count++] = (struct launch_grant){service, ENDPOINT_SERVICE_RIGHT_CREATE, 0};
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
  handle_t profile = startup_resource("profile");
  enum call_status status;
  if (profile != HANDLE_INVALID) {
    uint64_t rights;
    status = handle_rights(profile, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query remote profile rights (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"profile", grant_count};
    grants[grant_count++] = (struct launch_grant){profile,
        rights & (PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_FILE | PROFILE_RIGHT_HOST), 0};
  }

  size_t inherited = startup_environment_count();
  if (inherited > SIZE_MAX / sizeof(*environment) - 2) {
    fputs("session: remote environment too large\n", stderr);
    goto done;
  }
  environment = malloc((inherited + 2) * sizeof(*environment));
  if (!environment) {
    fputs("session: cannot allocate remote environment\n", stderr);
    goto done;
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
      (uintptr_t)"TZ", (uintptr_t)config->timezone};
  environment[environment_count++] = (struct startup_variable){
      (uintptr_t)"DNS_SERVER", (uintptr_t)network->dns_server};
  char tab_width[sizeof("32")];
  snprintf(tab_width, sizeof(tab_width), "%zu", config->tab_width);
  const char *arguments[] = {"app://remote-terminal.pxe", tab_width};
  uint64_t working_directory = HOME;
  struct launch_request request = {
    .grants = (uintptr_t)grants, .grant_count = grant_count,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = root_count,
    .working_directories = (uintptr_t)&working_directory, .working_directory_count = 1,
    .working_path = (uintptr_t)"home://",
    .environment = (uintptr_t)environment, .environment_count = environment_count,
    .argv = (uintptr_t)arguments, .argc = 2,
    .streams = {
      [STARTUP_STDOUT] = {PROTOCOL_CONSOLE, STDOUT},
      [STARTUP_STDERR] = {PROTOCOL_CONSOLE, STDERR},
    },
  };
  handle_t namespace_handle = startup_namespace();
  if (namespace_handle != HANDLE_INVALID) {
    uint64_t rights, transport;
    status = handle_rights(namespace_handle, &rights, &transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query remote namespace rights (status %u)\n", status);
      goto done;
    }
    request.namespace_grant = grant_count + 1;
    grants[grant_count++] = (struct launch_grant){namespace_handle,
        rights & NAMESPACE_RIGHT_LOOKUP, transport};
    request.grant_count = grant_count;
  }
  status = directory_lookup(app, "remote-terminal.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open app://remote-terminal.pxe (status %u)\n", status);
    goto done;
  }
  request.image = image;
  uint32_t address;
  if (!wait_for_address(authority, clock, &address)) {
    goto done;
  }
  struct tcp_listen_reply reply;
  status = tcp_listen(tcp, address, port, &reply);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot start remote listener (status %u)\n", status);
    goto done;
  }
  listener = reply.handle;
  grants[LISTENER] = (struct launch_grant){listener,
      TCP_LISTENER_RIGHT_INSPECT | TCP_LISTENER_RIGHT_ACCEPT, 0};
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch remote server (status %u)\n", status);
    goto done;
  }
  printf("remote: listening on %u.%u.%u.%u:%u\n", address >> 24,
      (address >> 16) & 255, (address >> 8) & 255, address & 255, port);
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  free(environment);
  if (listener != HANDLE_INVALID && handle_close(listener) != 0) {
    result = EXIT_FAILURE;
  }
  if (image != HANDLE_INVALID && handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}
