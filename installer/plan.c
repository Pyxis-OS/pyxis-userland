#include "plan.h"
#include "digest.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <path.h>
#include <system_info.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>

#define CLEANUP_WAIT_NS UINT64_C(30000000000)

static bool cleanup_deadline(handle_t clock, uint64_t *deadline)
{
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - CLEANUP_WAIT_NS) {
    return false;
  }
  *deadline = now + CLEANUP_WAIT_NS;
  return true;
}

static bool source_open(const char *path, struct install_source *source)
{
  install_progress_begin("Inspecting installation sources", INSTALL_PROGRESS_NONE, NULL);
  source->handle = HANDLE_INVALID;
  handle_t directories[4];
  char component[64];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 4,
    .component = component, .component_capacity = sizeof(component),
  };
  enum call_status status = path_resolve(NULL, path, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &source->handle);
  if (status != CALL_OK || file_size(source->handle, &source->bytes) != CALL_OK || !source->bytes) {
    install_progress_pause();
    fprintf(stderr, "installer: cannot read source %s\n", path);
    return false;
  }
  return true;
}

static char *boot_configuration(const uint8_t guid[16], size_t *bytes)
{
  /* Installed entries are an allowlist, independent of live-media tools. */
  static const char format[] =
    "timeout: %d\n"
    "serial: yes\n"
    "\n"
    "/Pyxis OS (Caelum)\n"
    "  protocol: limine\n"
    "  path: boot():/boot/caelum.elf\n"
    "  module_path: boot():/boot/initrd.cpio\n"
    "  paging_mode: 4level\n"
    "  kaslr: no\n"
    "  cmdline: " INSTALL_BOOT_INIT_OPTION " mount.disk=%s\n"
    "\n"
    "/Pyxis OS (rescue)\n"
    "  protocol: limine\n"
    "  path: boot():/boot/caelum.elf\n"
    "  module_path: boot():/boot/initrd.cpio\n"
    "  paging_mode: 4level\n"
    "  kaslr: no\n"
    "  cmdline: " INSTALL_BOOT_INIT_OPTION " mount.disk=%s " INSTALL_RESCUE_OPTION "\n";
  char disk_guid[37];
  install_guid_text(guid, disk_guid);
  int length = snprintf(NULL, 0, format, INSTALL_MENU_TIMEOUT_SECONDS, disk_guid, disk_guid);
  if (length < 0) {
    return NULL;
  }
  char *output = malloc((size_t)length + 1);
  if (!output) {
    return NULL;
  }
  if (snprintf(output, (size_t)length + 1, format, INSTALL_MENU_TIMEOUT_SECONDS,
        disk_guid, disk_guid) != length) {
    free(output);
    return NULL;
  }
  *bytes = (size_t)length;
  return output;
}

static bool hash_source(struct install_digest *digest, const struct install_source *source)
{
  uint8_t buffer[FILE_READ_MAX_BYTES], length[8];
  npfs_put_u64(length, source->bytes);
  if (!install_digest_update(digest, length, sizeof(length))) {
    return false;
  }
  for (uint64_t offset = 0; offset < source->bytes;) {
    size_t count = source->bytes - offset < sizeof(buffer) ?
        (size_t)(source->bytes - offset) : sizeof(buffer);
    if (!install_source_read(source, offset, buffer, count) ||
        !install_digest_update(digest, buffer, count)) {
      return false;
    }
    offset += count;
    install_progress_advance(count, 0);
  }
  return true;
}

