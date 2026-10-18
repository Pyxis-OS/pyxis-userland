#include "installed_publication.h"
#include "plan.h"
#include "pending.h"
#include "pending_archive.h"
#include "publication_snapshot.h"
#include "esp_directory.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <pyxis_fs/fat32.h>
#include <stdlib.h>
#include <string.h>

#if INSTALLER_PUBLICATION_SCRATCH

#define INSTALLED_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define INSTALLED_RESERVE_CLUSTERS UINT64_C(32)

static const char *const current_paths[CONFIG_ALL_FILES] = {
  "EFI/pyxis/config/boot.lua", "EFI/pyxis/config/session.lua", "EFI/pyxis/config/network.lua",
  "EFI/pyxis/config/catalog.lua", "boot/limine/limine.conf"
};

static bool blob_hash(struct install_digest *digest, const struct config_blob *blob)
{
  unsigned char present = blob->digest.present;
  uint8_t size[8];
  npfs_put_u64(size, blob->size);
  return install_digest_update(digest, &present, sizeof(present)) &&
      install_digest_update(digest, size, sizeof(size)) &&
      install_digest_update(digest, blob->bytes, blob->size);
}

static bool facts_match(const struct install_plan *plan, const struct config_target_facts *facts)
{
  const struct disk_partition_selection *selected = &plan->target.esp;
  return config_target_valid(facts) && facts->device == selected->id &&
      facts->partition == selected->partition && facts->first_block == selected->first_block &&
      facts->block_count == selected->block_count &&
      facts->device_block_size == selected->device_block_size &&
      facts->device_block_count == selected->device_block_count &&
      facts->partition_attributes == selected->attributes &&
      !memcmp(facts->disk_guid, selected->disk_guid, 16) &&
      !memcmp(facts->partition_guid, selected->partition_guid, 16) &&
      !memcmp(facts->partition_type, selected->partition_type, 16);
}

static bool read_current(handle_t root, struct config_blob current[CONFIG_ALL_FILES],
    struct config_blob *record)
{
  bool ok = true;
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    ok = install_snapshot_file(root, current_paths[i], CONFIG_FILE_MAX_BYTES, &current[i]);
  }
  return ok && install_snapshot_file(root, "EFI/pyxis/config/config.record", CONFIG_RECORD_MAX_BYTES, record);
}

static bool recovery_resolved(handle_t root, const struct config_blob *record)
{
  struct config_record parsed = {0};
  if (!record->digest.present) return true;
  if (!config_record_parse(record->bytes, record->size, &parsed) || !parsed.committed) return false;
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!parsed.obsolete[i].digest.present) continue;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", i == CONFIG_LIMINE ? "boot/limine" : "EFI/pyxis/config",
        parsed.obsolete[i].name);
    struct config_blob artifact = {0};
    bool ok = install_snapshot_file(root, path, CONFIG_FILE_MAX_BYTES, &artifact);
    bool present = artifact.digest.present;
    config_blob_destroy(&artifact);
    if (!ok || present) {
      fputs("installer: retained recovery needs a fresh pending completion Review or manual disposition\n", stderr);
      return false;
    }
  }
  return true;
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

void install_installed_destroy(struct install_installed_publication *publication)
{
  install_configuration_free(&publication->configuration);
  install_recovery_destroy(&publication->recovery);
  *publication = (struct install_installed_publication){0};
}

static bool binding_prepare(struct install_plan *plan)
{
  struct config_boot_binding *binding = &plan->installed.binding;
  binding->present = true;
  binding->source = plan->installed.configuration.source;
  strcpy(binding->kernel_revision, plan->record.revision);
  strcpy(binding->program_revision, plan->record.revision);
  strcpy(binding->attempt, plan->record.attempt);
  binding->archive_catalog = plan->installed.configuration.catalog_source.digest;
  binding->legacy_override = plan->installed.configuration.pool_override.digest;
  binding->legacy_disposition = plan->installed.configuration.pool_disposition;
  struct config_boot_payload *files[] = {&binding->kernel, &binding->archive, &binding->revision};
  const char *names[] = {"kernel.elf", "boot.arc", "revision.txt"};
  struct install_source sources[] = {plan->kernel, plan->rescue,
    {.memory = (uint8_t *)plan->revision_record, .bytes = strlen(plan->revision_record)}};
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    if (!config_boot_payload_path(binding->kernel_revision, binding->attempt, names[i], files[i]->path) ||
        !install_boot_payload_digest(&sources[i], &files[i]->digest)) {
      return false;
    }
  }
  return config_boot_binding_valid(binding);
}

