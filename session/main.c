#include "config.h"
#include "network.h"
#include "remote_server.h"
#include "tcp_server.h"
#include "../common/udp.h"
#include <abi/clock.h>
#include <abi/system_info.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/random.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/screen_capture.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <abi/power.h>
#include <abi/keyboard.h>
#include <abi/pointer.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/endpoint.h>
#include <abi/namespace.h>
#include <abi/net_config.h>
#include <abi/terminal.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <network_environment.h>
#include <limits.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

static int launch_session(const struct session_config *config, const struct network_config *network,
    struct network_runtime *runtime, bool configure_network,
    bool start_services, bool start_remote_services)
{
  struct terminal terminal = {startup_resource("input"), startup_resource("output")};
  handle_t launcher = startup_resource("launcher");
  handle_t memory = startup_resource("memory");
  handle_t boot = startup_root("boot"), tmp = startup_root("tmp");
  if (terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID ||
      launcher == HANDLE_INVALID || memory == HANDLE_INVALID ||
      boot == HANDLE_INVALID || tmp == HANDLE_INVALID) {
    fputs("session: missing startup resource or filesystem root\n", stderr);
    return EXIT_FAILURE;
  }

  bool script = start_services || start_remote_services;
  const char *image_name = start_remote_services ? "init-remote-services" :
      start_services ? "init-services" : "shell.pxe";
  const char *image_uri = start_remote_services ? "boot://init-remote-services" :
      start_services ? "boot://init-services" : "boot://shell.pxe";
  handle_t image;
  enum call_status status = directory_lookup(boot, image_name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open %s (status %u)\n", image_uri, status);
    return EXIT_FAILURE;
  }

  enum { INPUT, OUTPUT, MEMORY, LAUNCHER, FIRST_OPTIONAL };
  enum { OPTIONAL_RESOURCE_COUNT = 21, NAMESPACE_GRANT_COUNT = 1 };
  const struct startup_binding *selected_roots = startup_roots();
  size_t root_count = startup_root_count();
  size_t depth = startup_working_directory_count();
  size_t fixed_grants = FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT + NAMESPACE_GRANT_COUNT +
      STARTUP_ROOT_LIMIT + STARTUP_STREAM_COUNT;
  if (root_count > STARTUP_ROOT_LIMIT ||
      depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants) {
    handle_close(image);
    fputs("session: startup metadata too large\n", stderr);
    return EXIT_FAILURE;
  }
  struct launch_grant *grants = malloc((fixed_grants + depth) * sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  size_t environment_count = 0;
  struct startup_variable *environment =
      session_environment(config, network->dns_server, &environment_count);
  struct network_environment refreshed_environment = {0};
  int result = EXIT_FAILURE;
  if (!environment) {
    goto done;
  }
  if (!grants || (depth && !directories)) {
    fputs("session: cannot allocate launch metadata\n", stderr);
    goto done;
  }

  /* Pass Ctrl+C arming on to the successor shell when this session has it. */
  uint64_t input_rights;
  status = handle_rights(terminal.input, &input_rights, NULL);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot query input rights (status %u)\n", status);
    goto done;
  }
  grants[INPUT] = (struct launch_grant){terminal.input,
      input_rights & (CONSOLE_RIGHT_READ | CONSOLE_RIGHT_INTERRUPT), 0};
  grants[OUTPUT] = (struct launch_grant){terminal.output, CONSOLE_RIGHT_WRITE, 0};
  grants[MEMORY] = (struct launch_grant){memory, MEMORY_RIGHT_MANAGE, 0};
  grants[LAUNCHER] = (struct launch_grant){launcher, LAUNCHER_RIGHT_LAUNCH, 0};
  if (start_remote_services) {
    uint64_t rights;
    status = handle_rights(launcher, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query remote launcher rights (status %u)\n", status);
      goto done;
    }
    grants[LAUNCHER].rights = rights &
        (LAUNCHER_RIGHT_LAUNCH | LAUNCHER_RIGHT_CREATE_GROUP);
  }
  struct launch_binding resources[FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT] = {
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"launcher", LAUNCHER},
  };
  size_t resource_count = 4, grant_count = FIRST_OPTIONAL;
  handle_t child_launcher = startup_resource("child_launcher");
  if (child_launcher != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"child_launcher", grant_count};
    grants[grant_count++] = (struct launch_grant){child_launcher, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  handle_t display = startup_resource("display");
  if (display != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"display", grant_count};
    grants[grant_count++] = (struct launch_grant){display, DISPLAY_RIGHT_DRAW, 0};
  }
  handle_t screen_capture = startup_resource("screen_capture");
  if (screen_capture != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){
        (uintptr_t)"screen_capture", grant_count};
    grants[grant_count++] = (struct launch_grant){screen_capture,
        SCREEN_CAPTURE_RIGHT_CAPTURE, 0};
  }
  handle_t clock = startup_resource("clock");
  if (clock != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"clock", grant_count};
    grants[grant_count++] = (struct launch_grant){clock, CLOCK_RIGHTS, 0};
  }
  handle_t system_info = startup_resource("system_info");
  if (system_info != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"system_info", grant_count};
    grants[grant_count++] = (struct launch_grant){system_info, SYSTEM_INFO_RIGHT_READ, 0};
  }
  handle_t log = startup_resource("log");
  if (log != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"log", grant_count};
    grants[grant_count++] = (struct launch_grant){log, LOG_RIGHT_READ, 0};
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
  if (start_remote_services) {
    handle_t udp_beacons = startup_resource("udp_beacons");
    if (udp_beacons != HANDLE_INVALID) {
      resources[resource_count++] = (struct launch_binding){(uintptr_t)"udp_beacons", grant_count};
      grants[grant_count++] = (struct launch_grant){udp_beacons, UDP_SERVICE_RIGHT_BROADCAST, 0};
    }
  }
  handle_t tcp = startup_resource("tcp");
  if (tcp != HANDLE_INVALID) {
    uint64_t rights = TCP_SERVICE_RIGHT_CONNECT;
    if (start_remote_services) {
      status = handle_rights(tcp, &rights, NULL);
      if (status != CALL_OK) {
        fprintf(stderr, "session: cannot query remote TCP rights (status %u)\n", status);
        goto done;
      }
      rights &= TCP_SERVICE_RIGHT_CONNECT | TCP_SERVICE_RIGHT_LISTEN;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"tcp", grant_count};
    grants[grant_count++] = (struct launch_grant){tcp, rights, 0};
  }
  handle_t net_config = startup_resource("net_config");
  if (net_config != HANDLE_INVALID) {
    uint64_t rights;
    status = handle_rights(net_config, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query network rights (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"net_config", grant_count};
    grants[grant_count++] = (struct launch_grant){net_config,
        rights & NET_CONFIG_RIGHT_READ, 0};
  }
  if (start_remote_services) {
    handle_t terminal_service = startup_resource("terminal");
    if (terminal_service == HANDLE_INVALID || net_config == HANDLE_INVALID) {
      fputs("session: missing remote service bootstrap authority\n", stderr);
      goto done;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"terminal", grant_count};
    grants[grant_count++] = (struct launch_grant){terminal_service,
        TERMINAL_SERVICE_RIGHT_CREATE, 0};
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
  handle_t pointer = startup_resource("pointer");
  if (pointer != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"pointer", grant_count};
    grants[grant_count++] = (struct launch_grant){pointer, POINTER_RIGHT_INPUT, 0};
  }

  /* Power stays with local sessions; the remote terminal server is
   * unauthenticated, so its services and shells never receive it. */
  handle_t power = startup_resource("power");
  if (power != HANDLE_INVALID && !start_remote_services) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"power", grant_count};
    grants[grant_count++] = (struct launch_grant){power, POWER_RIGHTS, 0};
  }

  handle_t space = startup_resource("space");
  if (space != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"space", grant_count};
    grants[grant_count++] = (struct launch_grant){space, SPACE_RIGHT_SET_TITLE, 0};
  }

  handle_t profile = startup_resource("profile");
  if (profile != HANDLE_INVALID) {
    uint64_t rights;
    status = handle_rights(profile, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query profile rights (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"profile", grant_count};
    grants[grant_count++] = (struct launch_grant){profile,
        rights & (PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_FILE | PROFILE_RIGHT_HOST), 0};
  }

  struct launch_binding roots[STARTUP_ROOT_LIMIT];
  for (size_t i = 0; i < root_count; ++i) {
    roots[i] = (struct launch_binding){selected_roots[i].name, grant_count};
    struct launch_grant *grant = &grants[grant_count++];
    *grant = (struct launch_grant){.source = selected_roots[i].handle};
    status = handle_rights(grant->source, &grant->rights, &grant->transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query directory rights (status %u)\n", status);
      goto done;
    }
  }

  /* This full-root handoff also preserves the explicitly selected cwd chain.
   * The display path cannot authorize reopening roots or widening its grants. */
  const char *working_path = startup_working_path();
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = grant_count;
    struct launch_grant *grant = &grants[grant_count++];
    *grant = (struct launch_grant){.source = startup_working_directory(i)};
    status = handle_rights(grant->source, &grant->rights, &grant->transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query directory rights (status %u)\n", status);
      goto done;
    }
  }

  const char *arguments[] = {image_uri};
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
    uint64_t transport = 0;
    if (stream.protocol == PROTOCOL_FILE) {
      struct handle_info info;
      status = handle_query(stream.handle, &info);
      if (status != CALL_OK) {
        fprintf(stderr, "session: cannot query stream (status %u)\n", status);
        goto done;
      }
      if (info.protocol != PROTOCOL_FILE ||
          (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED)) {
        fputs("session: invalid file stream\n", stderr);
        goto done;
      }
      if (info.kind == HANDLE_KIND_EXPORTED) {
        transport = HANDLE_TRANSPORT_CALL;
      }
    }
    grants[request.grant_count++] = (struct launch_grant){stream.handle, rights, transport};
  }

  if (configure_network && !network_config_apply(network, runtime)) {
    goto done;
  }
  if (!configure_network && script) {
    status = network_environment_wait(net_config, startup_resource("clock"));
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      fprintf(stderr, "session: cannot wait for network setup (status %u)\n", status);
      goto done;
    }
  }
  status = network_environment_read(&refreshed_environment, net_config,
      environment, environment_count, network->dns_server);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot read network environment (status %u)\n", status);
    goto done;
  }
  request.environment = (uintptr_t)refreshed_environment.variables;
  request.environment_count = refreshed_environment.count;
  status = term_set_tab_width(&terminal, config->tab_width);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot set tab width (status %u)\n", status);
    goto done;
  }
  handle_t child;
  status = script ? program_launch(launcher, &request, NULL, &child) :
      launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch %s (status %u)\n", image_uri, status);
    goto done;
  }
  /* The child owns its copied grants and strings. Never read terminal input
   * or wait for the child after handing off; closing this observer leaves it alive. */
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  network_environment_free(&refreshed_environment);
  free(environment);
  free(directories);
  free(grants);
  if (handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}

