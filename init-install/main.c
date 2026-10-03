#include <abi/clock.h>
#include <abi/console.h>
#include <abi/disk.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/random.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>

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
  enum { DISKS, KERNEL, ARCHIVE, MEMORY, INPUT, OUTPUT, CLOCK, RANDOM, APP,
    LAUNCHER, SOURCE_COUNT, FIRST_STREAM = APP + 1,
    GRANT_COUNT = FIRST_STREAM + STARTUP_STREAM_COUNT };
  const char *names[SOURCE_COUNT] = {"disks", "boot_kernel", "boot_archive", "memory",
    "input", "output", "clock", "random", "app", "launcher"};
  handle_t sources[SOURCE_COUNT] = {
    startup_resource("disks"), startup_resource("boot_kernel"), startup_resource("boot_archive"),
    startup_resource("memory"), startup_resource("input"), startup_resource("output"),
    startup_resource("clock"), startup_resource("random"), startup_root("app"),
    startup_resource("launcher"),
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  int result = EXIT_FAILURE;
  for (unsigned i = 0; i < SOURCE_COUNT; i++) {
    if (sources[i] == HANDLE_INVALID) {
      fprintf(stderr, "init-install: missing resource %s; use the Install Pyxis boot entry\n", names[i]);
      goto done;
    }
  }
  enum call_status status = directory_lookup(sources[APP], "installer.pxe",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    if (status == CALL_NOT_FOUND) {
      fputs("init-install: app://installer.pxe is not packaged; installation unavailable\n", stderr);
    } else {
      fprintf(stderr, "init-install: cannot open app://installer.pxe (status %u)\n", status);
    }
    goto done;
  }

  struct launch_grant grants[GRANT_COUNT] = {
    [DISKS] = {sources[DISKS], DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN, 0},
    [KERNEL] = {sources[KERNEL], FILE_RIGHT_READ, 0},
    [ARCHIVE] = {sources[ARCHIVE], FILE_RIGHT_READ, 0},
    [MEMORY] = {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    [INPUT] = {sources[INPUT], CONSOLE_RIGHT_READ, 0},
    [OUTPUT] = {sources[OUTPUT], CONSOLE_RIGHT_WRITE, 0},
    [CLOCK] = {sources[CLOCK], CLOCK_RIGHT_READ, 0},
    [RANDOM] = {sources[RANDOM], RANDOM_RIGHT_READ, 0},
    [APP] = {sources[APP], DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES, 0},
    [FIRST_STREAM + STARTUP_STDIN] = {sources[INPUT], CONSOLE_RIGHT_READ, 0},
    [FIRST_STREAM + STARTUP_STDOUT] = {sources[OUTPUT], CONSOLE_RIGHT_WRITE, 0},
    [FIRST_STREAM + STARTUP_STDERR] = {sources[OUTPUT], CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"disks", DISKS}, {(uintptr_t)"boot_kernel", KERNEL},
    {(uintptr_t)"boot_archive", ARCHIVE}, {(uintptr_t)"memory", MEMORY},
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"clock", CLOCK}, {(uintptr_t)"random", RANDOM},
  };
  struct launch_binding roots[] = {{(uintptr_t)"app", APP}};
  const char *arguments[] = {"app://installer.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources, .resource_count = sizeof(resources) / sizeof(resources[0]),
    .roots = (uintptr_t)roots, .root_count = sizeof(roots) / sizeof(roots[0]),
    .argv = (uintptr_t)arguments, .argc = 1,
    .streams = {
      [STARTUP_STDIN] = {PROTOCOL_CONSOLE, FIRST_STREAM + STARTUP_STDIN},
      [STARTUP_STDOUT] = {PROTOCOL_CONSOLE, FIRST_STREAM + STARTUP_STDOUT},
      [STARTUP_STDERR] = {PROTOCOL_CONSOLE, FIRST_STREAM + STARTUP_STDERR},
    },
  };
  status = launcher_launch(sources[LAUNCHER], &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot launch app://installer.pxe (status %u)\n", status);
    goto done;
  }
  struct process_result completion;
  status = process_wait(child, &completion);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot wait for installer (status %u)\n", status);
    goto done;
  }
  if (completion.kind == PROCESS_EXITED) {
    fprintf(stderr, "init-install: installer exited with status %lld\n", (long long)completion.exit_status);
    result = completion.exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  } else {
    fprintf(stderr, "init-install: installer stopped (completion kind %llu)\n",
        (unsigned long long)completion.kind);
  }

done:
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
