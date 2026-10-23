#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include "../libconfig/boot.h"
#include "../libconfig/config.h"
#include "../pkg/store.h"
#include "network.h"
#include "../libconfig/handoff.h"
#include "../libconfig/network.h"
#include "../libconfig/session.h"
#include "handoff.h"
#include <abi/clock.h>
#include <abi/directory.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/launcher.h>
#include <abi/retention.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/thread.h>
#include <abi/net_config.h>
#include <abi/pipe.h>
#include <abi/power.h>
#include <abi/wifi.h>
#include <wifi.h>
#include <forwarder.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/acpi_tables.h>
#include <abi/screen_capture.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/terminal.h>
#include <abi/udp.h>
#include <bundle_launch.h>
#include <package.h>
#include <package_maintenance.h>
#include <clock.h>
#include <command_search.h>
#include <directory.h>
#include <disk.h>
#include <file.h>
#include <handle.h>
#include <input_policy.h>
#include <launcher.h>
#include <literal.h>
#include <job_work.h>
#include <machine_settings.h>
#include <mount.h>
#include <path.h>
#include <package_commands.h>
#include <process.h>
#include <wait.h>
#include <remote/beacon.h>
#include <space.h>
#include <startup.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_info.h>

#define LIVE_CONFIG "boot://config/live.lua"
/* An installed boot's whole configuration, on the system volume of the pool
 * that mount.pool names; administrator-owned, never written by boot. */
#define POOL_BOOT_PATH "config/boot.lua"
#define POOL_SESSION_PATH "config/session.lua"
#define POOL_NETWORK_PATH "config/network.lua"
/* The literal parser's own bound. */
#define POOL_FILE_MAX_BYTES LITERAL_SOURCE_LIMIT
#define HOSTNAME_NAME "system://" MACHINE_HOSTNAME_PATH
#define RESCUE_INIT "boot://init-shell.pxe"
/* The bin volume shares the system volume's pool; see the installer. */
#define BIN_VOLUME "bin"
#define BIN_UNKNOWN_REVISION "unknown"
#define FAT32_SELECTION_WAIT_NS UINT64_C(10000000000)
#define FAT32_SELECTION_POLL_NS UINT64_C(1000000)
/* Start failure reasons print in full; a tab keeps SPACE_REASON_MAX bytes. */
#define START_REASON_MAX 511
#define ROOT_OBSERVER_POLL_NS UINT64_C(100000000)

#define BOOT_ROOT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)
#define TMP_ROOT_RIGHTS (BOOT_ROOT_RIGHTS | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define READ_ONLY_ROOT_RIGHTS (BOOT_ROOT_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO)

/* Services forwarded to every space init, in grant order. The network owner
 * additionally receives the rights in owner_rights. */
enum service {
  SERVICE_MEMORY,
  SERVICE_THREADS,
  SERVICE_LAUNCHER,
  SERVICE_CLOCK,
  SERVICE_SYSTEM_INFO,
  SERVICE_LOG,
  SERVICE_ECHO,
  SERVICE_UDP,
  SERVICE_TCP,
  SERVICE_RANDOM,
  SERVICE_NET_REGISTRY,
  SERVICE_PROFILE,
  SERVICE_PIPE,
  SERVICE_ENDPOINT,
  SERVICE_TERMINAL,
  SERVICE_RETENTION_FACTORY,
  SERVICE_COUNT,
};

static const struct {
  const char *name;
  uint64_t rights;
  uint64_t owner_rights;
} services[SERVICE_COUNT] = {
  [SERVICE_MEMORY] = {"memory", MEMORY_RIGHT_MANAGE, 0},
  [SERVICE_THREADS] = {"threads", THREAD_RIGHT_MANAGE, 0},
  [SERVICE_LAUNCHER] = {"launcher", LAUNCHER_RIGHT_LAUNCH, 0},
  [SERVICE_CLOCK] = {"clock", CLOCK_RIGHTS, 0},
  [SERVICE_SYSTEM_INFO] = {"system_info", SYSTEM_INFO_RIGHT_READ, 0},
  [SERVICE_LOG] = {"log", LOG_RIGHT_READ, 0},
  [SERVICE_ECHO] = {"echo", ECHO_RIGHT_SEND, 0},
  [SERVICE_UDP] = {"udp", UDP_SERVICE_RIGHT_OPEN, 0},
  [SERVICE_TCP] = {"tcp", TCP_SERVICE_RIGHTS, 0},
  [SERVICE_RANDOM] = {"random", RANDOM_RIGHT_READ, 0},
  [SERVICE_NET_REGISTRY] = {"net_registry", NET_REGISTRY_RIGHT_READ, 0},
  [SERVICE_PROFILE] = {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST, 0},
  [SERVICE_PIPE] = {"pipe", PIPE_SERVICE_RIGHT_CREATE, 0},
  [SERVICE_ENDPOINT] = {"service", ENDPOINT_SERVICE_RIGHT_CREATE, 0},
  [SERVICE_TERMINAL] = {"terminal", TERMINAL_SERVICE_RIGHT_CREATE, 0},
  [SERVICE_RETENTION_FACTORY] = {"retention_factory", RETENTION_FACTORY_RIGHT_CREATE, 0},
};

/* Grant order after the services: the boot, tmp and bin roots, then volumes. */
enum {
  GRANT_BOOT = SERVICE_COUNT,
  GRANT_TMP,
  GRANT_BIN,
  GRANT_FIRST_VOLUME,
};

struct authority {
  struct boot_network network;
  handle_t factory;
  handle_t services[SERVICE_COUNT];
  handle_t boot, tmp;
  /* The running revision's program directory, or the archive itself. */
  handle_t bin;
  /* A private RAM directory; each ram volume is one of its subdirectories. */
  handle_t ram;
  handle_t native_mount, host_mount, esp_mount, fat_mounts;
  handle_t ram_npfs; /* Boot-only pool creation, never forwarded to a space init. */
  uint64_t ram_npfs_bytes; /* Captured from the boot configuration. */
  handle_t input_policy; /* Boot-only, never forwarded through services. */
  handle_t net_policy; /* Boot-only until granted to the network owner. */
  handle_t loaded_kernel, loaded_archive; /* Private kernel-issued immutable FILEs. */
  struct config_handoff configuration;
  handle_t boot_evidence; /* READ-only; granted only to package_upgrade spaces. */
  /* Granted only to spaces that set screenshot. */
  handle_t screen_capture;
  /* The read-only ACPI table root, bound as acpi only for spaces that set acpi_tables. */
  handle_t acpi_tables;
  /* Granted only to spaces that set power or remote_power. */
  handle_t power;
  handle_t wifi; /* Boot-only policy right is never forwarded. */
  uint64_t cpu_count;
  const char *remote_beacon;
};

struct root_observer {
  struct root_observer *next;
  handle_t process;
  char name[SPACE_NAME_MAX + 1];
};

static struct root_observer *root_observers;

/* Monitoring keeps this process alive. Release the original boot authority
 * publications as well as the separately retained bootstrap copies. Libc owns
 * its allocator, thread and stdio references independently of this table. */
static void release_boot_environment(void)
{
  struct environment_capture capture = {0};
  handle_t environment = startup_environment_handle();
  enum call_status status = environment_capture_init(environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
  if (status == CALL_OK) {
    for (size_t i = 0; i < capture.count; ++i) {
      struct environment_unset_entry entry = {.identity = capture.values[i].identity};
      environment_name(entry.name, capture.values[i].name);
      enum call_status removed = environment_unset_batch(environment, &entry, 1);
      if (status == CALL_OK && removed != CALL_OK) status = removed;
    }
  }
  environment_capture_close(&capture);
  if (status != CALL_OK) {
    fprintf(stderr, "boot-init: startup authority release unconfirmed (status %u)\n", status);
  }
}

/* Launch every root before waiting. A healthy root must not hide a later
 * root's failure, and configuration does not limit the number to one wait batch. */
static void observe_roots(handle_t clock)
{
  while (root_observers) {
    struct wait_interest pending[WAIT_MAX_INTERESTS];
    size_t count = 0, total = 0;
    struct root_observer **link = &root_observers;
    while (*link) {
      struct root_observer *root = *link;
      struct wait_interest interest = {.handle = root->process, .events = WAIT_COMPLETE};
      uint64_t events = 0;
      enum call_status status = wait_many(&interest, 1, 0, &events);
      if (status == CALL_OK && !(events & WAIT_COMPLETE)) {
        if (count < WAIT_MAX_INTERESTS) pending[count++] = interest;
        ++total;
        link = &root->next;
        continue;
      }
      struct process_result completion;
      if (status == CALL_OK) status = process_wait(root->process, &completion);
      if (status != CALL_OK) {
        fprintf(stderr, "boot-init: cannot observe space %s init (status %u)\n", root->name, status);
      } else if (completion.kind == PROCESS_START_FAILED) {
        fprintf(stderr, "boot-init: space %s init did not start: %s: %s\n", root->name,
            process_start_step_name(completion.exit_status),
            process_start_status_name(completion.exit_status));
      }
      *link = root->next;
      handle_close(root->process);
      free(root);
    }
    if (!count) continue;
    uint64_t now, events[WAIT_MAX_INTERESTS];
    enum call_status status = clock_now(clock, &now);
    if (status == CALL_OK) {
      uint64_t delay = total > count ? ROOT_OBSERVER_POLL_NS : WAIT_MAX_WAIT_NS;
      status = delay > UINT64_MAX - now ? CALL_BAD_REQUEST :
          wait_many(pending, count, now + delay, events);
    }
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      fprintf(stderr, "boot-init: space startup observation unavailable (status %u)\n", status);
      break;
    }
  }
  while (root_observers) {
    struct root_observer *root = root_observers;
    root_observers = root->next;
    handle_close(root->process);
    free(root);
  }
}

/* One configured volume, mounted at most once. */
struct mounted_volume {
  const struct boot_volume *volume;
  handle_t root, control;
  bool published;
  enum call_status status; /* Why root is absent. */
};

struct mounts {
  struct mounted_volume *entries;
  size_t count;
};