static uint64_t required_space(struct install_plan *plan, uint64_t cluster)
{
  const struct install_installed_publication *publication = &plan->installed;
  if (!cluster || cluster > UINT64_MAX / INSTALLED_RESERVE_CLUSTERS) return UINT64_MAX;
  uint64_t total = INSTALLED_RESERVE_CLUSTERS * cluster;
  const uint64_t sizes[] = {publication->binding.kernel.digest.size,
    publication->binding.archive.digest.size, publication->binding.revision.digest.size,
    CONFIG_RECORD_MAX_BYTES, CONFIG_RECORD_MAX_BYTES, CONFIG_RECORD_MAX_BYTES};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    uint64_t blocks = sizes[i] / cluster + (sizes[i] % cluster != 0);
    if (blocks > (UINT64_MAX - total) / cluster) return UINT64_MAX;
    total += blocks * cluster;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    uint64_t copies[] = {publication->configuration.current[i].size,
      publication->configuration.after[i].size};
    for (unsigned n = 0; n < 2; ++n) {
      uint64_t blocks = copies[n] / cluster + (copies[n] % cluster != 0);
      if (blocks > (UINT64_MAX - total) / cluster) return UINT64_MAX;
      total += blocks * cluster;
    }
  }
  uint64_t recovery = install_recovery_space(&publication->recovery, cluster);
  return recovery > UINT64_MAX - total ? UINT64_MAX : total + recovery;
}

bool install_installed_prepare(struct install_plan *plan)
{
  struct install_installed_publication *publication = &plan->installed;
  install_installed_destroy(publication);
  publication->fresh = !plan->record.coexist && (!plan->record.updating || plan->record.esp_rebuild);
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0}, pool = {0};
  struct config_blob defaults[CONFIG_SET_FILES] = {0};
  bool legacy = false, ok = install_pending_archive_configs(&plan->rescue, defaults);
  struct install_disk reader = {.handle = HANDLE_INVALID, .info = plan->disk.info,
    .bytes = plan->disk.bytes};
  if (ok && plan->record.updating) {
    ok = disks_open(plan->disks, plan->disk.info.id, DISK_ACCESS_READ_ONLY, &reader.handle) == CALL_OK &&
        install_pool_boot_snapshot_at(&reader, plan->record.layout.pool_partition, &pool);
  }
  if (reader.handle != HANDLE_INVALID) {
    ok = handle_close(reader.handle) == CALL_OK && ok;
  }
  if (ok && publication->fresh) {
    const struct disk_partition_selection *selected = &plan->target.esp;
    publication->facts = (struct config_target_facts){.device = selected->id,
      .partition = selected->partition, .first_block = selected->first_block,
      .block_count = selected->block_count, .device_block_size = selected->device_block_size,
      .device_block_count = selected->device_block_count, .partition_attributes = selected->attributes};
    memcpy(publication->facts.disk_guid, selected->disk_guid, 16);
    memcpy(publication->facts.partition_guid, selected->partition_guid, 16);
    memcpy(publication->facts.partition_type, selected->partition_type, 16);
    ok = config_blob_set(&current[CONFIG_LIMINE], plan->configuration, plan->configuration_bytes) &&
        install_recovery_prepare_fresh(plan->boot, plan->kernel.handle,
            &publication->facts, &publication->recovery) &&
        install_esp_capacity(plan->esp, &publication->cluster_bytes, &publication->available_bytes);
  } else if (ok) {
    struct mount_fat32_reply mounted = {0};
    enum call_status status = disks_open_owned_esp(plan->disks, &plan->target.esp,
        MOUNT_ACCESS_READ_ONLY, INSTALLED_READ, &mounted);
    plan->view.root = mounted.root;
    plan->view.control = mounted.control;
    ok = status == CALL_OK && install_snapshot_facts(mounted.control, &publication->facts) &&
        facts_match(plan, &publication->facts) && read_current(mounted.root, current, &record) &&
        recovery_resolved(mounted.root, &record) &&
        install_recovery_menu_review(mounted.root, &record, &publication->facts) &&
        install_recovery_prepare(mounted.root, plan->boot, plan->kernel.handle, plan->workspace,
            &publication->facts, &publication->recovery);
    if (ok) {
      struct mount_state_reply state;
      ok = mount_observe(mounted.control, &state) == CALL_OK &&
          (state.flags & MOUNT_STATE_CAPACITY_VALID) && state.cluster_bytes &&
          state.free_clusters <= UINT64_MAX / state.cluster_bytes;
      if (ok) {
        publication->cluster_bytes = state.cluster_bytes;
        publication->available_bytes = state.free_clusters * state.cluster_bytes;
        struct config_record inherited = {0};
        bool owned = record.digest.present && config_record_parse(record.bytes, record.size, &inherited) &&
            inherited.committed && inherited.boot.present;
        if (!owned) legacy = install_boot_legacy_certify(mounted.root, publication->legacy);
      }
    }
    ok = close_planning(plan) && ok;
  }
  struct install_configuration_input input = {.current = current, .record = &record,
    .archive = defaults, .catalog = &defaults[CONFIG_CATALOG], .pool_override = &pool,
    .kernel_revision = plan->record.revision, .program_revision = plan->record.revision};
  ok = ok && install_configuration_prepare(&input, &publication->configuration);
  if (ok && !publication->configuration.applicable) {
    install_configuration_review(&publication->configuration, stderr);
    fputs("installer: configuration needs explicit choices; use a fresh configuration Review; nothing written\n", stderr);
    ok = false;
  }
  if (ok) ok = binding_prepare(plan);
  if (ok) {
    const struct install_boot_entry_paths next = {publication->binding.kernel.path,
      publication->binding.archive.path};
    if (publication->fresh) {
      ok = install_boot_entry_render_owned(&next, plan->record.layout.disk_guid,
          &publication->configuration.after[CONFIG_LIMINE]);
    } else {
      struct config_record previous = {0};
      bool recorded = record.digest.present && config_record_parse(record.bytes, record.size, &previous) &&
          previous.committed && previous.boot.present;
      const struct install_boot_entry_paths old = {previous.boot.kernel.path, previous.boot.archive.path};
      bool adoption = false;
      ok = install_boot_entry_plan(&current[CONFIG_LIMINE], recorded ? &old : NULL,
          plan->record.layout.disk_guid, &next, legacy,
          &publication->configuration.after[CONFIG_LIMINE], &adoption, &publication->activation_pending);
      publication->legacy_active = adoption;
      if (ok && recorded && publication->activation_pending) {
        fputs("installer: pending or ambiguous boot activation requires a fresh pending completion Review; nothing written\n", stderr);
        ok = false;
      }
    }
  }
  if (ok) {
    publication->required_bytes = required_space(plan, publication->cluster_bytes);
    ok = publication->required_bytes <= publication->available_bytes;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) config_blob_destroy(&current[i]);
  for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) config_blob_destroy(&defaults[i]);
  config_blob_destroy(&record);
  config_blob_destroy(&pool);
  publication->prepared = ok;
  return ok;
}