static bool hash_choices(struct install_digest *digest, const struct install_plan_record *record)
{
  static const char domain[] = "pyxis-install-plan-choices-1";
  uint8_t flags[] = {record->updating, record->coexist, record->esp_rebuild,
    record->read_the_room, record->legacy, record->portable};
  uint8_t numbers[11 * 8];
  npfs_put_u64(numbers, record->block_count);
  npfs_put_u64(numbers + 8, record->block_size);
  npfs_put_u64(numbers + 16, record->journal_mib);
  npfs_put_u64(numbers + 24, record->layout.esp_start);
  npfs_put_u64(numbers + 32, record->layout.esp_bytes);
  npfs_put_u64(numbers + 40, record->layout.pool_start);
  npfs_put_u64(numbers + 48, record->layout.pool_bytes);
  npfs_put_u64(numbers + 56, record->layout.esp_partition);
  npfs_put_u64(numbers + 64, record->layout.pool_partition);
  npfs_put_u64(numbers + 72, record->layout.esp_attributes);
  npfs_put_u64(numbers + 80, record->layout.pool_attributes);
  return install_digest_update(digest, domain, sizeof(domain)) &&
      install_digest_update(digest, flags, sizeof(flags)) &&
      install_digest_update(digest, numbers, sizeof(numbers)) &&
      install_digest_update(digest, record->existing_guid, 16) &&
      install_digest_update(digest, record->pool_id, 16) &&
      install_digest_update(digest, record->volume_id, 16) &&
      install_digest_update(digest, record->layout.disk_guid, 16) &&
      install_digest_update(digest, record->layout.esp_guid, 16) &&
      install_digest_update(digest, record->layout.pool_guid, 16) &&
      install_digest_update(digest, record->revision, strlen(record->revision) + 1) &&
      install_digest_update(digest, record->previous, strlen(record->previous) + 1) &&
      install_digest_update(digest, record->attempt, sizeof(record->attempt));
}

static bool source_fingerprint(const struct install_plan *plan, char output[65])
{
  struct install_digest digest = {0};
  install_progress_begin("Hashing immutable install sources", INSTALL_PROGRESS_WORK, NULL);
  bool ok = install_digest_begin(&digest) && hash_choices(&digest, &plan->record) &&
      hash_source(&digest, &plan->kernel) && hash_source(&digest, &plan->archive) &&
      hash_source(&digest, &plan->efi) &&
      hash_source(&digest, &plan->rescue) &&
      install_digest_update(&digest, plan->manifest_text, plan->manifest_bytes) &&
      install_digest_update(&digest, plan->configuration, plan->configuration_bytes) &&
      hash_source(&digest, &(struct install_source){.memory = plan->entry.bytes,
        .bytes = plan->entry.size}) &&
#if INSTALLER_PUBLICATION_SCRATCH
      install_installed_hash(plan, &digest) &&
#endif
      install_digest_finish(&digest, output);
  install_digest_abort(&digest);
  install_progress_finish();
  return ok;
}

bool install_plan_sources(struct install_plan *plan, handle_t disks, handle_t boot, handle_t kernel,
    handle_t archive, handle_t clock, handle_t system_info)
{
  *plan = (struct install_plan){.disks = disks, .boot = boot, .clock = clock,
    .disk.handle = HANDLE_INVALID,
    .raw_disk.handle = HANDLE_INVALID,
    .kernel.handle = kernel, .archive.handle = archive,
    .efi.handle = HANDLE_INVALID,
    .rescue.handle = HANDLE_INVALID};
  install_progress_begin("Reading source revision", INSTALL_PROGRESS_NONE, NULL);
  struct system_info_identity identity;
  if (system_info_get_identity(system_info, &identity) != CALL_OK) {
    return false;
  }
  const char *revision = identity.build_revision[0] ? identity.build_revision :
      INSTALL_UNKNOWN_REVISION;
  if (strlen(revision) >= sizeof(plan->record.revision)) {
    return false;
  }
  strcpy(plan->record.revision, revision);
  bool ok = source_open("boot://share/installer/BOOTX64.EFI", &plan->efi) &&
      file_size(kernel, &plan->kernel.bytes) == CALL_OK && plan->kernel.bytes &&
      file_size(archive, &plan->archive.bytes) == CALL_OK && plan->archive.bytes &&
      install_programs_list(boot, &plan->programs) &&
      install_manifest_source(boot, &plan->programs, revision, &plan->manifest) == INSTALL_MANIFEST_OK &&
      install_manifest_encode(&plan->manifest, &plan->manifest_text,
        &plan->manifest_bytes) == INSTALL_MANIFEST_OK;
  install_progress_finish();
  return ok;
}

