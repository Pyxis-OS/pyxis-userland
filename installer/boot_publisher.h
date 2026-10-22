#ifndef USERSPACE_INSTALLER_BOOT_PUBLISHER_H
#define USERSPACE_INSTALLER_BOOT_PUBLISHER_H

#include "publication.h"
#include "recovery.h"
#include "digest.h"
#include "esp_view.h"

#include <stdio.h>

/* The shared Classic boot publisher: stage a kernel/archive attempt on an
 * already admitted ESP, then the boot record, menu and retention. Front ends own
 * target selection, view admission/retirement and consent; nothing here opens
 * disks or mounts. All handles and sources are borrowed and must stay
 * unchanged from prepare through publish. */
struct boot_publisher_input {
  const char *name; /* Message prefix, e.g. "installer". */
  handle_t workspace, clock;
  handle_t boot, loaded_kernel; /* Recovery sources: boot tree, kernel-issued FILE. */
  struct install_source kernel, archive;
  const char *revision; /* Kernel and program revision. */
  const char *attempt, *revision_record;
  const uint8_t *disk_guid; /* Menu mount.disk= of the installed pool. */
  uint64_t pool_partition; /* Menu mount.pool=, its one-based GPT entry. */
  struct config_target_facts target; /* Reviewed ESP facts; domain is not compared. */
};

/* Frozen review state. All snapshots and choices are fixed once prepared.
 * MENU and RECORD are the consumed ESP bytes; NEXT_MENU is the reviewed menu. */
struct boot_publication {
  struct config_blob menu, next_menu, record;
  struct install_recovery recovery;
  struct config_target_facts facts;
  struct config_boot_binding binding;
  uint64_t cluster_bytes, required_bytes, available_bytes;
  bool prepared, fresh, activation_pending;
};

/* An existing install. ROOT/CONTROL are a read-only planning view of the
 * target, borrowed; the caller retires it afterwards. */
bool boot_publication_prepare(struct boot_publication *publication,
    const struct boot_publisher_input *input, handle_t root, handle_t control);
/* A freshly formatted ESP: LIMINE is the new menu's starting bytes and the
 * capacity comes from the caller's formatted geometry. */
bool boot_publication_prepare_fresh(struct boot_publication *publication,
    const struct boot_publisher_input *input, const char *limine, size_t limine_bytes,
    uint64_t cluster_bytes, uint64_t available_bytes);
bool boot_publication_hash(const struct boot_publication *publication,
    struct install_digest *digest);
void boot_publication_report(const struct boot_publication *publication,
    const struct boot_publisher_input *input, FILE *output, bool verbose);
/* VIEW is the opened RW view of the reviewed target. Inside one checked
 * interval, revalidate every consumed input before the caller's first write;
 * create no parents and provision no recovery. */
bool boot_publication_admit(struct boot_publication *publication,
    const struct boot_publisher_input *input, struct install_esp_view *view);
/* Provision or verify recovery, stage, then the final publication interval. */
bool boot_publication_publish(struct boot_publication *publication,
    const struct boot_publisher_input *input, const struct install_esp_view *view,
    struct config_storage_report *report);
void boot_publication_destroy(struct boot_publication *publication);

#endif