bool install_installed_hash(const struct install_plan *plan, struct install_digest *digest)
{
  const struct install_installed_publication *publication = &plan->installed;
  static const char domain[] = "pyxis-installed-publication-1";
  unsigned char choices[] = {publication->configuration.source, publication->configuration.pool_disposition,
    publication->legacy_active, publication->activation_pending, publication->fresh};
  bool ok = publication->prepared && install_digest_update(digest, domain, sizeof(domain)) &&
      install_digest_update(digest, choices, sizeof(choices));
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    ok = blob_hash(digest, &publication->configuration.current[i]) &&
        blob_hash(digest, &publication->configuration.after[i]);
  }
  ok = ok && blob_hash(digest, &publication->configuration.record) &&
      blob_hash(digest, &publication->configuration.pool_override) &&
      blob_hash(digest, &publication->configuration.catalog_source) &&
      blob_hash(digest, &publication->recovery.manifest) && blob_hash(digest, &publication->recovery.fallback);
  for (unsigned i = 0; ok && i < INSTALL_BOOT_PAYLOADS; ++i) {
    unsigned char present = publication->legacy[i].present;
    uint8_t size[8];
    npfs_put_u64(size, publication->legacy[i].size);
    ok = install_digest_update(digest, &present, sizeof(present)) &&
        install_digest_update(digest, size, sizeof(size)) &&
        install_digest_update(digest, publication->legacy[i].sha256, 65);
  }
  return ok;
}