static bool space_budget(struct install_plan *plan)
{
  /* Deliberately overestimate mapping and per-entry metadata, without credit
   * for removing partial revisions or a promise against every allocation limit. */
  uint64_t blocks = 64;
  for (size_t i = 0; i < plan->manifest.count; i++) {
    const struct install_manifest_entry *entry = &plan->manifest.entries[i];
    uint64_t data = entry->size / NPFS_BLOCK_SIZE + (entry->size % NPFS_BLOCK_SIZE != 0);
    if (data > (UINT64_MAX - blocks - 4) / 4) {
      return false;
    }
    blocks += data * 4 + 4;
  }
  uint64_t manifest_blocks = plan->manifest_bytes / NPFS_BLOCK_SIZE +
      (plan->manifest_bytes % NPFS_BLOCK_SIZE != 0);
  if (manifest_blocks > (UINT64_MAX - blocks) / 4) {
    return false;
  }
  plan->required_blocks = blocks + manifest_blocks * 4;
  return plan->required_blocks <= plan->available_blocks;
}

static bool payload_plan(struct install_plan *plan)
{
#if INSTALLER_PUBLICATION_SCRATCH
  plan->payload_request.workspace = plan->workspace;
  return true;
#endif
  if (!plan->record.coexist) {
    plan->classic_request = (struct install_esp_activation_request){
      .configuration = plan->configuration, .configuration_bytes = plan->configuration_bytes,
      .revision = plan->revision_record, .revision_bytes = strlen(plan->revision_record),
      .payloads = {plan->efi, plan->kernel, plan->rescue},
    };
    for (size_t i = 0; i < 3; ++i) {
      if (!install_boot_payload_digest(&plan->classic_request.payloads[i],
          &plan->classic_request.expected_payloads[i])) {
        return false;
      }
    }
    return true;
  }
  char kernel_path[INSTALL_ESP_PAYLOAD_PATH_BYTES];
  char archive_path[INSTALL_ESP_PAYLOAD_PATH_BYTES];
  struct install_boot_entry_paths paths = {.kernel = kernel_path, .archive = archive_path};
  if (!install_esp_payload_path(plan->record.revision, plan->record.attempt,
        INSTALL_ESP_KERNEL, kernel_path) ||
      !install_esp_payload_path(plan->record.revision, plan->record.attempt,
        INSTALL_ESP_ARCHIVE, archive_path) ||
      !install_boot_entry_render(&paths, plan->record.layout.disk_guid,
        plan->record.layout.esp_guid, &plan->entry)) {
    return false;
  }
  plan->payload_request = (struct install_esp_payload_request){
    .revision = plan->record.revision, .attempt = plan->record.attempt,
    .sources = {plan->kernel, plan->rescue,
      {.memory = (uint8_t *)plan->revision_record, .bytes = strlen(plan->revision_record)},
      {.memory = plan->entry.bytes, .bytes = plan->entry.size}},
    .entry_present = true,
  };
  for (size_t i = 0; i < INSTALL_ESP_PAYLOAD_COUNT; ++i) {
    struct config_file_digest digest;
    if (!install_boot_payload_digest(&plan->payload_request.sources[i], &digest) ||
        !digest.present || !digest.size || digest.size > INSTALL_ESP_PAYLOAD_MAX_BYTES) {
      return false;
    }
    plan->payload_request.expected_payloads[i].size = digest.size;
    memcpy(plan->payload_request.expected_payloads[i].sha256, digest.sha256, sizeof(digest.sha256));
  }
  return true;
}

