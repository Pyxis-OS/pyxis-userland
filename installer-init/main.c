/* installer-init: the Install entry's space init (docs/wip/pkg-upgrade.md,
 * decisions 63, 66, 69 and 70). Boot init gives it the installer capability and
 * the archive's network. It brings the network up, starts httpfs for the
 * installer's package downloads, offers Install, Update and Replay, and launches
 * the installer with the chosen action's narrowed authority. It services the
 * network until the installer exits. No shell or session runs in this space, so
 * the installer capability never passes through one. */
#include "../common/bootstrap_environment.h"
#include "../common/launch_environment.h"
#include "../common/provider_setup.h"
#include "../installer/plan_record.h"
#include "../network-runtime/network.h"
#include "../network-runtime/wifi_supplicant.h"
#include "update.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/disk.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/power.h>
#include <abi/uefi.h>
#include <abi/provider.h>
#include <abi/random.h>
#include <abi/system_info.h>
#include <bundle.h>
#include <bundle_launch.h>
#include <directory.h>
#include <endpoint.h>
#include <handle.h>
#include <launcher.h>
#include <network_environment.h>
#include <process.h>
#include <pyxis/descriptor.h>
#include <pyxis/working_path.h>
#include <pthread.h>
#include <stdatomic.h>
#include <net_config.h>
#include <abi/net_config.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>
#include <unistd.h>

#if INSTALLER_PUBLICATION_SCRATCH
#define INSTALL_ESP_DISK_RIGHTS DISKS_RIGHT_OPEN_OWNED_ESP
#else
#define INSTALL_ESP_DISK_RIGHTS (DISKS_RIGHT_MOUNT_READ_ONLY | DISKS_RIGHT_MOUNT_READ_WRITE)
#endif

#define INSTALL_COMMON_DISK_RIGHTS (DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN_READ_ONLY | \
    DISKS_RIGHT_OPEN_POOL | INSTALL_ESP_DISK_RIGHTS)
#define SNAPSHOT_PARENT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES | \
    DIRECTORY_RIGHT_FILESYSTEM_INFO)
#define STAGING_RIGHTS (DIRECTORY_CONTENT_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO)
/* How long Install waits for the network's DNS choice, which a lease brings. */
#define INSTALL_NETWORK_WAIT_NS UINT64_C(30000000000)
#define INSTALL_NETWORK_POLL_NS UINT64_C(250000000)

/* The archive's network. After the profile is applied, its maintenance thread
 * alone touches RUNTIME and SUPPLICANT, from the menu until the installer
 * exits, so a late lease still completes. */
struct network {
  struct network_config config;
  struct network_runtime runtime;
  struct network_wifi_supplicant supplicant;
  atomic_bool stop;
  pthread_t thread;
  bool started, maintaining;
};

/* The installer's GET-only package downloads. */
struct providers {
  handle_t httpfs; /* The provider process, while it runs. */
  handle_t http, https; /* Provider roots; HTTPS is absent when TLS is not ready. */
  bool dns; /* httpfs started with a DNS server. */
  unsigned starts; /* Names each start's staging leaf. */
  char missing[160]; /* Why Install cannot fetch; empty when it can. */
};

static void *maintain_network(void *argument)
{
  struct network *network = argument;
  network_config_maintain(&network->config, &network->runtime, &network->supplicant,
      &network->stop);
  return NULL;
}

/* Applies the archive's network profile, starts the Wi-Fi supplicant it names
 * and keeps both serviced. Package staging needs this network. */
static void start_network(struct network *network)
{
  if (!network_config_read(&network->config)) {
    return;
  }
  if (!network_config_apply(&network->config, &network->runtime)) {
    fputs("installer-init: the network profile cannot be applied\n", stderr);
    return;
  }
  network->started = true;
  network_wifi_start(&network->supplicant);
  network->maintaining = pthread_create(&network->thread, NULL, maintain_network, network) == 0;
  if (!network->maintaining) {
    fputs("installer-init: network maintenance unavailable; a late lease is not taken\n", stderr);
  }
}

