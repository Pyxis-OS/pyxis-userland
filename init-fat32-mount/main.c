#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
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
  READ_BYTES = FILE_READ_MAX_BYTES,
  COMPONENT_BYTES = 1024,
  PATH_HANDLES = 65,
  HELD_FILES = 8,
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
  struct path_context context = {.directories = &root, .count = 1};
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = PATH_HANDLES,
    .component = component, .component_capacity = sizeof(component),
  };
  *object = HANDLE_INVALID;
  if (!volume_path(path, uri, sizeof(uri))) {
    return CALL_BAD_REQUEST;
  }
  return path_resolve_native(&context, uri + sizeof("esp://") - 1, kind, rights, &workspace, object);
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
  if (!decimal(arguments, &offset) || !decimal(length_text, &length) || length > READ_BYTES ||
      length > UINT64_MAX - offset) {
    fprintf(stderr, "fat32 mount: offset/length invalid; length is at most %u\n", READ_BYTES);
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

static bool take_number(char **arguments, uint64_t *value)
{
  char *separator = strchr(*arguments, ' ');
  if (!separator) {
    return false;
  }
  *separator = 0;
  bool valid = decimal(*arguments, value);
  *arguments = separator + 1;
  return valid;
}

static enum call_status change_name(handle_t root, char *paths, uint64_t operation,
    uint64_t kind, uint64_t policy)
{
  char source[COMMAND_BYTES], destination[COMMAND_BYTES];
  char component[COMPONENT_BYTES], other_component[COMPONENT_BYTES];
  handle_t directories[PATH_HANDLES], other_directories[PATH_HANDLES];
  struct path_context context = {.directories = &root, .count = 1};
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = PATH_HANDLES,
    .component = component, .component_capacity = sizeof(component),
  };
  if (operation == DIRECTORY_RENAME) {
    char *second = strstr(paths, " | ");
    if (!second) {
      return CALL_BAD_REQUEST;
    }
    *second = 0;
    second += 3;
    if (!volume_path(second, destination, sizeof(destination))) {
      return CALL_BAD_REQUEST;
    }
  }
  if (!volume_path(paths, source, sizeof(source))) {
    return CALL_BAD_REQUEST;
  }
  if (operation == DIRECTORY_CREATE) {
    if (kind == DIRECTORY_KIND_DIRECTORY) {
      return path_create_directory(&context, source + sizeof("esp://") - 1, &workspace);
    }
    handle_t file = HANDLE_INVALID;
    enum call_status status = path_create_file(&context, source + sizeof("esp://") - 1, FILE_RIGHTS,
        &workspace, &file);
    if (!close_handle(&file) && status == CALL_OK) {
      return CALL_OUTCOME_UNKNOWN;
    }
    return status;
  }
  if (operation == DIRECTORY_REMOVE) {
    return path_remove(&context, source + sizeof("esp://") - 1, kind, &workspace);
  }
  struct path_workspace other_workspace = {
    .directories = other_directories, .directory_capacity = PATH_HANDLES,
    .component = other_component, .component_capacity = sizeof(other_component),
  };
  return path_rename(&context, source + sizeof("esp://") - 1, destination + sizeof("esp://") - 1, policy, &workspace, &other_workspace);
}

