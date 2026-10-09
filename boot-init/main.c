#include "config.h"
#include <abi/clock.h>
#include <abi/directory.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/launcher.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/namespace.h>
#include <abi/net_config.h>
#include <abi/pipe.h>
#include <abi/power.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/terminal.h>
#include <abi/udp.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <mount.h>
#include <path.h>
#include <remote/beacon.h>
#include <space.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_info.h>

#define LIVE_CONFIG "boot://config/live.lua"
#define INSTALLED_CONFIG "boot://config/installed.lua"
#define OVERRIDE_PATH "config/boot.lua"
#define OVERRIDE_NAME "system://" OVERRIDE_PATH
#define OVERRIDE_MAX_BYTES (64 * 1024)
#define RESCUE_INIT "boot://shell.pxe"
/* The bin volume shares the system volume's pool; see the installer. */
#define BIN_VOLUME "bin"
#define BIN_UNKNOWN_REVISION "unknown"

#define BOOT_ROOT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)
#define TMP_ROOT_RIGHTS (BOOT_ROOT_RIGHTS | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define READ_ONLY_ROOT_RIGHTS (BOOT_ROOT_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO)

/* Services forwarded to every space init, in grant order. The network owner
 * additionally receives the rights in owner_rights. */
enum service {
  SERVICE_MEMORY,
  SERVICE_LAUNCHER,
  SERVICE_CLOCK,
  SERVICE_SYSTEM_INFO,
  SERVICE_LOG,
  SERVICE_ECHO,
  SERVICE_UDP,
  SERVICE_TCP,
  SERVICE_RANDOM,
  SERVICE_NET_CONFIG,
  SERVICE_PROFILE,
  SERVICE_PIPE,
  SERVICE_ENDPOINT,
  SERVICE_NAMESPACE,
  SERVICE_TERMINAL,
  SERVICE_COUNT,
};

static const struct {
  const char *name;
  uint64_t rights;
  uint64_t owner_rights;
} services[SERVICE_COUNT] = {
  [SERVICE_MEMORY] = {"memory", MEMORY_RIGHT_MANAGE, 0},
  [SERVICE_LAUNCHER] = {"launcher", LAUNCHER_RIGHT_LAUNCH | LAUNCHER_RIGHT_CREATE_GROUP, 0},
  [SERVICE_CLOCK] = {"clock", CLOCK_RIGHTS, 0},
  [SERVICE_SYSTEM_INFO] = {"system_info", SYSTEM_INFO_RIGHT_READ, 0},
  [SERVICE_LOG] = {"log", LOG_RIGHT_READ, 0},
  [SERVICE_ECHO] = {"echo", ECHO_RIGHT_SEND, 0},
  [SERVICE_UDP] = {"udp", UDP_SERVICE_RIGHT_OPEN, UDP_SERVICE_RIGHT_BROADCAST},
  [SERVICE_TCP] = {"tcp", TCP_SERVICE_RIGHTS, 0},
  [SERVICE_RANDOM] = {"random", RANDOM_RIGHT_READ, 0},
  [SERVICE_NET_CONFIG] = {"net_config", NET_CONFIG_RIGHT_READ, NET_CONFIG_RIGHT_WRITE},
  [SERVICE_PROFILE] = {"profile", PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST, 0},
  [SERVICE_PIPE] = {"pipe", PIPE_SERVICE_RIGHT_CREATE, 0},
  [SERVICE_ENDPOINT] = {"service", ENDPOINT_SERVICE_RIGHT_CREATE, 0},
  [SERVICE_NAMESPACE] = {"namespace_service", NAMESPACE_SERVICE_RIGHT_CREATE, 0},
  [SERVICE_TERMINAL] = {"terminal", TERMINAL_SERVICE_RIGHT_CREATE, 0},
};

/* Grant order after the services: the boot, tmp and bin roots, then volumes. */
enum {
  GRANT_BOOT = SERVICE_COUNT,
  GRANT_TMP,
  GRANT_BIN,
  GRANT_FIRST_VOLUME,
};

struct authority {
  handle_t factory;
  handle_t services[SERVICE_COUNT];
  handle_t boot, tmp;
  /* The running revision's program directory, or the archive itself. */
  handle_t bin;
  /* A private RAM directory; each ram volume is one of its subdirectories. */
  handle_t ram;
  handle_t native_mount, host_mount;
  /* Granted only to spaces that set screenshot. */
  handle_t screen_capture;
  /* Granted only to spaces that set power or remote_power. */
  handle_t power;
  uint64_t cpu_count;
  const char *remote_beacon;
};

