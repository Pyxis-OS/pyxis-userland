#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <pyxis/environment.h>
#include "remote_server.h"
#include <abi/console.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/thread.h>
#include <abi/pipe.h>
#include <abi/retention.h>
#include <abi/power.h>
#include <abi/wifi.h>
#include <abi/forwarder.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/audio.h>
#include <abi/system_info.h>
#include <abi/terminal.h>
#include <abi/udp.h>
#include <clock.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <job_work.h>
#include <net_config.h>
#include <network_environment.h>
#include <package_maintenance.h>
#include <program.h>
#include <remote/beacon.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>

#define NETWORK_ASSIGNMENT_SLEEP_NS UINT64_C(100000000)
#define READ_ONLY_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | \
    DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO)

static bool wait_for_address(handle_t authority, handle_t clock,
    const struct network_config *network, struct net_interface_reply *selected)
{
  (void)network;
  for (;;) {
    struct net_interface_reply snapshot;
    enum call_status status = net_interface_query(authority, &snapshot);
    if (status != CALL_OK) {
      fprintf(stderr, "session: selected remote interface unavailable (status %u)\n", status);
      return false;
    }
    if (snapshot.flags & NET_INTERFACE_ADDRESS_VALID) {
      *selected = snapshot;
      return true;
    }
    if (clock_sleep_for(clock, NETWORK_ASSIGNMENT_SLEEP_NS) != CALL_OK) {
      return false;
    }
  }
}

static bool directory_grant(handle_t source, bool read_only, struct launch_grant *grant)
{
  *grant = (struct launch_grant){.source = source};
  enum call_status status = handle_rights(source, &grant->rights, &grant->transport);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot query remote directory rights (status %u)\n", status);
    return false;
  }
  if (read_only) {
    grant->rights &= READ_ONLY_DIRECTORY_RIGHTS;
  }
  return true;
}