static bool take_authority(struct authority *authority)
{
  authority->factory = bootstrap_capability("space_factory");
  authority->boot = bootstrap_capability("boot");
  authority->tmp = bootstrap_capability("tmp");
  authority->ram = bootstrap_capability("ram");
  authority->ram_npfs = bootstrap_capability("ram_npfs");
  authority->native_mount = bootstrap_capability("native_mount");
  authority->host_mount = bootstrap_capability("host_mount");
  authority->esp_mount = bootstrap_capability("esp_mount");
  authority->fat_mounts = bootstrap_capability("fat_mounts");
  authority->screen_capture = bootstrap_capability("screen_capture");
  authority->acpi_tables = bootstrap_capability("acpi_tables");
  authority->power = bootstrap_capability("power");
  authority->wifi = bootstrap_capability("wifi");
  authority->input_policy = bootstrap_capability("input_policy");
  authority->loaded_kernel = bootstrap_take_capability("boot_kernel");
  authority->loaded_archive = bootstrap_take_capability("boot_archive");
  bool complete = authority->factory != HANDLE_INVALID &&
      authority->boot != HANDLE_INVALID && authority->tmp != HANDLE_INVALID &&
      authority->input_policy != HANDLE_INVALID;
  for (size_t i = 0; i < SERVICE_COUNT; ++i) {
    if (i == SERVICE_ECHO || i == SERVICE_UDP || i == SERVICE_TCP) {
      continue;
    }
    authority->services[i] = bootstrap_capability(services[i].name);
    if (authority->services[i] == HANDLE_INVALID) {
      fprintf(stderr, "boot-init: missing resource %s\n", services[i].name);
      complete = false;
    }
  }
  authority->net_policy = bootstrap_capability("net_policy");
  struct system_info_cpu cpu;
  enum call_status status = system_info_get_cpu(authority->services[SERVICE_SYSTEM_INFO], &cpu);
  if (status != CALL_OK || !cpu.online_count) {
    fprintf(stderr, "boot-init: cannot read the CPU count (status %u)\n", status);
    return false;
  }
  authority->cpu_count = cpu.online_count;
  return complete;
}

static uint64_t observe_rights(handle_t mount)
{
  struct handle_info info;
  return handle_query(mount, &info) == CALL_OK && (info.rights & MOUNT_RIGHT_OBSERVE) ?
      DIRECTORY_RIGHT_FILESYSTEM_INFO : 0;
}

/* RAM volumes are named subdirectories of boot init's RAM directory, so
 * spaces granted one cannot reach the others. */
static enum call_status open_ram_volume(handle_t ram, const char *name, handle_t *root)
{
  if (ram == HANDLE_INVALID) {
    return CALL_UNAVAILABLE;
  }
  enum call_status status = directory_create(ram, name, DIRECTORY_KIND_DIRECTORY,
      TMP_ROOT_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO, root);
  if (status == CALL_ALREADY_EXISTS) {
    status = directory_lookup(ram, name, DIRECTORY_KIND_DIRECTORY,
        TMP_ROOT_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO, root);
  }
  return status;
}

static enum call_status selected_fat32_disk(handle_t disks, handle_t clock, const uint8_t guid[16],
    uint64_t *device)
{
  *device = 0;
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - FAT32_SELECTION_WAIT_NS) {
    return status == CALL_OK ? CALL_LIMIT : status;
  }
  uint64_t deadline = now + FAT32_SELECTION_WAIT_NS;
  for (;;) {
    bool pending = false;
    *device = 0;
    for (uint64_t index = 0;; ++index) {
      struct disk_info info;
      status = disks_enumerate(disks, index, &info);
      if (status == CALL_NOT_FOUND) {
        break;
      }
      if (status != CALL_OK) {
        return status;
      }
      pending |= info.gpt_status == DISK_GPT_PENDING;
      if (memcmp(info.gpt_guid, guid, sizeof(info.gpt_guid))) {
        continue;
      }
      if (*device) {
        return CALL_BAD_REQUEST;
      }
      *device = info.id;
    }
    if (!pending) {
      return *device ? CALL_OK : CALL_NOT_FOUND;
    }
    status = clock_now(clock, &now);
    if (status != CALL_OK || now >= deadline) {
      *device = 0;
      return status == CALL_OK ? CALL_TIMED_OUT : status;
    }
    uint64_t delay = deadline - now < FAT32_SELECTION_POLL_NS ?
        deadline - now : FAT32_SELECTION_POLL_NS;
    status = clock_sleep_until(clock, now + delay);
    if (status != CALL_OK) {
      return status;
    }
  }
}

static enum call_status mount_volume(const struct authority *authority,
    const struct boot_volume *volume, bool read_write, struct mounted_volume *mounted)
{
  handle_t *root = &mounted->root;
  if (volume->kind == BOOT_VOLUME_BOOT_ESP || volume->kind == BOOT_VOLUME_FAT32) {
    struct mount_fat32_reply pair = {HANDLE_INVALID, HANDLE_INVALID};
    enum call_status status;
    if (volume->kind == BOOT_VOLUME_BOOT_ESP) {
      status = authority->esp_mount == HANDLE_INVALID ? CALL_UNAVAILABLE :
          mount_open_fat32(authority->esp_mount, BOOT_ROOT_RIGHTS, &pair);
    } else {
      if (authority->fat_mounts == HANDLE_INVALID) {
        return CALL_UNAVAILABLE;
      }
      uint64_t device;
      status = selected_fat32_disk(authority->fat_mounts, authority->services[SERVICE_CLOCK],
          volume->disk_guid, &device);
      if (status != CALL_OK) {
        return status;
      }
      uint64_t access = volume->read_write ? MOUNT_ACCESS_READ_WRITE : MOUNT_ACCESS_READ_ONLY;
      uint64_t rights = volume->read_write ? DIRECTORY_CONTENT_RIGHTS : BOOT_ROOT_RIGHTS;
      status = disks_open_fat32(authority->fat_mounts, device, volume->partition,
          access, rights, volume->disk_guid, &pair);
    }
    if (status == CALL_OK) {
      mounted->root = pair.root;
      mounted->control = pair.control;
    }
    return status;
  }
  if (volume->kind == BOOT_VOLUME_RAM_NPFS) {
    if (authority->ram_npfs == HANDLE_INVALID || !authority->ram_npfs_bytes) {
      return CALL_UNAVAILABLE;
    }
    uint64_t rights = (read_write ? DIRECTORY_CONTENT_RIGHTS : BOOT_ROOT_RIGHTS) |
        observe_rights(authority->ram_npfs);
    return mount_open_ram_volume(authority->ram_npfs, authority->ram_npfs_bytes,
        volume->volume, rights, root);
  }
  if (volume->kind == BOOT_VOLUME_RAM) {
    return open_ram_volume(authority->ram, volume->name, root);
  }
  if (volume->kind == BOOT_VOLUME_VIRTIO_FS) {
    if (authority->host_mount == HANDLE_INVALID) {
      return CALL_UNAVAILABLE;
    }
    return mount_open_root(authority->host_mount,
        read_write ? MOUNT_ACCESS_READ_WRITE : MOUNT_ACCESS_READ_ONLY, root);
  }
  /* A pool volume in a configuration that is not the pool's, such as live.lua. */
  if (authority->native_mount == HANDLE_INVALID || volume->partition == BOOT_POOL_PARTITION) {
    return CALL_UNAVAILABLE;
  }
  uint64_t rights = (read_write ? DIRECTORY_CONTENT_RIGHTS : BOOT_ROOT_RIGHTS) |
      observe_rights(authority->native_mount);
  return mount_open_volume(authority->native_mount, volume->partition, volume->volume,
      rights, root);
}

static struct mounted_volume *find_mount(struct mounts *mounts, const char *name)
{
  for (size_t i = 0; i < mounts->count; ++i) {
    if (!strcmp(mounts->entries[i].volume->name, name)) {
      return &mounts->entries[i];
    }
  }
  return NULL;
}

/* A damaged current or set record never stops boot: report it once, and
 * every package lookup then fails with that reason until pkg rollback. */
static void check_packages(struct mounts *mounts)
{
  struct mounted_volume *mounted = find_mount(mounts, BOOT_PACKAGES_VOLUME);
  if (!mounted || mounted->root == HANDLE_INVALID) {
    return;
  }
  struct package_set *set;
  const char *reason;
  enum call_status status = package_set_load(mounted->root, &set, &reason);
  if (status != CALL_OK) {
    printf("boot-init: packages: %s (status %u); package lookups fail until pkg rollback\n",
        reason, status);
  }
  package_set_free(set);
}

/* FAT mode is configured independently of views; other backends keep the
 * existing aggregate requested access. An already mounted system is kept. */
static bool mount_plan(const struct authority *authority, const struct boot_plan *plan,
    struct mounts *mounts)
{
  struct mounted_volume *entries = realloc(mounts->entries,
      (plan->volume_count + 1) * sizeof(*entries));
  if (!entries) {
    return false;
  }
  mounts->entries = entries;
  for (size_t i = 0; i < plan->volume_count; ++i) {
    const struct boot_volume *volume = plan->volumes[i];
    bool maintenance = (volume->kind == BOOT_VOLUME_NPFS ||
        volume->kind == BOOT_VOLUME_RAM_NPFS) &&
        (!strcmp(volume->name, BOOT_PACKAGES_VOLUME) ||
         !strcmp(volume->name, BOOT_HTTP_WORK_VOLUME) ||
         (volume->kind == BOOT_VOLUME_RAM_NPFS && !strcmp(volume->name, BOOT_SYSTEM_VOLUME)));
    bool used = maintenance, read_write = maintenance;
    for (size_t j = 0; j < plan->space_count; ++j) {
      const struct boot_space *space = plan->spaces[j];
      for (size_t k = 0; k < space->root_count; ++k) {
        if (!strcmp(space->roots[k].volume, volume->name)) {
          used = true;
          read_write |= space->roots[k].read_write;
        }
      }
      for (size_t k = 0; k < space->grant_count; ++k) {
        if (!strcmp(space->grants[k].volume, volume->name)) {
          used = true;
          read_write |= space->grants[k].read_write;
        }
      }
    }
    if (!used || find_mount(mounts, volume->name)) {
      continue;
    }
    struct mounted_volume *mounted = &mounts->entries[mounts->count++];
    *mounted = (struct mounted_volume){.volume = volume,
      .root = HANDLE_INVALID, .control = HANDLE_INVALID};
    mounted->status = mount_volume(authority, volume, read_write, mounted);
    if (mounted->status != CALL_OK) {
      printf("boot-init: volume %s unavailable (status %u)\n", volume->name, mounted->status);
    }
  }
  return true;
}

/* Seed only the volatile system before any consumer starts. Existing files,
 * including malformed ones, are retained for explicit owner recovery. */