/* One configured volume, mounted at most once. */
struct mounted_volume {
  const struct boot_volume *volume;
  handle_t root;
  enum call_status status; /* Why root is absent. */
};

struct mounts {
  struct mounted_volume *entries;
  size_t count;
};

static bool take_authority(struct authority *authority)
{
  authority->factory = startup_resource("space_factory");
  authority->boot = startup_root("boot");
  authority->tmp = startup_root("tmp");
  authority->ram = startup_resource("ram");
  authority->native_mount = startup_resource("native_mount");
  authority->host_mount = startup_resource("host_mount");
  authority->screen_capture = startup_resource("screen_capture");
  authority->power = startup_resource("power");
  bool complete = authority->factory != HANDLE_INVALID &&
      authority->boot != HANDLE_INVALID && authority->tmp != HANDLE_INVALID;
  for (size_t i = 0; i < SERVICE_COUNT; ++i) {
    authority->services[i] = startup_resource(services[i].name);
    if (authority->services[i] == HANDLE_INVALID) {
      fprintf(stderr, "boot-init: missing resource %s\n", services[i].name);
      complete = false;
    }
  }
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
      TMP_ROOT_RIGHTS, root);
  if (status == CALL_ALREADY_EXISTS) {
    status = directory_lookup(ram, name, DIRECTORY_KIND_DIRECTORY, TMP_ROOT_RIGHTS, root);
  }
  return status;
}

static enum call_status mount_volume(const struct authority *authority,
    const struct boot_volume *volume, bool read_write, handle_t *root)
{
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
  if (authority->native_mount == HANDLE_INVALID) {
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

/* Mounts every volume some space uses, once, read-write if any space needs it.
 * An already mounted system volume is kept. */
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
    bool used = false, read_write = false;
    for (size_t j = 0; j < plan->space_count; ++j) {
      const struct boot_space *space = plan->spaces[j];
      for (size_t k = 0; k < space->root_count; ++k) {
        if (!strcmp(space->roots[k].volume, volume->name)) {
          used = true;
          read_write |= space->roots[k].read_write;
        }
      }
    }
    if (!used || find_mount(mounts, volume->name)) {
      continue;
    }
    struct mounted_volume *mounted = &mounts->entries[mounts->count++];
    *mounted = (struct mounted_volume){.volume = volume, .root = HANDLE_INVALID};
    mounted->status = mount_volume(authority, volume, read_write, &mounted->root);
    if (mounted->status != CALL_OK) {
      printf("boot-init: volume %s unavailable (status %u)\n", volume->name, mounted->status);
    }
  }
  return true;
}

/* Reads the override from the mounted system root. MISSING when absent. */
static enum boot_config_result read_override(handle_t system, struct boot_config *override)
{
  *override = (struct boot_config){0};
  struct path_root root = {"system", system};
  struct path_context context = {.roots = &root, .root_count = 1};
  handle_t directories[8];
  char component[256];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 8,
    .component = component, .component_capacity = sizeof(component),
  };
  handle_t file;
  enum call_status status = path_resolve(&context, OVERRIDE_NAME, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &file);
  if (status == CALL_NOT_FOUND) {
    return BOOT_CONFIG_MISSING;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "boot-init: cannot open " OVERRIDE_NAME " (status %u)\n", status);
    return BOOT_CONFIG_INVALID;
  }
  uint64_t size;
  char *bytes = NULL;
  status = file_size(file, &size);
  if (status == CALL_OK && size > OVERRIDE_MAX_BYTES) {
    fprintf(stderr, "boot-init: " OVERRIDE_NAME " exceeds %d bytes\n", OVERRIDE_MAX_BYTES);
    status = CALL_LIMIT;
  }
  if (status == CALL_OK) {
    bytes = malloc(size ? size : 1);
    status = bytes ? CALL_OK : CALL_NO_MEMORY;
  }
  for (uint64_t used = 0; status == CALL_OK && used < size;) {
    size_t count;
    status = file_read(file, used, bytes + used, size - used, &count);
    if (status == CALL_OK && !count) {
      status = CALL_BAD_REQUEST;
    }
    used += count;
  }
  handle_close(file);
  enum boot_config_result result = BOOT_CONFIG_INVALID;
  if (status == CALL_OK) {
    result = boot_config_read_bytes(OVERRIDE_NAME, bytes, size, override);
  } else {
    fprintf(stderr, "boot-init: cannot read " OVERRIDE_NAME " (status %u)\n", status);
  }
  free(bytes);
  return result;
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

