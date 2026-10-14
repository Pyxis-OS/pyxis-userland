#ifndef USERSPACE_INSTALLER_PLAN_H
#define USERSPACE_INSTALLER_PLAN_H

#include "plan_record.h"
#include "inventory.h"
#include <pyxis_fs/npfs.h>

/* Owned preparation, immutable after Review. Native source handles and BOOT
 * are borrowed except EFI; no target mount or write occurs here. */
struct install_plan {
  struct install_plan_record record;
  struct install_disk disk;
  handle_t disks, boot, clock;
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
  uint64_t available_blocks, required_blocks;
  bool ready;
};

bool install_plan_sources(struct install_plan *plan, handle_t disks, handle_t boot, handle_t kernel,
    handle_t archive, handle_t clock, handle_t system_info);
/* Rebuilds inspection, hashes and derived assets for the recorded choices;
 * caller compares the result to a serialized expected record before Review. */
bool install_plan_prepare(struct install_plan *plan, const struct disk_info *target);
bool install_plan_revalidate(const struct install_plan *plan);
bool install_plan_report(const struct install_plan *plan, FILE *output, bool verbose);
void install_plan_destroy(struct install_plan *plan);

/* The only mutating entry point. Explicit consent must match this ready plan;
 * all frontends pass through the same exclusive revalidation before writing. */
bool install_plan_execute(struct install_plan *plan, const char *consent);

#endif