static bool stop_network(struct network *network)
{
  bool clean = true;
  if (network->maintaining) {
    atomic_store(&network->stop, true);
    clean = pthread_join(network->thread, NULL) == 0;
    network->maintaining = false;
  }
  if (network->started) {
    if (!network_wifi_stop(&network->supplicant)) {
      fputs("installer-init: Wi-Fi authentication cleanup not confirmed\n", stderr);
    }
    clean &= network_config_stop(&network->config, &network->runtime);
    network->started = false;
  }
  network_config_free(&network->config);
  return clean;
}

static void stop_httpfs(struct providers *providers)
{
  handle_t *handles[] = {&providers->http, &providers->https};
  for (size_t i = 0; i < sizeof(handles) / sizeof(*handles); ++i) {
    if (*handles[i] != HANDLE_INVALID) {
      handle_close(*handles[i]);
      *handles[i] = HANDLE_INVALID;
    }
  }
  if (providers->httpfs != HANDLE_INVALID) {
    struct process_result completion;
    if (process_terminate(providers->httpfs) != CALL_OK ||
        process_wait(providers->httpfs, &completion) != CALL_OK) {
      fputs("installer-init: httpfs termination unconfirmed\n", stderr);
    }
    handle_close(providers->httpfs);
    providers->httpfs = HANDLE_INVALID;
  }
}

/* Takes the http and https roots from httpfs's setup report and answers it. */
static enum call_status take_roots(struct providers *providers, struct endpoint_packet *packet)
{
  static const char *const roles[] = {"http", "https"};
  struct provider_setup setup;
  enum provider_setup_outcome outcome = provider_setup_read(packet, &setup);
  size_t selected[2] = {SIZE_MAX, SIZE_MAX};
  enum call_status status = outcome == PROVIDER_SETUP_READY ?
      provider_setup_match(&setup, packet, roles, 2, selected) :
      outcome == PROVIDER_SETUP_FAILED ? (enum call_status)setup.status : CALL_BAD_REQUEST;
  if (status == CALL_OK && selected[0] == SIZE_MAX) {
    status = CALL_UNAVAILABLE;
  }
  for (size_t i = 0; i < packet->grant_count; ++i) {
    handle_t *kept = status != CALL_OK ? NULL :
        i == selected[0] ? &providers->http : i == selected[1] ? &providers->https : NULL;
    if (kept) {
      *kept = packet->grants[i].handle;
    } else {
      handle_close(packet->grants[i].handle);
    }
  }
  enum call_status reply = packet->kind == ENDPOINT_MESSAGE_CALL ?
      endpoint_reply(packet->receipt, outcome == PROVIDER_SETUP_FAILED ? CALL_OK : status,
          NULL, 0, NULL, 0) :
      endpoint_finish(packet->receipt);
  return status != CALL_OK ? status : reply;
}

/* Starts boot://httpfs.pxb as the bundle declares it, with the roles http and
 * https, staging in a leaf of the space's RAM http-work directory and writing
 * its messages to the Caelum console. Failure is recorded in MISSING. */
