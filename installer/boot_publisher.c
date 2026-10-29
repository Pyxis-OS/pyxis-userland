#include "boot_publisher.h"
#include "boot_entry.h"
#include "publication_snapshot.h"
#include "esp_directory.h"
#include "esp_transfer.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <pyxis_fs/npfs.h>
#include <string.h>

#define BOOT_PUBLISHER_RESERVE_CLUSTERS UINT64_C(32)

#define BOOT_PUBLISHER_MENU_PATH "boot/limine/" CONFIG_MENU_NAME
#define BOOT_PUBLISHER_RECORD_PATH "EFI/pyxis/config/" CONFIG_RECORD_NAME

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

static bool read_current(handle_t root, struct config_blob *menu, struct config_blob *record)
{
  return install_snapshot_file(root, BOOT_PUBLISHER_MENU_PATH, CONFIG_FILE_MAX_BYTES, menu) &&
      install_snapshot_file(root, BOOT_PUBLISHER_RECORD_PATH, CONFIG_RECORD_MAX_BYTES, record);
}

/* A pre-existing fixed loader must be byte-identical to the reviewed source.
 * An alias or an uncertain parent is never treated as an absent owned file. */
static bool loader_inspect(handle_t root, const struct config_file_digest *expected,
    bool *present)
{
  const uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES;
  handle_t efi = HANDLE_INVALID, pyxis = HANDLE_INVALID, file = HANDLE_INVALID;
  enum call_status status = install_esp_child_open(root, "EFI", DIRECTORY_KIND_DIRECTORY,
      directory_rights, false, &efi);
  if (status == CALL_OK) status = install_esp_child_open(efi, "pyxis",
      DIRECTORY_KIND_DIRECTORY, directory_rights, false, &pyxis);
  if (status == CALL_OK) status = install_esp_child_open(pyxis, "BOOTX64.EFI",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, false, &file);
  *present = status == CALL_OK;
  bool ok = status == CALL_OK || status == CALL_NOT_FOUND;
  if (ok && *present) {
    uint64_t size = 0, final_size = 0;
    struct config_file_digest actual = {0};
    struct install_source source = {.handle = file};
    ok = file_size(file, &size) == CALL_OK && size == expected->size;
    source.bytes = size;
    ok = ok && install_boot_payload_digest(&source, &actual) &&
        file_size(file, &final_size) == CALL_OK && final_size == size &&
        actual.present && !strcmp(actual.sha256, expected->sha256);
  }
  if (file != HANDLE_INVALID) ok = handle_close(file) == CALL_OK && ok;
  if (pyxis != HANDLE_INVALID) ok = handle_close(pyxis) == CALL_OK && ok;
  if (efi != HANDLE_INVALID) ok = handle_close(efi) == CALL_OK && ok;
  return ok;
}

void boot_publication_destroy(struct boot_publication *publication)
{
  config_blob_destroy(&publication->menu);
  config_blob_destroy(&publication->next_menu);
  config_blob_destroy(&publication->record);
  install_recovery_destroy(&publication->recovery);
  *publication = (struct boot_publication){0};
}

static bool binding_prepare(struct boot_publication *publication, const struct boot_publisher_input *input)
{
  struct config_boot_binding *binding = &publication->binding;
  binding->present = true;
  strcpy(binding->kernel_revision, input->revision);
  strcpy(binding->program_revision, input->revision);
  strcpy(binding->attempt, input->attempt);
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
  /* The previous menu's recovery copy and the staged new menu. */
  uint64_t copies[] = {publication->menu.size, publication->next_menu.size};
  for (unsigned n = 0; n < 2; ++n) {
    uint64_t blocks = copies[n] / cluster + (copies[n] % cluster != 0);
    if (blocks > (UINT64_MAX - total) / cluster) return UINT64_MAX;
    total += blocks * cluster;
  }
  uint64_t recovery = install_recovery_space(&publication->recovery, cluster);
  if (recovery > UINT64_MAX - total) return UINT64_MAX;
  total += recovery;
  if (publication->loader_digest.present && !publication->loader_present) {
    uint64_t size = publication->loader_digest.size;
    uint64_t blocks = size / cluster + (size % cluster != 0);
    if (blocks > (UINT64_MAX - total) / cluster) return UINT64_MAX;
    total += blocks * cluster;
  }
  return total;
}