static void seed_live_repositories(const struct authority *authority, struct mounts *mounts)
{
  struct mounted_volume *system = find_mount(mounts, BOOT_SYSTEM_VOLUME);
  if (!system || system->root == HANDLE_INVALID ||
      system->volume->kind != BOOT_VOLUME_RAM_NPFS) return;
  handle_t config = HANDLE_INVALID, archive_config = HANDLE_INVALID;
  handle_t existing = HANDLE_INVALID;
  const char *reason = NULL;
  char *bytes = NULL;
  size_t size = 0;
  bool ok = pkg_directory(system->root, "config", true, DIRECTORY_CONTENT_RIGHTS,
      &config, &reason);
  if (ok) {
    enum call_status status = directory_lookup(config, "packages.lua", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &existing);
    if (status == CALL_OK) {
      handle_close(existing);
      handle_close(config);
      return;
    }
    if (status != CALL_NOT_FOUND) {
      reason = "cannot inspect existing packages.lua";
      ok = false;
    }
  }
  ok = ok && pkg_directory(authority->boot, "config", false, BOOT_ROOT_RIGHTS,
      &archive_config, &reason);
  ok = ok && pkg_text(archive_config, "packages.lua", LITERAL_SOURCE_LIMIT,
      &bytes, &size, &reason);
  ok = ok && pkg_write_text(config, "packages.lua", bytes, size, &reason);
  if (ok && (directory_sync(config) != CALL_OK || directory_sync(system->root) != CALL_OK)) {
    reason = "live repository configuration synchronization unconfirmed";
    ok = false;
  }
  free(bytes);
  if (archive_config != HANDLE_INVALID) handle_close(archive_config);
  if (config != HANDLE_INVALID) handle_close(config);
  if (!ok) {
    printf("boot-init: live repository configuration unavailable: %s\n",
        reason ? reason : "unknown error");
  }
}

/* Reads PATH from the mounted pool system root into BYTES, NUL-terminated.
 * MISSING when absent; INVALID fills FAILURE. */
static enum boot_config_result read_pool_file(handle_t system, const char *path,
    struct config_handoff_bytes *bytes, struct config_failure *failure)
{
  *bytes = (struct config_handoff_bytes){0};
  struct path_context context = {.directories = &system, .count = 1};
  handle_t directories[8];
  char component[256];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 8,
    .component = component, .component_capacity = sizeof(component),
  };
  handle_t file;
  enum call_status status = path_resolve(&context, path, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &file);
  if (status == CALL_NOT_FOUND) {
    config_failure_format(failure, "missing");
    return BOOT_CONFIG_MISSING;
  }
  if (status != CALL_OK) {
    config_failure_format(failure, "cannot open (status %u)", status);
    return BOOT_CONFIG_INVALID;
  }
  uint64_t size;
  status = file_size(file, &size);
  bool too_large = status == CALL_OK && size > POOL_FILE_MAX_BYTES;
  if (too_large) {
    status = CALL_LIMIT;
  }
  if (status == CALL_OK) {
    bytes->bytes = malloc(size + 1);
    status = bytes->bytes ? CALL_OK : CALL_NO_MEMORY;
  }
  for (uint64_t used = 0; status == CALL_OK && used < size;) {
    size_t count;
    status = file_read(file, used, bytes->bytes + used, size - used, &count);
    if (status == CALL_OK && !count) {
      status = CALL_BAD_REQUEST;
    }
    used += count;
  }
  handle_close(file);
  if (status != CALL_OK) {
    config_handoff_bytes_free(bytes);
    if (too_large) {
      config_failure_format(failure, "exceeds %d bytes", POOL_FILE_MAX_BYTES);
    } else {
      config_failure_format(failure, "cannot read (status %u)", status);
    }
    return BOOT_CONFIG_INVALID;
  }
  bytes->bytes[size] = '\0';
  bytes->size = size;
  return BOOT_CONFIG_OK;
}

/* Probe one byte beyond the longest name plus LF. A full buffer is invalid,
 * never a truncated name. Short native reads continue until EOF. */
static enum call_status read_hostname(handle_t system, struct system_info_hostname *hostname)
{
  struct path_context context = {.directories = &system, .count = 1};
  handle_t directories[8];
  char component[256];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 8,
    .component = component, .component_capacity = sizeof(component),
  };
  handle_t file;
  enum call_status status = path_resolve_native(&context, MACHINE_HOSTNAME_PATH, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &file);
  if (status != CALL_OK) {
    return status;
  }
  char bytes[MACHINE_HOSTNAME_MAX + 2];
  size_t used = 0;
  while (used < sizeof(bytes)) {
    size_t count;
    status = file_read(file, used, bytes + used, sizeof(bytes) - used, &count);
    if (status != CALL_OK || !count) {
      break;
    }
    used += count;
  }
  if (handle_close(file) && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    return status;
  }
  if (used == sizeof(bytes)) {
    return CALL_LIMIT;
  }
  if (used && bytes[used - 1] == '\n') {
    --used;
  }
  if (!machine_hostname_valid(bytes, used)) {
    return CALL_BAD_REQUEST;
  }
  *hostname = (struct system_info_hostname){0};
  memcpy(hostname->name, bytes, used);
  return CALL_OK;
}

static bool set_hostname(const struct authority *authority, const struct boot_config *defaults,
    struct mounts *mounts, bool installed)
{
  struct system_info_hostname hostname = {MACHINE_HOSTNAME_DEFAULT};
  if (defaults->hostname[0]) {
    memcpy(hostname.name, defaults->hostname, sizeof(defaults->hostname));
  }
  if (installed) {
    struct mounted_volume *system = find_mount(mounts, BOOT_SYSTEM_VOLUME);
    enum call_status status = CALL_UNAVAILABLE;
    if (system) {
      status = system->root == HANDLE_INVALID ? system->status :
          read_hostname(system->root, &hostname);
    }
    if (status != CALL_OK) {
      const char *reason = "key unreadable";
      if (!system || system->root == HANDLE_INVALID) {
        reason = "system volume unavailable";
      } else if (status == CALL_NOT_FOUND) {
        reason = "key missing";
      } else if (status == CALL_BAD_REQUEST || status == CALL_LIMIT) {
        reason = "key invalid";
      }
      fprintf(stderr, "boot-init: hostname: %s (status %u); using configured %s\n",
          reason, status, hostname.name);
    }
  }
  enum call_status status = system_info_set_hostname_once(
      authority->services[SERVICE_SYSTEM_INFO], &hostname);
  if (status != CALL_OK) {
    fprintf(stderr, "boot-init: cannot set the hostname (status %u); no spaces started\n", status);
    return false;
  }
  return true;
}

/* Probe past the longest value and optional LF; never accept a truncated key. */
static enum call_status read_scroll_direction(handle_t system, const char *path, bool *natural)
{
  struct path_context context = {.directories = &system, .count = 1};
  handle_t directories[8];
  char component[256];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 8,
    .component = component, .component_capacity = sizeof(component),
  };
  handle_t file;
  enum call_status status = path_resolve_native(&context, path + sizeof("system://") - 1, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &file);
  if (status != CALL_OK) {
    return status;
  }
  char bytes[MACHINE_SCROLL_DIRECTION_MAX + 2];
  size_t used = 0;
  while (used < sizeof(bytes)) {
    size_t count;
    status = file_read(file, used, bytes + used, sizeof(bytes) - used, &count);
    if (status != CALL_OK || !count) {
      break;
    }
    used += count;
  }
  if (handle_close(file) && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    return status;
  }
  if (used == sizeof(bytes)) {
    return CALL_LIMIT;
  }
  if (used && bytes[used - 1] == '\n') {
    --used;
  }
  return machine_scroll_direction_parse(bytes, used, natural) ? CALL_OK : CALL_BAD_REQUEST;
}

static bool set_input_policy(const struct authority *authority, const struct boot_config *defaults,
    struct mounts *mounts, bool installed)
{
  bool mouse = defaults->mouse_scroll_natural;
  bool touchpad = defaults->touchpad_scroll_natural;
  int32_t touchpad_speed = defaults->touchpad_speed;
  bool num_lock = defaults->num_lock;
  if (installed) {
    struct mounted_volume *system = find_mount(mounts, BOOT_SYSTEM_VOLUME);
    const char *paths[] = {"system://" MACHINE_MOUSE_SCROLL_PATH,
        "system://" MACHINE_TOUCHPAD_SCROLL_PATH};
    bool *directions[] = {&mouse, &touchpad};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
      enum call_status status = !system ? CALL_UNAVAILABLE :
          system->root == HANDLE_INVALID ? system->status :
          read_scroll_direction(system->root, paths[i], directions[i]);
      if (status != CALL_OK && defaults->input_diagnostics) {
        fprintf(stderr, "boot-init: %s unavailable or invalid (status %u); using configured %s\n",
            paths[i], status, *directions[i] ? "natural" : "traditional");
      }
    }
  }
  enum call_status status = input_policy_install_once(authority->input_policy,
      mouse ? INPUT_SCROLL_NATURAL : INPUT_SCROLL_TRADITIONAL,
      touchpad ? INPUT_SCROLL_NATURAL : INPUT_SCROLL_TRADITIONAL, touchpad_speed, num_lock);
  if (status != CALL_OK) {
    fprintf(stderr, "boot-init: cannot install input policy (status %u); no spaces started\n", status);
    return false;
  }
  return true;
}

static size_t cpu_words(uint64_t cpu_count)
{
  return (cpu_count + 63) / 64;
}

/* Fills CPUS for SPACE, or returns a reason the space cannot start. */
static const char *space_cpus(const struct authority *authority,
    const struct boot_space *space, uint64_t *cpus, char *reason, size_t size)
{
  memset(cpus, 0, cpu_words(authority->cpu_count) * sizeof(*cpus));
  if (!space->cpu_count) {
    for (uint64_t cpu = 0; cpu < authority->cpu_count; ++cpu) {
      cpus[cpu / 64] |= UINT64_C(1) << (cpu % 64);
    }
    return NULL;
  }
  for (size_t i = 0; i < space->cpu_count; ++i) {
    uint64_t cpu = space->cpus[i];
    if (cpu >= authority->cpu_count) {
      snprintf(reason, size, "CPU set names absent CPU %llu", (unsigned long long)cpu);
      return reason;
    }
    cpus[cpu / 64] |= UINT64_C(1) << (cpu % 64);
  }
  return NULL;
}

/* The most build_launch can add for one space, counted from its definition. */
struct launch_capacity {
  size_t grants, resources, roots, views;
};

/* Grants, bindings and roots for one space init, built in caller storage. */
struct space_launch {
  struct launch_capacity capacity;
  bool overflow; /* An addition found its table full; the space is refused. */
  struct launch_grant *grants;
  struct launch_environment_entry *resources;
  char source_names[BUNDLE_SOURCE_LIMIT][sizeof(BUNDLE_SOURCE_PREFIX) + BOOT_NAME_MAX];
  handle_t source_handles[BUNDLE_SOURCE_LIMIT];
  struct mounted_volume *source_mounts[BUNDLE_SOURCE_LIMIT];
  size_t source_handle_count;
  handle_t *views; /* View roots opened for this space. */
  size_t view_count;
  struct job_work *job_provisioner;
  handle_t commands;
  struct forwarder_reply wifi_session;
  struct mounted_volume *commands_mount;
  handle_t http_staging;
  handle_t http_staging_parent; /* Borrowed until source handles close. */
  struct mounted_volume *http_staging_mount;
  char http_staging_name[96];
  struct launch_environment_entry *roots;
  size_t grant_count, resource_count, root_count;
  uint64_t working_directory;
  char working_path[BOOT_NAME_MAX + sizeof("://")];
};

