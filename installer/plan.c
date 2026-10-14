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

static char *boot_configuration(const struct install_source *source, const uint8_t guid[16],
    size_t *bytes)
{
  if (source->bytes > SIZE_MAX - 512) {
    return NULL;
  }
  size_t size = (size_t)source->bytes;
  install_progress_begin("Reading boot configuration template", INSTALL_PROGRESS_SOURCE_READ, NULL);
  install_progress_bytes_total(size);
  char *input = malloc(size + 1), *output = malloc(size + 512);
  if (!input || !output || !install_source_read(source, 0, input, size)) {
    free(input);
    free(output);
    return NULL;
  }
  input[size] = '\0';
  if (memchr(input, '\0', size)) {
    free(input);
    free(output);
    return NULL;
  }
  char disk_guid[37], normal[256], rescue[256], menu_timeout[32];
  install_guid_text(guid, disk_guid);
  snprintf(normal, sizeof(normal),
      "  cmdline: " INSTALL_BOOT_INIT_OPTION " mount.disk=%s\n", disk_guid);
  snprintf(rescue, sizeof(rescue),
      "  cmdline: " INSTALL_BOOT_INIT_OPTION " mount.disk=%s " INSTALL_RESCUE_OPTION "\n",
      disk_guid);
  snprintf(menu_timeout, sizeof(menu_timeout), "timeout: %d\n", INSTALL_MENU_TIMEOUT_SECONDS);
  bool skip = false, timeout = false, command = false, rescue_command = false;
  bool install_entry = false;
  size_t used = 0;
  char *line = input;
  while (*line) {
    char *end = strchr(line, '\n');
    size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length == strlen("/Install Pyxis") && !memcmp(line, "/Install Pyxis", length)) {
      skip = true;
      install_entry = true;
    } else if (length && line[0] == '/') {
      skip = false;
    }
    bool default_entry = length >= strlen("default_entry:") &&
      !memcmp(line, "default_entry:", strlen("default_entry:"));
    if (!skip && !default_entry) {
      const char *replacement = NULL;
      if (length == strlen("# PYXIS_BOOT_MENU_TIMEOUT") &&
          !memcmp(line, "# PYXIS_BOOT_MENU_TIMEOUT", length)) {
        if (timeout) {
          goto invalid;
        }
        timeout = true;
        replacement = menu_timeout;
      } else if (length == strlen("# PYXIS_NORMAL_COMMAND_LINE") &&
          !memcmp(line, "# PYXIS_NORMAL_COMMAND_LINE", length)) {
        if (command) {
          goto invalid;
        }
        command = true;
        replacement = normal;
      } else if (length == strlen("# PYXIS_RESCUE_COMMAND_LINE") &&
          !memcmp(line, "# PYXIS_RESCUE_COMMAND_LINE", length)) {
        if (rescue_command) {
          goto invalid;
        }
        rescue_command = true;
        replacement = rescue;
      }
      size_t copy = replacement ? strlen(replacement) : length;
      if (copy + 1 > size + 512 - used) {
        goto invalid;
      }
      memcpy(output + used, replacement ? replacement : line, copy);
      used += copy;
      if (!replacement) {
        output[used++] = '\n';
      }
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  if (!timeout || !command || !rescue_command || !install_entry) {
    goto invalid;
  }
  output[used] = '\0';
  *bytes = used;
  free(input);
  return output;

invalid:
  free(input);
  free(output);
  return NULL;
}

static bool inspect_target(const struct install_plan *plan, struct install_disk *disk,
    struct install_prior *prior, struct install_consent *consent, uint64_t *free_blocks,
    char fingerprint[65])
{
  bool ok = install_read_fingerprint_begin();
  if (ok && plan->record.updating) {
    struct install_update update;
    ok = install_scan_update(disk, &update) && update.eligible;
    const char *reason = NULL;
    if (ok) {
      enum install_esp_state state = install_esp_inspect(disk, &update.layout,
          update.revision, &reason);
      ok = state != INSTALL_ESP_REFUSED &&
          !memcmp(&update.layout, &plan->record.layout, sizeof(update.layout)) &&
          !strcmp(update.revision, plan->record.previous);
      *free_blocks = update.free_blocks;
    }
    if (ok) {
      ok = install_prior_read(disk, &update, plan->record.legacy, prior);
    }
  } else if (ok) {
    ok = install_scan_consent(disk, plan->record.read_the_room, consent) && consent->eligible &&
        !memcmp(consent->existing_guid, plan->record.existing_guid, 16);
  }
  if (ok) {
    ok = install_read_fingerprint_finish(fingerprint);
  } else {
    install_read_fingerprint_abort();
  }
  return ok;
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
  uint8_t flags[] = {record->updating, record->read_the_room, record->legacy, record->portable};
  uint8_t numbers[7 * 8];
  npfs_put_u64(numbers, record->block_count);
  npfs_put_u64(numbers + 8, record->block_size);
  npfs_put_u64(numbers + 16, record->journal_mib);
  npfs_put_u64(numbers + 24, record->layout.esp_start);
  npfs_put_u64(numbers + 32, record->layout.esp_bytes);
  npfs_put_u64(numbers + 40, record->layout.pool_start);
  npfs_put_u64(numbers + 48, record->layout.pool_bytes);
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
      install_digest_update(digest, record->previous, strlen(record->previous) + 1);
}

static bool source_fingerprint(const struct install_plan *plan, char output[65])
{
  struct install_digest digest = {0};
  install_progress_begin("Hashing immutable install sources", INSTALL_PROGRESS_WORK, NULL);
  bool ok = install_digest_begin(&digest) && hash_choices(&digest, &plan->record) &&
      hash_source(&digest, &plan->kernel) && hash_source(&digest, &plan->archive) &&
      hash_source(&digest, &plan->efi) && hash_source(&digest, &plan->template) &&
      hash_source(&digest, &plan->rescue) &&
      install_digest_update(&digest, plan->manifest_text, plan->manifest_bytes) &&
      install_digest_update(&digest, plan->configuration, plan->configuration_bytes) &&
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
    .kernel.handle = kernel, .archive.handle = archive,
    .efi.handle = HANDLE_INVALID, .template.handle = HANDLE_INVALID,
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
      source_open("boot://share/installer/limine.conf.template", &plan->template) &&
      file_size(kernel, &plan->kernel.bytes) == CALL_OK && plan->kernel.bytes &&
      file_size(archive, &plan->archive.bytes) == CALL_OK && plan->archive.bytes &&
      install_programs_list(boot, &plan->programs) &&
      install_archive_filter(&plan->archive, &plan->programs, &plan->rescue) &&
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

bool install_plan_prepare(struct install_plan *plan, const struct disk_info *target)
{
  plan->ready = false;
  install_prior_destroy(&plan->prior);
  install_consent_destroy(&plan->consent);
  install_esp_destroy(plan->esp);
  plan->esp = NULL;
  free(plan->configuration);
  plan->configuration = NULL;
  if (!target->block_size || target->block_size < 512 ||
      target->block_size > NPFS_BLOCK_SIZE ||
      (target->block_size & (target->block_size - 1)) ||
      target->block_count != plan->record.block_count ||
      target->block_size != plan->record.block_size ||
      target->block_count > UINT64_MAX / target->block_size) {
    return false;
  }
  plan->disk.info = *target;
  plan->disk.bytes = target->block_count * target->block_size;
  if (plan->record.updating) {
    plan->available_blocks = 0;
  } else {
    if (npfs_header_layout(plan->record.layout.pool_bytes / NPFS_BLOCK_SIZE,
          plan->record.journal_mib * INSTALL_MIB / NPFS_BLOCK_SIZE,
          plan->record.pool_id, &plan->header) != NPFS_OK) {
      return false;
    }
    uint64_t reserved = 2 + plan->header.bitmap_blocks + plan->header.volume_blocks +
        plan->header.journal_blocks;
    plan->available_blocks = plan->header.pool_blocks - reserved;
  }
  install_progress_begin("Rechecking reviewed target read-only", INSTALL_PROGRESS_NONE, NULL);
  enum call_status status = disks_open(plan->disks, target->id,
      DISK_ACCESS_READ_ONLY, &plan->disk.handle);
  if (status != CALL_OK) {
    return false;
  }
  bool ok = inspect_target(plan, &plan->disk, &plan->prior, &plan->consent,
      &plan->available_blocks, plan->record.target_hash);
  bool closed = handle_close(plan->disk.handle) == 0;
  plan->disk.handle = HANDLE_INVALID;
  if (!ok || !closed || plan->prior.needs_legacy) {
    return false;
  }
  plan->configuration = boot_configuration(&plan->template, plan->record.layout.disk_guid,
      &plan->configuration_bytes);
  if (!plan->configuration || !source_fingerprint(plan, plan->record.source_hash) || !space_budget(plan) ||
      !install_plan_record_valid(&plan->record)) {
    return false;
  }
  size_t revision_bytes = strlen(plan->record.revision);
  memcpy(plan->revision_record, plan->record.revision, revision_bytes);
  plan->revision_record[revision_bytes++] = '\n';
  /* The ESP plan borrows these held text and source views until execution and
   * verification finish. */
  plan->esp = install_esp_plan(&plan->disk, &plan->record.layout, &plan->efi,
      &plan->kernel, &plan->rescue, plan->configuration, plan->configuration_bytes,
      plan->revision_record, revision_bytes, npfs_get_u32(plan->record.layout.esp_guid));
  plan->ready = plan->esp != NULL;
  install_progress_finish();
  return plan->ready;
}

bool install_plan_revalidate(const struct install_plan *plan)
{
  struct install_disk disk = plan->disk;
  struct disk_info current = {0};
  struct install_prior prior = {0};
  struct install_consent consent = {0};
  char fingerprint[65], source_hash[65];
  uint64_t free_blocks = plan->available_blocks;
  bool ok = disk_get_info(disk.handle, &current) == CALL_OK;
  disk.info = current;
  ok = ok && current.block_count == plan->record.block_count && current.block_size == plan->record.block_size &&
      inspect_target(plan, &disk, &prior, &consent, &free_blocks, fingerprint) &&
      !prior.needs_legacy && !strcmp(fingerprint, plan->record.target_hash) &&
      (!plan->record.updating || free_blocks >= plan->required_blocks);
  ok = ok && source_fingerprint(plan, source_hash) &&
      !strcmp(source_hash, plan->record.source_hash);
  install_prior_destroy(&prior);
  install_consent_destroy(&consent);
  return ok;
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
  if (plan->record.updating) {
    fputs("GPT and volumes stay; the whole ESP will be replaced.\n"
        "Removed programs are absent from the new revision.\n", output);
    if (!strcmp(plan->record.previous, INSTALL_UNKNOWN_REVISION)) {
      fputs("Previous program revision unknown; old revision cleanup is skipped.\n", output);
    } else if (!strcmp(plan->record.previous, plan->record.revision)) {
      fputs("Programs reuse the previous revision tree.\n", output);
    } else {
      fputs("Cleanup excludes the known previous program revision.\n", output);
    }
    if (!strcmp(plan->record.previous, plan->record.revision) ||
        !strcmp(plan->record.previous, INSTALL_UNKNOWN_REVISION)) {
      fputs("A failed program stage can leave the previously used revision incomplete.\n", output);
    }
  } else {
    fputs("The entire disk will be rebuilt; all existing volumes will be destroyed.\n", output);
    if (verbose) {
      fputs("Existing volumes:\n", output);
      fprintf(output, "%s\n", plan->consent.volume_labels ? plan->consent.volume_labels : "  none readable");
    }
  }
  if (!verbose) {
    fputs("Details: use -v or Print plan for programs and existing volumes.\n", output);
  }
  fputs("Programs are synced/verified before replacing EFI. Failure is partial,\n"
      "with no whole-install rollback; use live-media recovery.\n", output);
  return !ferror(output);
}

void install_plan_destroy(struct install_plan *plan)
{
  install_read_fingerprint_abort();
  install_esp_destroy(plan->esp);
  install_prior_destroy(&plan->prior);
  install_consent_destroy(&plan->consent);
  install_manifest_destroy(&plan->manifest);
  install_programs_destroy(&plan->programs);
  free(plan->manifest_text);
  free(plan->configuration);
  free(plan->rescue.memory);
  if (plan->efi.handle != HANDLE_INVALID) {
    handle_close(plan->efi.handle);
  }
  if (plan->template.handle != HANDLE_INVALID) {
    handle_close(plan->template.handle);
  }
}
