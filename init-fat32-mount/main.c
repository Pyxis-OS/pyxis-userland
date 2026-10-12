#include <abi/console.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <directory.h>
#include <disk.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <path.h>
#include <launcher.h>
#include <process.h>
#include <space.h>
#include <startup.h>
#include <system_info.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool success = handle_close(*handle) == 0;
  if (!success) {
    fputs("fat32 mount: handle close failed\n", stderr);
  }
  *handle = HANDLE_INVALID;
  return success;
}

static bool decimal(const char *text, uint64_t *value)
{
  uint64_t number = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || number > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    number = number * 10 + (*text - '0');
  }
  *value = number;
  return true;
}

static bool prompt(struct terminal *terminal, const char *label, char *line, size_t bytes)
{
  if (fflush(stdout) != 0) {
    fputs("fat32 mount: output failed\n", stderr);
    return false;
  }
  struct term_line_result result = term_read_line(terminal, label, line, bytes);
  if (result.status != TERM_LINE_OK || result.limit_reached) {
    fputs("fat32 mount: selection cancelled or input refused\n", stderr);
    return false;
  }
  return true;
}

static int wait_child(handle_t child)
{
  struct process_result completion;
  enum call_status status = process_wait(child, &completion);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: wait failed (status %u)\n", status);
    return EXIT_FAILURE;
  }
  if (completion.kind == PROCESS_EXITED) {
    if (completion.exit_status == 0) {
      return EXIT_SUCCESS;
    }
    fprintf(stderr, "fat32 mount: child exited with status %lld\n",
        (long long)completion.exit_status);
  } else {
    fprintf(stderr, "fat32 mount: child stopped (completion kind %llu)\n",
        (unsigned long long)completion.kind);
  }
  return EXIT_FAILURE;
}

#define ROOT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)

enum {
  COMMAND_BYTES = 4096,
  READ_BYTES = 4096,
  COMPONENT_BYTES = 1024,
  PATH_HANDLES = 65,
};

static bool volume_path(const char *path, char *uri, size_t capacity)
{
  if (!path || path[0] != '/' || strlen(path) + sizeof("esp://") > capacity) {
    fputs("fat32 mount: use a bounded /PATH within the selected volume\n", stderr);
    return false;
  }
  snprintf(uri, capacity, "esp://%s", path + 1);
  return true;
}

static enum call_status lookup(handle_t root, const char *path, uint64_t kind,
    uint64_t rights, handle_t *object)
{
  char uri[COMMAND_BYTES], component[COMPONENT_BYTES];
  handle_t directories[PATH_HANDLES];
  struct path_root binding = {"esp", root};
  struct path_context context = {.roots = &binding, .root_count = 1};
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = PATH_HANDLES,
    .component = component, .component_capacity = sizeof(component),
  };
  *object = HANDLE_INVALID;
  if (!volume_path(path, uri, sizeof(uri))) {
    return CALL_BAD_REQUEST;
  }
  return path_resolve_native(&context, uri, kind, rights, &workspace, object);
}

static enum call_status object_info(handle_t root, const char *path, handle_t *object,
    struct file_info_reply *info)
{
  size_t length = strlen(path);
  if (length && path[length - 1] == '/') {
    enum call_status status = lookup(root, path, DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_LOOKUP, object);
    return status == CALL_OK ? directory_info(*object, info) : status;
  }
  enum call_status status = lookup(root, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, object);
  if (status == CALL_OK) {
    return file_info(*object, info);
  }
  if (status != CALL_NOT_FOUND && status != CALL_WRONG_TYPE) {
    return status;
  }
  status = lookup(root, path, DIRECTORY_KIND_DIRECTORY, DIRECTORY_RIGHT_LOOKUP, object);
  return status == CALL_OK ? directory_info(*object, info) : status;
}