/* Borrow the mount root and retain only the final owned subtree. Every child
 * requests the source's existing rights, so traversal cannot widen authority. */
static enum call_status open_source_directory(handle_t root, const char *path,
    uint64_t rights, handle_t *handle)
{
  *handle = HANDLE_INVALID;
  handle_t parent = root;
  bool owned = false;
  for (size_t start = 0;;) {
    size_t end = start;
    while (path[end] && path[end] != '/') ++end;
    char component[BOOT_SOURCE_PATH_MAX + 1];
    memcpy(component, path + start, end - start);
    component[end - start] = '\0';
    handle_t child = HANDLE_INVALID;
    enum call_status status = directory_lookup(parent, component, DIRECTORY_KIND_DIRECTORY,
        rights, &child);
    if (owned) handle_close(parent);
    if (status != CALL_OK) return status;
    if (!path[end]) {
      *handle = child;
      return CALL_OK;
    }
    parent = child;
    owned = true;
    start = end + 1;
  }
}

static enum call_status create_http_staging(const struct authority *authority,
    struct mounted_volume *mounted, handle_t directory, const struct boot_space *space,
    struct space_launch *launch, uint64_t rights)
{
  uint64_t now;
  enum call_status status = clock_now(authority->services[SERVICE_CLOCK], &now);
  if (status != CALL_OK) {
    return status;
  }
  /* Exclusive creation prevents old persistent leaves or another provisioner
   * from sharing a service ancestor, even when clock values repeat. */
  for (unsigned suffix = 0; suffix < 64; ++suffix) {
    snprintf(launch->http_staging_name, sizeof(launch->http_staging_name),
        ".http-%s-%llx-%u", space->name, (unsigned long long)now, suffix);
    status = directory_create(directory, launch->http_staging_name,
        DIRECTORY_KIND_DIRECTORY, rights, &launch->http_staging);
    if (status != CALL_ALREADY_EXISTS) {
      break;
    }
  }
  if (status == CALL_OK) {
    launch->http_staging_mount = mounted;
    launch->http_staging_parent = directory;
  }
  return status;
}

enum launch_table { LAUNCH_RESOURCES, LAUNCH_ROOTS };

/* Every addition after the fixed service and root layout passes here. ENTRY
 * joins TABLE; with GRANT, that grant is appended and ENTRY's value names it.
 * If launch_capacity ever counts less than build_launch adds, the launch is
 * marked overflowed and refused instead of writing past a table. */
static void launch_add(struct space_launch *launch, enum launch_table table,
    struct launch_environment_entry entry, const struct launch_grant *grant)
{
  bool roots = table == LAUNCH_ROOTS;
  size_t *count = roots ? &launch->root_count : &launch->resource_count;
  size_t capacity = roots ? launch->capacity.roots : launch->capacity.resources;
  if (*count == capacity || (grant && launch->grant_count == launch->capacity.grants)) {
    launch->overflow = true;
    return;
  }
  if (grant) {
    entry.value = launch->grant_count;
    launch->grants[launch->grant_count++] = *grant;
  }
  (roots ? launch->roots : launch->resources)[(*count)++] = entry;
}

static void add_package_source(struct space_launch *launch, size_t *count,
    const char *name, handle_t handle, uint64_t rights)
{
  snprintf(launch->source_names[*count], sizeof(launch->source_names[*count]),
      "%s%s", BUNDLE_SOURCE_PREFIX, name);
  launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)
      PROFILE_PRIVATE_CAP((uintptr_t)launch->source_names[(*count)++], 0),
      &(struct launch_grant){handle, rights, 0});
}

/* The writer for the install that booted: the issuer's own facts and admission
 * bind it, and the evidence lets the holder check what this boot loaded. A boot
 * without that evidence, such as one whose committed record is missing or
 * damaged, delegates neither. */
static void upgrade_sources(const struct authority *authority, const struct boot_space *space,
    struct space_launch *launch, size_t *count)
{
  const uint64_t rights = MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_UPGRADE_BOOT_ESP;
  if (authority->boot_evidence == HANDLE_INVALID) {
    printf("boot-init: space %s boot ESP upgrade unavailable: no boot evidence\n", space->name);
    return;
  }
  struct handle_info info;
  enum call_status status = authority->esp_mount == HANDLE_INVALID ? CALL_UNAVAILABLE :
      handle_query(authority->esp_mount, &info);
  if (status != CALL_OK || (rights & ~info.rights)) {
    printf("boot-init: space %s boot ESP upgrade unavailable (status %u)\n",
        space->name, status == CALL_OK ? CALL_DENIED : status);
    return;
  }
  add_package_source(launch, count, PACKAGE_UPGRADE_SOURCE, authority->esp_mount, rights);
  add_package_source(launch, count, PACKAGE_EVIDENCE_SOURCE, authority->boot_evidence,
      FILE_RIGHT_READ);
}

static void package_sources(struct mounts *mounts, const struct boot_space *space,
    struct space_launch *launch, size_t *count)
{
  struct mounted_volume *packages = find_mount(mounts, BOOT_PACKAGES_VOLUME);
  if (packages && packages->root != HANDLE_INVALID) {
    add_package_source(launch, count, PACKAGE_INSPECT_SOURCE, packages->root, PACKAGE_TREE_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO);
    if (space->package_management) {
      struct handle_info info;
      enum call_status status = handle_query(packages->root, &info);
      if (status == CALL_OK && !(DIRECTORY_RIGHTS & ~info.rights)) {
        add_package_source(launch, count, PACKAGE_MANAGEMENT_SOURCE, packages->root,
            DIRECTORY_RIGHTS);
      } else {
        printf("boot-init: space %s package management unavailable (status %u)\n",
            space->name, status == CALL_OK ? CALL_DENIED : status);
      }
    }
  }
  struct mounted_volume *system = find_mount(mounts, BOOT_SYSTEM_VOLUME);
  handle_t config = HANDLE_INVALID;
  enum call_status status = system && system->root != HANDLE_INVALID ?
      directory_lookup(system->root, "config", DIRECTORY_KIND_DIRECTORY,
          PACKAGE_TREE_RIGHTS, &config) : CALL_UNAVAILABLE;
  if (status == CALL_OK) {
    launch->source_handles[launch->source_handle_count] = config;
    launch->source_mounts[launch->source_handle_count++] = system;
    add_package_source(launch, count, PACKAGE_CONFIG_SOURCE, config, PACKAGE_TREE_RIGHTS);
  } else {
    printf("boot-init: space %s package configuration unavailable (status %u)\n",
        space->name, status);
  }
  if (!space->package_management || !system || system->root == HANDLE_INVALID) return;
  handle_t writable = HANDLE_INVALID, view = HANDLE_INVALID;
  status = directory_lookup(system->root, "config", DIRECTORY_KIND_DIRECTORY,
      DIRECTORY_REPOSITORY_VIEW_RIGHTS, &writable);
  if (status == CALL_OK) {
    status = directory_repository_view(writable, &view);
    handle_close(writable);
  }
  if (status == CALL_OK) {
    launch->source_handles[launch->source_handle_count] = view;
    launch->source_mounts[launch->source_handle_count++] = system;
    add_package_source(launch, count, PACKAGE_CONFIG_WRITE_SOURCE, view,
        DIRECTORY_REPOSITORY_VIEW_RIGHTS);
  } else {
    printf("boot-init: space %s repository writer unavailable (status %u)\n",
        space->name, status);
  }
}

/* Starts SPACE in the root its start names, or home. When that root is absent,
 * as for an optional root or a space without home, it starts in tmp. */
static void select_start(const struct boot_space *space, struct space_launch *launch)
{
  const char *start = space->start ? space->start : BOOT_HOME_ROOT;
  for (size_t i = 0; i < launch->root_count; ++i) {
    if (!strcmp((const char *)(uintptr_t)launch->roots[i].name, start)) {
      launch->working_directory = launch->roots[i].value;
      snprintf(launch->working_path, sizeof(launch->working_path), "%s://", start);
      return;
    }
  }
  printf("boot-init: space %s has no %s://; it starts in tmp://\n", space->name, start);
  launch->working_directory = GRANT_TMP;
  snprintf(launch->working_path, sizeof(launch->working_path), "tmp://");
}

static enum call_status build_commands(const struct authority *authority,
    struct mounts *mounts, struct space_launch *launch)
{
  struct command_list_create_entry entries[3] = {
    {.label = "bin://", .handle = authority->bin, .rights = BOOT_ROOT_RIGHTS},
  };
  size_t count = 1;
  if (authority->bin != authority->boot) {
    entries[count++] = (struct command_list_create_entry){
      .label = "boot://", .handle = authority->boot, .rights = BOOT_ROOT_RIGHTS,
    };
  }
  handle_t packages = HANDLE_INVALID;
  struct mounted_volume *mounted = find_mount(mounts, BOOT_PACKAGES_VOLUME);
  enum call_status status = CALL_OK;
  if (mounted && mounted->root != HANDLE_INVALID) {
    status = package_commands_open(mounted->root, &packages);
    if (status == CALL_OK) {
      entries[count++] = (struct command_list_create_entry){
        .label = "packages", .handle = packages, .rights = BOOT_ROOT_RIGHTS,
      };
      launch->commands_mount = mounted;
    }
  }
  if (status == CALL_OK) {
    status = command_list_create(startup_environment_handle(), entries, count,
        &launch->commands);
  }
  if (packages != HANDLE_INVALID) {
    handle_close(packages);
  }
  if (status != CALL_OK) {
    return status;
  }
  launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)PROFILE_CAP("commands", 0),
      &(struct launch_grant){launch->commands, COMMAND_LIST_RIGHT_READ, 0});
  return CALL_OK;
}

static bool capacity_add(size_t *total, size_t amount)
{
  return !ckd_add(total, *total, amount);
}

/* Counts each table's largest size for SPACE: the fixed layout, the configured
 * roots and sources, and every resource its flags, the packages volume and the
 * network can add. Returns a refusal reason when a count overflows or the launch
 * would pass what one request can carry. */