static bool held_command(handle_t root, handle_t *files, char *line)
{
  char *arguments = strchr(line, ' ');
  if (!arguments) {
    return false;
  }
  size_t command_bytes = arguments - line;
  bool hold = command_bytes == 4 && !strncmp(line, "hold", 4);
  bool read = command_bytes == 5 && !strncmp(line, "hread", 5);
  bool write = command_bytes == 6 && !strncmp(line, "hwrite", 6);
  bool resize = command_bytes == 6 && !strncmp(line, "resize", 6);
  bool info = command_bytes == 5 && !strncmp(line, "hinfo", 5);
  bool drop = command_bytes == 4 && !strncmp(line, "drop", 4);
  bool sync = command_bytes == 5 && !strncmp(line, "hsync", 5);
  if (!hold && !read && !write && !resize && !info && !drop && !sync) {
    return false;
  }
  ++arguments;
  uint64_t slot;
  bool valid = info || drop || sync ? decimal(arguments, &slot) :
      take_number(&arguments, &slot);
  if (!valid || slot < 1 || slot > HELD_FILES) {
    fputs("fat32 mount: invalid slot or arguments; slots are 1–8\n", stderr);
    return true;
  }
  handle_t *file = &files[slot - 1];
  enum call_status status = CALL_BAD_REQUEST;
  if (hold) {
    char *path = strchr(arguments, ' ');
    if (!path || *file != HANDLE_INVALID || root == HANDLE_INVALID) {
      fputs("fat32 mount: use hold SLOT ro|rw /PATH on an empty slot with root held\n", stderr);
      return true;
    }
    *path++ = 0;
    uint64_t rights = !strcmp(arguments, "ro") ? FILE_RIGHT_READ :
        !strcmp(arguments, "rw") ? FILE_RIGHTS : 0;
    status = rights ? lookup(root, path, DIRECTORY_KIND_FILE, rights, file) : CALL_BAD_REQUEST;
  } else if (*file == HANDLE_INVALID) {
    fputs("fat32 mount: slot is empty\n", stderr);
    return true;
  } else if (drop) {
    printf("DROP %llu %s\n", (unsigned long long)slot, close_handle(file) ? "OK" : "failed");
    return true;
  } else if (info) {
    struct file_info_reply metadata;
    status = file_info(*file, &metadata);
    if (status == CALL_OK) {
      print_info("held FILE", &metadata);
    }
  } else if (sync) {
    status = file_sync(*file);
  } else if (resize) {
    uint64_t size;
    status = decimal(arguments, &size) && size <= UINT32_MAX ?
        file_resize(*file, size) : CALL_BAD_REQUEST;
  } else {
    uint64_t offset;
    if (!take_number(&arguments, &offset)) {
      fputs("fat32 mount: invalid offset\n", stderr);
      return true;
    }
    size_t count = 0;
    if (read) {
      uint64_t length;
      unsigned char bytes[FILE_READ_MAX_BYTES];
      if (decimal(arguments, &length) && length <= sizeof(bytes) &&
          length <= UINT64_MAX - offset) {
        status = file_read(*file, offset, bytes, (size_t)length, &count);
        if (status == CALL_OK) {
          fwrite(bytes, 1, count, stdout);
          printf("\nRead %zu bytes.\n", count);
        }
      }
    } else if (!strncmp(arguments, "= ", 2)) {
      const char *payload = arguments + 2;
      size_t length = strlen(payload);
      if (length <= FILE_WRITE_MAX_BYTES && offset <= UINT32_MAX &&
          length <= UINT32_MAX - offset) {
        status = file_write(*file, offset, payload, length, &count);
        if (status == CALL_OK) {
          printf("Wrote %zu of %zu bytes.\n", count, length);
        }
      }
    }
  }
  printf("FILE slot %llu status %u\n", (unsigned long long)slot, status);
  return true;
}