static void start_httpfs(struct providers *providers, handle_t diagnostics)
{
  handle_t service = bootstrap_capability("service"), clock = bootstrap_capability("clock");
  handle_t launcher = bootstrap_capability("launcher");
  handle_t source = bootstrap_capability(BUNDLE_SOURCE_PREFIX "http_staging");
  struct bundle_program *program = NULL;
  struct bundle_launch *prepared = NULL;
  struct network_environment environment = {.dns_udp = HANDLE_INVALID};
  struct endpoint_create_reply publication = {0};
  struct launch_environment_entry *entries = NULL;
  handle_t staging = HANDLE_INVALID;
  const char *step = "staging";
  char leaf[32];
  snprintf(leaf, sizeof(leaf), "httpfs-%u", providers->starts++);
  enum call_status status = source == HANDLE_INVALID ? CALL_UNAVAILABLE :
      directory_create(source, leaf, DIRECTORY_KIND_DIRECTORY, STAGING_RIGHTS, &staging);
  const struct path_context *context;
  if (status == CALL_OK) {
    step = "bundle";
    status = pyxis_working_context(&context);
  }
  if (status == CALL_OK) {
    status = bundle_open(context, "boot://httpfs.pxb", &program);
  }
  if (status == CALL_OK) {
    step = "DNS";
    status = network_environment_read(&environment, bootstrap_capability("net_registry"),
        bootstrap_capability("udp"), NULL, 0);
  }
  if (status == CALL_OK) {
    step = "publication";
    status = endpoint_create(service, &publication);
  }
  if (status == CALL_OK) {
    step = "launch";
    static const char roles[] = "http,https";
    /* boot:// holds the public CA bundle. */
    struct launch_grant grants[] = {
      {diagnostics, CONSOLE_RIGHT_WRITE, 0},
      {bootstrap_capability("boot"), DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
        DIRECTORY_RIGHT_READ_FILES, 0},
    };
    const struct launch_environment_entry fixed[] = {
      profile_stdio_entry(ENVIRONMENT_STDERR, 0),
      PROFILE_CAP((uintptr_t)"boot", 1),
      {.name = (uintptr_t)PROVIDER_SETUP_ROOTS_ENTRY, .kind = ENVIRONMENT_TEXT,
        .value = (uintptr_t)roles, .length = sizeof(roles) - 1},
    };
    size_t entry_count = sizeof(fixed) / sizeof(*fixed);
    entries = calloc(entry_count + environment.count, sizeof(*entries));
    status = entries ? CALL_OK : CALL_NO_MEMORY;
    if (entries) {
      memcpy(entries, fixed, sizeof(fixed));
      memcpy(entries + entry_count, environment.variables,
          environment.count * sizeof(*environment.variables));
      entry_count += environment.count;
    }
    const char *arguments[] = {"boot://httpfs.pxb"};
    struct launch_request request = {
      .grants = (uintptr_t)grants, .grant_count = sizeof(grants) / sizeof(*grants),
      .entries = (uintptr_t)entries, .entry_count = entry_count,
      .argv = (uintptr_t)arguments, .argc = 1,
    };
    struct bundle_authority authority[] = {
      {"memory", bootstrap_capability("memory"), PROTOCOL_MEMORY},
      {"service", service, PROTOCOL_ENDPOINT_SERVICE},
      {"publication", publication.caller, PROTOCOL_ENDPOINT},
      {"clock", clock, PROTOCOL_CLOCK},
      {"tcp", bootstrap_capability("tcp"), PROTOCOL_TCP_SERVICE},
      {"dns_udp", environment.dns_udp, PROTOCOL_UDP_SERVICE},
      {"random", bootstrap_capability("random"), PROTOCOL_RANDOM},
      {"http_staging", staging, PROTOCOL_DIRECTORY},
    };
    if (status == CALL_OK) {
      status = bundle_launch_prepare(program, &request, authority,
          sizeof(authority) / sizeof(*authority), &prepared);
    }
    if (status == CALL_OK) {
      status = launcher_launch(launcher, bundle_launch_request(prepared), &providers->httpfs);
      bundle_launch_admitted(prepared, status, providers->httpfs);
    }
  }
  if (status == CALL_OK) {
    step = "setup";
    struct endpoint_packet packet = {0};
    status = provider_setup_receive(clock, providers->httpfs, publication.receiver,
        HANDLE_INVALID, &packet);
    if (status == CALL_OK) {
      status = take_roots(providers, &packet);
    }
  }
  bundle_launch_close(prepared);
  bundle_program_close(program);
  providers->dns = status == CALL_OK && environment.dns_udp != HANDLE_INVALID;
  network_environment_free(&environment);
  free(entries);
  handle_t owned[] = {publication.caller, publication.receiver, staging};
  for (size_t i = 0; i < sizeof(owned) / sizeof(*owned); ++i) {
    if (owned[i] != HANDLE_INVALID) {
      handle_close(owned[i]);
    }
  }
  if (status != CALL_OK) {
    snprintf(providers->missing, sizeof(providers->missing),
        "the HTTP provider did not start (%s, status %u)", step, status);
    stop_httpfs(providers);
  }
}

/* Package staging fetches through httpfs. Waits up to INSTALL_NETWORK_WAIT_NS for the
 * network's DNS choice, then starts httpfs again if it is missing or started
 * before DNS was known, so a late lease still reaches a named repository. */