/* Grants, bindings and roots for one space init, built in caller storage. */
struct space_launch {
  struct launch_grant *grants;
  struct launch_binding resources[SERVICE_COUNT + 6];
  struct launch_binding *roots;
  size_t grant_count, resource_count, root_count;
  uint64_t working_directory;
  char working_path[BOOT_NAME_MAX + sizeof("://")];
};

/* Starts SPACE in the root its start names, or home. When that root is absent,
 * as for an optional root or a space without home, it starts in tmp. */
static void select_start(const struct boot_space *space, struct space_launch *launch)
{
  const char *start = space->start ? space->start : BOOT_HOME_ROOT;
  for (size_t i = 0; i < launch->root_count; ++i) {
    if (!strcmp((const char *)(uintptr_t)launch->roots[i].name, start)) {
      launch->working_directory = launch->roots[i].grant;
      snprintf(launch->working_path, sizeof(launch->working_path), "%s://", start);
      return;
    }
  }
  printf("boot-init: space %s has no %s://; it starts in tmp://\n", space->name, start);
  launch->working_directory = GRANT_TMP;
  snprintf(launch->working_path, sizeof(launch->working_path), "tmp://");
}

static const char *build_launch(const struct authority *authority, struct mounts *mounts,
    const struct boot_space *space, struct space_launch *launch, char *reason, size_t size)
{
  if (space->screenshot && authority->screen_capture == HANDLE_INVALID) {
    return "screenshot requested but screen_capture is unavailable";
  }
  launch->grants = calloc(GRANT_FIRST_VOLUME + space->root_count + 6, sizeof(*launch->grants));
  launch->roots = calloc(3 + space->root_count, sizeof(*launch->roots));
  if (!launch->grants || !launch->roots) {
    return "boot init is out of memory";
  }
  for (size_t i = 0; i < SERVICE_COUNT; ++i) {
    uint64_t rights = services[i].rights | (space->network ? services[i].owner_rights : 0);
    launch->grants[i] = (struct launch_grant){authority->services[i], rights, 0};
    launch->resources[i] = (struct launch_binding){(uintptr_t)services[i].name, i};
  }
  launch->grants[GRANT_BOOT] = (struct launch_grant){authority->boot, BOOT_ROOT_RIGHTS, 0};
  launch->grants[GRANT_TMP] = (struct launch_grant){authority->tmp, TMP_ROOT_RIGHTS, 0};
  launch->roots[0] = (struct launch_binding){(uintptr_t)"boot", GRANT_BOOT};
  launch->roots[1] = (struct launch_binding){(uintptr_t)"tmp", GRANT_TMP};
  launch->grants[GRANT_BIN] = (struct launch_grant){authority->bin, BOOT_ROOT_RIGHTS, 0};
  launch->roots[2] = (struct launch_binding){(uintptr_t)"bin", GRANT_BIN};
  launch->grant_count = GRANT_FIRST_VOLUME;
  launch->resource_count = SERVICE_COUNT;
  launch->root_count = 3;
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
    struct handle_info info;
    enum call_status status = handle_query(mounted->root, &info);
    if (status != CALL_OK) {
      snprintf(reason, size, "volume %s unavailable (status %u)", root->volume, status);
      return reason;
    }
    uint64_t rights = info.rights & (root->read_write ? DIRECTORY_RIGHTS : READ_ONLY_ROOT_RIGHTS);
    launch->grants[launch->grant_count] = (struct launch_grant){mounted->root, rights, 0};
    launch->roots[launch->root_count++] =
        (struct launch_binding){(uintptr_t)root->volume, launch->grant_count++};
  }
  if (space->launch) {
    /* Ordinary child launch is independent of init's administrative grant. */
    launch->resources[launch->resource_count++] =
        (struct launch_binding){(uintptr_t)"child_launcher", launch->grant_count};
    launch->grants[launch->grant_count++] = (struct launch_grant){
      authority->services[SERVICE_LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0
    };
  }
  if (space->multiplexer) {
    /* The distinct name records opt-in, separate from trusted init authority. */
    launch->resources[launch->resource_count++] =
        (struct launch_binding){(uintptr_t)"mux_terminal", launch->grant_count};
    launch->grants[launch->grant_count++] = (struct launch_grant){
      authority->services[SERVICE_TERMINAL], TERMINAL_SERVICE_RIGHT_CREATE, 0
    };
  }
  if (space->screenshot) {
    launch->resources[launch->resource_count++] =
        (struct launch_binding){(uintptr_t)"screen_capture", launch->grant_count};
    launch->grants[launch->grant_count++] = (struct launch_grant){
      authority->screen_capture, SCREEN_CAPTURE_RIGHT_CAPTURE, 0
    };
  }
  if (space->power && authority->power != HANDLE_INVALID) {
    launch->grants[launch->grant_count] = (struct launch_grant){authority->power, POWER_RIGHTS, 0};
    launch->resources[launch->resource_count++] =
        (struct launch_binding){(uintptr_t)"power", launch->grant_count++};
  }
  if (space->remote_power && authority->power != HANDLE_INVALID) {
    launch->grants[launch->grant_count] = (struct launch_grant){authority->power, POWER_RIGHTS, 0};
    launch->resources[launch->resource_count++] =
        (struct launch_binding){(uintptr_t)"remote_power", launch->grant_count++};
  }
  if (authority->remote_beacon && !strcmp(space->name, "remote")) {
    launch->resources[launch->resource_count++] =
        (struct launch_binding){(uintptr_t)"udp_beacons", launch->grant_count};
    launch->grants[launch->grant_count++] = (struct launch_grant){
      authority->services[SERVICE_UDP], UDP_SERVICE_RIGHT_BROADCAST, 0
    };
  }
  select_start(space, launch);
  return NULL;
}