static void run_reader(handle_t boot, handle_t launcher, handle_t root,
    const char *program, const char *path)
{
  bool shell = !strcmp(program, "init-shell.pxe");
  char uri[COMMAND_BYTES];
  if (!shell && !volume_path(path, uri, sizeof(uri))) {
    return;
  }
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  enum call_status status = directory_lookup(boot, program, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: reader lookup failed (status %u)\n", status);
    return;
  }
  enum { ROOT, MEMORY, INPUT, OUTPUT, ERROR, READER_GRANTS, BOOT = READER_GRANTS,
    TMP, LAUNCHER, TERM_INPUT, TERM_OUTPUT, SHELL_GRANTS };
  struct pyxis_stdio_binding streams[] = {
    bootstrap_stdio(ENVIRONMENT_STDIN), bootstrap_stdio(ENVIRONMENT_STDOUT), bootstrap_stdio(ENVIRONMENT_STDERR),
  };
  struct launch_grant grants[] = {
    {root, ROOT_RIGHTS, 0}, {bootstrap_capability("memory"), MEMORY_RIGHT_MANAGE, 0},
    {streams[0].handle, CONSOLE_RIGHT_READ, 0},
    {streams[1].handle, CONSOLE_RIGHT_WRITE, 0},
    {streams[2].handle, CONSOLE_RIGHT_WRITE, 0},
    {boot, ROOT_RIGHTS, 0}, {bootstrap_capability("tmp"), ROOT_RIGHTS, 0},
    {launcher, LAUNCHER_RIGHT_LAUNCH, 0},
    {streams[0].handle, CONSOLE_RIGHT_READ, 0},
    {streams[1].handle, CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"memory", MEMORY), PROFILE_CAP((uintptr_t)"input", TERM_INPUT),
    PROFILE_CAP((uintptr_t)"output", TERM_OUTPUT), PROFILE_CAP((uintptr_t)"launcher", LAUNCHER),
  };
  struct launch_environment_entry roots[] = {
    PROFILE_CAP((uintptr_t)"esp", ROOT), PROFILE_CAP((uintptr_t)"boot", BOOT), PROFILE_CAP((uintptr_t)"tmp", TMP),
  };
  const char *arguments[] = {program, "--", uri};
  if (!strcmp(program, "cat.pxe")) {
    arguments[1] = uri;
  }
  uint64_t working_directory = ROOT;
    struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (shell ? 4 : 1) * sizeof(*entries));
  entry_count += shell ? 4 : 1;
  memcpy(entries + entry_count, roots, (shell ? 3 : 1) * sizeof(*entries));
  entry_count += shell ? 3 : 1;
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDIN, INPUT);
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDOUT, OUTPUT);
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDERR, ERROR);
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = shell ? SHELL_GRANTS : READER_GRANTS,
    .argv = (uintptr_t)arguments,
    .argc = shell ? 1 : !strcmp(program, "cat.pxe") ? 2 : 3,
    .working_directories = shell ? (uintptr_t)&working_directory : 0,
    .working_directory_count = shell ? 1 : 0,
    .working_path = shell ? (uintptr_t)"esp://" : 0,
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
  if ((state.flags & MOUNT_STATE_READ_WRITE) &&
      (state.flags & MOUNT_STATE_ALLOCATION_INVALID)) {
    puts("allocation proof invalid");
  }
  printf("interval=%llu deadline_ns=%llu detached=%llu failure=%llu phase=%llu sector=%llu\n",
      (unsigned long long)state.interval_id, (unsigned long long)state.interval_deadline_ns,
      (unsigned long long)state.detached_objects, (unsigned long long)state.failure_status,
      (unsigned long long)state.failure_phase, (unsigned long long)state.failed_sector);
  if (state.flags & MOUNT_STATE_FAILED) {
    printf("source=%.*s destination=%.*s\n", (int)sizeof(state.source_name), state.source_name,
        (int)sizeof(state.destination_name), state.destination_name);
  }
}