static void prepare_fetch(struct providers *providers, handle_t diagnostics)
{
  handle_t registry = bootstrap_capability("net_registry"), clock = bootstrap_capability("clock");
  struct net_policy_reply policy = {0};
  uint64_t now = 0, end = 0;
  bool waited = false;
  if (clock_now(clock, &now) == CALL_OK && now <= UINT64_MAX - INSTALL_NETWORK_WAIT_NS) {
    end = now + INSTALL_NETWORK_WAIT_NS;
  }
  while (net_registry_policy(registry, &policy) == CALL_OK &&
      !(policy.flags & NET_POLICY_DNS_VALID) && now < end) {
    if (!waited) {
      puts("Waiting for the network...");
      fflush(stdout);
      waited = true;
    }
    if (clock_sleep_for(clock, INSTALL_NETWORK_POLL_NS) != CALL_OK ||
        clock_now(clock, &now) != CALL_OK) {
      break;
    }
  }
  bool dns = policy.flags & NET_POLICY_DNS_VALID;
  if (providers->httpfs != HANDLE_INVALID && (providers->dns || !dns)) {
    return;
  }
  stop_httpfs(providers);
  providers->missing[0] = '\0';
  start_httpfs(providers, diagnostics);
}

static bool close_sources(const handle_t *sources, size_t count)
{
  bool success = true;
  for (size_t i = 0; i < count; i++) {
    if (sources[i] == HANDLE_INVALID) {
      continue;
    }
    bool duplicate = false;
    for (size_t j = 0; j < i; j++) {
      duplicate |= sources[i] == sources[j];
    }
    if (!duplicate && handle_close(sources[i]) != 0) {
      success = false;
    }
  }
  return success;
}

static bool prepare_replay(handle_t parent, handle_t *record_file, bool *coexist,
    bool *updating, bool *clean)
{
  struct install_plan_record record;
  puts("Supply one framed printed plan on standard input (ending with end).");
  if (fflush(stdout) != 0 || !install_plan_record_parse(stdin, &record)) {
    fputs("installer-init: invalid, incomplete or unsupported plan; returning to installer actions\n", stderr);
    clearerr(stdin);
    return false;
  }
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_create(parent, "installer-replay", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ | FILE_RIGHT_WRITE, &file);
  if (status != CALL_OK) {
    fprintf(stderr, "installer-init: private Replay file unavailable (status %u)\n", status);
    return false;
  }
  status = directory_remove(parent, "installer-replay", DIRECTORY_KIND_FILE);
  if (status != CALL_OK) {
    fprintf(stderr, "installer-init: private Replay file unlink failed (status %u)\n", status);
    *clean = false;
    if (handle_close(file) != CALL_OK) {
      *clean = false;
    }
    return false;
  }
  int descriptor = pyxis_descriptor_adopt(&file, PYXIS_DESCRIPTOR_READ | PYXIS_DESCRIPTOR_WRITE);
  if (descriptor < 0) {
    if (handle_close(file) != CALL_OK) {
      *clean = false;
    }
    fputs("installer-init: cannot prepare Replay stream\n", stderr);
    return false;
  }
  FILE *output = fdopen(descriptor, "w");
  if (!output) {
    if (close(descriptor) != 0) {
      *clean = false;
    }
    fputs("installer-init: cannot prepare Replay output\n", stderr);
    return false;
  }
  struct pyxis_descriptor_binding binding;
  bool success = install_plan_record_format(output, &record) &&
      pyxis_descriptor_borrow(descriptor, &binding) == 0 &&
      handle_copy_restricted(binding.handle, FILE_RIGHT_READ, 0, record_file) == CALL_OK;
  if (fclose(output) != 0) {
    *clean = false;
    success = false;
  }
  if (!success) {
    if (*record_file != HANDLE_INVALID) {
      if (handle_close(*record_file) != CALL_OK) {
        *clean = false;
      }
      *record_file = HANDLE_INVALID;
    }
    fputs("installer-init: Replay capture failed; installer not launched\n", stderr);
    return false;
  }
  *coexist = record.coexist;
  *updating = record.updating;
  return true;
}

