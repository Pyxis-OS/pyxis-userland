#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <pyxis/environment.h>
#include <pyxis/working_path.h>
#include "config.h"
#include "retention.h"
#include "screenshot.h"
#include "network.h"
#include "wifi_supplicant.h"
#include "remote_server.h"
#include "tcp_server.h"
#include "../common/udp.h"
#include "../common/directory.h"
#include <abi/clock.h>
#include <abi/system_info.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/random.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/screen_capture.h>
#include <abi/audio.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <abi/stream.h>
#include <abi/power.h>
#include <abi/wifi.h>
#include <abi/forwarder.h>
#include <abi/keyboard.h>
#include <abi/pointer.h>
#include <abi/space.h>
#include <abi/profile.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/thread.h>
#include <abi/endpoint.h>
#include <abi/net_config.h>
#include <abi/terminal.h>
#include <abi/terminal_pointer.h>
#include <abi/clipboard.h>
#include <directory.h>
#include <bundle_launch.h>
#include <handle.h>
#include <launcher.h>
#include <job_work.h>
#include <network_environment.h>
#include <package_maintenance.h>
#include <program.h>
#include <shell_settings.h>
#include <limits.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

static int launch_session(const struct session_config *config, struct network_config *network,
    struct network_runtime *runtime, bool configure_network,
    bool start_services, bool start_remote_services, const char *services_script,
    handle_t retention_account, struct session_wifi_supplicant *supplicant)
{
  struct terminal terminal = {bootstrap_capability("input"), bootstrap_capability("output")};
  handle_t launcher = bootstrap_capability("launcher");
  handle_t memory = bootstrap_capability("memory");
  handle_t boot = bootstrap_capability("boot"), tmp = bootstrap_capability("tmp");
  if (terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID ||
      launcher == HANDLE_INVALID || memory == HANDLE_INVALID ||
      boot == HANDLE_INVALID || tmp == HANDLE_INVALID) {
    fputs("session: missing startup resource or filesystem root\n", stderr);
    return EXIT_FAILURE;
  }

  bool script = start_services || start_remote_services;
  if (configure_network && !network_config_apply(network, runtime)) {
    return EXIT_FAILURE;
  }
  if (!configure_network && script) {
    enum call_status status = network_environment_wait(bootstrap_capability("net_registry"), bootstrap_capability("clock"));
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      fprintf(stderr, "session: cannot wait for network setup (status %u)\n", status);
      return EXIT_FAILURE;
    }
  }

  session_wifi_start(supplicant);
  handle_t mux_terminal = bootstrap_capability("mux_terminal");
  bool multiplex = !script && mux_terminal != HANDLE_INVALID;
  /* The services script names its own location: the archive's on live images,
   * system://config/ on installed ones. mux is a package; the core archive keeps
   * the rescue shell. */
  const char *image_uri = script ? services_script : multiplex ? "mux" : "boot://init-shell.pxe";
  handle_t image;
  enum call_status status = script ? resolve_file(services_script, FILE_RIGHT_READ, &image) :
      multiplex ? program_image("mux", &image) :
      directory_lookup(boot, "init-shell.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open %s (status %u)\n", image_uri, status);
    return EXIT_FAILURE;
  }

  enum { INPUT, OUTPUT, MEMORY, LAUNCHER, FIRST_OPTIONAL };
  enum { OPTIONAL_RESOURCE_COUNT = 36 };
  struct environment_capture captured = {0};
  status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &captured);
  struct profile_directory selected_roots[PROFILE_DIRECTORY_LIMIT];
  size_t root_count = 0;
  if (status == CALL_OK) {
    status = profile_directories(&captured, selected_roots, PROFILE_DIRECTORY_LIMIT, &root_count);
  }
  if (status != CALL_OK) {
    environment_capture_close(&captured);
    handle_close(image);
    fprintf(stderr, "session: cannot capture selected directories (status %u)\n", status);
    return EXIT_FAILURE;
  }
  const struct path_context *context;
  status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    environment_capture_close(&captured);
    handle_close(image);
    fprintf(stderr, "session: cannot retain working directory (status %u)\n", status);
    return EXIT_FAILURE;
  }
  size_t depth = context->count;
  size_t fixed_grants = FIRST_OPTIONAL + OPTIONAL_RESOURCE_COUNT +
      PROFILE_DIRECTORY_LIMIT + ENVIRONMENT_STREAM_COUNT + 1 + BUNDLE_SOURCE_LIMIT;
  if (root_count > PROFILE_DIRECTORY_LIMIT ||
      depth > SIZE_MAX / sizeof(struct launch_grant) - fixed_grants) {
    environment_capture_close(&captured);
    handle_close(image);
    fputs("session: startup metadata too large\n", stderr);
    return EXIT_FAILURE;
  }
  size_t grant_capacity = fixed_grants + depth + captured.count;
  struct launch_grant *grants = malloc(grant_capacity * sizeof(*grants));
  struct launch_environment_entry *entries = NULL;
  struct launch_environment_entry *resources = NULL;
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  size_t environment_count = 0;
  struct pyxis_environment_snapshot inherited = {0};
  struct launch_environment_entry *environment = NULL;
  struct network_environment refreshed_environment = {0};
  int result = EXIT_FAILURE;
  handle_t settings = HANDLE_INVALID;
  status = pyxis_environment_snapshot_init(&inherited);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot snapshot environment (status %u)\n", status);
    goto done;
  }
  environment = session_environment(config, &inherited,
      &environment_count);
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
  if (start_remote_services || multiplex || (start_services && mux_terminal != HANDLE_INVALID)) {
    uint64_t rights;
    status = handle_rights(launcher, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query launcher rights (status %u)\n", status);
      goto done;
    }
    grants[LAUNCHER].rights = rights & LAUNCHER_RIGHT_LAUNCH;
  }
  resources = calloc(grant_capacity, sizeof(*resources));
  if (!resources) {
    goto done;
  }
  resources[0] = (struct launch_environment_entry)PROFILE_CAP("input", INPUT);
  resources[1] = (struct launch_environment_entry)PROFILE_CAP("output", OUTPUT);
  resources[2] = (struct launch_environment_entry)PROFILE_CAP("memory", MEMORY);
  resources[3] = (struct launch_environment_entry)PROFILE_CAP("launcher", LAUNCHER);
  size_t resource_count = 4, grant_count = FIRST_OPTIONAL;
  resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP("retention_account", grant_count);
  grants[grant_count++] = (struct launch_grant){retention_account, RETENTION_RIGHT_CAPTURE, 0};
  static const char *const configuration_names[] = {"session_config", "network_config"};
  for (size_t i = 0; i < sizeof(configuration_names) / sizeof(*configuration_names); ++i) {
    const struct environment_value *value = environment_capture_find(&captured, configuration_names[i]);
    if (!value) {
      continue;
    }
    struct handle_info info;
    status = value->kind == ENVIRONMENT_CAP ? handle_query(value->handle, &info) : CALL_WRONG_TYPE;
    if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE ||
        info.rights != FILE_RIGHT_READ || info.transport)) {
      status = CALL_DENIED;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot retain selected %s FILE (status %u)\n",
          configuration_names[i], status);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP(configuration_names[i], grant_count);
    grants[grant_count++] = (struct launch_grant){value->handle, FILE_RIGHT_READ, 0};
  }
  handle_t threads = bootstrap_capability("threads");
  if (threads != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"threads", grant_count);
    grants[grant_count++] = (struct launch_grant){threads, THREAD_RIGHT_MANAGE, 0};
  }
  handle_t job_provisioner = bootstrap_capability("job_provisioner");
  if (job_provisioner != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"job_provisioner", grant_count);
    grants[grant_count++] = (struct launch_grant){job_provisioner, JOB_PROVISIONER_RIGHTS, 0};
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
  handle_t display = bootstrap_capability("display");
  if (display != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"display", grant_count);
    grants[grant_count++] = (struct launch_grant){display, DISPLAY_RIGHT_DRAW, 0};
  }
  handle_t audio = bootstrap_capability("audio");
  if (audio != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"audio", grant_count);
    grants[grant_count++] = (struct launch_grant){audio, AUDIO_RIGHT_PLAYBACK, 0};
  }
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
  handle_t screen_capture = bootstrap_capability("screen_capture");
  if (screen_capture != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"screen_capture", grant_count);
    grants[grant_count++] = (struct launch_grant){screen_capture,
        SCREEN_CAPTURE_RIGHT_CAPTURE, 0};
  }
  handle_t clock = bootstrap_capability("clock");
  if (clock != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"clock", grant_count);
    grants[grant_count++] = (struct launch_grant){clock, CLOCK_RIGHTS, 0};
  }
  handle_t system_info = bootstrap_capability("system_info");
  if (system_info != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"system_info", grant_count);
    grants[grant_count++] = (struct launch_grant){system_info, SYSTEM_INFO_RIGHT_READ, 0};
  }
  handle_t log = bootstrap_capability("log");
  if (log != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"log", grant_count);
    grants[grant_count++] = (struct launch_grant){log, LOG_RIGHT_READ, 0};
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
  if (start_remote_services) {
    handle_t udp_beacons = bootstrap_capability("udp_beacons");
    if (udp_beacons != HANDLE_INVALID) {
      resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"udp_beacons", grant_count);
      grants[grant_count++] = (struct launch_grant){udp_beacons, UDP_SERVICE_RIGHT_BROADCAST, 0};
    }
  }
  if (start_remote_services) {
    static const struct { const char *name; uint64_t rights; } remote[] = {
      {"remote_interface", NET_INTERFACE_RIGHT_READ}, {"remote_tcp", TCP_SERVICE_RIGHTS},
    };
    for (size_t i = 0; i < sizeof(remote) / sizeof(*remote); ++i) {
      handle_t source = bootstrap_capability(remote[i].name);
      if (source != HANDLE_INVALID) {
        resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP(remote[i].name, grant_count);
        grants[grant_count++] = (struct launch_grant){source, remote[i].rights, 0};
      }
    }
  }
  handle_t tcp = bootstrap_capability("tcp");
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
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"tcp", grant_count);
    grants[grant_count++] = (struct launch_grant){tcp, rights, 0};
  }
  handle_t net_registry = bootstrap_capability("net_registry");
  if (net_registry != HANDLE_INVALID) {
    uint64_t rights;
    status = handle_rights(net_registry, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query network rights (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"net_registry", grant_count);
    grants[grant_count++] = (struct launch_grant){net_registry,
        rights & NET_REGISTRY_RIGHT_READ, 0};
  }
  /* Only a direct interactive shell; mux opens the file for each pane. */
  if (!script && !multiplex) {
    const struct environment_value *home = environment_capture_find(&captured, "home");
    enum call_status opened = shell_settings_open(home && home->kind == ENVIRONMENT_CAP ?
        home->handle : HANDLE_INVALID, &settings);
    if (opened == CALL_OK) {
      resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP(SHELL_SETTINGS_RESOURCE, grant_count);
      grants[grant_count++] = (struct launch_grant){settings, FILE_RIGHT_READ, 0};
    } else if (opened != CALL_NOT_FOUND) {
      fprintf(stderr, "session: cannot open %s (status %u); shell settings skipped\n",
          SHELL_SETTINGS_PATH, opened);
    }
  }
  if (start_remote_services) {
    handle_t terminal_service = bootstrap_capability("terminal");
    if (terminal_service == HANDLE_INVALID || net_registry == HANDLE_INVALID) {
      fputs("session: missing remote service bootstrap authority\n", stderr);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"terminal", grant_count);
    grants[grant_count++] = (struct launch_grant){terminal_service,
        TERMINAL_SERVICE_RIGHT_CREATE, 0};
  }
  if (mux_terminal != HANDLE_INVALID && !start_remote_services) {
    uint64_t rights, transport;
    status = handle_rights(mux_terminal, &rights, &transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query multiplexer authority (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)(multiplex ? "terminal" : "mux_terminal"), grant_count);
    grants[grant_count++] = (struct launch_grant){mux_terminal,
        rights & TERMINAL_SERVICE_RIGHT_CREATE, transport};
    handle_t terminal_pointer = bootstrap_capability("terminal_pointer");
    if (terminal_pointer == HANDLE_INVALID) {
      fputs("session: missing multiplexer pointer authority\n", stderr);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"terminal_pointer", grant_count);
    grants[grant_count++] = (struct launch_grant){terminal_pointer,
        TERMINAL_POINTER_RIGHT_CONTROL, 0};
  }
  /* Local session successors retain each explicit grant independently of mux
   * startup. Store authority alone grants no terminal or graphics ownership. */
  if (!start_remote_services) {
    static const char *const clipboard_names[] = {"clipboard_local", "clipboard_shared"};
    for (size_t i = 0; i < sizeof(clipboard_names) / sizeof(clipboard_names[0]); ++i) {
      handle_t clipboard = bootstrap_capability(clipboard_names[i]);
      if (clipboard == HANDLE_INVALID) {
        continue;
      }
      resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)clipboard_names[i], grant_count);
      grants[grant_count++] = (struct launch_grant){clipboard, CLIPBOARD_RIGHTS, 0};
    }
  }
  handle_t pipe = bootstrap_capability("pipe");
  if (pipe != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"pipe", grant_count);
    grants[grant_count++] = (struct launch_grant){pipe, PIPE_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t service = bootstrap_capability("service");
  if (service != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"service", grant_count);
    grants[grant_count++] = (struct launch_grant){service, ENDPOINT_SERVICE_RIGHT_CREATE, 0};
  }
  handle_t random = bootstrap_capability("random");
  if (random != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"random", grant_count);
    grants[grant_count++] = (struct launch_grant){random, RANDOM_RIGHT_READ, 0};
  }
  handle_t keyboard = bootstrap_capability("keyboard");
  if (keyboard != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"keyboard", grant_count);
    grants[grant_count++] = (struct launch_grant){keyboard, KEYBOARD_RIGHT_INPUT, 0};
  }
  handle_t pointer = bootstrap_capability("pointer");
  if (pointer != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"pointer", grant_count);
    grants[grant_count++] = (struct launch_grant){pointer, POINTER_RIGHT_INPUT, 0};
  }

  /* Local power never authorizes remote shells; they need the separate opt-in. */
  handle_t power = bootstrap_capability("power");
  if (power != HANDLE_INVALID && !start_remote_services) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"power", grant_count);
    grants[grant_count++] = (struct launch_grant){power, POWER_RIGHTS, 0};
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

  handle_t space = bootstrap_capability("space");
  if (space != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"space", grant_count);
    grants[grant_count++] = (struct launch_grant){space, SPACE_RIGHT_SET_TITLE, 0};
  }

  handle_t profile = bootstrap_capability("profile");
  if (profile != HANDLE_INVALID) {
    uint64_t rights;
    status = handle_rights(profile, &rights, NULL);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query profile rights (status %u)\n", status);
      goto done;
    }
    resources[resource_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"profile", grant_count);
    grants[grant_count++] = (struct launch_grant){profile,
        rights & (PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST), 0};
  }

  if (start_remote_services) {
    for (size_t i = 0; i < captured.count; ++i) {
      const struct environment_value *value = &captured.values[i];
      if (value->kind != ENVIRONMENT_CAP || !package_launch_source(value->name)) {
        continue;
      }
      if (grant_count == grant_capacity || resource_count == grant_capacity) {
        status = CALL_LIMIT;
        goto done;
      }
      resources[resource_count++] = (struct launch_environment_entry)
          PROFILE_PRIVATE_CAP((uintptr_t)value->name, grant_count);
      grants[grant_count++] = (struct launch_grant){value->handle, value->rights, value->transport};
    }
    /* The remote bootstrap needs its declared private source for provider setup;
     * the remote server and its clients never receive this ancestor. */
    handle_t staging = bootstrap_capability(BUNDLE_SOURCE_PREFIX "http_staging");
    if (staging != HANDLE_INVALID) {
      struct handle_info info;
      status = handle_query(staging, &info);
      if (status != CALL_OK || info.kind != HANDLE_KIND_NATIVE ||
          info.protocol != PROTOCOL_DIRECTORY || info.transport) {
        goto done;
      }
      resources[resource_count++] = (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)BUNDLE_SOURCE_PREFIX "http_staging", grant_count);
      grants[grant_count++] = (struct launch_grant){staging, info.rights, 0};
    }
  } else {
    status = bundle_sources_forward_capture(&captured, grants, grant_capacity, &grant_count,
        resources, grant_capacity, &resource_count);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot retain selected sources (status %u)\n", status);
      goto done;
    }
  }

  struct launch_environment_entry roots[PROFILE_DIRECTORY_LIMIT];
  for (size_t i = 0; i < root_count; ++i) {
    roots[i] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)selected_roots[i].name, grant_count);
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
  const char *working_path = pyxis_working_path();
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = grant_count;
    struct launch_grant *grant = &grants[grant_count++];
    *grant = (struct launch_grant){.source = context->directories[i]};
    status = handle_rights(grant->source, &grant->rights, &grant->transport);
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot query directory rights (status %u)\n", status);
      goto done;
    }
  }

  char tab_width[3];
  snprintf(tab_width, sizeof(tab_width), "%zu", config->tab_width);
  const char *arguments[] = {image_uri, "--tab-width", tab_width};
  size_t entry_capacity = resource_count + root_count + environment_count +
      captured.count + ENVIRONMENT_STREAM_COUNT + 4;
  entries = calloc(entry_capacity, sizeof(*entries));
  if (!entries) {
    goto done;
  }
  size_t entry_count = resource_count;
  memcpy(entries, resources, resource_count * sizeof(*entries));
  memcpy(entries + entry_count, roots, root_count * sizeof(*entries));
  entry_count += root_count;
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = grant_count,
    .entries = (uintptr_t)entries, .entry_count = entry_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)working_path,
    .argv = (uintptr_t)arguments, .argc = multiplex ? 3 : 1,
  };
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    struct pyxis_stdio_binding stream = bootstrap_stdio(i);
    if (stream.protocol == 0) {
      continue;
    }
    bool input = i == ENVIRONMENT_STDIN;
    bool response = stream.protocol == PROTOCOL_STREAM;
    if (response && !input) {
      fputs("session: invalid output stream\n", stderr);
      goto done;
    }
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        stream.protocol == PROTOCOL_PIPE ?
        (input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE) :
        response ? STREAM_RIGHT_READ :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    entries[request.entry_count++] = profile_stdio_entry(i, request.grant_count);
    if (stream.append) {
      entries[request.entry_count++] = environment_stdio_append_entry(i);
    }
    uint64_t transport = 0;
    if (stream.protocol == PROTOCOL_FILE || response) {
      struct handle_info info;
      status = handle_query(stream.handle, &info);
      if (status != CALL_OK) {
        fprintf(stderr, "session: cannot query stream (status %u)\n", status);
        goto done;
      }
      if (info.protocol != stream.protocol ||
          (info.kind != HANDLE_KIND_EXPORTED && (response || info.kind != HANDLE_KIND_NATIVE))) {
        fputs("session: invalid stream\n", stderr);
        goto done;
      }
      if (info.kind == HANDLE_KIND_EXPORTED) {
        transport = HANDLE_TRANSPORT_CALL;
      }
    }
    grants[request.grant_count++] = (struct launch_grant){stream.handle, rights, transport};
  }

  status = network_environment_read(&refreshed_environment, net_registry,
      bootstrap_capability(net_registry != HANDLE_INVALID ? "udp" : "dns_udp"),
      environment, environment_count);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot read network environment (status %u)\n", status);
    goto done;
  }
  /* Inherited text never replaces an entry the session already supplies. */
  size_t supplied_count = request.entry_count;
  for (size_t i = 0; i < refreshed_environment.count; ++i) {
    const char *name = (const char *)(uintptr_t)refreshed_environment.variables[i].name;
    bool supplied = false;
    for (size_t j = 0; j < supplied_count; ++j) {
      supplied |= !strcmp(name, (const char *)(uintptr_t)entries[j].name);
    }
    if (!supplied) {
      entries[request.entry_count++] = refreshed_environment.variables[i];
    }
  }
  if (refreshed_environment.dns_udp != HANDLE_INVALID) {
    entries[request.entry_count++] = (struct launch_environment_entry)PROFILE_CAP("dns_udp", request.grant_count);
    grants[request.grant_count++] = (struct launch_grant){refreshed_environment.dns_udp, UDP_SERVICE_RIGHT_OPEN, 0};
  }
  size_t prepared_grants = request.grant_count;
  size_t prepared_entries = request.entry_count;
  status = profile_inherit_services(&captured, true, grants, grant_capacity,
      &prepared_grants, entries, entry_capacity, &prepared_entries);
  if (status != CALL_OK) {
    goto done;
  }
  request.grant_count = prepared_grants;
  request.entry_count = prepared_entries;
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
  const struct environment_value *control = environment_capture_find(&captured, "wifi_session_control");
  if (control) {
    struct environment_unset_entry removed = {.identity = control->identity};
    environment_name(removed.name, "wifi_session_control");
    if (environment_unset_batch(startup_environment_handle(), &removed, 1) != CALL_OK) {
      result = EXIT_FAILURE;
    }
  }

