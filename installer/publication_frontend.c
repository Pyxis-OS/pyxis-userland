#include "publication_frontend.h"
#include "battery.h"
#include "coexist.h"
#include "pending.h"
#include "recovery.h"
#include "publication_snapshot.h"
#include "publication.h"
#include "boot_entry.h"
#include "esp_directory.h"
#include "../xfer/hash.h"
#include "../common/bootstrap_environment.h"

#include <directory.h>
#include <handle.h>
#include <mount.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>

#define READ_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define WRITE_RIGHTS (READ_RIGHTS | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_WRITE_FILES)
#define MENU_PATH "boot/limine/" CONFIG_MENU_NAME
#define RECORD_PATH "EFI/pyxis/config/" CONFIG_RECORD_NAME

static bool confirm_word(struct terminal *terminal, const char *word)
{
  char text[32], prompt[80];
  snprintf(prompt, sizeof(prompt), "Type %s to confirm this Review: ", word);
  for (;;) {
    struct term_line_result input = term_read_line(terminal, prompt, text, sizeof(text));
    if (input.status != TERM_LINE_OK) {
      return false;
    }
    if (!input.limit_reached && !strcmp(text, word)) {
      return true;
    }
    fputs("Confirmation did not match; try again or Ctrl+C to cancel.\n", stderr);
  }
}

static bool choose(struct terminal *terminal, const char *prompt, const char *const labels[],
    size_t count, size_t initial, size_t *selected, int *result)
{
  struct term_choice_result input = term_read_choice(terminal, prompt, labels, count, initial);
  if (input.status == TERM_LINE_OK) {
    *selected = input.selected;
    return true;
  }
  if (input.status == TERM_LINE_CANCELLED || input.status == TERM_LINE_EOF) {
    fputs("Cancelled; nothing written.\n", stderr);
    *result = EXIT_SUCCESS;
  } else {
    fprintf(stderr, "Review input unavailable (status %u, error %u); nothing written.\n",
        input.status, input.error);
    *result = EXIT_FAILURE;
  }
  return false;
}