static void print_info(const char *path, const struct file_info_reply *info)
{
  printf("%s: valid=0x%llx", path, (unsigned long long)info->valid);
  if (info->valid & FILE_INFO_SIZE_VALID) {
    printf(" size=%llu", (unsigned long long)info->size);
  }
  if (info->valid & FILE_INFO_DOMAIN_VALID) {
    printf(" domain=%llu", (unsigned long long)info->domain);
  }
  if (info->valid & FILE_INFO_OBJECT_VALID) {
    printf(" object=%llu", (unsigned long long)info->object);
  }
  putchar('\n');
}

static void inspect_info(handle_t root, char *paths)
{
  char *second = strstr(paths, " | ");
  if (second) {
    *second = 0;
    second += 3;
  }
  handle_t first_object = HANDLE_INVALID, second_object = HANDLE_INVALID;
  struct file_info_reply first, other;
  enum call_status status = object_info(root, paths, &first_object, &first);
  if (status == CALL_OK) {
    print_info(paths, &first);
    if (second) {
      status = object_info(root, second, &second_object, &other);
      if (status == CALL_OK) {
        print_info(second, &other);
        if ((first.valid & FILE_INFO_DOMAIN_VALID) &&
            (other.valid & FILE_INFO_DOMAIN_VALID) && first.domain != other.domain) {
          puts("Held references identify distinct objects.");
        } else if ((first.valid & (FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID)) ==
            (FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID) &&
            (other.valid & (FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID)) ==
            (FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID)) {
          puts(first.domain == other.domain && first.object == other.object ?
              "Held references identify the same object." :
              "Held references identify distinct objects.");
        } else {
          puts("Identity comparison unavailable.");
        }
      }
    }
  }
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: info failed (status %u)\n", status);
  }
  close_handle(&second_object);
  close_handle(&first_object);
}

static void inspect_read(handle_t root, char *arguments)
{
  char *length_text = strchr(arguments, ' ');
  char *path = length_text ? strchr(length_text + 1, ' ') : NULL;
  uint64_t offset, length;
  if (!path) {
    fputs("fat32 mount: use read OFFSET LENGTH /PATH\n", stderr);
    return;
  }
  *length_text++ = 0;
  *path++ = 0;
  if (!decimal(arguments, &offset) || !decimal(length_text, &length) || length > READ_BYTES) {
    fputs("fat32 mount: offset/length invalid; length is at most 4096\n", stderr);
    return;
  }
  handle_t file = HANDLE_INVALID;
  enum call_status status = lookup(root, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file);
  unsigned char bytes[READ_BYTES];
  size_t count = 0;
  if (status == CALL_OK) {
    status = file_read(file, offset, bytes, (size_t)length, &count);
  }
  if (status == CALL_OK) {
    if (fwrite(bytes, 1, count, stdout) != count) {
      fputs("fat32 mount: output failed\n", stderr);
    }
    printf("\nRead %zu bytes.\n", count);
  } else {
    fprintf(stderr, "fat32 mount: read failed (status %u)\n", status);
  }
  close_handle(&file);
}