done:
  if (settings != HANDLE_INVALID) {
    handle_close(settings);
  }
  free(resources);
  free(entries);
  environment_capture_close(&captured);
  network_environment_free(&refreshed_environment);
  free(environment);
  pyxis_environment_snapshot_close(&inherited);
  free(directories);
  free(grants);
  if (handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}

static bool release_bootstrap_grants(struct network_runtime *runtime)
{
  pyxis_working_clear();
  struct environment_capture capture = {0};
  enum call_status status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
  if (status != CALL_OK) {
    return false;
  }
  struct environment_unset_entry *removed = calloc(capture.count, sizeof(*removed));
  if (capture.count && !removed) {
    environment_capture_close(&capture);
    return false;
  }
  size_t count = 0;
  for (size_t i = 0; i < capture.count; ++i) {
    const struct environment_value *entry = &capture.values[i];
    if (entry->kind != ENVIRONMENT_CAP) {
      continue;
    }
    bool keep = !strcmp(entry->name, "memory") || !strcmp(entry->name, "output") ||
        !strcmp(entry->name, "stdout") || !strcmp(entry->name, "stderr") ||
        !strcmp(entry->name, "clock") || !strcmp(entry->name, "random") ||
        !strcmp(entry->name, "net_registry") || !strcmp(entry->name, "net_policy") ||
        !strncmp(entry->name, "config.", 7) || !strncmp(entry->name, "dhcp.", 5);
    if (!keep) {
      environment_name(removed[count].name, entry->name);
      removed[count++].identity = entry->identity;
    }
  }
  for (size_t i = 0; status == CALL_OK && i < count; ++i) {
    status = environment_unset_batch(startup_environment_handle(), &removed[i], 1);
  }
  free(removed);
  environment_capture_close(&capture);
  bool success = status == CALL_OK && network_runtime_release(runtime);
  const handle_t retained[] = {
    bootstrap_capability("memory"), bootstrap_capability("output"),
    bootstrap_stdio(ENVIRONMENT_STDOUT).handle, bootstrap_stdio(ENVIRONMENT_STDERR).handle,
  };
  success &= bootstrap_release_except(retained, sizeof(retained) / sizeof(*retained));
  for (size_t i = 0; i < startup_working_directory_count(); ++i) {
    bool duplicate = false;
    for (size_t j = 0; j < i; ++j) {
      duplicate |= startup_working_directory(i) == startup_working_directory(j);
    }
    if (!duplicate && handle_close(startup_working_directory(i)) != 0) {
      success = false;
    }
  }
  if (bootstrap_stdio(ENVIRONMENT_STDIN).protocol != 0 && fclose(stdin) != 0) {
    success = false;
  }
  runtime->background = true;
  if (!success) {
    fputs("session: cannot release bootstrap grants\n", stderr);
  }
  return success;
}