static const char *launch_capacity(const struct authority *authority,
    const struct boot_space *space, struct launch_capacity *capacity, char *reason, size_t size)
{
  bool remote = !strcmp(space->name, "remote");
  /* Resources that each bring one grant. */
  size_t granted = 1; /* commands */
  for (size_t i = CONFIG_LUA_SESSION; i < CONFIG_LUA_COUNT; ++i) {
    granted += authority->configuration.files[i] != HANDLE_INVALID;
  }
  granted += 2 + (space->package_management ? 2 : 0); /* package inspect and config sources */
  granted += space->package_upgrade ? 2 : 0; /* upgrade sources */
  granted += space->fat32_mount + space->jobs + (space->launch ? 2 : 0) +
      space->multiplexer + space->remote_multiplexer + space->screenshot +
      space->power + space->remote_power;
  if (space->wifi_rights) {
    /* wifi, plus wifi_auth_setup and wifi_session_control with CONNECT. */
    granted += 1 + ((space->wifi_rights & WIFI_RIGHT_CONNECT) ? 2 : 0);
  }
  granted += authority->remote_beacon && remote;
  granted += remote && authority->network.remote_interface != HANDLE_INVALID ? 2 : 0;
  /* Network controller names are text resources without a grant. */
  size_t text = 0;
  if (space->network) {
    granted += 1; /* net_policy */
    for (size_t i = 0; i < authority->network.config.count; ++i) {
      granted += 1 + (authority->network.dhcp[i] != HANDLE_INVALID);
      text += authority->network.config.interfaces[i].resolved_controller != 0;
    }
  }

  size_t root_grants = space->acpi_tables, views = 0;
  for (size_t i = 0; i < space->root_count; ++i) {
    views += space->roots[i].view != BOOT_ROOT_VOLUME;
  }
  /* boot, tmp and bin come first; start_space adds OS_NAME and, in the remote
   * space, the beacon text to the entries it sends. */
  size_t roots = 3, grants = GRANT_FIRST_VOLUME, resources = SERVICE_COUNT, entries = 2;
  bool counted = capacity_add(&root_grants, space->root_count) &&
      capacity_add(&granted, space->grant_count) &&
      capacity_add(&roots, root_grants) &&
      capacity_add(&grants, root_grants) && capacity_add(&grants, granted) &&
      capacity_add(&resources, granted) && capacity_add(&resources, text) &&
      capacity_add(&entries, resources) && capacity_add(&entries, roots);
  const size_t grant_max = LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant);
  const size_t entry_max = LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_environment_entry);
  if (!counted) {
    return "launch table size overflows";
  }
  if (grants > grant_max || entries > entry_max) {
    snprintf(reason, size, "launch needs %zu grants and %zu entries; at most %zu and %zu fit",
        grants, entries, grant_max, entry_max);
    return reason;
  }
  *capacity = (struct launch_capacity){
    .grants = grants, .resources = resources, .roots = roots,
    .views = views ? views : 1,
  };
  return NULL;
}

static const char *build_launch(const struct authority *authority, struct mounts *mounts,
    const struct boot_space *space, struct space_launch *launch, char *reason, size_t size)
{
  if (space->wifi_rights && authority->wifi == HANDLE_INVALID) {
    return "Wi-Fi delegation requested but radio capability is unavailable";
  }
  if (space->screenshot && authority->screen_capture == HANDLE_INVALID) {
    return "screenshot requested but screen_capture is unavailable";
  }
  if (space->acpi_tables && authority->acpi_tables == HANDLE_INVALID) {
    return "acpi_tables requested but the ACPI table root is unavailable";
  }
  const char *refused = launch_capacity(authority, space, &launch->capacity, reason, size);
  if (refused) {
    return refused;
  }
  launch->resources = calloc(launch->capacity.resources, sizeof(*launch->resources));
  launch->grants = calloc(launch->capacity.grants, sizeof(*launch->grants));
  launch->roots = calloc(launch->capacity.roots, sizeof(*launch->roots));
  launch->views = calloc(launch->capacity.views, sizeof(*launch->views));
  if (!launch->resources || !launch->grants || !launch->roots || !launch->views) {
    return "boot init is out of memory";
  }
  for (size_t i = 0; i < SERVICE_COUNT; ++i) {
    uint64_t rights = services[i].rights | (space->network ? services[i].owner_rights : 0);
    launch->grants[i] = (struct launch_grant){authority->services[i], rights, 0};
    launch->resources[i] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)services[i].name, i);
    if (i == SERVICE_ENDPOINT || i == SERVICE_RETENTION_FACTORY) {
      launch->resources[i].flags = 0;
    }
  }
  launch->grants[GRANT_BOOT] = (struct launch_grant){authority->boot, BOOT_ROOT_RIGHTS, 0};
  launch->grants[GRANT_TMP] = (struct launch_grant){authority->tmp, TMP_ROOT_RIGHTS, 0};
  launch->roots[0] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"boot", GRANT_BOOT);
  launch->roots[1] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"tmp", GRANT_TMP);
  launch->grants[GRANT_BIN] = (struct launch_grant){authority->bin, BOOT_ROOT_RIGHTS, 0};
  launch->roots[2] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"bin", GRANT_BIN);
  launch->grant_count = GRANT_FIRST_VOLUME;
  launch->resource_count = SERVICE_COUNT;
  launch->root_count = 3;
  enum call_status commands_status = build_commands(authority, mounts, launch);
  if (commands_status != CALL_OK) {
    snprintf(reason, size, "command search list unavailable (status %u)", commands_status);
    return reason;
  }
  static const char *const configuration_names[CONFIG_LUA_COUNT] = {
    NULL, "session_config", "network_config"
  };
  for (size_t i = CONFIG_LUA_SESSION; i < CONFIG_LUA_COUNT; ++i) {
    if (authority->configuration.files[i] == HANDLE_INVALID) {
      continue;
    }
    launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP((uintptr_t)configuration_names[i], 0),
        &(struct launch_grant){authority->configuration.files[i], FILE_RIGHT_READ, 0});
  }
  for (size_t i = 0; i < space->root_count; ++i) {
    const struct boot_root *root = &space->roots[i];
    struct mounted_volume *mounted = find_mount(mounts, root->volume);
    if (!mounted || mounted->root == HANDLE_INVALID) {
      if (root->optional) {
        continue;
      }
      snprintf(reason, size, "volume %s unavailable (status %u)", root->volume,
          mounted ? mounted->status : CALL_UNAVAILABLE);
      return reason;
    }
    if (root->view == BOOT_ROOT_PKG) {
      /* Live: each lookup reads the set active at that moment. */
      handle_t view = HANDLE_INVALID;
      enum call_status status = package_roots_open(mounted->root, &view);
      if (status != CALL_OK) {
        if (root->optional) {
          continue;
        }
        snprintf(reason, size, "view %s unavailable (status %u)", root->name, status);
        return reason;
      }
      launch->views[launch->view_count++] = view;
      launch_add(launch, LAUNCH_ROOTS,
          (struct launch_environment_entry)PROFILE_CAP((uintptr_t)root->name, 0),
          &(struct launch_grant){view, PACKAGE_TREE_RIGHTS, 0});
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(mounted->root, &info);
    if (status != CALL_OK) {
      snprintf(reason, size, "volume %s unavailable (status %u)", root->volume, status);
      return reason;
    }
    uint64_t rights = info.rights & (root->read_write ? DIRECTORY_RIGHTS : READ_ONLY_ROOT_RIGHTS);
    launch_add(launch, LAUNCH_ROOTS,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)root->name, 0),
        &(struct launch_grant){mounted->root, rights, 0});
  }
  /* A root like any configured one: session, remote and shell handoffs carry it on. */
  if (space->acpi_tables) {
    launch_add(launch, LAUNCH_ROOTS,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)BOOT_ACPI_ROOT, 0),
        &(struct launch_grant){authority->acpi_tables, ACPI_TABLES_RIGHTS, 0});
  }
  size_t source_count = 0;
  for (size_t i = 0; i < space->grant_count; ++i) {
    const struct boot_grant *source = &space->grants[i];
    struct mounted_volume *mounted = find_mount(mounts, source->volume);
    handle_t handle = !mounted ? HANDLE_INVALID : source->kind == BOOT_GRANT_MOUNT ?
        mounted->control : mounted->root;
    if (handle == HANDLE_INVALID) {
      if (source->optional) {
        continue;
      }
      snprintf(reason, size, "source %s unavailable (status %u)", source->name,
          mounted ? mounted->status : CALL_UNAVAILABLE);
      return reason;
    }
    uint64_t rights = source->kind == BOOT_GRANT_DIRECTORY ?
        (source->read_write ? DIRECTORY_CONTENT_RIGHTS : BOOT_ROOT_RIGHTS) :
        (MOUNT_RIGHT_OPEN_ROOT | MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_RETIRE |
          (source->read_write ? MOUNT_RIGHT_WRITE | MOUNT_RIGHT_INTERVAL : 0));
    bool http_staging = !strcmp(source->name, BOOT_HTTP_STAGING_SOURCE);
    if (http_staging) {
      rights |= DIRECTORY_RIGHT_FILESYSTEM_INFO;
    }
    struct handle_info info;
    enum call_status status = handle_query(handle, &info);
    if (status != CALL_OK || (rights & ~info.rights)) {
      if (http_staging && source->optional) {
        printf("boot-init: space %s HTTP staging refused (status %u); POST unavailable\n",
            space->name, status == CALL_OK ? CALL_DENIED : status);
        continue;
      }
      if (source->path && source->optional) continue;
      snprintf(reason, size, "source %s lacks requested authority (status %u)",
          source->name, status == CALL_OK ? CALL_DENIED : status);
      return reason;
    }
    if (source->path) {
      handle_t narrowed = HANDLE_INVALID;
      status = open_source_directory(handle, source->path, rights, &narrowed);
      if (status == CALL_OK) {
        status = handle_query(narrowed, &info);
        if (status == CALL_OK && (rights & ~info.rights)) status = CALL_DENIED;
      }
      if (status != CALL_OK) {
        if (narrowed != HANDLE_INVALID) handle_close(narrowed);
        if (source->optional) continue;
        snprintf(reason, size, "source %s path unavailable (status %u)", source->name, status);
        return reason;
      }
      handle = narrowed;
      launch->source_handles[launch->source_handle_count] = narrowed;
      launch->source_mounts[launch->source_handle_count++] = mounted;
    }

    if (http_staging) {
      status = create_http_staging(authority, mounted, handle, space, launch, rights);
      if (status != CALL_OK) {
        if (source->optional) {
          printf("boot-init: space %s HTTP staging unavailable (status %u); POST unavailable\n",
              space->name, status);
          continue;
        }
        snprintf(reason, size, "HTTP staging creation failed (status %u)", status);
        return reason;
      }
      handle = launch->http_staging;
    }
    snprintf(launch->source_names[source_count], sizeof(launch->source_names[source_count]),
        "%s%s", BUNDLE_SOURCE_PREFIX, source->name);
    launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP((uintptr_t)launch->source_names[source_count++], 0),
        &(struct launch_grant){handle, rights, 0});
  }
  package_sources(mounts, space, launch, &source_count);
  if (space->package_upgrade) {
    upgrade_sources(authority, space, launch, &source_count);
  }
  if (space->fat32_mount) {
    uint64_t rights = DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_MOUNT_READ_ONLY |
        (space->fat32_mount_write ? DISKS_RIGHT_MOUNT_READ_WRITE : 0);
    struct handle_info info;
    enum call_status status = handle_query(authority->fat_mounts, &info);
    if (status != CALL_OK || (rights & ~info.rights)) {
      return "mount-only factory requested but unavailable";
    }
    launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)
        PROFILE_PRIVATE_CAP((uintptr_t)BUNDLE_SOURCE_PREFIX "fat_mounts", 0),
        &(struct launch_grant){authority->fat_mounts, rights, 0});
  }
  if (space->jobs) {
    enum call_status status = job_work_create(authority->ram,
        authority->services[SERVICE_CLOCK], &launch->job_provisioner);
    if (status != CALL_OK) {
      snprintf(reason, size, "private job provisioning unavailable (status %u)", status);
      return reason;
    }
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"job_provisioner", 0),
        &(struct launch_grant){job_work_directory(launch->job_provisioner), JOB_PROVISIONER_RIGHTS, 0});
  }
  if (space->launch) {
    /* Ordinary child launch is independent of init's administrative grant. */
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"child_launcher", 0),
        &(struct launch_grant){authority->services[SERVICE_LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0});
    /* The archive root every trusted shell opens its script interpreter from.
     * Private: it is not a root name and the environment cannot rebind it. */
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_PRIVATE_CAP((uintptr_t)"interpreter_root", 0),
        &(struct launch_grant){authority->boot, BOOT_ROOT_RIGHTS, 0});
  }
  if (space->multiplexer) {
    /* The distinct name records opt-in, separate from trusted init authority. */
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"mux_terminal", 0),
        &(struct launch_grant){authority->services[SERVICE_TERMINAL], TERMINAL_SERVICE_RIGHT_CREATE, 0});
  }
  if (space->remote_multiplexer) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"remote_mux_terminal", 0),
        &(struct launch_grant){authority->services[SERVICE_TERMINAL], TERMINAL_SERVICE_RIGHT_CREATE, 0});
  }
  if (space->wifi_rights) {
    handle_t radio = authority->wifi;
    if (space->wifi_rights & WIFI_RIGHT_CONNECT) {
      enum call_status status = forwarder_revocable(radio, space->wifi_rights | WIFI_RIGHT_SUPPLICANT, 0, 0,
          &launch->wifi_session);
      if (status != CALL_OK) {
        snprintf(reason, size, "Wi-Fi session lifetime unavailable (status %u)", status);
        return reason;
      }
      radio = launch->wifi_session.holder;
      launch_add(launch, LAUNCH_RESOURCES,
          (struct launch_environment_entry)PROFILE_PRIVATE_CAP("wifi_auth_setup", 0),
          &(struct launch_grant){radio, WIFI_RIGHT_SUPPLICANT, 0});
      launch_add(launch, LAUNCH_RESOURCES,
          (struct launch_environment_entry)PROFILE_PRIVATE_CAP("wifi_session_control", 0),
          &(struct launch_grant){launch->wifi_session.controller, FORWARDER_RIGHT_CUT, 0});
    }
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"wifi", 0),
        &(struct launch_grant){radio, space->wifi_rights, 0});
  }
  if (space->screenshot) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"screen_capture", 0),
        &(struct launch_grant){authority->screen_capture, SCREEN_CAPTURE_RIGHT_CAPTURE, 0});
  }
  if (space->power && authority->power != HANDLE_INVALID) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"power", 0),
        &(struct launch_grant){authority->power, POWER_RIGHTS, 0});
  }
  if (space->remote_power && authority->power != HANDLE_INVALID) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"remote_power", 0),
        &(struct launch_grant){authority->power, POWER_RIGHTS, 0});
  }
  if (authority->remote_beacon && !strcmp(space->name, "remote")) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"udp_beacons", 0),
        &(struct launch_grant){authority->network.remote_udp, UDP_SERVICE_RIGHT_BROADCAST, 0});
  }
  if (space->network) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_PRIVATE_CAP("net_policy", 0),
        &(struct launch_grant){authority->network.policy, NET_POLICY_RIGHT_MANAGE, 0});
    for (size_t i = 0; i < authority->network.config.count; ++i) {
      if (authority->network.config.interfaces[i].resolved_controller) {
        launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry){
          .name = (uintptr_t)authority->network.controller_names[i], .kind = ENVIRONMENT_TEXT,
          .value = (uintptr_t)authority->network.controller_values[i],
          .length = strlen(authority->network.controller_values[i]),
        }, NULL);
      }
      launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)
          PROFILE_PRIVATE_CAP(authority->network.facet_names[i], 0),
          &(struct launch_grant){authority->network.facets[i], NET_INTERFACE_RIGHTS, 0});
      if (authority->network.dhcp[i] != HANDLE_INVALID) {
        launch_add(launch, LAUNCH_RESOURCES, (struct launch_environment_entry)
            PROFILE_PRIVATE_CAP(authority->network.dhcp_names[i], 0),
            &(struct launch_grant){authority->network.dhcp[i], UDP_SERVICE_RIGHT_BROADCAST, 0});
      }
    }
  }
  if (!strcmp(space->name, "remote") && authority->network.remote_interface != HANDLE_INVALID) {
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_PRIVATE_CAP("remote_interface", 0),
        &(struct launch_grant){authority->network.remote_interface, NET_INTERFACE_RIGHT_READ, 0});
    launch_add(launch, LAUNCH_RESOURCES,
        (struct launch_environment_entry)PROFILE_PRIVATE_CAP("remote_tcp", 0),
        &(struct launch_grant){authority->network.remote_tcp, TCP_SERVICE_RIGHTS, 0});
  }
  if (launch->overflow) {
    return "launch tables exceed their counted size";
  }
  select_start(space, launch);
  return NULL;
}