void install_installed_report(const struct install_plan *plan, FILE *output, bool verbose)
{
  if (verbose) install_configuration_review(&plan->installed.configuration, output);
  else install_configuration_summary(&plan->installed.configuration, output);
  fprintf(output, "Boot attempt: EFI/pyxis/%s/%s; ESP reserve %llu bytes, capacity bound %llu.\n",
      plan->record.revision, plan->record.attempt,
      (unsigned long long)plan->installed.required_bytes, (unsigned long long)plan->installed.available_bytes);
  fputs(plan->installed.recovery.present ? "Verified static recovery is retained.\n" :
      "Provision the reviewed immutable recovery pair and fallback before menu activation.\n", output);
  struct config_record history = {0};
  const struct config_blob *record = &plan->installed.configuration.record;
  if (record->digest.present && config_record_parse(record->bytes, record->size, &history) && history.boot.present) {
    fprintf(output, "Retain previous boot kernel %s (%s) and archive %s (%s).\n",
        history.boot.kernel.path, history.boot.kernel.digest.sha256,
        history.boot.archive.path, history.boot.archive.digest.sha256);
  }
  if (plan->installed.legacy_active) fputs("Adopt the exact verified legacy normal/rescue pair.\n", output);
  if (plan->installed.activation_pending) {
    fputs("Existing menu remains; boot activation will be pending.\n", output);
    const struct install_boot_entry_paths next = {plan->installed.binding.kernel.path,
      plan->installed.binding.archive.path};
    install_boot_entry_print(&next, plan->record.layout.disk_guid, output);
  }
}

static bool parent_open(handle_t root, const char *path, handle_t *output)
{
  handle_t parent = root;
  bool owned = false;
  while (*path) {
    const char *slash = strchr(path, '/');
    size_t size = slash ? (size_t)(slash - path) : strlen(path);
    char name[64];
    if (!size || size >= sizeof(name)) break;
    memcpy(name, path, size);
    name[size] = 0;
    handle_t next = HANDLE_INVALID;
    enum call_status status = install_esp_child_open(parent, name, DIRECTORY_KIND_DIRECTORY,
        INSTALL_ESP_VIEW_RIGHTS, true, &next);
    if (status == CALL_OK) status = directory_sync(parent);
    if (owned && handle_close(parent) != CALL_OK) status = CALL_OUTCOME_UNKNOWN;
    owned = false;
    if (status != CALL_OK) {
      if (next != HANDLE_INVALID) handle_close(next);
      return false;
    }
    parent = next;
    owned = true;
    if (!slash) {
      *output = parent;
      return true;
    }
    path = slash + 1;
  }
  if (owned) handle_close(parent);
  return false;
}

bool install_installed_admit(struct install_plan *plan)
{
  struct install_installed_publication *publication = &plan->installed;
  if (!publication->prepared || plan->view.root != HANDLE_INVALID || plan->view.control != HANDLE_INVALID) return false;
  enum call_status status = install_esp_view_open(&plan->view, plan->disks, &plan->target.esp,
      NULL, INSTALL_ESP_VIEW_RIGHTS);
  struct config_target_facts facts;
  bool ok = status == CALL_OK && install_snapshot_facts(plan->view.control, &facts) && facts_match(plan, &facts);
  if (ok) {
    status = install_esp_view_begin_checked(&plan->view);
    ok = status == CALL_OK;
  }
  if (ok) {
    struct mount_state_reply state;
    ok = mount_observe(plan->view.control, &state) == CALL_OK &&
        (state.flags & MOUNT_STATE_CAPACITY_VALID) &&
        state.cluster_bytes == publication->cluster_bytes && state.cluster_bytes &&
        state.free_clusters <= UINT64_MAX / state.cluster_bytes &&
        publication->required_bytes <= state.free_clusters * state.cluster_bytes;
  }
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0}, pool = {0};
  ok = ok && read_current(plan->view.root, current, &record);
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    ok = config_blob_equal(&current[i], &publication->configuration.current[i]);
  }
  ok = ok && config_blob_equal(&record, &publication->configuration.record);
  if (ok && plan->record.updating) {
    /* Pool admission is already selected and released. Open only its existing
     * native system root for this consumed override; never raw-read a mount. */
    handle_t system = HANDLE_INVALID;
    status = disk_open_volume(plan->disk.handle, plan->target.pool.partition, "system", INSTALLED_READ, &system);
    ok = status == CALL_OK && install_snapshot_file(system, "config/boot.lua", CONFIG_FILE_MAX_BYTES, &pool) &&
        config_blob_equal(&pool, &publication->configuration.pool_override);
    if (system != HANDLE_INVALID) ok = handle_close(system) == CALL_OK && ok;
  }
  if (ok) {
    struct install_recovery current_recovery = {0};
    ok = install_recovery_menu_review(plan->view.root, &record, &facts) &&
        install_recovery_prepare(plan->view.root, plan->boot, plan->kernel.handle,
            plan->workspace, &facts, &current_recovery) &&
        current_recovery.present == publication->recovery.present &&
        config_blob_equal(&current_recovery.manifest, &publication->recovery.manifest) &&
        config_blob_equal(&current_recovery.fallback, &publication->recovery.fallback);
    install_recovery_destroy(&current_recovery);
  }
  if (ok && publication->legacy_active) {
    struct config_storage_report report = {0};
    ok = install_publication_legacy_check(plan->view.root, plan->workspace, publication->legacy, &report);
  }
  struct config_record history = {0};
  struct install_esp_payloads *held = NULL;
  if (ok && record.digest.present && config_record_parse(record.bytes, record.size, &history) &&
      history.boot.present) {
    struct install_esp_payload_request open = {.root = plan->view.root, .control = plan->view.control,
      .workspace = plan->workspace, .expected = plan->view.expected,
      .revision = history.boot.kernel_revision, .attempt = history.boot.attempt};
    const struct config_boot_payload *files[] = {&history.boot.kernel, &history.boot.archive, &history.boot.revision};
    for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
      open.expected_payloads[i].size = files[i]->digest.size;
      memcpy(open.expected_payloads[i].sha256, files[i]->digest.sha256, 65);
    }
    struct install_esp_payload_report readback;
    ok = install_esp_payload_open(&open, &held, &readback);
  }
  ok = install_esp_payload_close(held) && ok;
  if (plan->view.interval_id) {
    status = ok ? install_esp_view_end_checked(&plan->view, plan->clock) : install_esp_view_end(&plan->view);
    ok = status == CALL_OK && ok;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) config_blob_destroy(&current[i]);
  config_blob_destroy(&record);
  config_blob_destroy(&pool);
  if (!ok) {
    fputs("installer: stale consumed configuration/ownership input; no program files written\n", stderr);
    return false;
  }
  publication->facts = facts;
  publication->recovery.facts = facts;
  return true;
}

