#include "installed_publication.h"
#include "plan.h"
#include "publication_snapshot.h"

#include <clock.h>
#include <directory.h>
#include <handle.h>
#include <mount.h>
#include <string.h>

#if INSTALLER_PUBLICATION_SCRATCH

#define INSTALLED_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)

/* The reviewed ESP as publisher facts; built the same way for every phase. */
static struct boot_publisher_input publisher_input(const struct install_plan *plan)
{
  const struct disk_partition_selection *selected = &plan->target.esp;
  struct boot_publisher_input input = {.name = "installer", .workspace = plan->workspace,
    .clock = plan->clock, .boot = plan->boot, .loaded_kernel = plan->kernel.handle,
    .kernel = plan->kernel, .archive = plan->rescue, .revision = plan->record.revision,
    .attempt = plan->record.attempt, .revision_record = plan->revision_record,
    .disk_guid = plan->record.layout.disk_guid,
    .target = {.device = selected->id, .partition = selected->partition,
      .first_block = selected->first_block, .block_count = selected->block_count,
      .device_block_size = selected->device_block_size,
      .device_block_count = selected->device_block_count,
      .partition_attributes = selected->attributes}};
  memcpy(input.target.disk_guid, selected->disk_guid, 16);
  memcpy(input.target.partition_guid, selected->partition_guid, 16);
  memcpy(input.target.partition_type, selected->partition_type, 16);
  return input;
}

static bool close_planning(struct install_plan *plan)
{
  uint64_t now;
  if (clock_now(plan->clock, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(30000000000) ||
      install_esp_view_close_and_retire(&plan->view, plan->clock,
          now + UINT64_C(30000000000)) != CALL_OK) {
    fputs("installer: configuration planning view retirement remains pending\n", stderr);
    return false;
  }
  plan->view = (struct install_esp_view){0};
  return true;
}

void install_installed_destroy(struct boot_publication *publication)
{
  boot_publication_destroy(publication);
}

bool install_installed_prepare(struct install_plan *plan)
{
  struct boot_publication *publication = &plan->installed;
  const struct boot_publisher_input input = publisher_input(plan);
  boot_publication_destroy(publication);
  bool fresh = !plan->record.coexist && (!plan->record.updating || plan->record.esp_rebuild);
  struct config_blob pool = {0};
  bool ok = true;
  struct install_disk reader = {.handle = HANDLE_INVALID, .info = plan->disk.info,
    .bytes = plan->disk.bytes};
  if (plan->record.updating) {
    ok = disks_open(plan->disks, plan->disk.info.id, DISK_ACCESS_READ_ONLY, &reader.handle) == CALL_OK &&
        install_pool_boot_snapshot_at(&reader, plan->record.layout.pool_partition, &pool);
  }
  if (reader.handle != HANDLE_INVALID) {
    ok = handle_close(reader.handle) == CALL_OK && ok;
  }
  if (ok && fresh) {
    uint64_t cluster_bytes = 0, available_bytes = 0;
    ok = install_esp_capacity(plan->esp, &cluster_bytes, &available_bytes) &&
        boot_publication_prepare_fresh(publication, &input, plan->configuration,
            plan->configuration_bytes, &pool, cluster_bytes, available_bytes);
  } else if (ok) {
    struct mount_fat32_reply mounted = {0};
    enum call_status status = disks_open_owned_esp(plan->disks, &plan->target.esp,
        MOUNT_ACCESS_READ_ONLY, INSTALLED_READ, &mounted);
    plan->view.root = mounted.root;
    plan->view.control = mounted.control;
    ok = status == CALL_OK &&
        boot_publication_prepare(publication, &input, mounted.root, mounted.control, &pool);
    ok = close_planning(plan) && ok;
  }
  config_blob_destroy(&pool);
  publication->prepared = ok;
  return ok;
}

bool install_installed_hash(const struct install_plan *plan, struct install_digest *digest)
{
  return boot_publication_hash(&plan->installed, digest);
}

void install_installed_report(const struct install_plan *plan, FILE *output, bool verbose)
{
  const struct boot_publisher_input input = publisher_input(plan);
  boot_publication_report(&plan->installed, &input, output, verbose);
}

bool install_installed_admit(struct install_plan *plan)
{
  struct boot_publication *publication = &plan->installed;
  if (!publication->prepared || plan->view.root != HANDLE_INVALID || plan->view.control != HANDLE_INVALID) return false;
  const struct boot_publisher_input input = publisher_input(plan);
  enum call_status status = install_esp_view_open(&plan->view, plan->disks, &plan->target.esp,
      NULL, INSTALL_ESP_VIEW_RIGHTS);
  /* Pool admission is already selected and released. Open only its existing
   * native system root for the consumed override; never raw-read a mount. */
  handle_t system = HANDLE_INVALID;
  if (status == CALL_OK && plan->record.updating) {
    status = disk_open_volume(plan->disk.handle, plan->target.pool.partition, "system", INSTALLED_READ, &system);
  }
  bool ok = status == CALL_OK;
  if (ok) {
    ok = boot_publication_admit(publication, &input, &plan->view, system);
  } else {
    fputs("installer: stale consumed configuration/ownership input; no program files written\n", stderr);
  }
  if (system != HANDLE_INVALID) ok = handle_close(system) == CALL_OK && ok;
  return ok;
}

bool install_installed_publish(struct install_plan *plan)
{
  const struct boot_publisher_input input = publisher_input(plan);
  return boot_publication_publish(&plan->installed, &input, &plan->view, &plan->publication_report);
}
#endif
