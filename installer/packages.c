#include "packages.h"
#include "installer.h"
#include "progress.h"
#include "plan.h"
#include "../common/bootstrap_environment.h"
#include "../common/launch_environment.h"
#include <package_maintenance.h>
#include <package_transition.h>
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
#include <string.h>

#define CONFIG_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define MANAGEMENT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | \
    DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_FILESYSTEM_INFO)

/* Runs pkg with ARGUMENTS and waits for it. MANAGEMENT is the packages volume
 * it may change, or INVALID for a command that needs none. Its output goes to
 * the installer's console, whose input also lets pkg guard its set switch
 * against Ctrl+C; boot:// gives it the running ABI (os-release). */
static bool run_pkg(const char *const *arguments, size_t argument_count, handle_t management,
    handle_t configured)
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
  handle_t config = configured, process = HANDLE_INVALID;
  bool own_config = config == HANDLE_INVALID;
  struct bundle_program *program = NULL;
  struct bundle_launch *prepared = NULL;
  const struct path_context *context;
  const char *step = "config";
  enum call_status status = own_config ?
      directory_lookup(boot, "config", DIRECTORY_KIND_DIRECTORY, CONFIG_RIGHTS, &config) : CALL_OK;
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
  if (own_config && config != HANDLE_INVALID) {
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
  if (run_pkg(arguments, 3, HANDLE_INVALID, HANDLE_INVALID)) {
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
  bool installed = run_pkg(arguments, 4, packages, HANDLE_INVALID);
  handle_close(packages);
  if (!installed) {
    fputs("installer: " INSTALL_BASE_PACKAGE " was not installed\n", stderr);
  }
  return installed;
}

static bool package_identity(const struct install_plan *plan,
    struct package_boot_identity *identity)
{
#if INSTALLER_PUBLICATION_SCRATCH
  if (!install_plan_publication(plan) || !plan->installed.prepared ||
      !plan->installed.binding.present || plan->record.layout.pool_partition > UINT32_MAX) {
    return false;
  }
  const struct config_boot_binding *binding = &plan->installed.binding;
  *identity = (struct package_boot_identity){
    .pool = (uint32_t)plan->record.layout.pool_partition,
    .kernel_size = binding->kernel.digest.size,
    .archive_size = binding->archive.digest.size,
  };
  memcpy(identity->attempt, binding->attempt, sizeof(identity->attempt));
  memcpy(identity->kernel_sha256, binding->kernel.digest.sha256,
      sizeof(identity->kernel_sha256));
  memcpy(identity->archive_sha256, binding->archive.digest.sha256,
      sizeof(identity->archive_sha256));
  return package_boot_identity_valid(identity);
#else
  (void)plan;
  (void)identity;
  return false;
#endif
}

bool install_packages_boot_mark(const struct install_plan *plan)
{
  struct package_boot_identity identity;
  if (!package_identity(plan, &identity)) return false;
  handle_t packages = HANDLE_INVALID;
  enum call_status status = disk_open_volume(plan->disk.handle,
      plan->target.pool.partition, INSTALL_PACKAGES_VOLUME, MANAGEMENT_RIGHTS, &packages);
  struct package_set *set = NULL;
  const char *reason = NULL;
  if (status == CALL_OK) status = package_set_load(packages, &set, &reason);
  if (status == CALL_OK && package_set_number(set)) {
    status = package_boot_publish(packages, package_set_number(set), &identity);
  } else if (status == CALL_OK) {
    status = CALL_BAD_REQUEST;
  }
  package_set_free(set);
  if (packages != HANDLE_INVALID && handle_close(packages) != CALL_OK && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "installer: package boot identity could not be synchronized (status %u)\n",
        status);
  }
  return status == CALL_OK;
}

bool install_packages_update_stage(const struct install_plan *plan)
{
  struct package_boot_identity identity;
  if (!package_identity(plan, &identity)) return false;
  handle_t packages = HANDLE_INVALID, system = HANDLE_INVALID, config = HANDLE_INVALID;
  enum call_status status = disk_open_volume(plan->disk.handle,
      plan->target.pool.partition, INSTALL_PACKAGES_VOLUME, MANAGEMENT_RIGHTS, &packages);
  if (status == CALL_OK) {
    status = disk_open_volume(plan->disk.handle, plan->target.pool.partition,
        "system", CONFIG_RIGHTS, &system);
  }
  if (status == CALL_OK) {
    status = directory_lookup(system, "config", DIRECTORY_KIND_DIRECTORY,
        CONFIG_RIGHTS, &config);
  }
  char pool[24], kernel_size[24], archive_size[24];
  snprintf(pool, sizeof(pool), "%u", identity.pool);
  snprintf(kernel_size, sizeof(kernel_size), "%llu",
      (unsigned long long)identity.kernel_size);
  snprintf(archive_size, sizeof(archive_size), "%llu",
      (unsigned long long)identity.archive_size);
  const char *const arguments[] = {"pkg", "stage-update", pool, identity.attempt,
    kernel_size, identity.kernel_sha256, archive_size, identity.archive_sha256};
  install_progress_pause();
  bool staged = status == CALL_OK && run_pkg(arguments,
      sizeof(arguments) / sizeof(arguments[0]), packages, config);
  if (config != HANDLE_INVALID) handle_close(config);
  if (system != HANDLE_INVALID) handle_close(system);
  if (packages != HANDLE_INVALID) handle_close(packages);
  if (!staged) {
    fprintf(stderr, "installer: target package set was not staged (status %u); boot selection unchanged\n",
        status);
  }
  return staged;
}