int main(int argc, char **argv)
{
  (void)argv;
  enum { DISKS, KERNEL, ARCHIVE, MEMORY, CLOCK, RANDOM, SYSTEM_INFO, LOG, BOOT_ROOT, DIAGNOSTICS,
    SNAPSHOT_PARENT, UEFI, POWER, LAUNCHER, INPUT, OUTPUT, ERROR, REPLAY, HTTP, HTTPS, SOURCE_COUNT };
  const char *names[SOURCE_COUNT] = {"disks", "boot_kernel", "boot_archive", "memory",
    "clock", "random", "system_info", "log", "boot", "installer_diagnostics", "installer_scratch",
    "uefi", "power", "launcher", "input", "output", "stderr", "replay", "http", "https"};
  handle_t sources[SOURCE_COUNT] = {
    bootstrap_capability("disks"), bootstrap_capability("boot_kernel"), bootstrap_capability("boot_archive"),
    bootstrap_capability("memory"), bootstrap_capability("clock"), bootstrap_capability("random"),
    bootstrap_capability("system_info"), bootstrap_capability("log"), bootstrap_capability("boot"),
    bootstrap_capability("installer_diagnostics"), HANDLE_INVALID,
    bootstrap_capability("uefi"), bootstrap_capability("power"), bootstrap_capability("launcher"),
    bootstrap_capability("input"), bootstrap_capability("output"),
    HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID,
  };
  handle_t scratch = bootstrap_capability("installer_scratch");
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  const char **arguments = NULL;
  int result = EXIT_FAILURE;
  bool coexist = false, updating = false, replay = false, inspection_clean = true;
  struct installer_update_choice update_choice = {0};
  char selected_target[64], selected_esp[64], selected_pool[64], selected_guid[64];
  bool snapshot_created = false, child_retired = false;
  static struct network network;
  struct providers providers = {.httpfs = HANDLE_INVALID};
  bool fetch = false; /* Every Install and Classic Update runs pkg. */
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  struct pyxis_stdio_binding error = bootstrap_stdio(ENVIRONMENT_STDERR);
  if (argc > 1) {
    fputs("installer-init: the action and layout are chosen here; it takes no arguments\n", stderr);
    goto done;
  }
  if (output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("installer-init: needs the Install console\n", stderr);
    goto done;
  }
  if (handle_copy_restricted(error.handle, CONSOLE_RIGHT_WRITE, 0, &sources[ERROR]) != CALL_OK) {
    fputs("installer-init: cannot delegate Install error output\n", stderr);
    goto done;
  }
  for (unsigned i = 0; i < SOURCE_COUNT; i++) {
    /* Without power the installer simply offers no restart. */
    bool required = i != POWER && i != SNAPSHOT_PARENT && i != REPLAY && i != HTTP && i != HTTPS;
    if (sources[i] == HANDLE_INVALID && required) {
      fprintf(stderr, "installer-init: missing resource %s; use the Install Pyxis boot entry\n", names[i]);
      goto done;
    }
  }
  enum call_status status = scratch == HANDLE_INVALID ? CALL_UNAVAILABLE :
      directory_create(scratch, "installer-config-snapshots",
          DIRECTORY_KIND_DIRECTORY, SNAPSHOT_PARENT_RIGHTS, &sources[SNAPSHOT_PARENT]);
  if (status != CALL_OK) {
    fprintf(stderr, "installer-init: private scratch snapshot parent refused (status %u)\n", status);
    goto done;
  }
  snapshot_created = true;

  puts("Starting the network for package downloads...");
  start_network(&network);
  if (network.started) {
    start_httpfs(&providers, sources[DIAGNOSTICS]);
  } else {
    snprintf(providers.missing, sizeof(providers.missing), "the network did not start");
    printf("Install and Classic Update need package downloads: %s.\n", providers.missing);
  }

  struct terminal terminal = {.input = sources[INPUT], .output = sources[OUTPUT]};
choose:
  for (;;) {
    const char *actions[] = {"Install", "Update", "Replay", "Cancel"};
    fetch = false;
    sources[HTTP] = sources[HTTPS] = HANDLE_INVALID;
    if (fflush(stdout) != 0) {
      goto done;
    }
    struct term_choice_result action = term_read_choice(&terminal,
        "Choose installer action", actions, sizeof(actions) / sizeof(actions[0]), 0);
    if (action.status == TERM_LINE_CANCELLED ||
        (action.status == TERM_LINE_OK && action.selected == 3)) {
      puts("Installation cancelled; installer not launched.");
      result = EXIT_SUCCESS;
      goto done;
    }
    if (action.status != TERM_LINE_OK) {
      fputs("installer-init: action selection interrupted; installer not launched\n", stderr);
      goto done;
    }
    if (!inspection_clean) {
      fputs("installer-init: inspection cleanup remains uncertain; cancel and restart the installer before launching\n", stderr);
      continue;
    }
    updating = action.selected == 1;
    replay = action.selected == 2;
    if (updating) {
      bool back, clean;
      handle_t discovery = HANDLE_INVALID;
      uint64_t rights = DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN_READ_ONLY |
          DISKS_RIGHT_OPEN_OWNED_ESP;
      status = handle_copy_restricted(sources[DISKS], rights, 0, &discovery);
      if (status != CALL_OK) {
        fprintf(stderr, "installer-init: read-only discovery grant unavailable (status %u)\n", status);
        continue;
      }
      bool selected = installer_update_choose(&terminal, discovery, sources[CLOCK],
          &update_choice, &back, &clean);
      bool closed = handle_close(discovery) == CALL_OK;
      inspection_clean &= clean && closed;
      if (!inspection_clean) {
        fputs("installer-init: Update discovery cleanup remains uncertain; returning to installer actions\n", stderr);
        continue;
      }
      if (!selected) {
        fputs("installer-init: Update discovery unavailable; returning to installer actions\n", stderr);
        continue;
      }
      if (back) {
        continue;
      }
      coexist = update_choice.coexist;
      fetch = true;
      char guid[37];
      install_guid_text(update_choice.layout.disk_guid, guid);
      snprintf(selected_target, sizeof(selected_target), "--selected-target=%llu",
          (unsigned long long)update_choice.disk);
      snprintf(selected_esp, sizeof(selected_esp), "--selected-esp=%llu",
          (unsigned long long)update_choice.layout.esp_partition);
      snprintf(selected_pool, sizeof(selected_pool), "--selected-pool=%llu",
          (unsigned long long)update_choice.layout.pool_partition);
      snprintf(selected_guid, sizeof(selected_guid), "--selected-disk-guid=%s", guid);
    } else if (replay) {
      bool replay_updating = false;
      if (!prepare_replay(sources[SNAPSHOT_PARENT], &sources[REPLAY], &coexist,
          &replay_updating, &inspection_clean)) {
        continue;
      }
      fetch = true;
    } else {
      fetch = true;
    }
    if (fetch) {
      if (network.started) {
        prepare_fetch(&providers, sources[DIAGNOSTICS]);
      }
      if (providers.missing[0]) {
        printf("Package staging needs the network: %s.\n", providers.missing);
        fetch = false;
        if (sources[REPLAY] != HANDLE_INVALID) {
          handle_close(sources[REPLAY]);
          sources[REPLAY] = HANDLE_INVALID;
        }
        continue;
      }
      sources[HTTP] = providers.http;
      sources[HTTPS] = providers.https;
    }
    if (!updating && !replay) {
      puts("Coexist preserves other partitions; classic permits whole-disk erase.");
      if (fflush(stdout) != 0) {
        goto done;
      }
      const char *profiles[] = {"Coexist", "Classic", "Cancel"};
      struct term_choice_result profile = term_read_choice(&terminal,
          "Choose installation layout", profiles, sizeof(profiles) / sizeof(profiles[0]), 0);
      if (profile.status == TERM_LINE_CANCELLED ||
          (profile.status == TERM_LINE_OK && profile.selected == 2)) {
        puts("Installation cancelled; installer not launched.");
        result = EXIT_SUCCESS;
        goto done;
      }
      if (profile.status != TERM_LINE_OK) {
        fputs("installer-init: layout selection interrupted; installer not launched\n", stderr);
        goto done;
      }
      coexist = profile.selected == 0;
    }
    break;
  }
  status = directory_lookup(sources[BOOT_ROOT], "installer.pxe",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "installer-init: cannot open boot://installer.pxe (status %u)\n", status);
    goto done;
  }

  /* The installer launches pkg for base (decision 67), with the providers and
   * an ordinary child launcher; it never creates a space. */
  struct launch_grant grants[SOURCE_COUNT] = {
    [DISKS] = {sources[DISKS], INSTALL_COMMON_DISK_RIGHTS |
        (coexist ? DISKS_RIGHT_APPEND_POOL : DISKS_RIGHT_ERASE | DISKS_RIGHT_REBUILD_ESP), 0},
    [KERNEL] = {sources[KERNEL], FILE_RIGHT_READ, 0},
    [ARCHIVE] = {sources[ARCHIVE], FILE_RIGHT_READ, 0},
    [MEMORY] = {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    [CLOCK] = {sources[CLOCK], CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE | CLOCK_RIGHT_SLEEP, 0},
    [RANDOM] = {sources[RANDOM], RANDOM_RIGHT_READ, 0},
    [SYSTEM_INFO] = {sources[SYSTEM_INFO], SYSTEM_INFO_RIGHT_READ, 0},
    [LOG] = {sources[LOG], LOG_RIGHT_READ, 0},
    [DIAGNOSTICS] = {sources[DIAGNOSTICS], CONSOLE_RIGHT_WRITE, 0},
    [SNAPSHOT_PARENT] = {sources[SNAPSHOT_PARENT], SNAPSHOT_PARENT_RIGHTS, 0},
    [UEFI] = {sources[UEFI], UEFI_RIGHT_BOOT_REGISTER, 0},
    [BOOT_ROOT] = {sources[BOOT_ROOT], DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES, 0},
    [POWER] = {sources[POWER], POWER_RIGHT_RESTART, 0},
    [LAUNCHER] = {sources[LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0},
    [INPUT] = {sources[INPUT], CONSOLE_RIGHT_READ, 0},
    [OUTPUT] = {sources[OUTPUT], CONSOLE_RIGHT_WRITE, 0},
    [ERROR] = {sources[ERROR], CONSOLE_RIGHT_WRITE, 0},
    [REPLAY] = {sources[REPLAY], FILE_RIGHT_READ, 0},
    [HTTP] = {sources[HTTP], PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL},
    [HTTPS] = {sources[HTTPS], PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL},
  };
  const struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"disks", DISKS), PROFILE_CAP((uintptr_t)"boot_kernel", KERNEL),
    PROFILE_CAP((uintptr_t)"boot_archive", ARCHIVE), PROFILE_CAP((uintptr_t)"memory", MEMORY),
    PROFILE_CAP((uintptr_t)"clock", CLOCK), PROFILE_CAP((uintptr_t)"random", RANDOM),
    PROFILE_CAP((uintptr_t)"system_info", SYSTEM_INFO), PROFILE_CAP((uintptr_t)"log", LOG),
    PROFILE_PRIVATE_CAP((uintptr_t)"installer_diagnostics", DIAGNOSTICS),
    PROFILE_PRIVATE_CAP((uintptr_t)"config_snapshot_parent", SNAPSHOT_PARENT),
    PROFILE_PRIVATE_CAP((uintptr_t)"uefi", UEFI),
    PROFILE_CAP((uintptr_t)"boot", BOOT_ROOT),
    PROFILE_CAP((uintptr_t)"input", INPUT), PROFILE_CAP((uintptr_t)"output", OUTPUT),
    profile_stdio_entry(ENVIRONMENT_STDIN, replay ? REPLAY : INPUT),
    profile_stdio_entry(ENVIRONMENT_STDOUT, OUTPUT),
    profile_stdio_entry(ENVIRONMENT_STDERR, ERROR),
  };
  struct launch_environment_entry entries[sizeof(resources) / sizeof(*resources) + 4];
  size_t entry_count = sizeof(resources) / sizeof(*resources);
  memcpy(entries, resources, sizeof(resources));
  /* Only actions that stage packages get a launcher and GET-only providers. */
  const unsigned optional[] = {POWER, LAUNCHER, HTTP, HTTPS};
  for (size_t i = 0; i < sizeof(optional) / sizeof(*optional); ++i) {
    if (sources[optional[i]] != HANDLE_INVALID && (optional[i] == POWER || fetch)) {
      entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP(
          (uintptr_t)names[optional[i]], optional[i]);
    }
  }
  struct launch_grant selected_grants[sizeof(entries) / sizeof(entries[0])];
  for (size_t i = 0; i < entry_count; ++i) {
    selected_grants[i] = grants[entries[i].value];
    entries[i].value = i;
  }

  size_t argument_count = 2 + coexist + (updating ? 4 + update_choice.pending : 0);
  arguments = calloc(argument_count, sizeof(*arguments));
  if (!arguments) {
    fputs("installer-init: cannot prepare installer arguments\n", stderr);
    goto done;
  }
  size_t argument_index = 0;
  arguments[argument_index++] = "boot://installer.pxe";
  if (coexist) {
    arguments[argument_index++] = "--coexist";
  }
  arguments[argument_index++] = replay ? "--apply" :
      updating ? "--selected-action=update" : "--selected-action=install";
  if (updating) {
    arguments[argument_index++] = selected_target;
    arguments[argument_index++] = selected_esp;
    arguments[argument_index++] = selected_pool;
    arguments[argument_index++] = selected_guid;
    if (update_choice.pending) {
      arguments[argument_index++] = "--selected-pending";
    }
  }
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)selected_grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = entry_count,
    .argv = (uintptr_t)arguments,
    .argc = argument_index,
  };
  status = launcher_launch(sources[LAUNCHER], &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "installer-init: cannot start boot://installer.pxe (status %u)\n", status);
    goto done;
  }
  struct process_result completion;
  status = process_wait(child, &completion);
  if (status != CALL_OK) {
    fprintf(stderr, "installer-init: cannot wait for installer (status %u)\n", status);
    goto done;
  }
  child_retired = true;
  if (completion.kind == PROCESS_EXITED) {
    printf("installer-init: installer exited with status %lld\n", (long long)completion.exit_status);
    result = completion.exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  } else if (completion.kind == PROCESS_START_FAILED) {
    fprintf(stderr, "installer-init: installer did not start: %s: %s\n",
        process_start_step_name(completion.exit_status),
        process_start_status_name(completion.exit_status));
  } else {
    printf("installer-init: installer stopped (completion kind %llu)\n",
        (unsigned long long)completion.kind);
  }
  if (result != EXIT_SUCCESS) {
    /* A refused Install, as without network, wrote nothing; a failed apply has
     * reported its partial changes. Either way Update and Replay remain. */
    bool closed = handle_close(child) == 0 && handle_close(image) == 0;
    child = image = HANDLE_INVALID;
    child_retired = false;
    if (sources[REPLAY] != HANDLE_INVALID) {
      closed &= handle_close(sources[REPLAY]) == 0;
      sources[REPLAY] = HANDLE_INVALID;
    }
    free(arguments);
    arguments = NULL;
    if (!closed) {
      goto done;
    }
    puts("Returning to installer actions.");
    goto choose;
  }

done:
  sources[HTTP] = sources[HTTPS] = HANDLE_INVALID;
  stop_httpfs(&providers);
  if (!stop_network(&network)) {
    result = EXIT_FAILURE;
  }
  if (snapshot_created && (child == HANDLE_INVALID || child_retired) &&
      directory_remove(scratch, "installer-config-snapshots", DIRECTORY_KIND_DIRECTORY) != CALL_OK) {
    fputs("installer-init: scratch snapshot parent cleanup unconfirmed\n", stderr);
    result = EXIT_FAILURE;
  }
  free(arguments);
  if (child != HANDLE_INVALID && handle_close(child) != 0) {
    result = EXIT_FAILURE;
  }
  if (image != HANDLE_INVALID && handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  if (!close_sources(sources, SOURCE_COUNT)) {
    result = EXIT_FAILURE;
  }
  return result;
}
