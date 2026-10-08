#include <abi/clock.h>
#include <abi/disk.h>
#include <abi/file.h>
#include <abi/log.h>
#include <abi/memory.h>
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
  (void)argc;
  (void)argv;
  /* The kernel adds the install space's console, input devices and display. */
  enum { DISKS, KERNEL, ARCHIVE, MEMORY, CLOCK, RANDOM, SYSTEM_INFO, LOG, BOOT_ROOT, FACTORY,
    SOURCE_COUNT, GRANT_COUNT = FACTORY };
  const char *names[SOURCE_COUNT] = {"disks", "boot_kernel", "boot_archive", "memory",
    "clock", "random", "system_info", "log", "boot", "space_factory"};
  handle_t sources[SOURCE_COUNT] = {
    startup_resource("disks"), startup_resource("boot_kernel"), startup_resource("boot_archive"),
    startup_resource("memory"), startup_resource("clock"), startup_resource("random"),
    startup_resource("system_info"), startup_resource("log"), startup_root("boot"),
    startup_resource("space_factory"),
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  uint64_t *cpus = NULL;
  int result = EXIT_FAILURE;
  for (unsigned i = 0; i < SOURCE_COUNT; i++) {
    if (sources[i] == HANDLE_INVALID) {
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
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"disks", DISKS}, {(uintptr_t)"boot_kernel", KERNEL},
    {(uintptr_t)"boot_archive", ARCHIVE}, {(uintptr_t)"memory", MEMORY},
    {(uintptr_t)"clock", CLOCK}, {(uintptr_t)"random", RANDOM},
    {(uintptr_t)"system_info", SYSTEM_INFO}, {(uintptr_t)"log", LOG},
  };
  struct launch_binding roots[] = {{(uintptr_t)"boot", BOOT_ROOT}};
  const char *arguments[] = {"boot://installer.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources, .resource_count = sizeof(resources) / sizeof(resources[0]),
    .roots = (uintptr_t)roots, .root_count = sizeof(roots) / sizeof(roots[0]),
    .argv = (uintptr_t)arguments, .argc = 1,
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