static bool esp_space_budget(struct install_plan *plan)
{
#if INSTALLER_PUBLICATION_SCRATCH
  return plan->installed.prepared;
#endif
  plan->esp_cluster_bytes = plan->esp_required_clusters = plan->esp_free_clusters = 0;
  if (!plan->record.coexist) {
    return true;
  }
  const struct mount_state_reply *state = NULL;
  for (size_t i = 0; i < plan->target.review.esp_count; ++i) {
    const struct install_review_esp *esp = &plan->target.review.esps[i];
    if (!memcmp(&esp->selection, &plan->target.esp, sizeof(esp->selection)) &&
        esp->available && esp->identity_matches && esp->retired) {
      state = &esp->observed;
      break;
    }
  }
  if (!state || !(state->flags & MOUNT_STATE_CAPACITY_VALID) ||
      !install_esp_payload_space(&plan->payload_request, state->cluster_bytes,
        &plan->esp_required_clusters)) {
    install_progress_pause();
    fputs("installer: ESP capacity is unknown; coexistence requires checked FAT32 free space; nothing was written\n", stderr);
    return false;
  }
  plan->esp_cluster_bytes = state->cluster_bytes;
  plan->esp_free_clusters = state->free_clusters;
  if (plan->esp_required_clusters > plan->esp_free_clusters) {
    install_progress_pause();
    fprintf(stderr, "installer: insufficient ESP space: fresh attempt needs %llu bytes, checked free space is %llu bytes; no old attempts are removed; nothing was written\n",
        (unsigned long long)(plan->esp_required_clusters * plan->esp_cluster_bytes),
        (unsigned long long)(plan->esp_free_clusters * plan->esp_cluster_bytes));
    return false;
  }
  return true;
}