static bool release_bootstrap_grants(struct network_runtime *runtime)
{
  size_t named_count = startup_resource_count();
  size_t roots = startup_root_count();
  size_t depth = startup_working_directory_count();
  if (named_count == SIZE_MAX || roots > SIZE_MAX - named_count - 1 ||
      depth > SIZE_MAX - named_count - roots - 1 ||
      named_count + roots + depth + 1 > SIZE_MAX / sizeof(handle_t)) {
    fputs("session: too many bootstrap grants to release\n", stderr);
    return false;
  }
  size_t count = named_count + roots + depth + 1;
  handle_t *handles = malloc(count * sizeof(*handles));
  if (!handles) {
    fputs("session: cannot allocate bootstrap grant cleanup\n", stderr);
    return false;
  }
  for (size_t i = 0; i < named_count; ++i) {
    handles[i] = startup_resources()[i].handle;
  }
  for (size_t i = 0; i < roots; ++i) {
    handles[named_count + i] = startup_roots()[i].handle;
  }
  for (size_t i = 0; i < depth; ++i) {
    handles[named_count + roots + i] = startup_working_directory(i);
  }
  handles[count - 1] = startup_namespace();
  const handle_t retained[] = {
    runtime->dhcp.endpoint, runtime->dhcp.clock, runtime->dhcp.random,
    runtime->authority, runtime->waiting_link ? runtime->udp : HANDLE_INVALID,
    startup_resource("memory"), startup_resource("output"),
    startup_stream(STARTUP_STDOUT).handle, startup_stream(STARTUP_STDERR).handle,
  };
  bool success = true;
  for (size_t i = 0; i < count; ++i) {
    handle_t handle = handles[i];
    bool keep = handle == HANDLE_INVALID;
    for (size_t j = 0; j < sizeof(retained) / sizeof(retained[0]); ++j) {
      keep |= handle == retained[j];
    }
    for (size_t j = 0; j < i; ++j) {
      keep |= handle == handles[j];
    }
    if (!keep && handle_close(handle) != 0) {
      success = false;
    }
  }
  free(handles);
  /* Libc owns the dedicated stream handles. No path operation or bootstrap
   * resource lookup follows this handoff; their snapshots now contain stale handles. */
  if (startup_stream(STARTUP_STDIN).protocol != STARTUP_STREAM_NONE && fclose(stdin) != 0) {
    success = false;
  }
  if (!runtime->waiting_link) {
    runtime->udp = HANDLE_INVALID;
  }
  runtime->background = true;
  if (!success) {
    fputs("session: cannot release bootstrap grants\n", stderr);
  }
  return success;
}