static bool parent_open(handle_t root, const char *path, bool create, handle_t *output)
{
  handle_t parent = root;
  bool owned = false;
  for (const char *cursor = path;;) {
    const char *slash = strchr(cursor, '/');
    size_t size = slash ? (size_t)(slash - cursor) : strlen(cursor);
    char name[256];
    if (!size || size >= sizeof(name)) {
      if (owned) {
        handle_close(parent);
      }
      return false;
    }
    memcpy(name, cursor, size);
    name[size] = 0;
    handle_t next = HANDLE_INVALID;
    enum call_status status = install_esp_child_open(parent, name,
        DIRECTORY_KIND_DIRECTORY, WRITE_RIGHTS, false, &next);
    if (create && status == CALL_NOT_FOUND) {
      status = install_esp_child_open(parent, name,
          DIRECTORY_KIND_DIRECTORY, WRITE_RIGHTS, true, &next);
      if (status == CALL_OK) {
        status = directory_sync(parent);
      }
    }
    if (owned && handle_close(parent) != CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (status != CALL_OK) {
      if (next != HANDLE_INVALID) {
        handle_close(next);
      }
      fprintf(stderr, "installer publication: parent %s status %u\n", name, status);
      return false;
    }
    parent = next;
    owned = true;
    if (!slash) {
      *output = parent;
      return true;
    }
    cursor = slash + 1;
  }
}

static bool retire(struct mount_fat32_reply *mount)
{
  bool ok = true;
  if (mount->root != HANDLE_INVALID) {
    ok = handle_close(mount->root) == CALL_OK;
    mount->root = HANDLE_INVALID;
  }
  if (mount->control != HANDLE_INVALID) {
    enum call_status status = mount_retire(mount->control);
    ok = status == CALL_OK && ok;
    if (status != CALL_OK) {
      fprintf(stderr, "installer publication: view retirement refused (status %u); retained, no revocation\n", status);
      return false;
    }
    ok = handle_close(mount->control) == CALL_OK && ok;
    mount->control = HANDLE_INVALID;
  }
  return ok;
}

static const char *publication_phase(enum config_publish_phase phase)
{
  const char *const names[] = {"check", "stage", "verify", "exclusive revalidation",
    "prepared record", "committed record", "boot activation", "cleanup", "done"};
  return (unsigned)phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "unknown";
}

static void publication_result(const struct install_publication_request *request,
    const struct config_storage_report *report, bool ok)
{
  fprintf(stderr, "Completion: status %u, phase %s, boot %s.\n",
      report->status, publication_phase(report->phase),
      report->boot_activated ? "entry confirmed" : "activation pending");
  if (ok) {
    return;
  }
  fprintf(stderr, "Attempt: %s; inspect EFI/pyxis/config/config.record before recovery.\n",
      request->binding.kernel.path);
  if (report->artifact[0]) {
    fprintf(stderr, "Last artifact: %s\n", report->artifact);
  }
  const struct config_recovery_file *previous = &report->record.previous_menu;
  if (previous->digest.present) {
    fprintf(stderr, "Recorded previous menu: boot/limine/%s\n", previous->name);
  }
  if (!ok) {
    fputs("Stopped; retain artifacts and verify the recorded hashes before explicit recovery.\n"
        "No automatic rollback, retry or reboot offer.\n", stderr);
  }
}


static bool completion_target(const struct install_disk *disk, uint64_t selected_esp,
    uint64_t selected_pool, const char *selected_guid, bool coexist)
{
  struct disk_partition_selection esp, native_pool;
  struct install_layout layout;
  struct install_update update;
  char guid[37];
  if (!selected_esp || !selected_pool || !selected_guid ||
      disk_partition_info(disk->handle, selected_esp, &esp) != CALL_OK ||
      disk_partition_info(disk->handle, selected_pool, &native_pool) != CALL_OK) {
    return false;
  }
  install_coexist_layout(&esp, &native_pool, &layout);
  install_guid_text(layout.disk_guid, guid);
  if (strcmp(guid, selected_guid)) {
    return false;
  }
  bool inspected = coexist ? install_scan_pool_update(disk, &layout, &update) :
      install_scan_update(disk, &update);
  return inspected && update.eligible && update.layout.esp_partition == selected_esp &&
      update.layout.pool_partition == selected_pool;
}

/* Fresh explicit completion Review of one selected installation's pending
 * activation or cleanup. No new attempt, record or retention generation. */
static int pending_frontend(struct terminal *terminal, bool verbose,
    uint64_t id, uint64_t selected_esp, uint64_t selected_pool, const char *selected_guid,
    bool coexist)
{
  handle_t disks = bootstrap_capability("disks"), boot = bootstrap_capability("boot");
  handle_t clock = bootstrap_capability("clock"), random = bootstrap_capability("random");
  handle_t workspace = bootstrap_capability("config_snapshot_parent");
  struct install_disk disk = {.handle = HANDLE_INVALID};
  struct mount_fat32_reply mount = {.root = HANDLE_INVALID, .control = HANDLE_INVALID};
  struct config_blob menu = {0}, record = {0};
  struct install_publication_request request = {.esp_root = HANDLE_INVALID,
    .workspace = workspace,
    .view = {.config_parent = HANDLE_INVALID, .limine_parent = HANDLE_INVALID, .control = HANDLE_INVALID, .clock = clock}};
  struct config_target_facts planned_facts = {0};
  struct config_file_digest legacy_digests[INSTALL_BOOT_LEGACY_PAYLOADS] = {0};
  bool legacy_pair = false;
  uint64_t available_bytes = 0, cluster_bytes = 0;
  int result = EXIT_FAILURE;
  bool crypto = hash_init(clock, random);
  struct disk_partition_selection target_selection = {0};
  struct config_blob previous_limine = {0};
  struct install_pending_plan completion = {0};
  struct install_recovery recovery = {.sources = {{.handle = HANDLE_INVALID}, {.handle = HANDLE_INVALID}}};
  if (!crypto || !install_battery_check(bootstrap_capability("system_info"), stderr)) {
    goto done;
  }
  if (disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk.handle) != CALL_OK ||
      disk_get_info(disk.handle, &disk.info) != CALL_OK || disk.info.id != id ||
      !disk.info.block_size || disk.info.block_count > UINT64_MAX / disk.info.block_size) {
    goto done;
  }
  disk.bytes = disk.info.block_count * disk.info.block_size;
  install_progress_plain_stream(clock, stderr, bootstrap_capability("installer_diagnostics"), verbose);
  if (!completion_target(&disk, selected_esp, selected_pool, selected_guid, coexist)) {
    fputs("Selected pending installation changed or is unavailable; nothing written.\n", stderr);
    goto done;
  }
  uint64_t pool_partition = selected_pool;
  if (disk_partition_info(disk.handle, selected_esp, &target_selection) != CALL_OK ||
      disks_open_owned_esp(disks, &target_selection, MOUNT_ACCESS_READ_ONLY, READ_RIGHTS, &mount) != CALL_OK ||
      !install_snapshot_facts(mount.control, &planned_facts)) {
    fputs("Planning view/identity unavailable; nothing written.\n", stderr);
    goto done;
  }
  if (!install_snapshot_file(mount.root, MENU_PATH, CONFIG_FILE_MAX_BYTES, &menu) ||
      !install_snapshot_file(mount.root, RECORD_PATH, CONFIG_RECORD_MAX_BYTES, &record)) {
    goto done;
  }
  /* Bind Review to canonical owned names and one complete prior record. */
  struct config_record inspected = {0};
  if (record.digest.present && config_record_parse(record.bytes, record.size, &inspected) &&
      inspected.previous_menu.digest.present) {
    char path[256];
    int length = snprintf(path, sizeof(path), "boot/limine/%s", inspected.previous_menu.name);
    if (length <= 0 || (size_t)length >= sizeof(path) ||
        !install_snapshot_file(mount.root, path, CONFIG_FILE_MAX_BYTES, &previous_limine)) goto done;
  }
  if (!install_recovery_menu_review(mount.root, &record, &planned_facts, pool_partition) ||
      !install_recovery_prepare(mount.root, boot, bootstrap_capability("boot_kernel"),
          workspace, &planned_facts, &recovery)) {
    fputs("Static recovery or menu ownership is unrecognized; nothing written.\n", stderr);
    goto done;
  }
  /* A current/previous owned binding supersedes legacy fixed files. Only an
   * old legacy menu needs that additional proof. */
  if (!install_pending_inspect(&record, &menu, &previous_limine, planned_facts.disk_guid,
        pool_partition, NULL, &completion)) goto done;
  if (completion.state == INSTALL_PENDING_REFUSED && inspected.committed &&
      !inspected.previous_boot.present) {
    legacy_pair = install_boot_legacy_certify(mount.root, workspace, legacy_digests);
  }
  if (!install_snapshot_capacity(&disk, &planned_facts, &available_bytes, &cluster_bytes)) {
    fputs("Capacity inspection unavailable; nothing written.\n", stderr);
    goto done;
  }
  if (!install_pending_inspect(&record, &menu, &previous_limine, planned_facts.disk_guid,
        pool_partition, legacy_pair ? legacy_digests : NULL, &completion)) goto done;
  if (completion.state == INSTALL_PENDING_REFUSED || completion.state == INSTALL_PENDING_NONE) {
    fputs("Boot record/menu state is ambiguous; explicit manual disposition required.\n", stderr);
    goto done;
  }
  bool completing = completion.state == INSTALL_PENDING_ACTIVATION;
  const struct config_recovery_file *old = &completion.record.obsolete_menu;
  if (old->digest.present) {
    char path[256];
    int length = snprintf(path, sizeof(path), "boot/limine/%s", old->name);
    struct config_blob bytes = {0};
    bool ok = length > 0 && (size_t)length < sizeof(path) &&
        install_snapshot_file(mount.root, path, CONFIG_FILE_MAX_BYTES, &bytes);
    if (ok && bytes.digest.present) {
      ok = bytes.digest.size == old->digest.size && !strcmp(bytes.digest.sha256, old->digest.sha256);
      completing = true;
    }
    config_blob_destroy(&bytes);
    if (!ok) {
      fputs("Recorded obsolete menu copy differs; explicit manual disposition required.\n", stderr);
      goto done;
    }
  }
  char staged[CONFIG_ARTIFACT_NAME_BYTES], path[256];
  struct config_blob bytes = {0};
  bool staged_ok = config_record_artifact_name(completion.record.transaction, CONFIG_MENU_NAME,
      "new", staged);
  int length = staged_ok ? snprintf(path, sizeof(path), "boot/limine/%s", staged) : -1;
  staged_ok = staged_ok && length > 0 && (size_t)length < sizeof(path) &&
      install_snapshot_file(mount.root, path, CONFIG_FILE_MAX_BYTES, &bytes);
  if (staged_ok && bytes.digest.present) {
    staged_ok = config_blob_equal(&bytes, &completion.menu);
    completing = true;
  }
  config_blob_destroy(&bytes);
  if (!staged_ok) {
    fputs("Recorded staged menu differs; explicit manual disposition required.\n", stderr);
    goto done;
  }
  if (!completing) {
    fputs("No provable pending activation/cleanup remains; prepare a fresh Update instead.\n", stderr);
    goto done;
  }
  if (!retire(&mount) || handle_close(disk.handle) != CALL_OK) {
    goto done;
  }
  disk.handle = HANDLE_INVALID;
  uint64_t required = install_recovery_space(&recovery, cluster_bytes);
  fprintf(stderr, "Pending activation: %s. Reuse attempt %s; the boot record stays unchanged.\n"
      "Immutable rescue: %s; %llu bytes free, %llu bytes required.\n", completion.menu_changed ? "publish recorded menu" :
      "finish recorded cleanup", completion.record.boot.attempt, recovery.present ? "verify existing" :
      "provision small rescue first", (unsigned long long)available_bytes, (unsigned long long)required);
  const char *const actions[] = {"Complete pending publication", "Cancel"};
  size_t action;
  if (!choose(terminal, "Review: ", actions, 2, 0, &action, &result)) {
    goto done;
  }
  if (action == 1) {
    fputs("Cancelled; nothing written.\n", stderr);
    result = EXIT_SUCCESS;
    goto done;
  }
  if (required > available_bytes) {
    fputs("Pending completion not confirmed or insufficient capacity; nothing written.\n", stderr);
    goto done;
  }
  if (!confirm_word(terminal, "complete")) {
    fputs("Completion not confirmed; nothing written.\n", stderr);
    goto done;
  }
  if (!install_battery_check(bootstrap_capability("system_info"), stderr)) goto done;
  install_progress_begin("Scoped exclusive revalidation (no owned writes yet)", INSTALL_PROGRESS_RAW_READ, NULL);
  enum call_status status = disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk.handle);
  if (status == CALL_OK) status = disk_get_info(disk.handle, &disk.info);
  if (status == CALL_OK &&
      !completion_target(&disk, selected_esp, selected_pool, selected_guid, coexist)) {
    fputs("Selected pending installation changed after Review; nothing staged.\n", stderr);
    goto done;
  }
  if (status == CALL_OK) status = disks_open_owned_esp(disks, &target_selection,
      MOUNT_ACCESS_READ_WRITE, WRITE_RIGHTS, &mount);
  if (status != CALL_OK) {
    fprintf(stderr, "Scoped admission/stale input refused (status %u); nothing staged.\n", status);
    goto done;
  }
  if (mount_observe(mount.control, &request.expected_target) != CALL_OK ||
      !install_snapshot_facts(mount.control, &request.facts) ||
      !install_snapshot_same_target(&planned_facts, &request.facts)) goto done;
  uint64_t fresh_available, fresh_cluster;
  if (!install_snapshot_capacity(&disk, &planned_facts, &fresh_available, &fresh_cluster) ||
      fresh_cluster != cluster_bytes || required > fresh_available) {
    fputs("Staging capacity changed or unavailable; no owned files changed.\n", stderr);
    goto done;
  }
  request.esp_root = mount.root;
  request.view.control = mount.control;
  struct config_blob fresh_menu = {0}, fresh_record = {0};
  bool same = install_snapshot_file(mount.root, MENU_PATH, CONFIG_FILE_MAX_BYTES, &fresh_menu) &&
      config_blob_equal(&fresh_menu, &menu) &&
      install_snapshot_file(mount.root, RECORD_PATH, CONFIG_RECORD_MAX_BYTES, &fresh_record) &&
      config_blob_equal(&fresh_record, &record);
  config_blob_destroy(&fresh_menu);
  config_blob_destroy(&fresh_record);
  if (!same) {
    fputs("Writable admission found a stale boot menu or record; no owned files changed.\n", stderr);
    goto done;
  }
  if (!install_recovery_menu_review(mount.root, &record, &request.facts, pool_partition) ||
      !install_recovery_provision(mount.root, mount.control, workspace, &recovery)) {
    fputs("Static fallback not verified; no menu publication.\n", stderr);
    goto done;
  }
  /* Static rescue is durable before any menu publication. */
  if (!parent_open(mount.root, "EFI/pyxis/config", false, &request.view.config_parent) ||
      !parent_open(mount.root, "boot/limine", false, &request.view.limine_parent)) {
    goto done;
  }
  struct install_pending_request resume = {.esp_root = mount.root, .workspace = workspace,
    .view = request.view, .facts = request.facts, .expected_target = request.expected_target,
    .record = &record, .current_menu = &menu, .previous_limine = &previous_limine,
    .pool_partition = pool_partition, .fallback_ready = true};
  memcpy(resume.legacy, legacy_digests, sizeof(resume.legacy));
  struct config_storage_report report = {0};
  bool ok = install_pending_complete(&resume, &report);
  request.binding = completion.record.boot;
  publication_result(&request, &report, ok);
  if (ok) fputs("Pending activation completed. Build and Review a fresh plan for another Update.\n", stderr);
  result = ok ? EXIT_SUCCESS : EXIT_FAILURE;