bool install_installed_publish(struct install_plan *plan)
{
  struct install_installed_publication *publication = &plan->installed;
  handle_t workspace = plan->workspace;
  bool ok = install_recovery_provision(plan->view.root, plan->view.control, workspace, &publication->recovery);
  if (!ok) {
    fputs("installer: static recovery verification/provision failed; no menu activation attempted; artifacts retained\n", stderr);
    return false;
  }
  struct install_publication_request request = {.esp_root = plan->view.root, .workspace = workspace,
    .expected_target = plan->view.expected, .facts = publication->facts,
    .view = {.control = plan->view.control, .clock = plan->clock},
    .expected = publication->configuration.current, .expected_record = &publication->configuration.record,
    .selected = publication->configuration.after, .transaction = plan->record.attempt,
    .revision = publication->configuration.catalog->revision, .binding = publication->binding,
    .legacy_active = publication->legacy_active,
    .sources = {plan->kernel, plan->rescue,
      {.memory = (uint8_t *)plan->revision_record, .bytes = strlen(plan->revision_record)}}};
  memcpy(request.legacy_expected, publication->legacy, sizeof(request.legacy_expected));
  ok = ok && parent_open(plan->view.root, "EFI/pyxis/config", &request.view.config_parent) &&
      parent_open(plan->view.root, "boot/limine", &request.view.limine_parent);
  struct config_blob selected[CONFIG_ALL_FILES];
  memcpy(selected, publication->configuration.after, sizeof(selected));
  if (publication->configuration.source == CONFIG_ACTIVE_ARCHIVE) memset(selected, 0, sizeof(selected[0]) * CONFIG_SET_FILES);
  request.selected = selected;
  if (ok) ok = install_publication_publish(&request, &plan->publication_report);
  if (request.view.config_parent != HANDLE_INVALID) ok = handle_close(request.view.config_parent) == CALL_OK && ok;
  if (request.view.limine_parent != HANDLE_INVALID) ok = handle_close(request.view.limine_parent) == CALL_OK && ok;
  if (!ok || !plan->publication_report.boot_activated) {
    fprintf(stderr, "installer: readiness phase %u status %u; boot activation %s; recovery retained\n",
        plan->publication_report.phase, plan->publication_report.status,
        plan->publication_report.boot_activated ? "confirmed, cleanup incomplete" : "pending");
    return false;
  }
  return true;
}

struct probe_read {
  unsigned char *bytes;
  size_t size, capacity;
};