/* Binding, menu and capacity from the frozen snapshots in PUBLICATION. A
 * present record has already passed install_recovery_resolved, which refuses
 * a prepared or malformed one; it is never treated as absent. */
static bool prepare_plan(struct boot_publication *publication, const struct boot_publisher_input *input)
{
  const struct config_blob *record = &publication->record;
  struct config_record previous = {0};
  bool recorded = record->digest.present;
  publication->reason = "the installed boot record is invalid or not committed";
  bool ok = !recorded || (config_record_parse(record->bytes, record->size, &previous) &&
      previous.committed);
  if (ok) {
    publication->reason = "cannot hash or bind the new boot payloads";
    ok = binding_prepare(publication, input);
  }
  if (ok) {
    const struct install_boot_entry_paths next = {publication->binding.kernel.path,
      publication->binding.archive.path};
    publication->reason = "the installed boot menu cannot be safely activated from its ownership evidence";
    if (publication->fresh || (!recorded && !publication->menu.digest.present)) {
      ok = install_boot_entry_render_owned(&next, input->disk_guid, input->pool_partition,
          &publication->next_menu);
    } else {
      const struct install_boot_entry_paths old = {previous.boot.kernel.path, previous.boot.archive.path};
      const struct install_boot_entry_paths old_previous = {previous.legacy_previous_boot.kernel.path,
        previous.legacy_previous_boot.archive.path};
      const struct install_boot_menu inherited = {&old,
        previous.legacy_previous_boot.present ? &old_previous : NULL, false};
      const struct install_boot_menu target = {&next, NULL, false};
      ok = install_boot_entry_plan(&publication->menu, recorded ? &inherited : NULL,
          input->disk_guid, input->pool_partition, &target,
          &publication->next_menu, &publication->activation_pending);
      if (ok && publication->activation_pending) {
        fprintf(stderr, "%s: pending or ambiguous boot activation requires a fresh pending completion Review; nothing written\n",
            input->name);
        ok = false;
      }
    }
  }
  if (ok) {
    publication->required_bytes = required_space(publication, publication->cluster_bytes);
    publication->reason = "insufficient checked ESP free space for the new boot publication";
    ok = publication->required_bytes <= publication->available_bytes;
    if (!ok) {
      fprintf(stderr, "%s: insufficient ESP space: need %llu bytes, checked free %llu bytes; nothing written\n",
          input->name, (unsigned long long)publication->required_bytes,
          (unsigned long long)publication->available_bytes);
    }
  }
  if (ok) publication->reason = NULL;
  return ok;
}

bool boot_publication_prepare(struct boot_publication *publication,
    const struct boot_publisher_input *input, handle_t root, handle_t control)
{
  boot_publication_destroy(publication);
  const struct config_blob *record = &publication->record;
  publication->reason = "the mounted ESP identity differs from the reviewed target";
  bool ok = install_snapshot_facts(control, &publication->facts) &&
      facts_match(&input->target, &publication->facts);
  if (ok) {
    publication->reason = "cannot snapshot the installed boot menu or ownership record";
    ok = read_current(root, &publication->menu, &publication->record);
  }
  if (ok) {
    publication->reason = "an incomplete or unresolved boot publication requires recovery";
    ok = install_recovery_resolved(root, record);
  }
  if (ok) {
    publication->reason = "unrecognized files or stale recovery artifacts exist in boot/limine";
    ok = install_recovery_menu_review(root, record, &publication->facts, input->pool_partition);
  }
  if (ok) {
    publication->reason = "the immutable rescue pair or fallback menu cannot be safely reused or provisioned";
    ok = install_recovery_prepare(root, input->boot, input->loaded_kernel, input->workspace,
        &publication->facts, &publication->recovery);
    if (!ok && publication->recovery.reason) publication->reason = publication->recovery.reason;
  }
  if (ok && input->coexist) {
    publication->reason = "the existing EFI/pyxis/BOOTX64.EFI differs from the installer loader or cannot be verified";
    ok = install_boot_payload_digest(&input->loader, &publication->loader_digest) &&
        loader_inspect(root, &publication->loader_digest, &publication->loader_present);
  }
  if (ok) {
    struct mount_state_reply state;
    publication->reason = "checked ESP free-space capacity is unavailable or invalid";
    ok = mount_observe(control, &state) == CALL_OK &&
        (state.flags & MOUNT_STATE_CAPACITY_VALID) && state.cluster_bytes &&
        state.free_clusters <= UINT64_MAX / state.cluster_bytes;
    if (ok) {
      publication->cluster_bytes = state.cluster_bytes;
      publication->available_bytes = state.free_clusters * state.cluster_bytes;
    }
  }
  ok = ok && prepare_plan(publication, input);
  publication->prepared = ok;
  return ok;
}