static void run_reader(handle_t boot, handle_t launcher, handle_t root,
    const char *program, const char *path)
{
  char uri[COMMAND_BYTES];
  if (!volume_path(path, uri, sizeof(uri))) {
    return;
  }
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  enum call_status status = directory_lookup(boot, program, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: reader lookup failed (status %u)\n", status);
    return;
  }
  enum { ROOT, MEMORY, INPUT, OUTPUT, ERROR, GRANT_COUNT };
  struct startup_stream streams[] = {
    startup_stream(STARTUP_STDIN), startup_stream(STARTUP_STDOUT), startup_stream(STARTUP_STDERR),
  };
  struct launch_grant grants[] = {
    {root, ROOT_RIGHTS, 0}, {startup_resource("memory"), MEMORY_RIGHT_MANAGE, 0},
    {streams[0].handle, CONSOLE_RIGHT_READ, 0},
    {streams[1].handle, CONSOLE_RIGHT_WRITE, 0},
    {streams[2].handle, CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_binding resources[] = {{(uintptr_t)"memory", MEMORY}};
  struct launch_binding roots[] = {{(uintptr_t)"esp", ROOT}};
  const char *arguments[] = {program, "--", uri};
  if (!strcmp(program, "cat.pxe")) {
    arguments[1] = uri;
  }
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants, .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources, .resource_count = 1,
    .roots = (uintptr_t)roots, .root_count = 1,
    .argv = (uintptr_t)arguments, .argc = !strcmp(program, "cat.pxe") ? 2 : 3,
    .streams = {
      [STARTUP_STDIN] = {PROTOCOL_CONSOLE, INPUT},
      [STARTUP_STDOUT] = {PROTOCOL_CONSOLE, OUTPUT},
      [STARTUP_STDERR] = {PROTOCOL_CONSOLE, ERROR},
    },
  };
  if (fflush(stdout) != 0) {
    fputs("fat32 mount: output failed\n", stderr);
  } else {
    status = launcher_launch(launcher, &request, &child);
    if (status != CALL_OK) {
      fprintf(stderr, "fat32 mount: reader launch failed (status %u)\n", status);
    } else {
      wait_child(child);
    }
  }
  close_handle(&child);
  close_handle(&image);
}

static void show_state(handle_t control)
{
  struct mount_state_reply state;
  enum call_status status = mount_observe(control, &state);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: observe failed (status %u)\n", status);
    return;
  }
  printf("domain=%llu disk=%llu GPT=%llu blocks=[%llu,+%llu) flags=0x%llx\n",
      (unsigned long long)state.domain, (unsigned long long)state.device,
      (unsigned long long)state.partition, (unsigned long long)state.first_block,
      (unsigned long long)state.block_count, (unsigned long long)state.flags);
}