static struct space_definition space_definition_of(const struct boot_space *space)
{
  return (struct space_definition){
    .name = space->name, .title = space->title,
    .flags = (space->multiplexer ? SPACE_CREATE_TERMINAL_CONTROL : 0) |
        (space->clipboard_local ? SPACE_CREATE_CLIPBOARD_LOCAL : 0) |
        (space->clipboard_shared ? SPACE_CREATE_CLIPBOARD_SHARED : 0) |
        (space->screenshot ? SPACE_CREATE_SCREENSHOT_SAVER : 0),
  };
}

/* An unstarted space shows REASON on its tab and keeps its name taken. The
 * kernel accepts no space flags without a launch, and a reason of at most
 * SPACE_REASON_MAX printable ASCII bytes, so the tab gets a cut, cleaned copy;
 * start_space has already printed the full reason. */
static void create_unstarted(const struct authority *authority, const struct boot_space *space,
    const char *reason)
{
  char text[SPACE_REASON_MAX + 1];
  size_t length = 0;
  for (; reason[length] && length < SPACE_REASON_MAX; ++length) {
    unsigned char byte = (unsigned char)reason[length];
    text[length] = byte < 0x20 || byte > 0x7e ? '?' : (char)byte;
  }
  text[length] = '\0';
  struct space_definition definition = {.name = space->name, .title = space->title};
  enum call_status status = space_create_unstarted(authority->factory, &definition, text);
  if (status != CALL_OK) {
    fprintf(stderr, "boot-init: cannot create space %s (status %u)\n", space->name, status);
  }
}

/* A space that cannot start is created unstarted, showing why on its tab. */
static bool start_space(const struct authority *authority, struct mounts *mounts,
    const struct boot_space *space)
{
  struct space_definition definition = space_definition_of(space);
  char text[START_REASON_MAX + 1];
  uint64_t *cpus = calloc(cpu_words(authority->cpu_count), sizeof(*cpus));
  struct space_launch launch = {0};
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  struct root_observer *observer = calloc(1, sizeof(*observer));
  const char *reason = cpus && observer ? space_cpus(authority, space, cpus, text, sizeof(text)) :
      "boot init is out of memory";
  if (!reason) {
    reason = build_launch(authority, mounts, space, &launch, text, sizeof(text));
  }
  if (!reason) {
    struct path_context context = {0};
    handle_t directories[8];
    char component[BOOT_INIT_MAX + 1];
    struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 8,
    .component = component, .component_capacity = sizeof(component),
  };
    enum call_status status;
    /* Boot init's own namespace has boot:// but no system://; pool inits come
     * from the mounted system volume. */
    if (!strncmp(space->init, BOOT_POOL_INIT_PREFIX, strlen(BOOT_POOL_INIT_PREFIX))) {
      struct mounted_volume *system = find_mount(mounts, BOOT_SYSTEM_VOLUME);
      status = !system ? CALL_UNAVAILABLE : system->root == HANDLE_INVALID ? system->status :
          path_resolve_native(&(struct path_context){.directories = &system->root, .count = 1},
            space->init + strlen(BOOT_POOL_INIT_PREFIX), DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
            &workspace, &image);
    } else {
      status = path_resolve(&context, space->init, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
          &workspace, &image);
    }
    if (status != CALL_OK) {
      snprintf(text, sizeof(text), "cannot open init %.64s (status %u)", space->init, status);
      reason = text;
    }
  }

  bool started = false, admission_attempted = false;
  enum call_status status;
  if (reason) {
    fprintf(stderr, "boot-init: space %s not started: %s\n", space->name, reason);
  }
  if (reason) {
    create_unstarted(authority, space, reason);
    status = CALL_OK;
  } else {
    if (space->focus) {
      definition.flags |= SPACE_CREATE_BOOT_FOCUS;
    }
    const char *arguments[] = {space->init};
    bool reverse = authority->remote_beacon && !strcmp(space->name, "remote");
    size_t entry_count = launch.resource_count + launch.root_count + (reverse ? 2 : 1);
    struct launch_environment_entry *entries = calloc(entry_count, sizeof(*entries));
    if (!entries) {
      status = CALL_NO_MEMORY;
      goto cleanup;
    }
    memcpy(entries, launch.resources, launch.resource_count * sizeof(*entries));
    memcpy(entries + launch.resource_count, launch.roots, launch.root_count * sizeof(*entries));
    entries[launch.resource_count + launch.root_count] =
        (struct launch_environment_entry)PROFILE_TEXT("OS_NAME", "Pyxis OS");
    if (reverse) {
      entries[entry_count - 1] = (struct launch_environment_entry)
          PROFILE_TEXT("PYXIS_REMOTE_BEACON", authority->remote_beacon);
    }
    struct launch_request request = {
      .image = image,
      .grants = (uintptr_t)launch.grants, .grant_count = launch.grant_count,
      .entries = (uintptr_t)entries, .entry_count = entry_count,
      .working_directories = (uintptr_t)&launch.working_directory,
      .working_directory_count = 1,
      .working_path = (uintptr_t)launch.working_path,
      .argv = (uintptr_t)arguments, .argc = 1,
    };
    struct path_context interpreters = {0};
    admission_attempted = true;
    status = program_create_space(authority->factory, &definition, cpus,
        authority->cpu_count, &request, &interpreters, &child);
    free(entries);
    if (status == CALL_OK) {
      printf("boot-init: space %s started: %s%s\n", space->name, space->init,
          space->network ? " (network owner)" : "");
      started = true;
      enum call_status observed = handle_copy_restricted(child, PROCESS_RIGHT_WAIT, 0,
          &observer->process);
      if (observed == CALL_OK) {
        memcpy(observer->name, space->name, strlen(space->name) + 1);
        observer->next = root_observers;
        root_observers = observer;
        observer = NULL;
      } else {
        fprintf(stderr, "boot-init: cannot observe space %s init (status %u)\n",
            space->name, observed);
      }
    } else {
      fprintf(stderr, "boot-init: space %s not started: cannot start %s (status %u)\n",
          space->name, space->init, status);
    }
  }
