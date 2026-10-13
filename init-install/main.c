#include "../common/launch_environment.h"
#include "../common/bootstrap_environment.h"
#include <abi/clock.h>
#include <abi/console.h>
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
#include <string.h>
#include <space.h>
#include <system_info.h>
#include <term.h>

#define INSTALL_COMMON_DISK_RIGHTS (DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN_READ_ONLY | \
    DISKS_RIGHT_OPEN_POOL | DISKS_RIGHT_MOUNT_READ_ONLY | DISKS_RIGHT_MOUNT_READ_WRITE)
#define SNAPSHOT_PARENT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES | \
    DIRECTORY_RIGHT_FILESYSTEM_INFO)

#ifndef INSTALLER_PUBLICATION_SCRATCH
#define INSTALLER_PUBLICATION_SCRATCH 0
#endif

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

static int run_install(int argc, char **argv, bool selector)
{
  enum { DISKS, KERNEL, ARCHIVE, MEMORY, CLOCK, RANDOM, SYSTEM_INFO, LOG, BOOT_ROOT, DIAGNOSTICS,
    SNAPSHOT_PARENT, POWER,
    LAUNCHER, INPUT, OUTPUT, ERROR, FACTORY, SOURCE_COUNT, GRANT_COUNT = FACTORY };
  const char *names[SOURCE_COUNT] = {"disks", "boot_kernel", "boot_archive", "memory",
    "clock", "random", "system_info", "log", "boot", "installer_diagnostics", "config_snapshot_parent", "power",
    "launcher", "input", "output", "stderr", "space_factory"};
  handle_t sources[SOURCE_COUNT] = {
    bootstrap_capability("disks"), bootstrap_capability("boot_kernel"), bootstrap_capability("boot_archive"),
    bootstrap_capability("memory"), bootstrap_capability("clock"), bootstrap_capability("random"),
    bootstrap_capability("system_info"), bootstrap_capability("log"), bootstrap_capability("boot"),
    selector ? bootstrap_capability("installer_diagnostics") : HANDLE_INVALID,
    selector ? bootstrap_capability("config_snapshot_parent") : HANDLE_INVALID,
    bootstrap_capability("power"), bootstrap_capability("launcher"),
    selector ? bootstrap_capability("input") : HANDLE_INVALID,
    selector ? bootstrap_capability("output") : HANDLE_INVALID,
    HANDLE_INVALID, selector ? HANDLE_INVALID : bootstrap_take_capability("space_factory"),
  };
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  uint64_t *cpus = NULL;
  const char **arguments = NULL;
  int result = EXIT_FAILURE;
  bool coexist = false;
  bool snapshot_created = false, child_retired = false;
  handle_t ram = selector ? HANDLE_INVALID : bootstrap_capability("ram");
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  struct pyxis_stdio_binding error = bootstrap_stdio(ENVIRONMENT_STDERR);
  if (selector) {
    if (output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
      fputs("init-install: selector needs the Install console\n", stderr);
      goto done;
    }
    if (handle_copy_restricted(error.handle, CONSOLE_RIGHT_WRITE, 0, &sources[ERROR]) != CALL_OK) {
      fputs("init-install: cannot delegate Install error output\n", stderr);
      goto done;
    }
  } else if (output.protocol != PROTOCOL_CONSOLE ||
      handle_copy_restricted(output.handle, CONSOLE_RIGHT_WRITE, 0, &sources[DIAGNOSTICS]) != CALL_OK) {
    fputs("init-install: cannot delegate Caelum diagnostic output\n", stderr);
    goto done;
  }
  if (!selector) {
    enum call_status status = directory_create(ram, "installer-config-snapshots",
        DIRECTORY_KIND_DIRECTORY, SNAPSHOT_PARENT_RIGHTS, &sources[SNAPSHOT_PARENT]);
    if (status != CALL_OK) {
      fprintf(stderr, "init-install: private scratch snapshot parent refused (status %u)\n", status);
      goto done;
    }
    snapshot_created = true;
  }
  for (unsigned i = 0; i < SOURCE_COUNT; i++) {
    /* Without power the installer simply offers no restart. */
    bool required = i != POWER && (selector ? i != FACTORY :
        i != INPUT && i != OUTPUT && i != ERROR);
    if (sources[i] == HANDLE_INVALID && required) {
      fprintf(stderr, "init-install: missing resource %s; use the Install Pyxis boot entry\n", names[i]);
      goto done;
    }
  }
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--coexist") || !strcmp(argv[i], "--select-profile")) {
      fputs("init-install: installer profile is selected by the trusted prompt\n", stderr);
      goto done;
    }
  }
  if (selector) {
    struct terminal terminal = {.input = sources[INPUT], .output = sources[OUTPUT]};
    puts("Choose installer authority: coexist preserves other partitions; classic permits whole-disk erase.");
    for (;;) {
      char line[32];
      if (fflush(stdout) != 0) {
        goto done;
      }
      struct term_line_result selected = term_read_line(&terminal,
          "Profile (coexist/classic/cancel): ", line, sizeof(line));
      if (selected.status != TERM_LINE_OK || selected.limit_reached) {
        fputs("init-install: profile selection interrupted; installer not launched\n", stderr);
        goto done;
      }
      if (!strcmp(line, "cancel")) {
        puts("Installation cancelled; installer not launched.");
        result = EXIT_SUCCESS;
        goto done;
      }
      if (!strcmp(line, "coexist") || !strcmp(line, "classic")) {
        coexist = !strcmp(line, "coexist");
        break;
      }
      puts("Enter coexist, classic or cancel.");
    }
  }
  const char *image_name = selector ? "installer.pxe" : "init-install.pxe";
  enum call_status status = directory_lookup(sources[BOOT_ROOT], image_name,
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot open boot://%s (status %u)\n", image_name, status);
    goto done;
  }
  struct system_info_cpu cpu = {0};
  if (!selector) {
    status = system_info_get_cpu(sources[SYSTEM_INFO], &cpu);
    if (status != CALL_OK || !cpu.online_count || cpu.online_count > UINT64_MAX - 63 ||
        (cpu.online_count + 63) / 64 > SIZE_MAX / sizeof(*cpus)) {
      fprintf(stderr, "init-install: cannot read the CPU count (status %u)\n", status);
      goto done;
    }
    cpus = calloc((cpu.online_count + 63) / 64, sizeof(*cpus));
    if (!cpus) {
      fputs("init-install: cannot allocate CPU selection\n", stderr);
      goto done;
    }
    for (uint64_t i = 0; i < cpu.online_count; i++) {
      cpus[i / 64] |= UINT64_C(1) << (i % 64);
    }
  }

  struct launch_grant grants[GRANT_COUNT] = {
    [DISKS] = {sources[DISKS], INSTALL_COMMON_DISK_RIGHTS |
        (!selector || coexist ? DISKS_RIGHT_APPEND_POOL : 0) |
        (!selector || !coexist ? DISKS_RIGHT_ERASE | DISKS_RIGHT_REBUILD_ESP : 0), 0},
    [KERNEL] = {sources[KERNEL], FILE_RIGHT_READ, 0},
    [ARCHIVE] = {sources[ARCHIVE], FILE_RIGHT_READ, 0},
    [MEMORY] = {sources[MEMORY], MEMORY_RIGHT_MANAGE, 0},
    [CLOCK] = {sources[CLOCK], CLOCK_RIGHT_READ | CLOCK_RIGHT_SLEEP, 0},
    [RANDOM] = {sources[RANDOM], RANDOM_RIGHT_READ, 0},
    [SYSTEM_INFO] = {sources[SYSTEM_INFO], SYSTEM_INFO_RIGHT_READ, 0},
    [LOG] = {sources[LOG], LOG_RIGHT_READ, 0},
    [DIAGNOSTICS] = {sources[DIAGNOSTICS], CONSOLE_RIGHT_WRITE, 0},
    [SNAPSHOT_PARENT] = {sources[SNAPSHOT_PARENT], SNAPSHOT_PARENT_RIGHTS, 0},
    [BOOT_ROOT] = {sources[BOOT_ROOT], DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES, 0},
    [POWER] = {sources[POWER], POWER_RIGHT_RESTART, 0},
    [LAUNCHER] = {sources[LAUNCHER], LAUNCHER_RIGHT_LAUNCH, 0},
    [INPUT] = {sources[INPUT], CONSOLE_RIGHT_READ, 0},
    [OUTPUT] = {sources[OUTPUT], CONSOLE_RIGHT_WRITE, 0},
    [ERROR] = {sources[ERROR], CONSOLE_RIGHT_WRITE, 0},
  };
  struct launch_environment_entry resources[] = {
    PROFILE_CAP((uintptr_t)"disks", DISKS), PROFILE_CAP((uintptr_t)"boot_kernel", KERNEL),
    PROFILE_CAP((uintptr_t)"boot_archive", ARCHIVE), PROFILE_CAP((uintptr_t)"memory", MEMORY),
    PROFILE_CAP((uintptr_t)"clock", CLOCK), PROFILE_CAP((uintptr_t)"random", RANDOM),
    PROFILE_CAP((uintptr_t)"system_info", SYSTEM_INFO), PROFILE_CAP((uintptr_t)"log", LOG),
    PROFILE_PRIVATE_CAP((uintptr_t)"installer_diagnostics", DIAGNOSTICS),
    PROFILE_PRIVATE_CAP((uintptr_t)"config_snapshot_parent", SNAPSHOT_PARENT),
    PROFILE_CAP((uintptr_t)"power", POWER),
  };
  bool has_power = sources[POWER] != HANDLE_INVALID;
  struct launch_environment_entry roots[] = {PROFILE_CAP((uintptr_t)"boot", BOOT_ROOT)};
  size_t argument_count = 1 + (!selector || coexist);
  if (argc > 1) {
    argument_count += (size_t)argc - 1;
  }
  arguments = calloc(argument_count, sizeof(*arguments));
  if (!arguments) {
    fputs("init-install: cannot prepare installer arguments\n", stderr);
    goto done;
  }
  arguments[0] = selector ? "boot://installer.pxe" : "boot://init-install.pxe";
  size_t argument_index = 1;
  if (!selector || coexist) {
    arguments[argument_index++] = selector ? "--coexist" : "--select-profile";
  }
  for (int i = 1; i < argc; i++) {
    arguments[argument_index++] = argv[i];
  }
  struct launch_environment_entry entries[128];
  size_t entry_count = 0;
  memcpy(entries + entry_count, resources, (sizeof(resources) / sizeof(resources[0]) - !has_power) * sizeof(*entries));
  entry_count += sizeof(resources) / sizeof(resources[0]) - !has_power;
  memcpy(entries + entry_count, roots, (sizeof(roots) / sizeof(roots[0])) * sizeof(*entries));
  entry_count += sizeof(roots) / sizeof(roots[0]);
  if (selector) {
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"input", INPUT);
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"output", OUTPUT);
    entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDIN, INPUT);
    entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDOUT, OUTPUT);
    entries[entry_count++] = profile_stdio_entry(ENVIRONMENT_STDERR, ERROR);
  } else {
    entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"launcher", LAUNCHER);
  }
  struct launch_grant selected_grants[sizeof(entries) / sizeof(entries[0])];
  for (size_t i = 0; i < entry_count; ++i) {
    selected_grants[i] = grants[entries[i].value];
    entries[i].value = i;
  }
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)selected_grants,
    .entries = (uintptr_t)entries,
    .entry_count = entry_count,
    .grant_count = entry_count,
    .argv = (uintptr_t)arguments,
    .argc = argument_count,
  };
  struct space_definition space = {.name = "install", .title = "Install"};
  status = selector ? launcher_launch(sources[LAUNCHER], &request, &child) :
      space_create_started(sources[FACTORY], &space, cpus, cpu.online_count, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot start boot://%s (status %u)\n", image_name, status);
    goto done;
  }
  /* Neither child can create another space or recover its issuer's disk grant. */
  for (unsigned i = 0; i < SOURCE_COUNT; ++i) {
    if ((i == DISKS || i == LAUNCHER || i == FACTORY) && sources[i] != HANDLE_INVALID) {
      if (handle_close(sources[i]) != 0) {
        goto done;
      }
      sources[i] = HANDLE_INVALID;
    }
  }
  struct process_result completion;
  status = process_wait(child, &completion);
  if (status != CALL_OK) {
    fprintf(stderr, "init-install: cannot wait for installer (status %u)\n", status);
    goto done;
  }
  child_retired = true;
  if (completion.kind == PROCESS_EXITED) {
    printf("init-install: installer exited with status %lld\n", (long long)completion.exit_status);
    result = completion.exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  } else {
    printf("init-install: installer stopped (completion kind %llu)\n",
        (unsigned long long)completion.kind);
  }

done:
  if (snapshot_created && (child == HANDLE_INVALID || child_retired) &&
      directory_remove(ram, "installer-config-snapshots", DIRECTORY_KIND_DIRECTORY) != CALL_OK) {
    fputs("init-install: scratch snapshot parent cleanup unconfirmed\n", stderr);
    result = EXIT_FAILURE;
  }
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

int main(int argc, char **argv)
{
  bool selector = argc > 1 && !strcmp(argv[1], "--select-profile");
  return selector ? run_install(argc - 1, argv + 1, true) : run_install(argc, argv, false);
}
