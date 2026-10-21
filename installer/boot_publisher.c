#include "boot_publisher.h"
#include "boot_entry.h"
#include "pending_archive.h"
#include "publication_snapshot.h"
#include "esp_directory.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <pyxis_fs/npfs.h>
#include <string.h>

#define BOOT_PUBLISHER_RESERVE_CLUSTERS UINT64_C(32)

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

static bool facts_match(const struct config_target_facts *expected,
    const struct config_target_facts *facts)
{
  return config_target_valid(facts) && facts->device == expected->device &&
      facts->partition == expected->partition && facts->first_block == expected->first_block &&
      facts->block_count == expected->block_count &&
      facts->device_block_size == expected->device_block_size &&
      facts->device_block_count == expected->device_block_count &&
      facts->partition_attributes == expected->partition_attributes &&
      !memcmp(facts->disk_guid, expected->disk_guid, 16) &&
      !memcmp(facts->partition_guid, expected->partition_guid, 16) &&
      !memcmp(facts->partition_type, expected->partition_type, 16);
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

void boot_publication_destroy(struct boot_publication *publication)
{
  install_configuration_free(&publication->configuration);
  install_recovery_destroy(&publication->recovery);
  *publication = (struct boot_publication){0};
}

static bool binding_prepare(struct boot_publication *publication, const struct boot_publisher_input *input)
{
  struct config_boot_binding *binding = &publication->binding;
  binding->present = true;
  binding->source = publication->configuration.source;
  strcpy(binding->kernel_revision, input->revision);
  strcpy(binding->program_revision, input->revision);
  strcpy(binding->attempt, input->attempt);
  binding->archive_catalog = publication->configuration.catalog_source.digest;
  binding->legacy_override = publication->configuration.pool_override.digest;
  binding->legacy_disposition = publication->configuration.pool_disposition;
  struct config_boot_payload *files[] = {&binding->kernel, &binding->archive, &binding->revision};
  const char *names[] = {"kernel.elf", "boot.arc", "revision.txt"};
  struct install_source sources[] = {input->kernel, input->archive,
    {.memory = (uint8_t *)input->revision_record, .bytes = strlen(input->revision_record)}};
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    if (!config_boot_payload_path(binding->kernel_revision, binding->attempt, names[i], files[i]->path) ||
        !install_boot_payload_digest(&sources[i], &files[i]->digest)) {
      return false;
    }
  }
  return config_boot_binding_valid(binding);
}

static uint64_t required_space(const struct boot_publication *publication, uint64_t cluster)
{
  if (!cluster || cluster > UINT64_MAX / BOOT_PUBLISHER_RESERVE_CLUSTERS) return UINT64_MAX;
  uint64_t total = BOOT_PUBLISHER_RESERVE_CLUSTERS * cluster;
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

/* Configuration, binding, menu and capacity from the frozen snapshots. */
static bool prepare_plan(struct boot_publication *publication, const struct boot_publisher_input *input,
    struct config_blob current[CONFIG_ALL_FILES], const struct config_blob *record,
    const struct config_blob *pool, const struct config_blob defaults[CONFIG_SET_FILES], bool legacy)
{
  struct install_configuration_input configuration = {.current = current, .record = record,
    .archive = defaults, .catalog = &defaults[CONFIG_CATALOG], .pool_override = pool,
    .kernel_revision = input->revision, .program_revision = input->revision};
  bool ok = install_configuration_prepare(&configuration, &publication->configuration);
  if (ok && !publication->configuration.applicable) {
    install_configuration_review(&publication->configuration, stderr);
    fprintf(stderr, "%s: configuration needs explicit choices; use a fresh configuration Review; nothing written\n",
        input->name);
    ok = false;
  }
  if (ok) ok = binding_prepare(publication, input);
  if (ok) {
    const struct install_boot_entry_paths next = {publication->binding.kernel.path,
      publication->binding.archive.path};
    if (publication->fresh) {
      ok = install_boot_entry_render_owned(&next, input->disk_guid,
          &publication->configuration.after[CONFIG_LIMINE]);
    } else {
      struct config_record previous = {0};
      bool recorded = record->digest.present && config_record_parse(record->bytes, record->size, &previous) &&
          previous.committed && previous.boot.present;
      const struct install_boot_entry_paths old = {previous.boot.kernel.path, previous.boot.archive.path};
      const struct install_boot_entry_paths old_previous = {previous.previous_boot.kernel.path,
        previous.previous_boot.archive.path};
      const struct install_boot_menu inherited = {&old,
        previous.previous_boot.present ? &old_previous : NULL, false};
      /* The outgoing attempt becomes the previous entry, as it becomes previous_boot. */
      const struct install_boot_menu target = {&next, recorded ? &old : NULL, false};
      bool adoption = false;
      ok = install_boot_entry_plan(&current[CONFIG_LIMINE], recorded ? &inherited : NULL,
          input->disk_guid, &target, legacy,
          &publication->configuration.after[CONFIG_LIMINE], &adoption, &publication->activation_pending);
      publication->legacy_active = adoption;
      if (ok && recorded && publication->activation_pending) {
        fprintf(stderr, "%s: pending or ambiguous boot activation requires a fresh pending completion Review; nothing written\n",
            input->name);
        ok = false;
      }
    }
  }
  if (ok) {
    publication->required_bytes = required_space(publication, publication->cluster_bytes);
    ok = publication->required_bytes <= publication->available_bytes;
  }
  return ok;
}

bool boot_publication_prepare(struct boot_publication *publication,
    const struct boot_publisher_input *input, handle_t root, handle_t control,
    const struct config_blob *pool)
{
  boot_publication_destroy(publication);
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0};
  struct config_blob defaults[CONFIG_SET_FILES] = {0};
  bool legacy = false;
  bool ok = install_pending_archive_configs(&input->archive, defaults) &&
      install_snapshot_facts(control, &publication->facts) &&
      facts_match(&input->target, &publication->facts) && read_current(root, current, &record) &&
      install_recovery_resolved(root, &record) &&
      install_recovery_menu_review(root, &record, &publication->facts) &&
      install_recovery_prepare(root, input->boot, input->loaded_kernel, input->workspace,
          &publication->facts, &publication->recovery);
  if (ok) {
    struct mount_state_reply state;
    ok = mount_observe(control, &state) == CALL_OK &&
        (state.flags & MOUNT_STATE_CAPACITY_VALID) && state.cluster_bytes &&
        state.free_clusters <= UINT64_MAX / state.cluster_bytes;
    if (ok) {
      publication->cluster_bytes = state.cluster_bytes;
      publication->available_bytes = state.free_clusters * state.cluster_bytes;
      struct config_record inherited = {0};
      bool owned = record.digest.present && config_record_parse(record.bytes, record.size, &inherited) &&
          inherited.committed && inherited.boot.present;
      if (!owned) legacy = install_boot_legacy_certify(root, input->workspace, publication->legacy);
    }
  }
  ok = ok && prepare_plan(publication, input, current, &record, pool, defaults, legacy);
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) config_blob_destroy(&current[i]);
  for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) config_blob_destroy(&defaults[i]);
  config_blob_destroy(&record);
  publication->prepared = ok;
  return ok;
}

