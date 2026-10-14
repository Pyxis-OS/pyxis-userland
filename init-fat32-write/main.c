#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <abi/console.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <directory.h>
#include <disk.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <space.h>
#include <startup.h>
#include <system_info.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#define WRITER_DISK_RIGHTS (DISK_RIGHT_INFO | DISK_RIGHT_READ | DISK_RIGHT_WRITE | DISK_RIGHT_RELEASE)

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool success = handle_close(*handle) == 0;
  if (!success) {
    fputs("fat32 bootstrap: handle close failed\n", stderr);
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
    fputs("fat32 selector: output failed\n", stderr);
    return false;
  }
  struct term_line_result result = term_read_line(terminal, label, line, bytes);
  if (result.status != TERM_LINE_OK || result.limit_reached) {
    fputs("fat32 selector: selection cancelled or input refused\n", stderr);
    return false;
  }
  return true;
}

static int wait_child(handle_t child)
{
  struct process_result completion;
  enum call_status status = process_wait(child, &completion);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 bootstrap: wait failed (status %u)\n", status);
    return EXIT_FAILURE;
  }
  if (completion.kind == PROCESS_EXITED) {
    if (completion.exit_status == 0) {
      return EXIT_SUCCESS;
    }
    fprintf(stderr, "fat32 bootstrap: child exited with status %lld\n",
        (long long)completion.exit_status);
  } else {
    fprintf(stderr, "fat32 bootstrap: child stopped (completion kind %llu)\n",
        (unsigned long long)completion.kind);
  }
  return EXIT_FAILURE;
}

static int select_disk(void)
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
  bool claimed = false;
  enum call_status status = CALL_OK;
  if (disks == HANDLE_INVALID || boot == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      memory == HANDLE_INVALID || input.protocol != PROTOCOL_CONSOLE ||
      output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("fat32 selector: missing trusted boot authority or console streams\n", stderr);
    goto done;
  }
  puts("Trusted FAT32 scratch writer. Select disposable media; commands change its contents.");
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      break;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "fat32 selector: inventory failed (status %u)\n", status);
      goto done;
    }
    printf("Disk ID %llu: %llu blocks of %llu bytes, preparation %llu, GPT %llu\n",
        (unsigned long long)info.id, (unsigned long long)info.block_count,
        (unsigned long long)info.block_size, (unsigned long long)info.preparation,
        (unsigned long long)info.gpt_status);
  }
  char line[32];
  uint64_t id, partition;
  if (!prompt(&terminal, "Explicit disk ID: ", line, sizeof(line)) || !decimal(line, &id) ||
      !prompt(&terminal, "One-based GPT entry: ", line, sizeof(line)) ||
      !decimal(line, &partition) || !partition || partition > UINT32_MAX) {
    fputs("fat32 selector: invalid disk ID or GPT entry\n", stderr);
    goto done;
  }
  puts("Only disposable VirtIO or qualified USB media is supported. Do not select an installed ESP.");
  if (fflush(stdout) != 0) {
    fputs("fat32 selector: output failed\n", stderr);
    goto done;
  }
  status = disks_open(disks, id, DISK_ACCESS_READ_WRITE, &opened);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 selector: read-write open failed (status %u)\n", status);
    goto done;
  }
  claimed = true;
  status = disk_release(opened);
  claimed = false;
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 selector: whole-disk release failed (status %u); claim ended\n", status);
    goto done;
  }
  status = disk_claim(opened, partition);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 selector: selected GPT claim failed (status %u)\n", status);
    goto done;
  }
  claimed = true;
  status = handle_copy_restricted(opened, WRITER_DISK_RIGHTS, 0, &selected);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 selector: disk attenuation failed (status %u)\n", status);
    goto done;
  }
  if (!close_handle(&opened)) {
    goto done;
  }
  status = directory_lookup(boot, "fat32-write.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 selector: cannot open boot://fat32-write.pxe (status %u)\n", status);
    goto done;
  }
  if (!close_handle(&disks) || !close_handle(&boot)) {
    goto done;
  }
  char entry[16];
  snprintf(entry, sizeof(entry), "%llu", (unsigned long long)partition);
  const char *arguments[] = {"boot://fat32-write.pxe", entry};
  enum { DISK, MEMORY, INPUT, OUTPUT, ERROR, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [DISK] = {selected, WRITER_DISK_RIGHTS, 0},
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [INPUT] = {input.handle, CONSOLE_RIGHT_READ, 0},
    [OUTPUT] = {output.handle, CONSOLE_RIGHT_WRITE, 0},
    [ERROR] = {error.handle, CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"disk", DISK), PROFILE_CAP((uintptr_t)"memory", MEMORY),
  };
    struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (2) * sizeof(*entries));
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
    .argc = 2,
  };
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 selector: launch failed (status %u)\n", status);
    goto done;
  }
  /* The launched writer owns explicit release; its last close also cleans up a crash. */
  claimed = false;
  bool closed_sources = close_handle(&selected);
  closed_sources = close_handle(&image) && closed_sources;
  closed_sources = close_handle(&launcher) && closed_sources;
  result = wait_child(child);
  if (!closed_sources) {
    result = EXIT_FAILURE;
  }