bool install_plan_prepare(struct install_plan *plan, const struct disk_info *target)
{
  plan->ready = false;
#if INSTALLER_PUBLICATION_SCRATCH
  if (plan->view.root != HANDLE_INVALID || plan->view.control != HANDLE_INVALID) {
    return false;
  }
  install_installed_destroy(&plan->installed);
#endif
  uint64_t deadline;
  if (!cleanup_deadline(plan->clock, &deadline) ||
      !install_target_destroy(&plan->target, deadline)) {
    return false;
  }
  install_prior_destroy(&plan->prior);
  install_consent_destroy(&plan->consent);
  install_esp_destroy(plan->esp);
  plan->esp = NULL;
  free(plan->configuration);
  plan->configuration = NULL;
  plan->configuration_bytes = 0;
  config_blob_destroy(&plan->entry);
  if (!target || (target->block_size != 512 && target->block_size != 4096) ||
      target->block_count != plan->record.block_count ||
      target->block_size != plan->record.block_size ||
      target->block_count > UINT64_MAX / target->block_size) {
    return false;
  }
  plan->disk.info = *target;
  plan->disk.bytes = target->block_count * target->block_size;
  plan->raw_disk.info = *target;
  plan->raw_disk.bytes = plan->disk.bytes;
  install_progress_begin("Preparing complete target Review read-only", INSTALL_PROGRESS_NONE, NULL);
  if (!install_target_prepare(&plan->target, plan->disks, plan->clock, target,
      &plan->record, deadline, &plan->prior, &plan->consent,
      &plan->available_blocks, plan->record.target_hash)) {
    install_progress_pause();
    fprintf(stderr, "installer: target Review unavailable: %s (status %u)\n",
        plan->target.reason ? plan->target.reason : "inspection failed", plan->target.status);
    return false;
  }
  if (!plan->record.updating) {
    if (npfs_header_layout(plan->record.layout.pool_bytes / NPFS_BLOCK_SIZE,
          plan->record.journal_mib * INSTALL_MIB / NPFS_BLOCK_SIZE,
          plan->record.pool_id, &plan->header) != NPFS_OK) {
      return false;
    }
    uint64_t reserved = 2 + plan->header.bitmap_blocks + plan->header.volume_blocks +
        plan->header.journal_blocks;
    plan->available_blocks = plan->header.pool_blocks - reserved;
  }
  free(plan->rescue.memory);
  plan->rescue = (struct install_source){.handle = HANDLE_INVALID};
  if (!install_archive_filter(&plan->archive, plan->record.layout.pool_partition,
        &plan->programs, &plan->rescue)) {
    return false;
  }
  if (!plan->record.coexist) {
    plan->configuration = boot_configuration(plan->record.layout.disk_guid,
        &plan->configuration_bytes);
    if (!plan->configuration) {
      return false;
    }
  }
  size_t revision_bytes = strlen(plan->record.revision);
  memcpy(plan->revision_record, plan->record.revision, revision_bytes);
  plan->revision_record[revision_bytes++] = '\n';
  plan->revision_record[revision_bytes] = '\0';
#if INSTALLER_PUBLICATION_SCRATCH
  if (!plan->record.coexist && (!plan->record.updating || plan->record.esp_rebuild)) {
    plan->esp = install_esp_plan(&plan->raw_disk, &plan->record.layout, &plan->efi,
        NULL, NULL, plan->configuration, plan->configuration_bytes,
        plan->revision_record, revision_bytes, npfs_get_u32(plan->record.layout.esp_guid));
    if (!plan->esp) return false;
  }
  if (!install_installed_prepare(plan)) {
    return false;
  }
#endif
  if (!payload_plan(plan) || !esp_space_budget(plan) || !source_fingerprint(plan, plan->record.source_hash) ||
      !space_budget(plan) || !install_plan_record_valid(&plan->record)) {
    return false;
  }
  if (!plan->record.coexist) {
    /* Preserve the complete fixed-tree fit check even though raw initialization
     * writes only the bootstrap tree. Native staging must not discover a known
     * oversized payload after erase or recovery has begun. */
    struct install_esp *complete = install_esp_plan(&plan->raw_disk, &plan->record.layout,
        &plan->efi, &plan->kernel, &plan->rescue, plan->configuration,
        plan->configuration_bytes, plan->revision_record, revision_bytes,
        npfs_get_u32(plan->record.layout.esp_guid));
    if (!complete) {
      return false;
    }
    install_esp_destroy(complete);
  }
#if !INSTALLER_PUBLICATION_SCRATCH
  if (!plan->record.coexist && (!plan->record.updating || plan->record.esp_rebuild)) {
    /* Initialization is confined by reviewed erase/ESP-rebuild authority.
     * Kernel/archive are subsequently written through the native FAT view. */
    plan->esp = install_esp_plan(&plan->raw_disk, &plan->record.layout, &plan->efi,
        NULL, NULL, plan->configuration, plan->configuration_bytes,
        plan->revision_record, revision_bytes, npfs_get_u32(plan->record.layout.esp_guid));
    if (!plan->esp) {
      return false;
    }
  }
#endif
  plan->ready = true;
  install_progress_finish();
  return true;
}

bool install_plan_revalidate(struct install_plan *plan)
{
  uint64_t deadline;
  char source_hash[65];
  return plan->ready && cleanup_deadline(plan->clock, &deadline) &&
      install_target_revalidate(&plan->target, deadline) &&
      source_fingerprint(plan, source_hash) && !strcmp(source_hash, plan->record.source_hash);
}

static void report_name(FILE *output, const char *name)
{
  fputc('"', output);
  for (const unsigned char *byte = (const unsigned char *)name; *byte; byte++) {
    if (*byte < 32 || *byte > 126 || *byte == '"' || *byte == '\\') {
      static const char digits[] = "0123456789abcdef";
      fputs("\\x", output);
      fputc(digits[*byte >> 4], output);
      fputc(digits[*byte & 15], output);
    } else {
      fputc(*byte, output);
    }
  }
  fputc('"', output);
}