cleanup:
  free(observer);
  if (child != HANDLE_INVALID) {
    handle_close(child);
  }
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  /* Successful or uncertain startup leaves the private tree with the space;
   * local startup deliberately has no retained teardown supervisor. */
  bool uncertain = admission_attempted && (status == CALL_OUTCOME_UNKNOWN ||
      status == CALL_BAD_BUFFER);
  if (started || uncertain) {
    /* The command list retains its private package-volume backing too. */
    if (launch.commands_mount) {
      launch.commands_mount->published = true;
    }
    for (size_t i = 0; i < launch.source_handle_count; ++i) {
      if (launch.source_mounts[i]->control != HANDLE_INVALID) {
        launch.source_mounts[i]->published = true;
      }
    }

    if (launch.http_staging_mount) {
      launch.http_staging_mount->published = true;
    }
    for (size_t i = 0; i < mounts->count; ++i) {
      struct mounted_volume *mounted = &mounts->entries[i];
      if (mounted->control == HANDLE_INVALID) {
        continue;
      }
      for (size_t j = 0; j < launch.grant_count; ++j) {
        if (launch.grants[j].source == mounted->root ||
            launch.grants[j].source == mounted->control) {
          mounted->published = true;
        }
      }
    }
  }
  if (launch.http_staging != HANDLE_INVALID) {
    if (!started && !uncertain && launch.http_staging_mount) {
      enum call_status removed = directory_remove(launch.http_staging_parent,
          launch.http_staging_name, DIRECTORY_KIND_DIRECTORY);
      if (removed != CALL_OK) {
        printf("boot-init: space %s HTTP staging retained (status %u)\n", space->name, removed);
      }
    }
    if (handle_close(launch.http_staging)) {
      printf("boot-init: space %s HTTP staging close failed\n", space->name);
    }
  }
  job_work_close(launch.job_provisioner, !started && !uncertain);
  if (launch.wifi_session.holder) handle_close(launch.wifi_session.holder);
  if (launch.wifi_session.controller) handle_close(launch.wifi_session.controller);
  if (launch.commands != HANDLE_INVALID) handle_close(launch.commands);
  for (size_t i = 0; i < launch.source_handle_count; ++i) handle_close(launch.source_handles[i]);
  for (size_t i = 0; i < launch.view_count; ++i) handle_close(launch.views[i]);
  free(launch.views);
  free(launch.roots);
  free(launch.resources);
  free(launch.grants);
  free(cpus);
  return started;
}

/* Installed boots bind bin:// to bin/REVISION on the system pool, for the
 * running kernel's revision. Live boots bind the archive itself, which holds
 * every program. Installed boots without that directory bind it too, but an
 * installed archive holds only the rescue set. */
static void bind_bin(struct authority *authority, const struct boot_plan *plan, bool installed)
{
  authority->bin = authority->boot;
  if (!installed) {
    return;
  }
  struct system_info_identity identity;
  const char *revision = BIN_UNKNOWN_REVISION;
  if (system_info_get_identity(authority->services[SERVICE_SYSTEM_INFO], &identity) == CALL_OK &&
      identity.build_revision[0]) {
    revision = identity.build_revision;
  }
  const struct boot_volume *system = boot_plan_volume(plan, BOOT_SYSTEM_VOLUME);
  handle_t root = HANDLE_INVALID, directory = HANDLE_INVALID;
  enum call_status status = !system || system->kind != BOOT_VOLUME_NPFS ||
      authority->native_mount == HANDLE_INVALID ? CALL_UNAVAILABLE :
      mount_open_volume(authority->native_mount, system->partition, BIN_VOLUME,
        BOOT_ROOT_RIGHTS, &root);
  if (status == CALL_OK) {
    status = directory_lookup(root, revision, DIRECTORY_KIND_DIRECTORY, BOOT_ROOT_RIGHTS,
        &directory);
    handle_close(root);
  }
  if (status != CALL_OK) {
    printf("boot-init: no bin://%s on the pool (status %u); only the rescue set is available\n",
        revision, status);
    return;
  }
  printf("boot-init: bin:// is revision %s\n", revision);
  authority->bin = directory;
}

/* The pool's system volume, as boot init mounts it for the configuration and
 * the rescue space before any configured plan exists. */
static struct boot_volume pool_system = {
  .name = BOOT_SYSTEM_VOLUME, .kind = BOOT_VOLUME_NPFS, .volume = BOOT_SYSTEM_VOLUME,
};

/* Mounts the system volume of pool partition POOL read-write, once. */
static struct mounted_volume *mount_pool_system(const struct authority *authority,
    struct mounts *mounts, uint32_t pool)
{
  struct mounted_volume *mounted = find_mount(mounts, BOOT_SYSTEM_VOLUME);
  if (mounted) {
    return mounted;
  }
  struct mounted_volume *entries = realloc(mounts->entries,
      (mounts->count + 1) * sizeof(*entries));
  if (!entries) {
    return NULL;
  }
  mounts->entries = entries;
  pool_system.partition = pool;
  mounted = &mounts->entries[mounts->count++];
  *mounted = (struct mounted_volume){.volume = &pool_system,
    .root = HANDLE_INVALID, .control = HANDLE_INVALID};
  mounted->status = mount_volume(authority, &pool_system, true, mounted);
  if (mounted->status != CALL_OK) {
    printf("boot-init: volume %s unavailable (status %u)\n", BOOT_SYSTEM_VOLUME, mounted->status);
  }
  return mounted;
}

/* Reads the installed configuration from the pool: boot.lua into CONFIG, with
 * its pool volumes bound to POOL, and the bytes of session.lua and network.lua
 * into SESSION and NETWORK. boot.lua and session.lua are parsed here; the
 * network profile is read against the controllers later. False fills REASON
 * and leaves all three empty. Nothing is applied or written. */
static bool read_pool_configuration(const struct authority *authority, uint32_t pool,
    struct mounts *mounts, struct boot_config *config, struct config_handoff_bytes *session,
    struct config_handoff_bytes *network, char *reason, size_t size)
{
  struct mounted_volume *system = mount_pool_system(authority, mounts, pool);
  if (!system || system->root == HANDLE_INVALID) {
    snprintf(reason, size, "the system volume on pool partition %u is unavailable (status %u)",
        pool, system ? system->status : CALL_NO_MEMORY);
    return false;
  }
  static const char *const paths[] = {POOL_BOOT_PATH, POOL_SESSION_PATH, POOL_NETWORK_PATH};
  struct config_handoff_bytes bytes[3] = {0};
  struct config_failure failure = {0};
  char name[64];
  bool ok = true;
  for (size_t i = 0; ok && i < 3; ++i) {
    snprintf(name, sizeof(name), "system://%s", paths[i]);
    ok = read_pool_file(system->root, paths[i], &bytes[i], &failure) == BOOT_CONFIG_OK;
    if (ok && i == 0) {
      ok = boot_config_read_pool_bytes(name, bytes[i].bytes, bytes[i].size, config,
          &failure) == BOOT_CONFIG_OK;
    } else if (ok && i == 1) {
      struct session_config parsed;
      ok = session_config_read_literal_bytes(name, bytes[i].bytes, bytes[i].size,
          &parsed) == CONFIG_OK;
      if (ok) {
        session_config_free(&parsed);
      } else {
        config_failure_format(&failure, "invalid");
      }
    }
  }
  if (!ok) {
    snprintf(reason, size, "%s: %s", name, failure.text[0] ? failure.text : "invalid");
    boot_config_free(config);
    for (size_t i = 0; i < 3; ++i) {
      config_handoff_bytes_free(&bytes[i]);
    }
    return false;
  }
  for (size_t i = 0; i < config->volume_count; ++i) {
    if (config->volumes[i].kind == BOOT_VOLUME_NPFS &&
        config->volumes[i].partition == BOOT_POOL_PARTITION) {
      config->volumes[i].partition = pool;
    }
  }
  config_handoff_bytes_free(&bytes[0]);
  *session = bytes[1];
  *network = bytes[2];
  return true;
}

/* The built-in rescue space owns the network. On an installed boot it holds
 * the pool's system volume read-write, so the configuration can be fixed or
 * restored (decision 22); otherwise boot and tmp only. */
static void start_rescue(const struct authority *authority, struct mounts *mounts,
    uint32_t pool, const char *reason)
{
  static struct boot_root roots[] = {
    {.name = BOOT_SYSTEM_VOLUME, .volume = BOOT_SYSTEM_VOLUME, .read_write = true},
  };
  static const struct boot_space rescue = {
    .name = "rescue", .title = "Rescue", .init = RESCUE_INIT, .start = "tmp", .power = true,
    .clipboard_local = true, .clipboard_shared = true, .network = true,
  };
  static const struct boot_space pool_rescue = {
    .name = "rescue", .title = "Rescue", .init = RESCUE_INIT, .start = "tmp", .power = true,
    .clipboard_local = true, .clipboard_shared = true, .roots = roots, .root_count = 1,
    .network = true,
  };
  printf("boot-init: %s; starting the rescue space\n", reason);
  struct mounted_volume *system = pool ? mount_pool_system(authority, mounts, pool) : NULL;
  bool with_pool = system && system->root != HANDLE_INVALID;
  start_space(authority, mounts, with_pool ? &pool_rescue : &rescue);
}