bool boot_publication_prepare_fresh(struct boot_publication *publication,
    const struct boot_publisher_input *input, const char *limine, size_t limine_bytes,
    const struct config_blob *pool, uint64_t cluster_bytes, uint64_t available_bytes)
{
  boot_publication_destroy(publication);
  publication->fresh = true;
  publication->facts = input->target;
  publication->cluster_bytes = cluster_bytes;
  publication->available_bytes = available_bytes;
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0};
  struct config_blob defaults[CONFIG_SET_FILES] = {0};
  bool ok = install_pending_archive_configs(&input->archive, defaults) &&
      config_blob_set(&current[CONFIG_LIMINE], limine, limine_bytes) &&
      install_recovery_prepare_fresh(input->boot, input->loaded_kernel,
          &publication->facts, &publication->recovery) &&
      prepare_plan(publication, input, current, &record, pool, defaults, false);
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) config_blob_destroy(&current[i]);
  for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) config_blob_destroy(&defaults[i]);
  publication->prepared = ok;
  return ok;
}

bool boot_publication_hash(const struct boot_publication *publication, struct install_digest *digest)
{
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

void boot_publication_report(const struct boot_publication *publication,
    const struct boot_publisher_input *input, FILE *output, bool verbose)
{
  if (verbose) install_configuration_review(&publication->configuration, output);
  else install_configuration_summary(&publication->configuration, output);
  fprintf(output, "Boot attempt: EFI/pyxis/%s/%s; ESP reserve %llu bytes, capacity bound %llu.\n",
      input->revision, input->attempt,
      (unsigned long long)publication->required_bytes, (unsigned long long)publication->available_bytes);
  fputs(publication->recovery.present ? "Verified static recovery is retained.\n" :
      "Provision the reviewed immutable recovery pair and fallback before menu activation.\n", output);
  struct config_record history = {0};
  const struct config_blob *record = &publication->configuration.record;
  if (record->digest.present && config_record_parse(record->bytes, record->size, &history) && history.boot.present) {
    fprintf(output, "Retain previous boot kernel %s (%s) and archive %s (%s) as the previous menu entry.\n",
        history.boot.kernel.path, history.boot.kernel.digest.sha256,
        history.boot.archive.path, history.boot.archive.digest.sha256);
  }
  if (publication->legacy_active) fputs("Adopt the exact verified legacy normal/rescue pair.\n", output);
  if (publication->activation_pending) {
    fputs("Existing menu remains; boot activation will be pending.\n", output);
    const struct install_boot_entry_paths next = {publication->binding.kernel.path,
      publication->binding.archive.path};
    install_boot_entry_print(&next, input->disk_guid, output);
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

bool boot_publication_admit(struct boot_publication *publication,
    const struct boot_publisher_input *input, struct install_esp_view *view,
    handle_t pool_system)
{
  struct config_target_facts facts;
  bool ok = install_snapshot_facts(view->control, &facts) && facts_match(&input->target, &facts);
  if (ok) ok = install_esp_view_begin_checked(view) == CALL_OK;
  if (ok) {
    struct mount_state_reply state;
    ok = mount_observe(view->control, &state) == CALL_OK &&
        (state.flags & MOUNT_STATE_CAPACITY_VALID) &&
        state.cluster_bytes == publication->cluster_bytes && state.cluster_bytes &&
        state.free_clusters <= UINT64_MAX / state.cluster_bytes &&
        publication->required_bytes <= state.free_clusters * state.cluster_bytes;
  }
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0}, pool = {0};
  ok = ok && read_current(view->root, current, &record);
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    ok = config_blob_equal(&current[i], &publication->configuration.current[i]);
  }
  ok = ok && config_blob_equal(&record, &publication->configuration.record) &&
      install_recovery_resolved(view->root, &record);
  if (ok && pool_system != HANDLE_INVALID) {
    ok = install_snapshot_file(pool_system, "config/boot.lua", CONFIG_FILE_MAX_BYTES, &pool) &&
        config_blob_equal(&pool, &publication->configuration.pool_override);
  }
  if (ok) {
    struct install_recovery current_recovery = {0};
    ok = install_recovery_menu_review(view->root, &record, &facts) &&
        install_recovery_prepare(view->root, input->boot, input->loaded_kernel,
            input->workspace, &facts, &current_recovery) &&
        current_recovery.present == publication->recovery.present &&
        config_blob_equal(&current_recovery.manifest, &publication->recovery.manifest) &&
        config_blob_equal(&current_recovery.fallback, &publication->recovery.fallback);
    install_recovery_destroy(&current_recovery);
  }
  if (ok && publication->legacy_active) {
    struct config_storage_report report = {0};
    ok = install_publication_legacy_check(view->root, input->workspace, publication->legacy, &report);
  }
  struct config_record history = {0};
  struct install_esp_payloads *held = NULL;
  if (ok && record.digest.present && config_record_parse(record.bytes, record.size, &history) &&
      history.boot.present) {
    struct install_esp_payload_request open = {.root = view->root, .control = view->control,
      .workspace = input->workspace, .expected = view->expected,
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
  if (view->interval_id) {
    enum call_status status = ok ? install_esp_view_end_checked(view, input->clock) :
        install_esp_view_end(view);
    ok = status == CALL_OK && ok;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) config_blob_destroy(&current[i]);
  config_blob_destroy(&record);
  config_blob_destroy(&pool);
  if (!ok) {
    fprintf(stderr, "%s: stale consumed configuration/ownership input; no program files written\n",
        input->name);
    return false;
  }
  publication->facts = facts;
  publication->recovery.facts = facts;
  return true;
}

bool boot_publication_publish(struct boot_publication *publication,
    const struct boot_publisher_input *input, const struct install_esp_view *view,
    struct config_storage_report *report)
{
  bool ok = install_recovery_provision(view->root, view->control, input->workspace, &publication->recovery);
  if (!ok) {
    fprintf(stderr, "%s: static recovery verification/provision failed; no menu activation attempted; artifacts retained\n",
        input->name);
    return false;
  }
  struct install_publication_request request = {.esp_root = view->root, .workspace = input->workspace,
    .expected_target = view->expected, .facts = publication->facts,
    .view = {.control = view->control, .clock = input->clock},
    .expected = publication->configuration.current, .expected_record = &publication->configuration.record,
    .selected = publication->configuration.after, .transaction = input->attempt,
    .revision = publication->configuration.catalog->revision, .binding = publication->binding,
    .legacy_active = publication->legacy_active,
    .sources = {input->kernel, input->archive,
      {.memory = (uint8_t *)input->revision_record, .bytes = strlen(input->revision_record)}}};
  memcpy(request.legacy_expected, publication->legacy, sizeof(request.legacy_expected));
  ok = ok && parent_open(view->root, "EFI/pyxis/config", &request.view.config_parent) &&
      parent_open(view->root, "boot/limine", &request.view.limine_parent);
  struct config_blob selected[CONFIG_ALL_FILES];
  memcpy(selected, publication->configuration.after, sizeof(selected));
  if (publication->configuration.source == CONFIG_ACTIVE_ARCHIVE) memset(selected, 0, sizeof(selected[0]) * CONFIG_SET_FILES);
  request.selected = selected;
  if (ok) ok = install_publication_publish(&request, report);
  if (request.view.config_parent != HANDLE_INVALID) ok = handle_close(request.view.config_parent) == CALL_OK && ok;
  if (request.view.limine_parent != HANDLE_INVALID) ok = handle_close(request.view.limine_parent) == CALL_OK && ok;
  if (!ok || !report->boot_activated) {
    fprintf(stderr, "%s: readiness phase %u status %u; boot activation %s; recovery retained\n",
        input->name, report->phase, report->status,
        report->boot_activated ? "confirmed, cleanup incomplete" : "pending");
    return false;
  }
  return true;
}
