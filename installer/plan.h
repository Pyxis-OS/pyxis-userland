#ifndef USERSPACE_INSTALLER_PLAN_H
#define USERSPACE_INSTALLER_PLAN_H

#include "plan_record.h"
#include "inventory.h"
#include "target.h"
#include "esp_activate.h"
#include "esp_payload.h"
#include "esp_view.h"
#include "boot_entry.h"
#if INSTALLER_PUBLICATION_SCRATCH
#include "installed_publication.h"
#endif
#include <pyxis_fs/npfs.h>

/* Owned preparation, immutable after Review. Native source handles and BOOT
 * are borrowed except EFI; no target mount or write occurs here. */
struct install_plan {
  struct install_plan_record record;
  struct install_disk disk, raw_disk;
  struct install_target target;
  handle_t disks, boot, clock;
#if INSTALLER_PUBLICATION_SCRATCH
  handle_t workspace;
#endif
  struct install_source kernel, archive, efi, rescue;
  struct install_programs programs;
  struct install_manifest manifest;
  struct install_prior prior;
  struct install_consent consent;
  struct npfs_header header;
  char *manifest_text, *configuration;
  size_t manifest_bytes, configuration_bytes;
  char revision_record[65];
  struct install_esp *esp;
  struct install_esp_payload_request payload_request;
  struct install_esp_activation_request classic_request;
  struct config_blob entry;
  struct install_esp_view view;
  struct install_esp_payloads *payloads;
  struct install_esp_classic_files *classic_files;
#if INSTALLER_PUBLICATION_SCRATCH
  struct install_installed_publication installed;
  struct config_storage_report publication_report;
#endif
  uint64_t available_blocks, required_blocks, cleanup_deadline_ns;
  uint64_t esp_cluster_bytes, esp_required_clusters, esp_free_clusters;
  bool ready, disk_claimed, raw_claimed;
};

/* Default publication is qualified for Classic only. Coexist keeps the
 * immutable-payload/manual-entry path until separately qualified. */
static inline bool install_plan_publication(const struct install_plan *plan)
{
#if INSTALLER_PUBLICATION_SCRATCH
  return !plan->record.coexist;
#else
  (void)plan;
  return false;
#endif
}

bool install_plan_sources(struct install_plan *plan, handle_t disks, handle_t boot, handle_t kernel,
    handle_t archive, handle_t clock, handle_t system_info);
/* Rebuilds inspection, hashes and derived assets for the recorded choices;
 * caller compares the result to a serialized expected record before Review. */
bool install_plan_prepare(struct install_plan *plan, const struct disk_info *target);
bool install_plan_revalidate(struct install_plan *plan);
bool install_plan_report(const struct install_plan *plan, FILE *output, bool verbose);
void install_plan_destroy(struct install_plan *plan);

/* The only mutating entry point. Explicit consent must match this ready plan;
 * all frontends pass through the same exclusive revalidation before writing. */
bool install_plan_execute(struct install_plan *plan, const char *consent);
/* Releases only execution-owned references/claims and retires its FAT view.
 * A failed cleanup stays represented in PLAN; it never retries mutation. */
bool install_plan_finish_execution(struct install_plan *plan);

#endif