done:
  if (claimed) {
    handle_t owner = selected != HANDLE_INVALID ? selected : opened;
    status = disk_release(owner);
    claimed = false;
    if (status != CALL_OK) {
      fprintf(stderr, "fat32 selector: cleanup release failed (status %u); claim ended\n", status);
      result = EXIT_FAILURE;
    }
  }
  bool closed = close_handle(&opened);
  closed = close_handle(&selected) && closed;
  closed = close_handle(&image) && closed;
  closed = close_handle(&child) && closed;
  closed = close_handle(&disks) && closed;
  closed = close_handle(&boot) && closed;
  closed = close_handle(&launcher) && closed;
  return closed ? result : EXIT_FAILURE;
}

static int create_selector(void)
{
  enum { DISKS, MEMORY, LAUNCHER, BOOT, SYSTEM, FACTORY, SOURCE_COUNT };
  handle_t sources[SOURCE_COUNT] = {
    bootstrap_capability("disks"), bootstrap_capability("memory"), bootstrap_capability("launcher"),
    bootstrap_capability("boot"), bootstrap_capability("system_info"), bootstrap_capability("space_factory"),
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  uint64_t *cpus = NULL;
  int result = EXIT_FAILURE;
  for (unsigned i = 0; i < SOURCE_COUNT; ++i) {
    if (sources[i] == HANDLE_INVALID) {
      fputs("fat32 bootstrap: use the trusted Write scratch FAT32 boot entry\n", stderr);
      goto done;
    }
  }
  enum call_status status = directory_lookup(sources[BOOT], "init-fat32-write.pxe",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 bootstrap: image lookup failed (status %u)\n", status);
    goto done;
  }
  struct system_info_cpu cpu;
  status = system_info_get_cpu(sources[SYSTEM], &cpu);
  if (status != CALL_OK || !cpu.online_count || cpu.online_count > UINT64_MAX - 63 ||
      (cpu.online_count + 63) / 64 > SIZE_MAX / sizeof(*cpus)) {
    fprintf(stderr, "fat32 bootstrap: CPU inventory unavailable (status %u)\n", status);
    goto done;
  }
  cpus = calloc((cpu.online_count + 63) / 64, sizeof(*cpus));
  if (!cpus) {
    fputs("fat32 bootstrap: no memory for CPU selection\n", stderr);
    goto done;
  }
  for (uint64_t i = 0; i < cpu.online_count; ++i) {
    cpus[i / 64] |= UINT64_C(1) << (i % 64);
  }
  struct launch_grant grants[] = {
    {sources[DISKS], DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN, 0},
    {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    {sources[LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0},
    {sources[BOOT], DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"disks", DISKS), PROFILE_CAP((uintptr_t)"memory", MEMORY),
    PROFILE_CAP((uintptr_t)"launcher", LAUNCHER),
  };
  struct launch_environment_entry roots[] = {PROFILE_CAP((uintptr_t)"boot", BOOT)};
  const char *arguments[] = {"boot://init-fat32-write.pxe", "--select"};
    struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (3) * sizeof(*entries));
  entry_count += 3;
  memcpy(entries + entry_count, roots, (1) * sizeof(*entries));
  entry_count += 1;
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = 4,
    .argv = (uintptr_t)arguments,
    .argc = 2,
  };
  struct space_definition space = {.name = "install", .title = "FAT32 Scratch Writer"};
  status = space_create_started(sources[FACTORY], &space, cpus, cpu.online_count,
      &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "fat32 bootstrap: space launch failed (status %u)\n", status);
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
  if (argc == 2 && strcmp(argv[1], "--select") == 0) {
    return select_disk();
  }
  if (argc != 1) {
    fputs("init-fat32-write: invalid bootstrap arguments\n", stderr);
    return EXIT_FAILURE;
  }
  return create_selector();
}