done:
  if (request.view.config_parent != HANDLE_INVALID) {
    if (handle_close(request.view.config_parent) != CALL_OK) {
      result = EXIT_FAILURE;
    }
  }
  if (request.view.limine_parent != HANDLE_INVALID) {
    if (handle_close(request.view.limine_parent) != CALL_OK) {
      result = EXIT_FAILURE;
    }
  }
  if (!retire(&mount)) {
    fputs("Publication cleanup unconfirmed; view was not revoked or retried.\n", stderr);
    result = EXIT_FAILURE;
  }
  if (disk.handle != HANDLE_INVALID) {
    if (handle_close(disk.handle) != CALL_OK) {
      result = EXIT_FAILURE;
    }
  }
  install_pending_destroy(&completion);
  install_recovery_destroy(&recovery);
  config_blob_destroy(&previous_limine);
  config_blob_destroy(&menu);
  config_blob_destroy(&record);
  install_progress_finish();
  if (crypto) {
    hash_close();
  }
  return result;
}

int install_publication_complete(struct terminal *terminal, bool verbose, uint64_t disk,
    uint64_t esp, uint64_t pool, const char *guid, bool coexist)
{
  if (!disk || !esp || !pool || !guid) {
    return EXIT_FAILURE;
  }
  return pending_frontend(terminal, verbose, disk, esp, pool, guid, coexist);
}