static int browse(bool boot_selected, bool writable, uint64_t selected_partition)
{
  handle_t boot = bootstrap_capability("boot"), launcher = bootstrap_capability("launcher");
  handle_t authority = writable ? HANDLE_INVALID :
      bootstrap_capability(boot_selected ? "esp_mount" : "disks");
  handle_t disk = writable ? bootstrap_capability("disk") : HANDLE_INVALID;
  struct mount_fat32_reply mounted = {HANDLE_INVALID, HANDLE_INVALID};
  struct pyxis_stdio_binding input = bootstrap_stdio(ENVIRONMENT_STDIN);
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  struct pyxis_stdio_binding error = bootstrap_stdio(ENVIRONMENT_STDERR);
  struct terminal terminal = {.input = input.handle, .output = output.handle};
  uint64_t partition = selected_partition;
  uint64_t root_rights = writable ? DIRECTORY_CONTENT_RIGHTS : ROOT_RIGHTS;
  uint64_t access = writable ? MOUNT_ACCESS_READ_WRITE : MOUNT_ACCESS_READ_ONLY;
  handle_t files[HELD_FILES] = {0};
  struct mount_interval_reply interval = {0};
  int result = EXIT_FAILURE;
  if (boot == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      (writable ? disk == HANDLE_INVALID : authority == HANDLE_INVALID) ||
      bootstrap_capability("memory") == HANDLE_INVALID || input.protocol != PROTOCOL_CONSOLE ||
      output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("fat32 mount: required trusted authority or console unavailable\n", stderr);
    goto done;
  }
  puts(writable ? "Native FAT32 mounted writes on disposable scratch media." :
      "Native FAT32 read-only inspection. Keep media quiescent; no external writers.");
  if (writable) {
    uint64_t rights, transport;
    enum call_status status = handle_rights(disk, &rights, &transport);
    if (status != CALL_OK || rights != DISK_RIGHTS || transport || !partition ||
        partition > UINT32_MAX) {
      fputs("fat32 mount: selected disk authority or GPT entry invalid\n", stderr);
      goto done;
    }
  } else if (!boot_selected) {
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
      mount_open_fat32(authority, root_rights, &mounted) :
      disk_open_fat32(disk, partition, access, root_rights, &mounted);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: admission failed (status %u); no fallback\n", status);
    goto done;
  }
  show_state(mounted.control);
  puts("ls /PATH; cat /PATH; info /PATH [ | /PATH]; read OFFSET LENGTH /PATH");
  puts("state; close; open; retire; quit. File bytes may contain terminal controls.");
  if (writable) {
    puts("create /PATH; mkdir /PATH; remove /PATH; rmdir /PATH");
    puts("rename /FROM | /TO; replace /FROM | /TO");
    puts("hold SLOT ro|rw /PATH; hread SLOT OFFSET LENGTH; hwrite SLOT OFFSET = LITERAL");
    puts("resize SLOT SIZE; hinfo SLOT; hsync SLOT; drop SLOT (slots 1–8)");
    puts("sync; begin; check; end ID; shell (read-only roots). No automatic mutation retry.");
  }
  char line[COMMAND_BYTES];
  while (prompt(&terminal, "fat32-mount> ", line, sizeof(line))) {
    if (!strcmp(line, "quit")) {
      break;
    }
    if (!strcmp(line, "state")) {
      show_state(mounted.control);
    } else if (writable && !strcmp(line, "sync")) {
      printf("SYNC status %u\n", mount_sync(mounted.control));
    } else if (writable && !strcmp(line, "check")) {
      printf("CHECK status %u\n", mount_check(mounted.control));
    } else if (writable && !strcmp(line, "begin")) {
      status = mount_begin(mounted.control, &interval);
      printf("BEGIN status %u", status);
      if (status == CALL_OK) {
        printf(" id=%llu deadline_ns=%llu", (unsigned long long)interval.id,
            (unsigned long long)interval.deadline_ns);
      }
      putchar('\n');
    } else if (writable && !strncmp(line, "end ", 4)) {
      uint64_t id;
      status = decimal(line + 4, &id) ? mount_end(mounted.control, id) : CALL_BAD_REQUEST;
      printf("END status %u\n", status);
      if (status == CALL_OK && id == interval.id) {
        interval = (struct mount_interval_reply){0};
      }
    } else if (writable && held_command(mounted.root, files, line)) {
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
        status = boot_selected ? mount_open_fat32(authority, root_rights, &next) :
            disk_open_fat32(disk, partition, access, root_rights, &next);
        if (status == CALL_OK) {
          close_handle(&mounted.control);
          mounted = next;
        }
      } else if (status == CALL_OK) {
        status = mount_reopen_fat32(mounted.control, root_rights, &mounted.root);
      }
      printf("OPEN status %u\n", status);
    } else if (mounted.root == HANDLE_INVALID) {
      puts("No root held; use open.");
    } else if (writable && !strncmp(line, "create ", 7)) {
      printf("CREATE status %u\n", change_name(mounted.root, line + 7, DIRECTORY_CREATE, DIRECTORY_KIND_FILE, 0));
    } else if (writable && !strncmp(line, "mkdir ", 6)) {
      printf("MKDIR status %u\n", change_name(mounted.root, line + 6, DIRECTORY_CREATE,
          DIRECTORY_KIND_DIRECTORY, 0));
    } else if (writable && !strncmp(line, "rmdir ", 6)) {
      printf("RMDIR status %u\n", change_name(mounted.root, line + 6, DIRECTORY_REMOVE,
          DIRECTORY_KIND_DIRECTORY, 0));
    } else if (writable && !strncmp(line, "remove ", 7)) {
      printf("REMOVE status %u\n", change_name(mounted.root, line + 7, DIRECTORY_REMOVE, DIRECTORY_KIND_FILE, 0));
    } else if (writable && (!strncmp(line, "rename ", 7) || !strncmp(line, "replace ", 8))) {
      bool replace = line[2] == 'p';
      printf("RENAME status %u\n", change_name(mounted.root, line + (replace ? 8 : 7),
          DIRECTORY_RENAME, DIRECTORY_KIND_FILE, replace ? DIRECTORY_RENAME_REPLACE : DIRECTORY_RENAME_NO_REPLACE));
    } else if (writable && !strcmp(line, "shell")) {
      run_reader(boot, launcher, mounted.root, "init-shell.pxe", NULL);
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
  bool closed = true;
  for (unsigned i = 0; i < HELD_FILES; ++i) {
    closed = close_handle(&files[i]) && closed;
  }
  if (interval.id && mounted.control != HANDLE_INVALID) {
    enum call_status ended = mount_end(mounted.control, interval.id);
    if (ended != CALL_OK) {
      fprintf(stderr, "fat32 mount: exit END failed (status %u)\n", ended);
      result = EXIT_FAILURE;
    }
  }
  closed = close_handle(&mounted.root) && closed;
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

static int select_scratch_write(void)
{
  handle_t disks = bootstrap_capability("disks"), boot = bootstrap_capability("boot");
  handle_t launcher = bootstrap_capability("launcher"), memory = bootstrap_capability("memory");
  handle_t opened = HANDLE_INVALID, selected = HANDLE_INVALID;
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  struct pyxis_stdio_binding input = bootstrap_stdio(ENVIRONMENT_STDIN);
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  struct pyxis_stdio_binding error = bootstrap_stdio(ENVIRONMENT_STDERR);
  struct terminal terminal = {.input = input.handle, .output = output.handle};
  int result = EXIT_FAILURE;
  enum call_status status = CALL_OK;
  if (disks == HANDLE_INVALID || boot == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      memory == HANDLE_INVALID || input.protocol != PROTOCOL_CONSOLE ||
      output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("fat32 mount: missing trusted boot authority or console streams\n", stderr);
    goto done;
  }
  puts("Trusted FAT32 mounted writer. Select disposable media; commands change its contents.");
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    status = disks_enumerate(disks, index, &info);
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
  char line[32];
  uint64_t id, partition;
  if (!prompt(&terminal, "Explicit disk ID: ", line, sizeof(line)) || !decimal(line, &id) || !id ||
      !prompt(&terminal, "One-based GPT entry: ", line, sizeof(line)) ||
      !decimal(line, &partition) || !partition || partition > UINT32_MAX) {
    fputs("fat32 mount: invalid disk ID or GPT entry\n", stderr);
    goto done;
  }
  puts("Use only disposable scratch VirtIO media for this qualification.");
  if (fflush(stdout) != 0) {
    fputs("fat32 mount: output failed\n", stderr);
    goto done;
  }
  status = disks_open(disks, id, DISK_ACCESS_READ_ONLY, &opened);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: read-only open failed (status %u)\n", status);
    goto done;
  }
  struct disk_info selected_info;
  status = disk_get_info(opened, &selected_info);
  uint64_t required_flags = DISK_FLAG_WRITABLE | DISK_FLAG_FLUSH_SUPPORTED;
  if (status != CALL_OK || selected_info.preparation != DISK_READY ||
      (selected_info.flags & required_flags) != required_flags ||
      (selected_info.flags & DISK_FLAG_WRITE_FAILED) ||
      (selected_info.gpt_status != DISK_GPT_HEALTHY &&
          selected_info.gpt_status != DISK_GPT_DEGRADED)) {
    fprintf(stderr, "fat32 mount: selected disk is not eligible (status %u)\n", status);
    goto done;
  }
  struct disk_partition_selection facts;
  status = disk_partition_info(opened, partition, &facts);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: selected GPT inspection failed (status %u)\n", status);
    goto done;
  }
  if (!close_handle(&opened)) {
    goto done;
  }
  status = disks_open_partition(disks, &facts, &opened);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: selected GPT claim failed (status %u)\n", status);
    goto done;
  }
  status = disk_release(opened);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: selected GPT release failed (status %u)\n", status);
    goto done;
  }
  status = handle_copy_restricted(opened, DISK_RIGHTS, 0, &selected);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: disk attenuation failed (status %u)\n", status);
    goto done;
  }
  if (!close_handle(&opened)) {
    goto done;
  }
  status = directory_lookup(boot, "init-fat32-mount.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: cannot open boot://init-fat32-mount.pxe (status %u)\n", status);
    goto done;
  }
  if (!close_handle(&disks)) {
    goto done;
  }
  char entry[16];
  snprintf(entry, sizeof(entry), "%llu", (unsigned long long)partition);
  const char *arguments[] = {"boot://init-fat32-mount.pxe", "--browse-scratch-write", entry};
  enum { DISK, MEMORY, INPUT, OUTPUT, ERROR, BOOT, TMP, LAUNCHER, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [DISK] = {selected, DISK_RIGHTS, 0},
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [INPUT] = {input.handle, CONSOLE_RIGHT_READ, 0},
    [OUTPUT] = {output.handle, CONSOLE_RIGHT_WRITE, 0},
    [ERROR] = {error.handle, CONSOLE_RIGHT_WRITE, 0},
    [BOOT] = {boot, ROOT_RIGHTS, 0},
    [TMP] = {bootstrap_capability("tmp"), ROOT_RIGHTS, 0},
    [LAUNCHER] = {launcher, LAUNCHER_RIGHT_LAUNCH, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"disk", DISK), PROFILE_CAP((uintptr_t)"memory", MEMORY),
    PROFILE_CAP((uintptr_t)"launcher", LAUNCHER),
  };
  struct launch_environment_entry roots[] = {PROFILE_CAP((uintptr_t)"boot", BOOT), PROFILE_CAP((uintptr_t)"tmp", TMP)};
    struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (3) * sizeof(*entries));
  entry_count += 3;
  memcpy(entries + entry_count, roots, (2) * sizeof(*entries));
  entry_count += 2;
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDIN, INPUT);
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDOUT, OUTPUT);
  entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDERR, ERROR);
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = GRANT_COUNT,
    .argv = (uintptr_t)arguments,
    .argc = 3,
  };
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 mount: launch failed (status %u)\n", status);
    goto done;
  }
  /* Browser owns only this selected disk; no inventory or other-device authority. */
  bool closed_sources = close_handle(&selected);
  closed_sources = close_handle(&image) && closed_sources;
  closed_sources = close_handle(&launcher) && closed_sources;
  result = wait_child(child);
  if (!closed_sources) {
    result = EXIT_FAILURE;
  }