static int finish_network(const struct network_config *config,
    struct network_runtime *runtime, int result)
{
  if (result == EXIT_SUCCESS &&
      (runtime->waiting_link || runtime->dhcp.endpoint != HANDLE_INVALID)) {
    if (release_bootstrap_grants(runtime)) {
      result = network_config_maintain(config, runtime);
    } else {
      result = EXIT_FAILURE;
    }
  }
  if (result != EXIT_SUCCESS && !network_config_stop(config, runtime)) {
    result = EXIT_FAILURE;
  }
  return result;
}

int main(int argc, char **argv)
{
  bool configure_network = false, start_services = false, start_remote_services = false;
  bool tcp_server = false, remote_server = false;
  uint32_t address = 0;
  unsigned port = 0;
  const char *count = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--configure-network") && !configure_network) {
      configure_network = true;
    } else if (!strcmp(argv[i], "--start-services") && !start_services) {
      start_services = true;
    } else if (!strcmp(argv[i], "--start-remote-services") && !start_remote_services) {
      start_remote_services = true;
    } else if (!strcmp(argv[i], "--tcp-server") && !tcp_server && i + 2 < argc) {
      if (!udp_parse_address(argv[i + 1], &address) ||
          !udp_parse_number(argv[i + 2], UINT16_MAX, &port) || !port) {
        goto usage;
      }
      tcp_server = true;
      i += 2;
    } else if (!strcmp(argv[i], "--remote-server") && !remote_server && i + 1 < argc) {
      if (!udp_parse_number(argv[++i], UINT16_MAX, &port) || !port) {
        goto usage;
      }
      remote_server = true;
    } else if (!strcmp(argv[i], "--tcp-count") && !count && i + 1 < argc) {
      unsigned value;
      count = argv[++i];
      if (!udp_parse_number(count, UINT_MAX, &value) || !value) {
        goto usage;
      }
    } else {
      goto usage;
    }
  }
  if (startup_resource("script") != HANDLE_INVALID ||
      (tcp_server && start_services) ||
      (count && !tcp_server) ||
      (remote_server && (start_services || tcp_server || start_remote_services)) ||
      (start_remote_services && (start_services || tcp_server))) {
    goto usage;
  }
  if (tcp_server) {
    struct network_config network;
    struct network_runtime runtime = {0};
    if (!network_config_read(&network)) {
      return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    if (!configure_network || network_config_apply(&network, &runtime)) {
      result = launch_tcp_server(address, (uint16_t)port, count);
    }
    result = finish_network(&network, &runtime, result);
    network_config_free(&network);
    return result;
  }
  struct session_config config;
  if (!session_config_read(&config)) {
    return EXIT_FAILURE;
  }
  struct network_config network;
  if (!network_config_read(&network)) {
    session_config_free(&config);
    return EXIT_FAILURE;
  }
  struct network_runtime runtime = {0};
  int result = EXIT_FAILURE;
  if (remote_server) {
    if (!configure_network || (network_config_apply(&network, &runtime) &&
        network_config_wait_address(&network, &runtime))) {
      result = launch_remote_server(&config, &network, (uint16_t)port);
    }
  } else {
    result = launch_session(&config, &network, &runtime, configure_network,
        start_services, start_remote_services);
  }
  session_config_free(&config);
  result = finish_network(&network, &runtime, result);
  network_config_free(&network);
  return result;

usage:
  fputs("Usage: session.pxe [--configure-network] [--start-services | --start-remote-services]\n"
      "       session.pxe [--configure-network] --tcp-server ADDRESS PORT [--tcp-count COUNT]\n"
      "       session.pxe [--configure-network] --remote-server PORT\n"
      "       (native init or trusted session handoff)\n", stderr);
  return EXIT_FAILURE;
}
