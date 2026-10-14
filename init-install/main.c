#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <abi/clock.h>
#include <abi/disk.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/power.h>
#include <abi/random.h>
#include <abi/system_info.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <space.h>
#include <system_info.h>

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

int main(int argc, char **argv)
{

  /* The kernel adds the install space's console, input devices and display. */
  enum { DISKS, KERNEL, ARCHIVE, MEMORY, CLOCK, RANDOM, SYSTEM_INFO, LOG, BOOT_ROOT, POWER,
    FACTORY, SOURCE_COUNT, GRANT_COUNT = FACTORY };
  const char *names[SOURCE_COUNT] = {"disks", "boot_kernel", "boot_archive", "memory",
    "clock", "random", "system_info", "log", "boot", "power", "space_factory"};
  handle_t sources[SOURCE_COUNT] = {
    bootstrap_capability("disks"), bootstrap_capability("boot_kernel"), bootstrap_capability("boot_archive"),
    bootstrap_capability("memory"), bootstrap_capability("clock"), bootstrap_capability("random"),
    bootstrap_capability("system_info"), bootstrap_capability("log"), bootstrap_capability("boot"),
    bootstrap_capability("power"), bootstrap_capability("space_factory"),
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  uint64_t *cpus = NULL;
  const char **arguments = NULL;
  int result = EXIT_FAILURE;
  for (unsigned i = 0; i < SOURCE_COUNT; i++) {
    /* Without power the installer simply offers no restart. */
    if (sources[i] == HANDLE_INVALID && i != POWER) {
      fprintf(stderr, "init-install: missing resource %s; use the Install Pyxis boot entry\n", names[i]);
      goto done;
    }
  }
  enum call_status status = directory_lookup(sources[BOOT_ROOT], "installer.pxe",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    if (status == CALL_NOT_FOUND) {
      fputs("init-install: boot://installer.pxe is not packaged; installation unavailable\n", stderr);
    } else {
      fprintf(stderr, "init-install: cannot open boot://installer.pxe (status %u)\n", status);
    }
    goto done;
  }
  struct system_info_cpu cpu;
  status = system_info_get_cpu(sources[SYSTEM_INFO], &cpu);
  cpus = status == CALL_OK && cpu.online_count ?
      calloc((cpu.online_count + 63) / 64, sizeof(*cpus)) : NULL;
  if (!cpus) {
    fprintf(stderr, "init-install: cannot read the CPU count (status %u)\n", status);
    goto done;
  }
  for (uint64_t i = 0; i < cpu.online_count; i++) {
    cpus[i / 64] |= UINT64_C(1) << (i % 64);
  }

  struct launch_grant grants[GRANT_COUNT] = {
    [DISKS] = {sources[DISKS], DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN, 0},
    [KERNEL] = {sources[KERNEL], FILE_RIGHT_READ, 0},
    [ARCHIVE] = {sources[ARCHIVE], FILE_RIGHT_READ, 0},
    [MEMORY] = {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    [CLOCK] = {sources[CLOCK], CLOCK_RIGHT_READ, 0},
    [RANDOM] = {sources[RANDOM], RANDOM_RIGHT_READ, 0},
    [SYSTEM_INFO] = {sources[SYSTEM_INFO], SYSTEM_INFO_RIGHT_READ, 0},
    [LOG] = {sources[LOG], LOG_RIGHT_READ, 0},
    [BOOT_ROOT] = {sources[BOOT_ROOT], DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES, 0},
    [POWER] = {sources[POWER], POWER_RIGHT_RESTART, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"disks", DISKS), PROFILE_CAP((uintptr_t)"boot_kernel", KERNEL),
    PROFILE_CAP((uintptr_t)"boot_archive", ARCHIVE), PROFILE_CAP((uintptr_t)"memory", MEMORY),
    PROFILE_CAP((uintptr_t)"clock", CLOCK), PROFILE_CAP((uintptr_t)"random", RANDOM),
    PROFILE_CAP((uintptr_t)"system_info", SYSTEM_INFO), PROFILE_CAP((uintptr_t)"log", LOG),
    PROFILE_CAP((uintptr_t)"power", POWER),
  };
  bool has_power = sources[POWER] != HANDLE_INVALID;
  struct launch_environment_entry roots[] = {PROFILE_CAP((uintptr_t)"boot", BOOT_ROOT)};
  arguments = argc > 0 ? calloc((size_t)argc, sizeof(*arguments)) : NULL;
  if (!arguments) {
    fputs("init-install: cannot prepare installer arguments\n", stderr);
    goto done;
  }
  arguments[0] = "boot://installer.pxe";
  for (int i = 1; i < argc; i++) {
    arguments[i] = argv[i];
  }
    struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (sizeof(resources) / sizeof(resources[0]) - !has_power) * sizeof(*entries));
  entry_count += sizeof(resources) / sizeof(resources[0]) - !has_power;
  memcpy(entries + entry_count, roots, (sizeof(roots) / sizeof(roots[0])) * sizeof(*entries));
  entry_count += sizeof(roots) / sizeof(roots[0]);
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = GRANT_COUNT - !has_power,
    .argv = (uintptr_t)arguments,
    .argc = (uint64_t)argc,
  };
  struct space_definition space = {.name = "install", .title = "Install"};
  status = space_create_started(sources[FACTORY], &space, cpus, cpu.online_count,
      &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot start boot://installer.pxe (status %u)\n", status);
    goto done;
  }
  /* Only the installer's space may be created; drop the factory before waiting. */
  if (handle_close(sources[FACTORY]) != 0) {
    goto done;
  }
  sources[FACTORY] = HANDLE_INVALID;
  struct process_result completion;
  status = process_wait(child, &completion);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot wait for installer (status %u)\n", status);
    goto done;
  }
  if (completion.kind == PROCESS_EXITED) {
    printf("init-install: installer exited with status %lld\n", (long long)completion.exit_status);
    result = completion.exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  } else {
    printf("init-install: installer stopped (completion kind %llu)\n",
        (unsigned long long)completion.kind);
  }

done:
  free(arguments);
  free(cpus);
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