bool install_plan_report(const struct install_plan *plan, FILE *output, bool verbose)
{
  if (!plan->ready) {
    return false;
  }
  char guid[37];
  install_guid_text(plan->record.existing_guid, guid);
  fprintf(output, "Review: %s on Disk %llu (%llu MiB).\n"
      "Existing GUID: %s.\n",
      plan->record.updating ? "Update" : "Install", (unsigned long long)plan->disk.info.id,
      (unsigned long long)(plan->disk.bytes / INSTALL_MIB),
      plan->record.portable ? guid : "unavailable (interactive apply only)");
  if (plan->target.review.text_bytes &&
      fwrite(plan->target.review.text, 1, plan->target.review.text_bytes, output) !=
        plan->target.review.text_bytes) {
    return false;
  }
  fprintf(output, "Mode: %s; ESP entry %llu; pool entry %llu.\n",
      plan->record.coexist ? "coexistence" : "classic",
      (unsigned long long)plan->record.layout.esp_partition,
      (unsigned long long)plan->record.layout.pool_partition);
  if (verbose) {
    fprintf(output, "ESP: byte %llu, length %llu; pool: byte %llu, length %llu.\n",
        (unsigned long long)plan->record.layout.esp_start,
        (unsigned long long)plan->record.layout.esp_bytes,
        (unsigned long long)plan->record.layout.pool_start,
        (unsigned long long)plan->record.layout.pool_bytes);
  }
  if (!plan->record.updating) {
    fprintf(output, "Journal: %llu MiB; Read the room: %s.\n",
        (unsigned long long)plan->record.journal_mib,
        plan->record.read_the_room ? "yes" : "no");
  } else {
    fprintf(output, "Previous revision: %s; explicit legacy scan: %s.\n",
        plan->record.previous, plan->record.legacy ? "yes" : "no");
  }
  fprintf(output, "Programs revision: %s\nSelected payload: %zu files, %llu bytes.\n",
      plan->record.revision, plan->manifest.files, (unsigned long long)plan->manifest.bytes);
  if (plan->record.updating && plan->prior.state == INSTALL_PRIOR_UNKNOWN) {
    fprintf(output, "Programs: %zu with unknown changes (%s).\n"
        "Updated/new/removed/unchanged counts and old sizes are unknown.\n",
        plan->programs.count, plan->prior.reason);
    for (size_t i = 0; verbose && i < plan->programs.count; i++) {
      const char *name = plan->programs.entries[i].name;
      size_t length = strlen(name);
      uint64_t bytes = 0;
      for (size_t entry = 0; entry < plan->manifest.count; entry++) {
        const struct install_manifest_entry *record = &plan->manifest.entries[entry];
        if (!strncmp(record->path, name, length) &&
            (!record->path[length] || record->path[length] == '/')) {
          bytes += record->size;
        }
      }
      fputs("  unknown ", output);
      report_name(output, name);
      fprintf(output, "; old size unknown, new %llu bytes\n", (unsigned long long)bytes);
    }
  } else {
    const struct install_manifest *previous = plan->record.updating ? &plan->prior.inventory : NULL;
    if (!install_manifest_report(output, previous, &plan->manifest, verbose)) {
      return false;
    }
  }
  fprintf(output, "Staging reserve: %llu blocks; available %llu.\n",
      (unsigned long long)plan->required_blocks, (unsigned long long)plan->available_blocks);
#if INSTALLER_PUBLICATION_SCRATCH
  install_installed_report(plan, output, verbose);
  if (!plan->record.updating && !plan->record.coexist) {
    fputs("ERASE: the entire disk will be rebuilt; every existing partition and volume\n"
        "will be destroyed, including any Fedora or Windows installation.\n", output);
    if (plan->consent.volume_labels) fprintf(output, "Existing volumes:\n%s\n", plan->consent.volume_labels);
  } else if (plan->record.esp_rebuild) {
    fputs("RECOVERY: the explicitly reviewed damaged ESP will be initialized; other partitions stay.\n", output);
  } else if (!plan->record.updating) {
    fputs("Append only the reviewed pool GPT entry and required CRCs; format only that new pool.\n", output);
  }
  fputs("The reviewed source commits before the owned menu changes; unknown menu ownership\n"
      "retains the menu and reports boot activation pending. No Update formats a healthy ESP.\n", output);
#else
  if (plan->record.coexist) {
    fprintf(output, "ESP staging reserve: %llu bytes; checked free space %llu bytes.\n",
        (unsigned long long)(plan->esp_required_clusters * plan->esp_cluster_bytes),
        (unsigned long long)(plan->esp_free_clusters * plan->esp_cluster_bytes));
    fputs(plan->record.updating ?
        "The selected pool is updated; GPT and all other partitions stay.\n" :
        "Only the reviewed unused GPT slot and required CRCs are appended, then\n"
        "only the new pool is formatted. A failed append can leave partial GPT effects.\n", output);
    fprintf(output, "ESP payloads use EFI/pyxis/%s/%s/.\n",
        plan->record.revision, plan->record.attempt);
    fputs("No firmware, Fedora, Windows or Limine configuration is changed.\n"
        "The verified entry is saved and printed for manual addition.\n", output);
  } else if (plan->record.updating) {
    fputs(plan->record.esp_rebuild ?
        "RECOVERY: the explicitly reviewed ESP will be initialized; other partitions stay.\n" :
        "GPT and volumes stay; fixed boot files are replaced through native FAT files.\n"
        "The healthy ESP is not formatted.\n", output);
  } else {
    fputs("ERASE: the entire disk will be rebuilt; every existing partition and volume\n"
        "will be destroyed, including any Fedora or Windows installation.\n", output);
    if (plan->consent.volume_labels) {
      fprintf(output, "Existing volumes:\n%s\n", plan->consent.volume_labels);
    }
  }
#endif
  if (plan->record.updating) {
    if (!strcmp(plan->record.previous, INSTALL_UNKNOWN_REVISION)) {
      fputs("Previous program revision unknown; old revision cleanup is skipped.\n", output);
    } else if (!strcmp(plan->record.previous, plan->record.revision)) {
      fputs("Programs reuse the previous revision tree.\n", output);
    } else {
      fputs("Cleanup excludes the known previous program revision.\n", output);
    }
    if (!strcmp(plan->record.previous, plan->record.revision) ||
        !strcmp(plan->record.previous, INSTALL_UNKNOWN_REVISION)) {
      fputs("A failed program stage can leave a previously used revision incomplete.\n", output);
    }
  }
  if (!verbose) {
    fputs("Details: use -v or Print plan for program changes.\n", output);
  }
  fputs("Programs are synced/verified before kernel/archive publication. Failure is partial,\n"
      "with no whole-install rollback or automatic retry.\n", output);
  return !ferror(output);
}

void install_plan_destroy(struct install_plan *plan)
{
  install_read_fingerprint_abort();
  if (!install_plan_finish_execution(plan)) {
    fputs("installer: execution cleanup remains pending; inspect the reported handles before retrying\n", stderr);
  }
  if (plan->target.initialized) {
    uint64_t deadline;
    if (!cleanup_deadline(plan->clock, &deadline) ||
        !install_target_destroy(&plan->target, deadline)) {
      fputs("installer: target Review cleanup remains pending\n", stderr);
    }
  }
  install_esp_destroy(plan->esp);
  install_prior_destroy(&plan->prior);
  install_consent_destroy(&plan->consent);
  install_manifest_destroy(&plan->manifest);
  install_programs_destroy(&plan->programs);
  free(plan->manifest_text);
  free(plan->configuration);
  config_blob_destroy(&plan->entry);
  free(plan->rescue.memory);
#if INSTALLER_PUBLICATION_SCRATCH
  install_installed_destroy(&plan->installed);
#endif
  if (plan->efi.handle != HANDLE_INVALID) {
    handle_close(plan->efi.handle);
  }
}