static bool close_frozen(struct authority *authority)
{
  bool ok = config_handoff_close(&authority->configuration);
  if (authority->boot_evidence != HANDLE_INVALID && handle_close(authority->boot_evidence)) {
    ok = false;
  }
  authority->boot_evidence = HANDLE_INVALID;
  return ok;
}

static bool close_boot_payloads(struct authority *authority)
{
  bool ok = true;
  handle_t *files[] = {&authority->loaded_kernel, &authority->loaded_archive};
  for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
    if (*files[i] != HANDLE_INVALID) {
      ok = handle_close(*files[i]) == 0 && ok;
      *files[i] = HANDLE_INVALID;
    }
  }
  return ok;
}

int main(int argc, char **argv)
{
  bool installed = false, default_config = false, pool_malformed = false;
  const char *remote_beacon = NULL;
  uint32_t pool = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--installed")) {
      installed = true;
    } else if (!strcmp(argv[i], "--pool") && i + 1 < argc) {
      char *end;
      unsigned long value = strtoul(argv[++i], &end, 10);
      pool = !*end && value && value <= UINT32_MAX ? (uint32_t)value : 0;
      pool_malformed |= !pool;
    } else if (!strcmp(argv[i], "--pool-malformed")) {
      pool_malformed = true;
    } else if (!strcmp(argv[i], "--default-config")) {
      default_config = true;
    } else if (!strcmp(argv[i], "--remote-beacon")) {
      if (remote_beacon || i + 1 >= argc) {
        fputs("boot-init: --remote-beacon requires one name and cannot be repeated\n", stderr);
        return EXIT_FAILURE;
      }
      remote_beacon = argv[++i];
      if (!remote_beacon_name_length(remote_beacon)) {
        fputs("boot-init: invalid remote beacon name\n", stderr);
        return EXIT_FAILURE;
      }
    } else {
      fprintf(stderr, "boot-init: unknown argument %s\n", argv[i]);
    }
  }

  struct authority authority = {.remote_beacon = remote_beacon};
  if (!take_authority(&authority)) {
    fputs("boot-init: missing boot authority; no spaces started\n", stderr);
    return EXIT_FAILURE;
  }

  struct boot_config defaults = {0};
  struct config_handoff_bytes session = {0}, network = {0};
  struct network_config profile = {0};
  struct boot_plan plan = {0};
  struct config_failure failure = {0};
  bool configured = false;
  struct mounts mounts = {0};
  int exit_status = EXIT_SUCCESS;
  char rescue[START_REASON_MAX + 1] = "no configured space started";
  handle_t registry = authority.services[SERVICE_NET_REGISTRY];
  if (installed) {
    struct boot_evidence_authority sources = {
      .esp_mount = authority.esp_mount, .ram = authority.ram,
      .clock = authority.services[SERVICE_CLOCK], .random = authority.services[SERVICE_RANDOM],
      .loaded_kernel = authority.loaded_kernel, .loaded_archive = authority.loaded_archive,
    };
    bool evidence = boot_evidence_prepare(&sources, &authority.boot_evidence);
    if (!close_boot_payloads(&authority) || !evidence) {
      exit_status = EXIT_FAILURE;
      goto finish;
    }
  } else if (!close_boot_payloads(&authority)) {
    exit_status = EXIT_FAILURE;
    goto finish;
  }

  /* Validate the whole configuration before applying any of it: a rejected one
   * contributes nothing, not even its hostname fallback or input defaults.
   * Installed entries name their pool. A development build bound to a disk
   * with MOUNT_DISK names none and describes that disk in live.lua. */
  bool pool_configured = installed && pool && !pool_malformed;
  if (installed && pool_malformed) {
    snprintf(rescue, sizeof(rescue), "the boot entry's mount.pool is malformed");
  } else if (default_config) {
    snprintf(rescue, sizeof(rescue), "rescue entry");
  } else if (pool_configured) {
    configured = read_pool_configuration(&authority, pool, &mounts, &defaults, &session,
        &network, rescue, sizeof(rescue));
    if (configured && !boot_plan_build(&defaults, &plan, &failure)) {
      snprintf(rescue, sizeof(rescue), "system://" POOL_BOOT_PATH ": %s",
          failure.text[0] ? failure.text : "invalid");
      configured = false;
    }
    if (configured && !boot_network_read(&profile, registry, &network)) {
      snprintf(rescue, sizeof(rescue), "system://" POOL_NETWORK_PATH
          ": invalid, or its controllers are absent");
      configured = false;
    }
  } else {
    printf("boot-init: reading %s\n", LIVE_CONFIG);
    enum boot_config_result result = boot_config_read(LIVE_CONFIG, &defaults);
    configured = result == BOOT_CONFIG_OK &&
        boot_plan_build(&defaults, &plan, NULL);
    if (!configured) {
      snprintf(rescue, sizeof(rescue), "%s %s", LIVE_CONFIG,
          result == BOOT_CONFIG_MISSING ? "is missing" : "is invalid");
    } else if (!boot_network_read(&profile, registry, NULL)) {
      snprintf(rescue, sizeof(rescue), "boot://config/network.lua: invalid, or its controllers are absent");
      configured = false;
    }
  }
  if (configured && !boot_network_prepare(&authority.network, registry, authority.net_policy,
      &profile)) {
    snprintf(rescue, sizeof(rescue), "the network profile cannot be applied");
    boot_network_release(&authority.network);
    configured = false;
  }
  if (configured && pool_configured) {
    enum call_status status;
    if (!boot_configuration_handoff(authority.ram, &session, &network,
        &authority.configuration, &status)) {
      snprintf(rescue, sizeof(rescue), "session and network handoff cannot be created (status %u)",
          status);
      boot_network_release(&authority.network);
      configured = false;
    }
  }
  network_config_free(&profile);
  config_handoff_bytes_free(&session);
  config_handoff_bytes_free(&network);
  if (configured) {
    if (pool_configured) {
      puts("boot-init: read system://config/");
    }
  } else {
    /* Built-in rescue settings, as the reader's empty configuration, and the
     * archive's network profile so rescue can fetch packages. If that profile
     * cannot be applied either, or a failed attempt left conflicting
     * definitions, rescue has loopback only. */
    boot_plan_free(&plan);
    boot_config_free(&defaults);
    defaults = (struct boot_config){.mouse_scroll_natural = true, .touchpad_scroll_natural = true};
    bool archive_network = boot_network_read(&profile, registry, NULL) &&
        boot_network_prepare(&authority.network, registry, authority.net_policy, &profile);
    network_config_free(&profile);
    if (!archive_network) {
      boot_network_release(&authority.network);
      struct network_config loopback = {0};
      puts("boot-init: boot://config/network.lua cannot be applied; rescue has loopback only");
      if (!boot_network_prepare(&authority.network, registry, authority.net_policy, &loopback)) {
        fputs("boot-init: loopback network setup failed; no spaces started\n", stderr);
        exit_status = EXIT_FAILURE;
        goto finish;
      }
    }
  }
  authority.services[SERVICE_ECHO] = authority.network.echo;
  authority.services[SERVICE_UDP] = authority.network.udp;
  authority.services[SERVICE_TCP] = authority.network.tcp;

  authority.ram_npfs_bytes = defaults.ram_npfs_bytes;
  bool machine_settings = installed && find_mount(&mounts, BOOT_SYSTEM_VOLUME);
  if (!set_hostname(&authority, &defaults, &mounts, machine_settings) ||
      !set_input_policy(&authority, &defaults, &mounts, machine_settings)) {
    exit_status = EXIT_FAILURE;
    goto finish;
  }
  if (defaults.wifi_channels) {
    enum call_status policy = wifi_install_policy(authority.wifi,
        defaults.wifi_country, defaults.wifi_channels);
    if (policy != CALL_OK) {
      fprintf(stderr, "boot-init: Wi-Fi country policy refused (status %u); no Wi-Fi TX admitted\n", policy);
    }
  }
  if (configured) {
    bind_bin(&authority, &plan, pool_configured);
  } else {
    authority.bin = authority.boot;
  }
  size_t started = 0;
  if (configured && mount_plan(&authority, &plan, &mounts)) {
    seed_live_repositories(&authority, &mounts);
    struct mounted_volume *packages = find_mount(&mounts, BOOT_PACKAGES_VOLUME);
    struct mounted_volume *http_work = find_mount(&mounts, BOOT_HTTP_WORK_VOLUME);
    package_boot_maintain(packages ? packages->root : HANDLE_INVALID,
        http_work && (http_work->volume->kind == BOOT_VOLUME_NPFS ||
            http_work->volume->kind == BOOT_VOLUME_RAM_NPFS) ? http_work->root : HANDLE_INVALID,
        authority.services[SERVICE_CLOCK]);
    check_packages(&mounts);
    for (size_t i = 0; i < plan.space_count; ++i) {
      started += start_space(&authority, &mounts, plan.spaces[i]);
    }
  }
  if (!started) {
    start_rescue(&authority, &mounts, pool_configured ? pool : 0, rescue);
  }

finish:
  /* Drop every local data view before retirement. Published instances remain
   * under their delegated controls; never revoke a space's references. */
  for (size_t i = 0; i < mounts.count; ++i) {
    struct mounted_volume *mounted = &mounts.entries[i];
    if (mounted->root != HANDLE_INVALID) {
      handle_close(mounted->root);
    }
  }
  for (size_t i = 0; i < mounts.count; ++i) {
    struct mounted_volume *mounted = &mounts.entries[i];
    if (mounted->control == HANDLE_INVALID) {
      continue;
    }
    if (!mounted->published) {
      enum call_status status = mount_retire(mounted->control);
      if (status != CALL_OK) {
        printf("boot-init: unused volume %s retained (status %u)\n",
            mounted->volume->name, status);
      }
    }
    handle_close(mounted->control);
  }
  boot_plan_free(&plan);
  boot_config_free(&defaults);
  network_config_free(&profile);
  config_handoff_bytes_free(&session);
  config_handoff_bytes_free(&network);
  close_frozen(&authority);
  boot_network_release(&authority.network);
  if (authority.bin != HANDLE_INVALID && authority.bin != authority.boot) {
    handle_close(authority.bin);
  }
  free(mounts.entries);
  handle_t clock = authority.services[SERVICE_CLOCK];
  release_boot_environment();
  bootstrap_release_except(&clock, 1);
  observe_roots(clock);
  return exit_status;
}