bool boot_publication_prepare_fresh(struct boot_publication *publication,
    const struct boot_publisher_input *input, const char *limine, size_t limine_bytes,
    uint64_t cluster_bytes, uint64_t available_bytes)
{
  boot_publication_destroy(publication);
  publication->fresh = true;
  publication->facts = input->target;
  publication->cluster_bytes = cluster_bytes;
  publication->available_bytes = available_bytes;
  publication->reason = "cannot prepare the fresh boot menu or independent rescue sources";
  bool ok = config_blob_set(&publication->menu, limine, limine_bytes) &&
      install_recovery_prepare_fresh(input->boot, input->loaded_kernel,
          &publication->facts, &publication->recovery) &&
      prepare_plan(publication, input);
  publication->prepared = ok;
  return ok;
}

bool boot_publication_hash(const struct boot_publication *publication, struct install_digest *digest)
{
  static const char domain[] = "pyxis-installed-publication-2";
  unsigned char choices[] = {publication->activation_pending, publication->fresh,
    publication->loader_present};
  uint8_t loader_size[8];
  npfs_put_u64(loader_size, publication->loader_digest.size);
  bool ok = publication->prepared && install_digest_update(digest, domain, sizeof(domain)) &&
      install_digest_update(digest, choices, sizeof(choices)) &&
      blob_hash(digest, &publication->menu) && blob_hash(digest, &publication->next_menu) &&
      blob_hash(digest, &publication->record) &&
      install_digest_update(digest, loader_size, sizeof(loader_size)) &&
      install_digest_update(digest, publication->loader_digest.sha256,
          sizeof(publication->loader_digest.sha256)) &&
      blob_hash(digest, &publication->recovery.manifest) && blob_hash(digest, &publication->recovery.fallback);
  return ok;
}