done:
  bool closed = close_handle(&opened);
  closed = close_handle(&selected) && closed;
  closed = close_handle(&image) && closed;
  closed = close_handle(&child) && closed;
  closed = close_handle(&disks) && closed;
  closed = close_handle(&boot) && closed;
  closed = close_handle(&launcher) && closed;
  return closed ? result : EXIT_FAILURE;
}

static int create_browser(bool boot_selected, bool writable)
{
  enum { AUTHORITY, MEMORY, LAUNCHER, BOOT, SYSTEM, FACTORY, TMP, SOURCE_COUNT };
  handle_t sources[SOURCE_COUNT] = {
    bootstrap_capability(boot_selected ? "esp_mount" : "disks"),
    bootstrap_capability("memory"), bootstrap_capability("launcher"),
    bootstrap_capability("boot"), bootstrap_capability("system_info"), bootstrap_capability("space_factory"),
    writable ? bootstrap_capability("tmp") : HANDLE_INVALID,
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  uint64_t *cpus = NULL;
  int result = EXIT_FAILURE;
  for (unsigned i = 0; i < SOURCE_COUNT; ++i) {
    if (sources[i] == HANDLE_INVALID && (i != TMP || writable)) {
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
  enum { GRANT_AUTHORITY, GRANT_MEMORY, GRANT_LAUNCHER, GRANT_BOOT, GRANT_TMP, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [GRANT_AUTHORITY] = {sources[AUTHORITY], boot_selected ? MOUNT_RIGHT_OPEN_ROOT :
        DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN_READ_ONLY |
            (writable ? DISKS_RIGHT_OPEN_PARTITION : 0), 0},
    [GRANT_MEMORY] = {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    [GRANT_LAUNCHER] = {sources[LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0},
    [GRANT_BOOT] = {sources[BOOT], writable ? ROOT_RIGHTS :
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, 0},
    [GRANT_TMP] = {sources[TMP], ROOT_RIGHTS, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)(boot_selected ? "esp_mount" : "disks"), GRANT_AUTHORITY),
    PROFILE_CAP((uintptr_t)"memory", GRANT_MEMORY),
    PROFILE_CAP((uintptr_t)"launcher", GRANT_LAUNCHER),
  };
  struct launch_environment_entry roots[] = {
    PROFILE_CAP((uintptr_t)"boot", GRANT_BOOT), PROFILE_CAP((uintptr_t)"tmp", GRANT_TMP),
  };
  const char *arguments[] = {
    "boot://init-fat32-mount.pxe", writable ? "--select-scratch-write" :
        boot_selected ? "--browse-boot" : "--browse-scratch",
  };
    struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (3) * sizeof(*entries));
  entry_count += 3;
  memcpy(entries + entry_count, roots, (writable ? 2 : 1) * sizeof(*entries));
  entry_count += writable ? 2 : 1;
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = writable ? GRANT_COUNT : GRANT_TMP,
    .argv = (uintptr_t)arguments,
    .argc = 2,
  };
  struct space_definition space = {
    .name = "install", .title = writable ? "Mounted FAT32 Scratch Writer" :
        boot_selected ? "Boot ESP (read-only)" : "Mounted FAT32 (read-only)",
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
    return browse(false, false, 0);
  }
  if (argc == 2 && !strcmp(argv[1], "--browse-boot")) {
    return browse(true, false, 0);
  }
  if (argc == 2 && !strcmp(argv[1], "--select-scratch-write")) {
    return select_scratch_write();
  }
  if (argc == 3 && !strcmp(argv[1], "--browse-scratch-write")) {
    uint64_t partition;
    if (!decimal(argv[2], &partition) || !partition || partition > UINT32_MAX) {
      fputs("init-fat32-mount: invalid selected GPT entry\n", stderr);
      return EXIT_FAILURE;
    }
    return browse(false, true, partition);
  }
  bool boot_selected = bootstrap_capability("disks") == HANDLE_INVALID;
  bool writable = !strcmp(argv[0], "boot://init-fat32-mount-write.pxe") ||
      !strcmp(argv[0], "init-fat32-mount-write.pxe");
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--boot")) {
      boot_selected = true;
    } else if (!strcmp(argv[i], "--scratch-write")) {
      writable = true;
    } else if (strcmp(argv[i], "--installed") && strcmp(argv[i], "--default-config")) {
      fputs("init-fat32-mount: invalid bootstrap arguments\n", stderr);
      return EXIT_FAILURE;
    }
  }
  if (writable && boot_selected) {
    fputs("init-fat32-mount: scratch writes need install-mode DISKS; no boot-ESP fallback\n", stderr);
    return EXIT_FAILURE;
  }
  return create_browser(boot_selected, writable);
}