static bool start_space(const struct authority *authority, struct mounts *mounts,
    const struct boot_space *space)
{
  struct space_definition definition = {
    .name = space->name, .title = space->title,
    .flags = (space->multiplexer ? SPACE_CREATE_TERMINAL_CONTROL : 0) |
        (space->clipboard_local ? SPACE_CREATE_CLIPBOARD_LOCAL : 0) |
        (space->clipboard_shared ? SPACE_CREATE_CLIPBOARD_SHARED : 0),
  };
  char text[SPACE_REASON_MAX + 1];
  uint64_t *cpus = calloc(cpu_words(authority->cpu_count), sizeof(*cpus));
  struct space_launch launch = {0};
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  const char *reason = cpus ? space_cpus(authority, space, cpus, text, sizeof(text)) :
      "boot init is out of memory";
  if (!reason) {
    reason = build_launch(authority, mounts, space, &launch, text, sizeof(text));
  }
  if (!reason) {
    struct path_root roots[] = {{"boot", authority->boot}};
    struct path_context context = {.roots = roots, .root_count = 1};
    handle_t directories[8];
    char component[BOOT_INIT_MAX + 1];
    struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 8,
    .component = component, .component_capacity = sizeof(component),
  };
    enum call_status status = path_resolve(&context, space->init, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &workspace, &image);
    if (status != CALL_OK) {
      snprintf(text, sizeof(text), "cannot open init %.64s (status %u)", space->init, status);
      reason = text;
    }
  }

  bool started = false;
  enum call_status status;
  if (reason) {
    status = space_create_unstarted(authority->factory, &definition, reason);
    if (status != CALL_OK) {
      fprintf(stderr, "boot-init: cannot create space %s (status %u)\n", space->name, status);
    }
  } else {
    const char *arguments[] = {space->init};
    bool reverse = authority->remote_beacon && !strcmp(space->name, "remote");
    const struct startup_variable environment[] = {
      {(uintptr_t)"OS_NAME", (uintptr_t)"Pyxis OS"},
      {(uintptr_t)"PYXIS_REMOTE_BEACON", (uintptr_t)authority->remote_beacon},
    };
    struct launch_request request = {
      .image = image,
      .grants = (uintptr_t)launch.grants, .grant_count = launch.grant_count,
      .resources = (uintptr_t)launch.resources, .resource_count = launch.resource_count,
      .roots = (uintptr_t)launch.roots, .root_count = launch.root_count,
      .working_directories = (uintptr_t)&launch.working_directory,
      .working_directory_count = 1,
      .working_path = (uintptr_t)launch.working_path,
      .environment = (uintptr_t)environment, .environment_count = reverse ? 2 : 1,
      .argv = (uintptr_t)arguments, .argc = 1,
    };
    struct path_root roots[] = {{"boot", authority->boot}};
    struct path_context interpreters = {.roots = roots, .root_count = 1};
    status = program_create_space(authority->factory, &definition, cpus,
        authority->cpu_count, &request, &interpreters, &child);
    if (status == CALL_OK) {
      printf("boot-init: space %s started: %s%s\n", space->name, space->init,
          space->network ? " (network owner)" : "");
      started = true;
    } else {
      fprintf(stderr, "boot-init: space %s: cannot start %s (status %u)\n", space->name,
          space->init, status);
    }
  }
  if (child != HANDLE_INVALID) {
    handle_close(child);
  }
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  free(launch.roots);
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