void boot_publication_report(const struct boot_publication *publication,
    const struct boot_publisher_input *input, FILE *output, bool verbose)
{
  (void)verbose;
  fprintf(output, "Boot attempt: EFI/pyxis/%s/%s; ESP reserve %llu bytes, capacity bound %llu.\n",
      input->revision, input->attempt,
      (unsigned long long)publication->required_bytes, (unsigned long long)publication->available_bytes);
  fputs(publication->recovery.present ? "Verified static recovery is retained.\n" :
      "Provision the reviewed immutable recovery pair and fallback before menu activation.\n", output);
  if (publication->activation_pending) {
    fputs("Existing menu remains; boot activation will be pending.\n", output);
    const struct install_boot_entry_paths next = {publication->binding.kernel.path,
      publication->binding.archive.path};
    install_boot_entry_print(&next, input->disk_guid, input->pool_partition, output);
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

static bool loader_publish(const struct boot_publication *publication,
    const struct boot_publisher_input *input, const struct install_esp_view *view)
{
  if (!input->coexist) return true;
  handle_t parent = HANDLE_INVALID, file = HANDLE_INVALID, ram = HANDLE_INVALID;
  bool ok = parent_open(view->root, "EFI/pyxis", &parent);
  enum call_status status = CALL_BAD_REQUEST;
  if (ok) {
    status = install_esp_child_open(parent, "BOOTX64.EFI", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, false, &file);
  }
  if (ok && publication->loader_present) {
    bool present = false;
    ok = status == CALL_OK && loader_inspect(view->root,
        &publication->loader_digest, &present) && present;
  } else if (ok) {
    ok = status == CALL_NOT_FOUND;
    if (ok) {
      status = directory_create(parent, "BOOTX64.EFI", DIRECTORY_KIND_FILE,
          FILE_RIGHT_READ | FILE_RIGHT_WRITE, &file);
      ok = status == CALL_OK;
    }
    if (ok) {
      ok = install_esp_transfer_workspace(input->workspace, &ram) == CALL_OK &&
          install_esp_transfer_source(&input->loader, file, ram,
              publication->loader_digest.size, publication->loader_digest.sha256,
              INSTALL_ESP_PAYLOAD_MAX_BYTES) == CALL_OK &&
          file_sync(file) == CALL_OK && directory_sync(parent) == CALL_OK &&
          mount_sync(view->control) == CALL_OK;
    }
    if (ok) {
      bool present = false;
      ok = loader_inspect(view->root, &publication->loader_digest, &present) && present;
    }
  }
  if (ram != HANDLE_INVALID) ok = handle_close(ram) == CALL_OK && ok;
  if (file != HANDLE_INVALID) ok = handle_close(file) == CALL_OK && ok;
  if (parent != HANDLE_INVALID) ok = handle_close(parent) == CALL_OK && ok;
  return ok;
}

bool boot_publication_admit(struct boot_publication *publication,
    const struct boot_publisher_input *input, struct install_esp_view *view)
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
  struct config_blob menu = {0}, record = {0};
  ok = ok && read_current(view->root, &menu, &record) &&
      config_blob_equal(&menu, &publication->menu) &&
      config_blob_equal(&record, &publication->record) &&
      install_recovery_resolved(view->root, &record);
  if (ok && input->coexist) {
    bool present = false;
    ok = loader_inspect(view->root, &publication->loader_digest, &present) &&
        present == publication->loader_present;
  }
  if (ok) {
    struct install_recovery current_recovery = {0};
    ok = install_recovery_menu_review(view->root, &record, &facts, input->pool_partition) &&
        install_recovery_prepare(view->root, input->boot, input->loaded_kernel,
            input->workspace, &facts, &current_recovery) &&
        current_recovery.present == publication->recovery.present &&
        config_blob_equal(&current_recovery.manifest, &publication->recovery.manifest) &&
        config_blob_equal(&current_recovery.fallback, &publication->recovery.fallback);
    install_recovery_destroy(&current_recovery);
  }
  struct config_record history = {0};
  struct install_esp_payloads *held = NULL;
  if (ok && record.digest.present && config_record_parse(record.bytes, record.size, &history)) {
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
  config_blob_destroy(&menu);
  config_blob_destroy(&record);
  if (!ok) {
    fprintf(stderr, "%s: stale consumed boot menu/ownership input; no program files written\n",
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
  bool ok = loader_publish(publication, input, view) &&
      install_recovery_provision(view->root, view->control, input->workspace, &publication->recovery);
  if (!ok) {
    fprintf(stderr, "%s: static recovery verification/provision failed; no menu activation attempted; artifacts retained\n",
        input->name);
    return false;
  }
  struct install_publication_request request = {.esp_root = view->root, .workspace = input->workspace,
    .expected_target = view->expected, .facts = publication->facts,
    .view = {.control = view->control, .clock = input->clock},
    .expected = &publication->menu, .expected_record = &publication->record,
    .selected = &publication->next_menu, .transaction = input->attempt,
    .stable_loader = input->coexist ? &publication->loader_digest : NULL,
    .binding = publication->binding,
    .sources = {input->kernel, input->archive,
      {.memory = (uint8_t *)input->revision_record, .bytes = strlen(input->revision_record)}}};
  ok = ok && parent_open(view->root, "EFI/pyxis/config", &request.view.config_parent) &&
      parent_open(view->root, "boot/limine", &request.view.limine_parent);
  if (ok) ok = install_publication_publish(&request, report);
  if (request.view.config_parent != HANDLE_INVALID) ok = handle_close(request.view.config_parent) == CALL_OK && ok;
  if (request.view.limine_parent != HANDLE_INVALID) ok = handle_close(request.view.limine_parent) == CALL_OK && ok;
  if (!ok || !report->boot_activated) {
    fprintf(stderr, "%s: boot record phase %u status %u; boot activation %s; recovery retained\n",
        input->name, report->phase, report->status,
        report->boot_activated ? "confirmed, cleanup incomplete" : "pending");
    return false;
  }
  return true;
}