static int finish_network(const struct network_config *config,
    struct network_runtime *runtime, struct session_wifi_supplicant *supplicant, int result)
{
  if (result == EXIT_SUCCESS &&
      (runtime->waiting_link || session_wifi_active(supplicant))) {
    if (release_bootstrap_grants(runtime)) {
      result = network_config_maintain(config, runtime, supplicant);
    } else {
      result = EXIT_FAILURE;
    }
  }
  if (!session_wifi_stop(supplicant)) {
    fputs("session: Wi-Fi authentication cleanup not confirmed\n", stderr);
  }
  if (result != EXIT_SUCCESS && !network_config_stop(config, runtime)) {
    result = EXIT_FAILURE;
  }
  return result;
}

int main(int argc, char **argv)
{
  bool configure_network = false, start_services = false, start_remote_services = false;
  const char *services_script = NULL;
  bool tcp_server = false, remote_server = false;
  uint32_t address = 0;
  unsigned port = 0;
  const char *count = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--configure-network") && !configure_network) {
      configure_network = true;
    } else if (!strcmp(argv[i], "--start-services") && !start_services && i + 1 < argc) {
      start_services = true;
      services_script = argv[++i];
    } else if (!strcmp(argv[i], "--start-remote-services") && !start_remote_services &&
        i + 1 < argc) {
      start_remote_services = true;
      services_script = argv[++i];
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
  if (bootstrap_capability("script") != HANDLE_INVALID ||
      (tcp_server && start_services) ||
      (count && !tcp_server) ||
      (remote_server && (start_services || tcp_server || start_remote_services)) ||
      (start_remote_services && (start_services || tcp_server))) {
    goto usage;
  }
  session_start_screenshot_saver();
  if (!start_services && !start_remote_services) {
    handle_t staging = bootstrap_take_capability(BUNDLE_SOURCE_PREFIX "http_staging");
    if (staging != HANDLE_INVALID && handle_close(staging)) {
      fputs("session: cannot consume private HTTP backing source\n", stderr);
      return EXIT_FAILURE;
    }
  }
  struct session_wifi_supplicant supplicant = {0};
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
    result = finish_network(&network, &runtime, &supplicant, result);
    network_config_free(&network);
    return result;
  }
  struct session_config config;
  if (!session_config_read(&config)) {
    return EXIT_FAILURE;
  }
  handle_t retention_account;
  enum call_status retention = session_retention_init(&config, &retention_account);
  if (retention != CALL_OK) {
    fprintf(stderr, "session: cannot establish retention account (status %u)\n", retention);
    session_config_free(&config);
    return EXIT_FAILURE;
  }
  struct network_config network;
  if (!network_config_read(&network)) {
    handle_close(retention_account);
    session_config_free(&config);
    return EXIT_FAILURE;
  }
  struct network_runtime runtime = {0};
  int result = EXIT_FAILURE;
  if (remote_server) {
    if (!configure_network || (network_config_apply(&network, &runtime) &&
        network_config_wait_address(&network, &runtime))) {
      result = launch_remote_server(&config, &network, (uint16_t)port, retention_account);
    }
  } else {
    result = launch_session(&config, &network, &runtime, configure_network,
        start_services, start_remote_services, services_script, retention_account, &supplicant);
  }
  if (handle_close(retention_account) != 0) {
    result = EXIT_FAILURE;
  }
  session_config_free(&config);
  result = finish_network(&network, &runtime, &supplicant, result);
  network_config_free(&network);
  return result;

usage:
  fputs("Usage: session.pxe [--configure-network] [--start-services SCRIPT | --start-remote-services SCRIPT]\n"
      "       session.pxe [--configure-network] --tcp-server ADDRESS PORT [--tcp-count COUNT]\n"
      "       session.pxe [--configure-network] --remote-server PORT\n"
      "       (native init or trusted session handoff)\n", stderr);
  return EXIT_FAILURE;
}
