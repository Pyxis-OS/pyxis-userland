#include "config.h"
#include "network.h"
#include "remote_server.h"
#include "tcp_server.h"
#include "udp_server.h"
#include "../common/udp.h"
#include <abi/clock.h>
#include <abi/system_info.h>
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
    bool configure_network, bool start_services, bool start_remote_services)
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

  bool script = start_services || start_remote_services;
  const char *image_name = start_remote_services ? "init-remote-services" :
      start_services ? "init-services" : "shell.pxe";
  const char *image_uri = start_remote_services ? "app://init-remote-services" :
      start_services ? "app://init-services" : "app://shell.pxe";
  handle_t image;
  enum call_status status = directory_lookup(app, image_name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open %s (status %u)\n", image_uri, status);
    return EXIT_FAILURE;
  }

  enum { INPUT, OUTPUT, MEMORY, LAUNCHER, FIRST_OPTIONAL };
  enum { OPTIONAL_RESOURCE_COUNT = 15, NAMESPACE_GRANT_COUNT = 1 };
  const struct startup_binding *selected_roots = startup_roots();
  size_t root_count = startup_root_count();
  size_t depth = startup_working_directory_count();
  size_t inherited = startup_environment_count();
  size_t fixed_grants = FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT + NAMESPACE_GRANT_COUNT +
      STARTUP_ROOT_LIMIT + STARTUP_STREAM_COUNT;
  if (root_count > STARTUP_ROOT_LIMIT ||
      depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants ||
      inherited > SIZE_MAX / sizeof(struct startup_variable) - 2) {
    handle_close(image);
    fputs("session: startup metadata too large\n", stderr);
    return EXIT_FAILURE;
  }
  struct launch_grant *grants = malloc((fixed_grants + depth) * sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  struct startup_variable *environment = malloc((inherited + 2) * sizeof(*environment));
  struct network_environment refreshed_environment = {0};
  int result = EXIT_FAILURE;
  if (!grants || (depth && !directories) || !environment) {
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
  handle_t system_info = startup_resource("system_info");
  if (system_info != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"system_info", grant_count};
    grants[grant_count++] = (struct launch_grant){system_info, SYSTEM_INFO_RIGHT_READ, 0};
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

  if (configure_network && !network_config_apply(network)) {
    goto done;
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
   * or wait after handing off; closing this observer leaves the child alive. */
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

int main(int argc, char **argv)
{
  bool configure_network = false, start_services = false, start_remote_services = false;
  bool tcp_server = false, remote_server = false, udp_broadcast = false;
  uint32_t address = 0;
  unsigned port = 0;
  const char *count = NULL;
  bool udp_count = false, udp_unassigned = false;
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
    } else if (!strcmp(argv[i], "--udp-broadcast") && !udp_broadcast && i + 1 < argc) {
      if (!udp_parse_number(argv[++i], UINT16_MAX, &port) || !port) {
        goto usage;
      }
      udp_broadcast = true;
    } else if (!strcmp(argv[i], "--udp-unassigned") && !udp_unassigned) {
      udp_unassigned = true;
    } else if (!strcmp(argv[i], "--remote-server") && !remote_server && i + 1 < argc) {
      if (!udp_parse_number(argv[++i], UINT16_MAX, &port) || !port) {
        goto usage;
      }
      remote_server = true;
    } else if ((!strcmp(argv[i], "--tcp-count") || !strcmp(argv[i], "--udp-count")) &&
        !count && i + 1 < argc) {
      udp_count = !strcmp(argv[i], "--udp-count");
      unsigned value;
      count = argv[++i];
      if (!udp_parse_number(count, udp_count ? UINT16_MAX : UINT_MAX, &value) || !value) {
        goto usage;
      }
    } else {
      goto usage;
    }
  }
  if (startup_resource("script") != HANDLE_INVALID ||
      (udp_unassigned && !udp_broadcast) ||
      (tcp_server && start_services) ||
      (count && (udp_count ? !udp_broadcast : !tcp_server)) ||
      (udp_broadcast && (tcp_server || remote_server || start_services || start_remote_services)) ||
      (remote_server && (start_services || tcp_server || start_remote_services)) ||
      (start_remote_services && (start_services || tcp_server))) {
    goto usage;
  }
  if (tcp_server || udp_broadcast) {
    struct network_config network;
    if (!network_config_read(&network) ||
        (configure_network && !network_config_apply(&network))) {
      return EXIT_FAILURE;
    }
    return udp_broadcast ? launch_udp_broadcast((uint16_t)port, count, udp_unassigned) :
        launch_tcp_server(address, (uint16_t)port, count);
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
  int result;
  if (remote_server) {
    if (configure_network && !network_config_apply(&network)) {
      free(config.timezone);
      return EXIT_FAILURE;
    }
    result = launch_remote_server(&config, &network, (uint16_t)port);
  } else {
    result = launch_session(&config, &network, configure_network,
        start_services, start_remote_services);
  }
  free(config.timezone);
  return result;

usage:
  fputs("Usage: session.pxe [--configure-network] [--start-services | --start-remote-services]\n"
      "       session.pxe [--configure-network] --tcp-server ADDRESS PORT [--tcp-count COUNT]\n"
      "       session.pxe [--configure-network] --udp-broadcast PORT [--udp-count COUNT] [--udp-unassigned]\n"
      "       session.pxe [--configure-network] --remote-server PORT\n"
      "       (native init or trusted session handoff)\n", stderr);
  return EXIT_FAILURE;
}
