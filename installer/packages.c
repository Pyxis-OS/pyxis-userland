#include "packages.h"
#include "installer.h"
#include "progress.h"
#include "../common/bootstrap_environment.h"
#include "../common/launch_environment.h"
#include <package_maintenance.h>
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/directory.h>
#include <abi/memory.h>
#include <abi/provider.h>
#include <abi/random.h>
#include <bundle.h>
#include <bundle_launch.h>
#include <directory.h>
#include <disk.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <pyxis/working_path.h>
#include <startup.h>
#include <stdio.h>

#define CONFIG_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define MANAGEMENT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | \
    DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_FILESYSTEM_INFO)

/* Runs pkg with ARGUMENTS and waits for it. MANAGEMENT is the packages volume
 * it may change, or INVALID for a command that needs none. Its output goes to
 * the installer's console, whose input also lets pkg guard its set switch
 * against Ctrl+C; boot:// gives it the running ABI (os-release). */
static bool run_pkg(const char *const *arguments, size_t argument_count, handle_t management)
{
  handle_t http = bootstrap_capability("http"), https = bootstrap_capability("https");
  handle_t boot = bootstrap_capability("boot"), launcher = bootstrap_capability("launcher");
  handle_t input = bootstrap_capability("input");
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  struct pyxis_stdio_binding error = bootstrap_stdio(ENVIRONMENT_STDERR);
  if (http == HANDLE_INVALID && https == HANDLE_INVALID) {
    fputs("installer: no HTTP provider; Install needs the network to fetch " INSTALL_BASE_PACKAGE "\n",
        stderr);
    return false;
  }
  if (launcher == HANDLE_INVALID) {
    fputs("installer: no launcher for pkg\n", stderr);
    return false;
  }
  handle_t config = HANDLE_INVALID, process = HANDLE_INVALID;
  struct bundle_program *program = NULL;
  struct bundle_launch *prepared = NULL;
  const struct path_context *context;
  const char *step = "config";
  enum call_status status = directory_lookup(boot, "config", DIRECTORY_KIND_DIRECTORY,
      CONFIG_RIGHTS, &config);
  if (status == CALL_OK) {
    step = "bundle";
    status = pyxis_working_context(&context);
  }
  if (status == CALL_OK) {
    status = bundle_open(context, "boot://pkg.pxb", &program);
  }
  if (status == CALL_OK) {
    step = "launch";
    /* pkg selects its providers by name from these entries. */
    struct launch_grant grants[] = {
      {output.handle, CONSOLE_RIGHT_WRITE, 0},
      {error.handle, CONSOLE_RIGHT_WRITE, 0},
      {http, PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL},
      {https, PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL},
      {boot, DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES, 0},
      {input, CONSOLE_RIGHT_READ, 0},
    };
    struct launch_environment_entry entries[6] = {
      profile_stdio_entry(ENVIRONMENT_STDOUT, 0),
      profile_stdio_entry(ENVIRONMENT_STDERR, 1),
      PROFILE_CAP((uintptr_t)"boot", 4),
      profile_stdio_entry(ENVIRONMENT_STDIN, 5),
    };
    size_t entry_count = 4;
    if (http != HANDLE_INVALID) {
      entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"http", 2);
    }
    if (https != HANDLE_INVALID) {
      entries[entry_count++] = (struct launch_environment_entry)PROFILE_CAP((uintptr_t)"https", 3);
    }
    struct launch_request request = {
      .grants = (uintptr_t)grants, .grant_count = sizeof(grants) / sizeof(*grants),
      .entries = (uintptr_t)entries, .entry_count = entry_count,
      .argv = (uintptr_t)arguments, .argc = argument_count,
    };
    struct bundle_authority authority[] = {
      {"memory", bootstrap_capability("memory"), PROTOCOL_MEMORY},
      {"clock", bootstrap_capability("clock"), PROTOCOL_CLOCK},
      {"random", bootstrap_capability("random"), PROTOCOL_RANDOM},
      {PACKAGE_CONFIG_SOURCE, config, PROTOCOL_DIRECTORY},
      {PACKAGE_MANAGEMENT_SOURCE, management, PROTOCOL_DIRECTORY},
    };
    status = bundle_launch_prepare(program, &request, authority,
        sizeof(authority) / sizeof(*authority) - (management == HANDLE_INVALID), &prepared);
    if (status == CALL_OK) {
      status = launcher_launch(launcher, bundle_launch_request(prepared), &process);
      bundle_launch_admitted(prepared, status, process);
    }
  }
  bundle_launch_close(prepared);
  bundle_program_close(program);
  if (config != HANDLE_INVALID) {
    handle_close(config);
  }
  if (status != CALL_OK) {
    fprintf(stderr, "installer: cannot run pkg (%s, status %u)\n", step, status);
    return false;
  }
  struct process_result completion;
  status = process_wait(process, &completion);
  handle_close(process);
  return status == CALL_OK && completion.kind == PROCESS_EXITED && completion.exit_status == 0;
}

bool install_packages_check(void)
{
  static const char *const arguments[] = {"pkg", "check", INSTALL_BASE_PACKAGE};
  if (run_pkg(arguments, 3, HANDLE_INVALID)) {
    return true;
  }
  fputs("installer: Install needs " INSTALL_BASE_PACKAGE
      " from the repository, and pkg could not confirm it (above); nothing was written\n", stderr);
  return false;
}

bool install_packages_base(handle_t disk, uint64_t pool_partition)
{
  static const char *const arguments[] = {"pkg", "--yes", "install", INSTALL_BASE_PACKAGE};
  handle_t packages = HANDLE_INVALID;
  enum call_status status = disk_open_volume(disk, pool_partition, INSTALL_PACKAGES_VOLUME,
      MANAGEMENT_RIGHTS, &packages);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: packages volume unavailable (status %u)\n", status);
    return false;
  }
  install_progress_pause();
  bool installed = run_pkg(arguments, 4, packages);
  handle_close(packages);
  if (!installed) {
    fputs("installer: " INSTALL_BASE_PACKAGE " was not installed\n", stderr);
  }
  return installed;
}