static int browse(bool boot_selected)
{
  handle_t boot = startup_root("boot"), launcher = startup_resource("launcher");
  handle_t authority = startup_resource(boot_selected ? "esp_mount" : "disks");
  handle_t disk = HANDLE_INVALID;
  struct mount_fat32_reply mounted = {HANDLE_INVALID, HANDLE_INVALID};
  struct startup_stream input = startup_stream(STARTUP_STDIN);
  struct startup_stream output = startup_stream(STARTUP_STDOUT);
  struct startup_stream error = startup_stream(STARTUP_STDERR);
  struct terminal terminal = {.input = input.handle, .output = output.handle};
  uint64_t partition = 0;
  int result = EXIT_FAILURE;
  if (boot == HANDLE_INVALID || launcher == HANDLE_INVALID || authority == HANDLE_INVALID ||
      startup_resource("memory") == HANDLE_INVALID || input.protocol != PROTOCOL_CONSOLE ||
      output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("fat32 mount: required trusted authority or console unavailable\n", stderr);
    goto done;
  }
  puts("Native FAT32 read-only inspection. Keep media quiescent; no external writers.");
  if (!boot_selected) {
    for (uint64_t index = 0;; ++index) {
      struct disk_info info;
      enum call_status status = disks_enumerate(authority, index, &info);
      if (status == CALL_NOT_FOUND) {
        break;
      }
      if (status != CALL_OK) {
        fprintf(stderr, "fat32 mount: inventory failed (status %u)\n", status);
        goto done;
      }
      printf("Disk ID %llu: %llu blocks of %llu bytes, preparation %llu, GPT %llu\n",
          (unsigned long long)info.id, (unsigned long long)info.block_count,
          (unsigned long long)info.block_size, (unsigned long long)info.preparation,
          (unsigned long long)info.gpt_status);
    }
    char selection[32];
    uint64_t id;
    if (!prompt(&terminal, "Explicit disk ID: ", selection, sizeof(selection)) ||
        !decimal(selection, &id) || !id ||
        !prompt(&terminal, "One-based GPT entry: ", selection, sizeof(selection)) ||
        !decimal(selection, &partition) || !partition || partition > UINT32_MAX) {
      fputs("fat32 mount: invalid selection\n", stderr);
      goto done;
    }
    enum call_status status = disks_open(authority, id, DISK_ACCESS_READ_ONLY, &disk);
    if (status != CALL_OK) {
      fprintf(stderr, "fat32 mount: disk open failed (status %u)\n", status);
      goto done;
    }
    close_handle(&authority);
  }
  enum call_status status = boot_selected ?
      mount_open_fat32(authority, ROOT_RIGHTS, &mounted) :
      disk_open_fat32(disk, partition, ROOT_RIGHTS, &mounted);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: admission failed (status %u); no fallback\n", status);
    goto done;
  }
  show_state(mounted.control);
  puts("ls /PATH; cat /PATH; info /PATH [ | /PATH]; read OFFSET LENGTH /PATH");
  puts("state; close; open; retire; quit. File bytes may contain terminal controls.");
  char line[COMMAND_BYTES];
  while (prompt(&terminal, "fat32-mount> ", line, sizeof(line))) {
    if (!strcmp(line, "quit")) {
      break;
    }
    if (!strcmp(line, "state")) {
      show_state(mounted.control);
    } else if (!strcmp(line, "close")) {
      close_handle(&mounted.root);
    } else if (!strcmp(line, "retire")) {
      status = mount_retire(mounted.control);
      printf("RETIRE status %u\n", status);
    } else if (!strcmp(line, "open")) {
      if (mounted.root != HANDLE_INVALID) {
        puts("Close the held root before opening another.");
        continue;
      }
      struct mount_state_reply state;
      status = mount_observe(mounted.control, &state);
      if (status == CALL_OK && (state.flags & MOUNT_STATE_RETIRED)) {
        struct mount_fat32_reply next = {HANDLE_INVALID, HANDLE_INVALID};
        status = boot_selected ? mount_open_fat32(authority, ROOT_RIGHTS, &next) :
            disk_open_fat32(disk, partition, ROOT_RIGHTS, &next);
        if (status == CALL_OK) {
          close_handle(&mounted.control);
          mounted = next;
        }
      } else if (status == CALL_OK) {
        status = mount_reopen_fat32(mounted.control, ROOT_RIGHTS, &mounted.root);
      }
      printf("OPEN status %u\n", status);
    } else if (mounted.root == HANDLE_INVALID) {
      puts("No root held; use open.");
    } else if (!strncmp(line, "ls ", 3)) {
      run_reader(boot, launcher, mounted.root, "ls.pxe", line + 3);
    } else if (!strncmp(line, "cat ", 4)) {
      run_reader(boot, launcher, mounted.root, "cat.pxe", line + 4);
    } else if (!strncmp(line, "info ", 5)) {
      inspect_info(mounted.root, line + 5);
    } else if (!strncmp(line, "read ", 5)) {
      inspect_read(mounted.root, line + 5);
    } else {
      fputs("fat32 mount: unknown command\n", stderr);
    }
  }
  result = EXIT_SUCCESS;

done:
  bool closed = close_handle(&mounted.root);
  if (mounted.control != HANDLE_INVALID) {
    enum call_status retirement = mount_retire(mounted.control);
    if (retirement != CALL_OK) {
      fprintf(stderr, "fat32 mount: exit retirement failed (status %u); state retained\n",
          retirement);
      result = EXIT_FAILURE;
    }
  }
  closed = close_handle(&mounted.control) && closed;
  closed = close_handle(&disk) && closed;
  closed = close_handle(&authority) && closed;
  closed = close_handle(&boot) && closed;
  closed = close_handle(&launcher) && closed;
  return closed ? result : EXIT_FAILURE;
}