static bool probe_sink(void *context, const void *bytes, size_t size)
{
  struct probe_read *read = context;
  if (size > read->capacity - read->size) return false;
  memcpy(read->bytes + read->size, bytes, size);
  read->size += size;
  return true;
}

static bool probe_sectors(void *context, uint64_t sector, uint32_t count, void *bytes)
{
  const struct install_disk *disk = context;
  return sector <= disk->info.block_count && count <= disk->info.block_count - sector &&
      install_read(disk, sector * disk->info.block_size, bytes, (size_t)count * disk->info.block_size);
}

static enum fat32_status probe_file(const struct fat32_volume *volume,
    struct fat32_workspace *work, const char *path, size_t limit, struct config_blob *blob)
{
  struct probe_read read = {.bytes = malloc(limit), .capacity = limit};
  if (!read.bytes) return FAT32_NO_MEMORY;
  enum fat32_status status = fat32_read(volume, path, strlen(path), work, probe_sink, &read);
  if (status == FAT32_OK && !config_blob_set(blob, read.bytes, read.size)) status = FAT32_NO_MEMORY;
  free(read.bytes);
  return status;
}

enum install_installed_probe install_installed_probe(const struct install_disk *disk,
    const struct install_layout *layout, char revision[64], const char **reason)
{
  struct fat32_source source = {.read = probe_sectors, .context = (void *)disk,
    .sectors = disk->info.block_count, .sector_bytes = (uint32_t)disk->info.block_size,
    .max_read_sectors = DISK_IO_MAX_BYTES / disk->info.block_size};
  struct fat32_workspace work = {.bytes = fat32_open_workspace_size(),
    .step_limit = UINT64_C(16000000), .sector_limit = UINT64_C(16000000), .directory_depth_limit = 16};
  work.memory = malloc(work.bytes);
  struct fat32_volume volume;
  enum install_installed_probe result = INSTALL_INSTALLED_ABSENT;
  if (!work.memory || fat32_open(&source, (uint32_t)layout->esp_partition, &work, &volume) != FAT32_OK) {
    free(work.memory);
    return result;
  }
  if (volume.partition_first != layout->esp_start / disk->info.block_size ||
      volume.partition_sectors != layout->esp_bytes / disk->info.block_size ||
      memcmp(volume.disk_guid, layout->disk_guid, 16) ||
      memcmp(volume.partition_guid, layout->esp_guid, 16)) {
    free(work.memory);
    *reason = "readiness ESP identity differs from the reviewed layout";
    return INSTALL_INSTALLED_REFUSED;
  }
  size_t bytes;
  bool ok = fat32_workspace_size(&volume, 16, &bytes) == FAT32_OK;
  if (ok && bytes > work.bytes) {
    free(work.memory);
    work.bytes = bytes;
    work.memory = malloc(bytes);
    ok = work.memory != NULL;
  }
  struct config_blob record = {0}, menu = {0}, rendered = {0};
  struct config_record parsed = {0};
  enum fat32_status status = ok ? probe_file(&volume, &work, "/EFI/pyxis/config/config.record",
      CONFIG_RECORD_MAX_BYTES, &record) : FAT32_NO_MEMORY;
  if (status == FAT32_NOT_FOUND) goto done;
  result = INSTALL_INSTALLED_REFUSED;
  *reason = "readiness is malformed, prepared or unavailable; manual disposition required";
  if (status != FAT32_OK || !config_record_parse(record.bytes, record.size, &parsed) || !parsed.committed) goto done;
  if (!parsed.boot.present) {
    result = INSTALL_INSTALLED_ABSENT;
    goto done;
  }
  *reason = "committed readiness has pending or ambiguous menu activation; use a fresh pending completion Review";
  if (probe_file(&volume, &work, "/boot/limine/limine.conf", CONFIG_FILE_MAX_BYTES, &menu) != FAT32_OK) goto done;
  const struct install_boot_entry_paths binding = {parsed.boot.kernel.path, parsed.boot.archive.path};
  bool adoption = false, pending = true;
  if (!install_boot_entry_plan(&menu, &binding, layout->disk_guid, &binding, false,
      &rendered, &adoption, &pending) || pending || !config_blob_equal(&menu, &rendered)) goto done;
  strcpy(revision, parsed.boot.program_revision);
  result = INSTALL_INSTALLED_READY;
  *reason = NULL;
done:
  config_blob_destroy(&record);
  config_blob_destroy(&menu);
  config_blob_destroy(&rendered);
  free(work.memory);
  return result;
}

#endif