static void start_rescue(const struct authority *authority, struct mounts *mounts)
{
  static const struct boot_space rescue = {
    .name = "rescue", .title = "Rescue", .init = RESCUE_INIT, .start = "tmp", .power = true,
    .clipboard_local = true, .clipboard_shared = true,
  };
  puts("boot-init: no configured space started; starting the rescue space");
  start_space(authority, mounts, &rescue);
}

int main(int argc, char **argv)
{
  bool installed = false, default_config = false;
  const char *remote_beacon = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--installed")) {
      installed = true;
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

  struct boot_config defaults, override = {0};
  struct boot_plan plan = {0};
  struct mounts mounts = {0};
  const char *path = installed ? INSTALLED_CONFIG : LIVE_CONFIG;
  printf("boot-init: reading %s\n", path);
  enum boot_config_result result = boot_config_read(path, &defaults);
  if (result != BOOT_CONFIG_OK) {
    fprintf(stderr, "boot-init: %s %s\n", path,
        result == BOOT_CONFIG_MISSING ? "is missing" : "is invalid");
  } else if (!boot_plan_build(&defaults, NULL, &plan)) {
    fprintf(stderr, "boot-init: %s is invalid\n", path);
  }

  const struct boot_volume *system = plan.space_count ?
      boot_plan_volume(&plan, BOOT_SYSTEM_VOLUME) : NULL;
  if (installed && system) {
    /* The override is read before the plan is final, so system is mounted
     * read-write and each space receives only the access its root asks for. */
    mounts.entries = calloc(1, sizeof(*mounts.entries));
    if (mounts.entries) {
      struct mounted_volume *mounted = &mounts.entries[mounts.count++];
      *mounted = (struct mounted_volume){.volume = system, .root = HANDLE_INVALID};
      mounted->status = mount_volume(&authority, system, true, &mounted->root);
      if (mounted->status != CALL_OK) {
        printf("boot-init: volume %s unavailable (status %u)\n", system->name,
            mounted->status);
      } else if (default_config) {
        puts("boot-init: rescue entry: ignoring " OVERRIDE_NAME);
      } else {
        result = read_override(mounted->root, &override);
        if (result == BOOT_CONFIG_MISSING) {
          puts("boot-init: no " OVERRIDE_NAME "; using the default");
        } else if (result == BOOT_CONFIG_OK) {
          struct boot_plan merged;
          if (boot_plan_build(&defaults, &override, &merged)) {
            boot_plan_free(&plan);
            plan = merged;
            puts("boot-init: applied " OVERRIDE_NAME);
          } else {
            result = BOOT_CONFIG_INVALID;
          }
        }
        if (result == BOOT_CONFIG_INVALID) {
          puts("boot-init: ignoring invalid " OVERRIDE_NAME "; using the default");
        }
      }
    }
  }

  bind_bin(&authority, &plan, installed);
  size_t started = 0;
  if (mount_plan(&authority, &plan, &mounts)) {
    for (size_t i = 0; i < plan.space_count; ++i) {
      started += start_space(&authority, &mounts, plan.spaces[i]);
    }
  }
  if (!started) {
    start_rescue(&authority, &mounts);
  }

  /* Spaces keep their own grants to every mounted root; boot init's go with it. */
  boot_plan_free(&plan);
  boot_config_free(&override);
  boot_config_free(&defaults);
  free(mounts.entries);
  return EXIT_SUCCESS;
}