static int create_browser(bool boot_selected)
{
  enum { AUTHORITY, MEMORY, LAUNCHER, BOOT, SYSTEM, FACTORY, SOURCE_COUNT };
  handle_t sources[SOURCE_COUNT] = {
    startup_resource(boot_selected ? "esp_mount" : "disks"),
    startup_resource("memory"), startup_resource("launcher"),
    startup_root("boot"), startup_resource("system_info"), startup_resource("space_factory"),
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  uint64_t *cpus = NULL;
  int result = EXIT_FAILURE;
  for (unsigned i = 0; i < SOURCE_COUNT; ++i) {
    if (sources[i] == HANDLE_INVALID) {
      fputs("fat32 mount: required inspection authority unavailable; no fallback\n", stderr);
      goto done;
    }
  }
  enum call_status status = directory_lookup(sources[BOOT], "init-fat32-mount.pxe",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: image lookup failed (status %u)\n", status);
    goto done;
  }
  struct system_info_cpu cpu;
  status = system_info_get_cpu(sources[SYSTEM], &cpu);
  if (status != CALL_OK || !cpu.online_count || cpu.online_count > UINT64_MAX - 63 ||
      (cpu.online_count + 63) / 64 > SIZE_MAX / sizeof(*cpus)) {
    fprintf(stderr, "fat32 mount: CPU inventory unavailable (status %u)\n", status);
    goto done;
  }
  cpus = calloc((cpu.online_count + 63) / 64, sizeof(*cpus));
  if (!cpus) {
    fputs("fat32 mount: no memory for CPU selection\n", stderr);
    goto done;
  }
  for (uint64_t i = 0; i < cpu.online_count; ++i) {
    cpus[i / 64] |= UINT64_C(1) << (i % 64);
  }
  struct launch_grant grants[] = {
    {sources[AUTHORITY], boot_selected ? MOUNT_RIGHT_OPEN_ROOT :
        DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN, 0},
    {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    {sources[LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0},
    {sources[BOOT], DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, 0},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)(boot_selected ? "esp_mount" : "disks"), AUTHORITY},
    {(uintptr_t)"memory", MEMORY},
    {(uintptr_t)"launcher", LAUNCHER},
  };
  struct launch_binding roots[] = {{(uintptr_t)"boot", BOOT}};
  const char *arguments[] = {
    "boot://init-fat32-mount.pxe", boot_selected ? "--browse-boot" : "--browse-scratch",
  };
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants, .grant_count = 4,
    .resources = (uintptr_t)resources, .resource_count = 3,
    .roots = (uintptr_t)roots, .root_count = 1,
    .argv = (uintptr_t)arguments, .argc = 2,
  };
  struct space_definition space = {
    .name = "install", .title = boot_selected ? "Boot ESP (read-only)" : "Mounted FAT32 (read-only)",
  };
  status = space_create_started(sources[FACTORY], &space, cpus, cpu.online_count,
      &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: space launch failed (status %u)\n", status);
    goto done;
  }
  free(cpus);
  cpus = NULL;
  bool closed = close_handle(&image);
  /* Libc needs its startup memory grant through process retirement. */
  for (unsigned i = 0; i < SOURCE_COUNT; ++i) {
    if (i != MEMORY) {
      closed = close_handle(&sources[i]) && closed;
    }
  }
  result = wait_child(child);
  if (!closed) {
    result = EXIT_FAILURE;
  }

done:
  free(cpus);
  bool closed_all = close_handle(&image);
  closed_all = close_handle(&child) && closed_all;
  for (unsigned i = 0; i < SOURCE_COUNT; ++i) {
    if (i != MEMORY) {
      closed_all = close_handle(&sources[i]) && closed_all;
    }
  }
  return closed_all ? result : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--browse-scratch")) {
    return browse(false);
  }
  if (argc == 2 && !strcmp(argv[1], "--browse-boot")) {
    return browse(true);
  }
  if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--boot"))) {
    return create_browser(argc == 2);
  }
  fputs("init-fat32-mount: invalid bootstrap arguments\n", stderr);
  return EXIT_FAILURE;
}