int launch_remote_server(const struct session_config *config,
    const struct network_config *network, uint16_t port, handle_t retention_account)
{
  const char *beacon = getenv("PYXIS_REMOTE_BEACON");
  handle_t udp_beacons = bootstrap_capability("udp_beacons");
  if (beacon && (!remote_beacon_name_length(beacon) || udp_beacons == HANDLE_INVALID)) {
    fputs("session: invalid reverse remote bootstrap\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t memory = bootstrap_capability("memory"), clock = bootstrap_capability("clock");
  handle_t launcher = bootstrap_capability("launcher"), tcp = bootstrap_capability("remote_tcp");
  handle_t terminal = bootstrap_capability("terminal"), output = bootstrap_capability("output");
  handle_t authority = bootstrap_capability("remote_interface");
  handle_t tmp = bootstrap_capability("tmp");
  if (memory == HANDLE_INVALID || clock == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      tcp == HANDLE_INVALID || terminal == HANDLE_INVALID || output == HANDLE_INVALID ||
      authority == HANDLE_INVALID || tmp == HANDLE_INVALID) {
    fputs("session: missing remote server launch authority\n", stderr);
    return EXIT_FAILURE;
  }

  struct net_interface_reply selected;
  if (!wait_for_address(authority, clock, network, &selected)) {
    return EXIT_FAILURE;
  }

  struct environment_capture captured = {0};
  enum call_status status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &captured);
  struct profile_directory selected_roots[PROFILE_DIRECTORY_LIMIT];
  size_t root_count = 0;
  if (status == CALL_OK) {
    status = profile_directories(&captured, selected_roots, PROFILE_DIRECTORY_LIMIT, &root_count);
  }
  if (status != CALL_OK) {
    environment_capture_close(&captured);
    return EXIT_FAILURE;
  }

  enum { MEMORY, CLOCK, LAUNCHER, TERMINAL, STDOUT, STDERR, NETWORK_ENDPOINT, FIRST_OPTIONAL };
  enum { OPTIONAL_COUNT = 27 };
  size_t grant_capacity = FIRST_OPTIONAL + OPTIONAL_COUNT + PROFILE_DIRECTORY_LIMIT + captured.count;
  struct launch_grant *grants = calloc(grant_capacity, sizeof(*grants));
  struct launch_environment_entry *entries = NULL;
  struct launch_environment_entry *resources = NULL;
  if (!grants) {
    environment_capture_close(&captured);
    return EXIT_FAILURE;
  }
  grants[MEMORY] = (struct launch_grant){memory, MEMORY_RIGHT_MANAGE, 0};
  grants[CLOCK] = (struct launch_grant){clock, CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE | CLOCK_RIGHT_SLEEP, 0};
  grants[LAUNCHER] = (struct launch_grant){launcher, LAUNCHER_RIGHT_LAUNCH, 0};
  grants[TERMINAL] = (struct launch_grant){terminal, TERMINAL_SERVICE_RIGHT_CREATE, 0};
  grants[STDOUT] = (struct launch_grant){output, CONSOLE_RIGHT_WRITE, 0};
  grants[STDERR] = (struct launch_grant){output, CONSOLE_RIGHT_WRITE, 0};
  resources = calloc(grant_capacity, sizeof(*resources));
  if (!resources) {
    free(grants);
    environment_capture_close(&captured);
    return EXIT_FAILURE;
  }
  resources[0] = (struct launch_environment_entry)PROFILE_CAP("memory", MEMORY);
  resources[1] = (struct launch_environment_entry)PROFILE_CAP("clock", CLOCK);
  resources[2] = (struct launch_environment_entry)PROFILE_CAP("launcher", LAUNCHER);
  resources[3] = (struct launch_environment_entry)PROFILE_CAP("terminal", TERMINAL);
  resources[4] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP(
      beacon ? "udp_beacons" : "tcp_listener", NETWORK_ENDPOINT);
  size_t resource_count = 5, grant_count = FIRST_OPTIONAL;
  resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("retention_account", grant_count);
  grants[grant_count++] = (struct launch_grant){retention_account, RETENTION_RIGHT_CAPTURE, 0};
  handle_t tcp_listen = bootstrap_capability(BUNDLE_TCP_LISTEN_SOURCE);
  if (tcp_listen != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP(BUNDLE_TCP_LISTEN_SOURCE, grant_count);
    grants[grant_count++] = (struct launch_grant){tcp_listen, TCP_SERVICE_RIGHTS, 0};
  }
  handle_t provisioner = bootstrap_capability("job_provisioner");
  if (provisioner != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"job_provisioner", grant_count);
    grants[grant_count++] = (struct launch_grant){provisioner, JOB_PROVISIONER_RIGHTS, 0};
  }
  handle_t threads = bootstrap_capability("threads");
  if (threads != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"threads", grant_count);
    grants[grant_count++] = (struct launch_grant){threads, THREAD_RIGHT_MANAGE, 0};
  }
  handle_t remote_mux_terminal = bootstrap_capability("remote_mux_terminal");
  if (remote_mux_terminal != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"remote_mux_terminal", grant_count);
    grants[grant_count++] = (struct launch_grant){remote_mux_terminal,
        TERMINAL_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t remote_power = bootstrap_capability("remote_power");
  if (remote_power != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"remote_power", grant_count);
    grants[grant_count++] = (struct launch_grant){remote_power, POWER_RIGHTS, 0};
  }
  struct launch_environment_entry roots[PROFILE_DIRECTORY_LIMIT];
  /* Reuse the selected tmp grant for cwd, including any withheld rights. */
  uint64_t working_directory = SIZE_MAX;
  handle_t image = HANDLE_INVALID, listener = HANDLE_INVALID;
  struct launch_environment_entry *environment = NULL;
  struct pyxis_environment_snapshot inherited = {0};
  struct network_environment refreshed_environment = {0};
  int result = EXIT_FAILURE;
  handle_t wifi = bootstrap_capability("wifi");
  if (wifi != HANDLE_INVALID) {
    struct launch_grant radio = {.source = wifi};
    if (handle_rights(wifi, &radio.rights, &radio.transport) != CALL_OK || radio.transport) {
      fputs("session: invalid Wi-Fi source\n", stderr);
      goto done;
    }
    radio.rights &= WIFI_RIGHT_READ | WIFI_RIGHT_SCAN | WIFI_RIGHT_CONNECT;
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"wifi", grant_count);
    grants[grant_count++] = radio;
  }

  for (size_t i = 0; i < root_count; ++i) {
    const char *name = (const char *)selected_roots[i].name;
    roots[i] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)selected_roots[i].name, grant_count);
    if (!directory_grant(selected_roots[i].handle, !strcmp(name, "boot"),
        &grants[grant_count])) {
      goto done;
    }
    if (!strcmp(name, "tmp")) {
      working_directory = grant_count;
    }
    ++grant_count;
  }
  if (working_directory == SIZE_MAX) {
    fputs("session: missing selected tmp root\n", stderr);
    goto done;
  }

  handle_t child_launcher = bootstrap_capability("child_launcher");
  if (child_launcher != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"child_launcher", grant_count);
    grants[grant_count++] = (struct launch_grant){child_launcher, LAUNCHER_RIGHT_LAUNCH, 0};
  }
  const struct environment_value *wifi_control =
      environment_capture_find(&captured, "wifi_session_control");
  if (wifi_control) {
    struct handle_info info;
    status = wifi_control->kind == ENVIRONMENT_CAP ?
        handle_query(wifi_control->handle, &info) : CALL_WRONG_TYPE;
    if (status != CALL_OK) {
      goto done;
    }
    if (wifi_control->flags || info.kind != HANDLE_KIND_NATIVE ||
        info.protocol != PROTOCOL_FORWARDER || info.transport ||
        !(info.rights & FORWARDER_RIGHT_CUT)) {
      status = CALL_DENIED;
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP("wifi_session_control", grant_count);
    grants[grant_count++] = (struct launch_grant){wifi_control->handle, FORWARDER_RIGHT_CUT, 0};
  }
  handle_t interpreter_root = bootstrap_capability("interpreter_root");
  if (interpreter_root != HANDLE_INVALID) {
    struct launch_grant root = {.source = interpreter_root};
    if (handle_rights(root.source, &root.rights, &root.transport) == CALL_OK) {
      resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"interpreter_root", grant_count);
      grants[grant_count++] = root;
    }
  }
  handle_t system_info = bootstrap_capability("system_info");
  if (system_info != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"system_info", grant_count);
    grants[grant_count++] = (struct launch_grant){system_info, SYSTEM_INFO_RIGHT_READ, 0};
  }
  handle_t audio = bootstrap_capability("audio");
  if (audio != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"audio", grant_count);
    grants[grant_count++] = (struct launch_grant){audio, AUDIO_RIGHT_PLAYBACK, 0};
  }
  handle_t screen_capture = bootstrap_capability("screen_capture");
  if (screen_capture != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"screen_capture", grant_count);
    grants[grant_count++] = (struct launch_grant){screen_capture,
        SCREEN_CAPTURE_RIGHT_CAPTURE, 0};
  }
  handle_t log = bootstrap_capability("log");
  if (log != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"log", grant_count);
    grants[grant_count++] = (struct launch_grant){log, LOG_RIGHT_READ, 0};
  }
  handle_t pipe = bootstrap_capability("pipe");
  if (pipe != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"pipe", grant_count);
    grants[grant_count++] = (struct launch_grant){pipe, PIPE_SERVICE_RIGHT_CREATE, 0};
  }
  resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"net_registry", grant_count);
  grants[grant_count++] = (struct launch_grant){bootstrap_capability("net_registry"), NET_REGISTRY_RIGHT_READ, 0};
  resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"tcp", grant_count);
  grants[grant_count++] = (struct launch_grant){bootstrap_capability("tcp"), TCP_SERVICE_RIGHT_CONNECT, 0};
  if (beacon) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("remote_tcp", grant_count);
    grants[grant_count++] = (struct launch_grant){tcp, TCP_SERVICE_RIGHT_CONNECT, 0};
  }
  handle_t random = bootstrap_capability("random");
  if (random != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"random", grant_count);
    grants[grant_count++] = (struct launch_grant){random, RANDOM_RIGHT_READ, 0};
  }
  handle_t service = bootstrap_capability("service");
  if (service != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"service", grant_count);
    grants[grant_count++] = (struct launch_grant){service, ENDPOINT_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t echo = bootstrap_capability("echo");
  if (echo != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"echo", grant_count);
    grants[grant_count++] = (struct launch_grant){echo, ECHO_RIGHT_SEND, 0};
  }
  handle_t udp = bootstrap_capability("udp");
  if (udp != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"udp", grant_count);
    grants[grant_count++] = (struct launch_grant){udp, UDP_SERVICE_RIGHT_OPEN, 0};
  }
  handle_t profile = bootstrap_capability("profile");
  if (profile != HANDLE_INVALID) {
    uint64_t rights;
    status = handle_rights(profile, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query remote profile rights (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"profile", grant_count);
    grants[grant_count++] = (struct launch_grant){profile,
        rights & (PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST), 0};
  }

  size_t environment_count = 0;
  status = pyxis_environment_snapshot_init(&inherited);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot snapshot remote environment (status %u)\n", status);
    goto done;
  }
  environment = session_environment(config, &inherited,
      &environment_count);
  if (!environment) {
    goto done;
  }
  char tab_width[sizeof("32")];
  snprintf(tab_width, sizeof(tab_width), "%zu", config->tab_width);
  const char *arguments[] = {"remote-terminal", tab_width, "--beacon", beacon};
  size_t entry_capacity = resource_count + root_count + environment_count + captured.count + 4;
  entries = calloc(entry_capacity, sizeof(*entries));
  if (!entries) {
    goto done;
  }
  size_t entry_count = resource_count;
  memcpy(entries, resources, resource_count * sizeof(*entries));
  memcpy(entries + entry_count, roots, root_count * sizeof(*entries));
  entry_count += root_count;
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDOUT, STDOUT);
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDERR, STDERR);
  struct launch_request request = {
    .grants = (uintptr_t)grants, .grant_count = grant_count,
    .entries = (uintptr_t)entries, .entry_count = entry_count,
    .working_directories = (uintptr_t)&working_directory, .working_directory_count = 1,
    .working_path = (uintptr_t)"tmp://",
    .argv = (uintptr_t)arguments, .argc = beacon ? 4 : 2,

  };
  status = program_image("remote-terminal", &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open the remote-terminal command (status %u)\n", status);
    goto done;
  }
  request.image = image;
  status = network_environment_read(&refreshed_environment, bootstrap_capability("net_registry"),
      bootstrap_capability("udp"), environment, environment_count);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot read remote network environment (status %u)\n", status);
    goto done;
  }
  memcpy(entries + request.entry_count, refreshed_environment.variables,
      refreshed_environment.count * sizeof(*entries));
  request.entry_count += refreshed_environment.count;
  if (refreshed_environment.dns_udp != HANDLE_INVALID) {
    entries[request.entry_count++] = (struct launch_environment_entry)PROFILE_CAP("dns_udp", request.grant_count);
    grants[request.grant_count++] = (struct launch_grant){refreshed_environment.dns_udp, UDP_SERVICE_RIGHT_OPEN, 0};
  }
  size_t prepared_grants = request.grant_count, prepared_entries = request.entry_count;
  for (size_t i = 0; i < captured.count; ++i) {
    const struct environment_value *value = &captured.values[i];
    if (value->kind != ENVIRONMENT_CAP || !package_launch_source(value->name)) continue;
    if (prepared_grants == grant_capacity || prepared_entries == entry_capacity) {
      status = CALL_LIMIT;
      goto done;
    }
    entries[prepared_entries++] = (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP((uintptr_t)value->name, prepared_grants);
    grants[prepared_grants++] = (struct launch_grant){value->handle, value->rights, value->transport};
  }
  status = profile_inherit_services(&captured, false, grants, grant_capacity,
      &prepared_grants, entries, entry_capacity, &prepared_entries);
  if (status != CALL_OK) {
    goto done;
  }
  request.grant_count = prepared_grants;
  request.entry_count = prepared_entries;
  if (beacon) {
    grants[NETWORK_ENDPOINT] = (struct launch_grant){udp_beacons, UDP_SERVICE_RIGHT_BROADCAST, 0};
  } else {
    struct tcp_listen_reply reply;
    status = tcp_listen_path(tcp, (struct net_path){.interface_id = selected.interface_id,
      .source = selected.address}, port, &reply);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot start remote listener (status %u)\n", status);
      goto done;
    }
    listener = reply.handle;
    grants[NETWORK_ENDPOINT] = (struct launch_grant){listener,
        TCP_LISTENER_RIGHT_INSPECT | TCP_LISTENER_RIGHT_ACCEPT, 0};
  }
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch remote server (status %u)\n", status);
    goto done;
  }
  if (beacon) {
    printf("remote: waiting for beacon %s\n", beacon);
  } else {
    printf("remote: listening on %u.%u.%u.%u:%u\n", selected.address >> 24,
        (selected.address >> 16) & 255, (selected.address >> 8) & 255, selected.address & 255, port);
  }
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  free(resources);
  free(entries);
  free(grants);
  environment_capture_close(&captured);
  network_environment_free(&refreshed_environment);
  free(environment);
  pyxis_environment_snapshot_close(&inherited);
  if (listener != HANDLE_INVALID && handle_close(listener) != 0) {
    result = EXIT_FAILURE;
  }
  if (image != HANDLE_INVALID && handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}
