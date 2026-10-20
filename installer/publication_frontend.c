#include "publication_frontend.h"
#include "battery.h"
#include "coexist.h"
#include "pending.h"
#include "recovery.h"
#include "publication_snapshot.h"
#include "configuration.h"
#include "publication.h"
#include "boot_entry.h"
#include "esp_directory.h"
#include "digest.h"
#include "../config-editor/readiness.h"
#include "../libconfig/handoff.h"
#include "../xfer/hash.h"
#include "../common/bootstrap_environment.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <random.h>
#include <startup.h>
#include <system_info.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH_NAMESPACE_RESERVE_CLUSTERS UINT64_C(32)

#define READ_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define WRITE_RIGHTS (READ_RIGHTS | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_WRITE_FILES)

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

static bool number(const char *text, uint64_t *value)
{
  if (!*text) {
    return false;
  }
  uint64_t result = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || result > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    result = result * 10 + (*text - '0');
  }
  *value = result;
  return true;
}

static bool read_number(struct terminal *terminal, const char *prompt, uint64_t *value)
{
  char text[128];
  for (;;) {
    struct term_line_result input = term_read_line(terminal, prompt, text, sizeof(text));
    if (input.status != TERM_LINE_OK) {
      return false;
    }
    if (!input.limit_reached && number(text, value)) {
      return true;
    }
    fputs("Enter a whole decimal number.\n", stderr);
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

static bool payload_path(struct config_boot_payload *payload, const char *revision,
    const char *attempt, const char *name)
{
  int count = snprintf(payload->path, sizeof(payload->path), "EFI/pyxis/%s/%s/%s", revision, attempt, name);
  return count > 0 && (size_t)count < sizeof(payload->path);
}

static uint64_t required_capacity(const struct install_configuration *plan,
    const struct config_boot_binding *binding, uint64_t cluster)
{
  if (!cluster || cluster > UINT64_MAX / SCRATCH_NAMESPACE_RESERVE_CLUSTERS) {
    return UINT64_MAX;
  }
  uint64_t required = SCRATCH_NAMESPACE_RESERVE_CLUSTERS * cluster;
  const uint64_t payload_sizes[] = {binding->kernel.digest.size, binding->archive.digest.size,
    binding->revision.digest.size, CONFIG_RECORD_MAX_BYTES, CONFIG_RECORD_MAX_BYTES,
    CONFIG_RECORD_MAX_BYTES};
  for (unsigned i = 0; i < sizeof(payload_sizes) / sizeof(payload_sizes[0]); ++i) {
    uint64_t size = payload_sizes[i];
    uint64_t blocks = size / cluster + (size % cluster != 0);
    if (blocks > (UINT64_MAX - required) / cluster) {
      return UINT64_MAX;
    }
    required += blocks * cluster;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    uint64_t sizes[] = {plan->current[i].size, plan->after[i].size};
    for (unsigned n = 0; n < 2; ++n) {
      uint64_t blocks = sizes[n] / cluster + (sizes[n] % cluster != 0);
      if (blocks > (UINT64_MAX - required) / cluster) {
        return UINT64_MAX;
      }
      required += blocks * cluster;
    }
  }
  return required;
}

static const char *publication_phase(enum config_publish_phase phase)
{
  const char *const names[] = {"check", "stage", "verify", "exclusive revalidation",
    "prepared record", "canonical swap", "committed record", "boot activation", "cleanup", "done"};
  return (unsigned)phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "unknown";
}

static void publication_result(const struct install_publication_request *request,
    const struct config_storage_report *report, const struct config_boot_binding *obsolete, bool ok)
{
  fprintf(stderr, "Publication: status %u, phase %s, readiness %s, boot %s.\n",
      report->status, publication_phase(report->phase), report->committed ? "committed" : "not confirmed",
      report->boot_activated ? "entry confirmed" : report->committed ?
      "staged, boot activation pending" : "entry unchanged; staging/readiness incomplete");
  if (report->boot_cleanup_skipped) {
    fputs("Older boot cleanup skipped: ownership/hash evidence is uncertain; no removal attempted.\n", stderr);
  }
  if (ok && !report->boot_cleanup_skipped) {
    return;
  }
  fprintf(stderr, "Attempt: %s; inspect EFI/pyxis/config/config.record before recovery.\n",
      request->binding.kernel.path);
  if (report->artifact[0]) {
    fprintf(stderr, "Last artifact: %s\n", report->artifact);
  }
  if (obsolete && obsolete->present &&
      (report->boot_cleanup_skipped || (!ok && report->phase == CONFIG_PHASE_CLEANUP))) {
    fprintf(stderr, "Older cleanup candidates (inspect before recovery):\n  %s\n  %s\n  %s\n",
        obsolete->kernel.path, obsolete->archive.path, obsolete->revision.path);
  }
  if (!ok && config_transaction_valid(report->record.transaction)) {
    char name[CONFIG_ARTIFACT_NAME_BYTES];
    config_record_artifact_name(report->record.transaction, CONFIG_CATALOG, "prepared", name);
    fprintf(stderr, "Prepared record candidate: EFI/pyxis/config/%s\n", name);
    config_record_artifact_name(report->record.transaction, CONFIG_CATALOG, "committed", name);
    fprintf(stderr, "Committed record candidate: EFI/pyxis/config/%s\n", name);
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    const struct config_recovery_file *previous = &report->record.previous[i];
    if (previous->digest.present) {
      fprintf(stderr, "Recorded previous file: %s/%s\n",
          i == CONFIG_LIMINE ? "boot/limine" : "EFI/pyxis/config", previous->name);
    }
  }
  if (!ok) {
    fputs("Stopped; retain artifacts and verify the recorded hashes before explicit recovery.\n"
        "No automatic rollback, retry or reboot offer.\n", stderr);
  }
}

static bool ready_preview(struct install_configuration *plan, const struct config_blob *record)
{
  struct config_bytes archive[CONFIG_LUA_COUNT], esp[CONFIG_LUA_COUNT];
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    archive[i] = (struct config_bytes){(char *)plan->archive[i].bytes, plan->archive[i].size};
    esp[i] = (struct config_bytes){(char *)plan->current[i].bytes, plan->current[i].size};
  }
  struct config_bytes catalog = {(char *)plan->catalog_source.bytes, plan->catalog_source.size};
  struct config_bytes completion = {(char *)record->bytes, record->size};
  struct config_bytes current_catalog = {(char *)plan->current[CONFIG_CATALOG].bytes,
    plan->current[CONFIG_CATALOG].size};
  struct config_selection frozen = {0};
  const char *reason = NULL;
  struct config_file_digest loaded[2] = {0};
  const char *names[] = {"boot_kernel", "boot_archive"};
  for (unsigned i = 0; i < 2; ++i) {
    struct install_source source = {.handle = bootstrap_capability(names[i])};
    if (source.handle != HANDLE_INVALID && file_size(source.handle, &source.bytes) == CALL_OK) {
      install_boot_payload_digest(&source, &loaded[i]);
    }
  }
  bool ok = config_readiness_choose(archive, &catalog, plan->kernel_revision, plan->program_revision,
      &loaded[0], &loaded[1], &completion, esp, &current_catalog, true, false, &frozen, &reason);
  fprintf(stderr, "Current loaded-pair readiness: %s (%s).\n",
      ok && frozen.from_esp ? "ESP" : "archive", reason ? reason : "unavailable");
  config_selection_free(&frozen);
  return ok;
}


static int publication_frontend(struct terminal *terminal, bool verbose,
    uint64_t selected_id, uint64_t selected_esp, uint64_t selected_pool, const char *selected_guid)
{
  handle_t disks = bootstrap_capability("disks"), boot = bootstrap_capability("boot");
  handle_t clock = bootstrap_capability("clock"), random = bootstrap_capability("random");
  handle_t workspace = bootstrap_capability("config_snapshot_parent");
  struct install_disk disk = {.handle = HANDLE_INVALID};
  struct mount_fat32_reply mount = {.root = HANDLE_INVALID, .control = HANDLE_INVALID};
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0}, archive[CONFIG_LUA_COUNT] = {0};
  struct config_blob catalog = {0}, pool = {0};
  struct install_configuration plan = {0};
  struct config_handoff handoff = {0};
  struct install_programs programs = {0};
  struct install_source filtered = {.handle = HANDLE_INVALID};
  struct install_publication_request request = {.esp_root = HANDLE_INVALID,
    .workspace = workspace,
    .view = {.config_parent = HANDLE_INVALID, .limine_parent = HANDLE_INVALID, .control = HANDLE_INVALID, .clock = clock}};
  struct config_target_facts planned_facts = {0};
  struct config_file_digest legacy_digests[INSTALL_BOOT_LEGACY_PAYLOADS] = {0};
  bool legacy_pair = false;
  uint64_t available_bytes = 0, cluster_bytes = 0;
  int result = EXIT_FAILURE;
  bool crypto = hash_init(clock, random);
  bool completing = false;
  struct disk_partition_selection target_selection = {0};
  struct config_blob previous_limine = {0};
  struct install_pending_plan completion = {0};
  struct install_recovery recovery = {.sources = {{.handle = HANDLE_INVALID}, {.handle = HANDLE_INVALID}}};
  char revision[65], attempt[33], revision_text[66];
  if (!crypto || !install_battery_check(bootstrap_capability("system_info"), stderr)) {
    goto done;
  }
  bool completion_only = selected_id != 0;
  uint64_t id = selected_id, partition = selected_esp;
  if (!completion_only) {
    fputs("SCRATCH configuration publication. Disposable GPT/FAT32 images only.\n"
        "No program installation, formatting or firmware changes.\n", stderr);
    for (uint64_t index = 0;; ++index) {
      struct disk_info info;
      enum call_status status = disks_enumerate(disks, index, &info);
      if (status == CALL_NOT_FOUND) {
        break;
      }
      if (status != CALL_OK) {
        goto done;
      }
      fprintf(stderr, "Disk %llu: %llu blocks of %llu; GPT %llu\n", (unsigned long long)info.id,
          (unsigned long long)info.block_count, (unsigned long long)info.block_size,
          (unsigned long long)info.gpt_status);
    }
    if (!read_number(terminal, "Disposable disk ID: ", &id) ||
        !read_number(terminal, "ESP partition number: ", &partition)) {
      goto done;
    }
  }
  if (disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk.handle) != CALL_OK ||
      disk_get_info(disk.handle, &disk.info) != CALL_OK || disk.info.id != id ||
      !disk.info.block_size || disk.info.block_count > UINT64_MAX / disk.info.block_size) {
    goto done;
  }
  disk.bytes = disk.info.block_count * disk.info.block_size;
  install_progress_plain_stream(clock, stderr, bootstrap_capability("installer_diagnostics"), verbose);
  if (completion_only) {
    struct disk_partition_selection esp, native_pool;
    struct install_layout layout;
    struct install_update update;
    char guid[37];
    if (!selected_esp || !selected_pool || !selected_guid ||
        disk_partition_info(disk.handle, selected_esp, &esp) != CALL_OK ||
        disk_partition_info(disk.handle, selected_pool, &native_pool) != CALL_OK) {
      goto done;
    }
    install_coexist_layout(&esp, &native_pool, &layout);
    install_guid_text(layout.disk_guid, guid);
    if (strcmp(guid, selected_guid) || !install_scan_pool_update(&disk, &layout, &update) ||
        !update.eligible) {
      fputs("Selected pending installation changed or is unavailable; nothing written.\n", stderr);
      goto done;
    }
  }
  uint64_t pool_partition = completion_only ? selected_pool : INSTALL_POOL_PARTITION;
  if (!install_pool_boot_snapshot_at(&disk, pool_partition, &pool) ||
      disk_partition_info(disk.handle, partition, &target_selection) != CALL_OK ||
      disks_open_owned_esp(disks, &target_selection, MOUNT_ACCESS_READ_ONLY, READ_RIGHTS, &mount) != CALL_OK ||
      !install_snapshot_facts(mount.control, &planned_facts)) {
    fputs("Planning view/identity unavailable; nothing written.\n", stderr);
    goto done;
  }
  const char *paths[CONFIG_ALL_FILES] = {"EFI/pyxis/config/boot.lua", "EFI/pyxis/config/session.lua",
    "EFI/pyxis/config/network.lua", "EFI/pyxis/config/catalog.lua", "boot/limine/limine.conf"};
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!install_snapshot_file(mount.root, paths[i], CONFIG_FILE_MAX_BYTES, &current[i])) {
      goto done;
    }
  }
  if (!install_snapshot_file(mount.root, "EFI/pyxis/config/config.record", CONFIG_RECORD_MAX_BYTES, &record)) {
    goto done;
  }
  /* Bind Review to canonical owned names and one complete prior record. */
  struct config_record inspected = {0};
  if (record.digest.present && config_record_parse(record.bytes, record.size, &inspected) &&
      inspected.previous[CONFIG_LIMINE].digest.present) {
    char path[256];
    int length = snprintf(path, sizeof(path), "boot/limine/%s", inspected.previous[CONFIG_LIMINE].name);
    if (length <= 0 || (size_t)length >= sizeof(path) ||
        !install_snapshot_file(mount.root, path, CONFIG_FILE_MAX_BYTES, &previous_limine)) goto done;
  }
  if (!install_recovery_menu_review(mount.root, &record, &planned_facts) ||
      !install_recovery_prepare(mount.root, boot, bootstrap_capability("boot_kernel"),
          workspace, &planned_facts, &recovery)) {
    fputs("Static recovery or menu ownership is unrecognized; nothing written.\n", stderr);
    goto done;
  }
  /* A current/previous owned binding supersedes legacy fixed files. Only
   * initial adoption or an old legacy menu needs that additional proof. */
  if (!install_pending_inspect(&record, current, &previous_limine, planned_facts.disk_guid,
        NULL, &completion)) goto done;
  if (completion.state == INSTALL_PENDING_NONE ||
      (completion.state == INSTALL_PENDING_REFUSED && inspected.committed &&
      inspected.boot.present && !inspected.previous_boot.present)) {
    legacy_pair = install_boot_legacy_certify(mount.root, workspace, legacy_digests);
  }
  if (!install_snapshot_capacity(&disk, &planned_facts, &available_bytes, &cluster_bytes)) {
    fputs("Capacity inspection unavailable; nothing written.\n", stderr);
    goto done;
  }
  if (!install_pending_inspect(&record, current, &previous_limine, planned_facts.disk_guid,
        legacy_pair ? legacy_digests : NULL, &completion)) goto done;
  if (completion.state == INSTALL_PENDING_REFUSED) {
    fputs("Readiness/menu state is ambiguous; explicit manual disposition required.\n", stderr);
    goto done;
  }
  completing = completion.state == INSTALL_PENDING_ACTIVATION;
  if (completion.available || completion.state == INSTALL_PENDING_MENU_CURRENT) {
    for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
      const struct config_recovery_file *old = &completion.record.obsolete[i];
      if (!old->digest.present) continue;
      char path[256];
      int length = snprintf(path, sizeof(path), "%s/%s", i == CONFIG_LIMINE ?
          "boot/limine" : "EFI/pyxis/config", old->name);
      struct config_blob bytes = {0};
      bool ok = length > 0 && (size_t)length < sizeof(path) &&
          install_snapshot_file(mount.root, path, CONFIG_FILE_MAX_BYTES, &bytes);
      if (ok && bytes.digest.present) {
        ok = bytes.digest.size == old->digest.size && !strcmp(bytes.digest.sha256, old->digest.sha256);
        completing = true;
      }
      config_blob_destroy(&bytes);
      if (!ok) {
        fputs("Recorded obsolete artifact differs; explicit manual disposition required.\n", stderr);
        goto done;
      }
    }
    char staged[CONFIG_ARTIFACT_NAME_BYTES], path[256];
    struct config_blob bytes = {0};
    bool ok = config_record_artifact_name(completion.record.transaction, CONFIG_LIMINE, "new", staged);
    int length = ok ? snprintf(path, sizeof(path), "boot/limine/%s", staged) : -1;
    ok = ok && length > 0 && (size_t)length < sizeof(path) &&
        install_snapshot_file(mount.root, path, CONFIG_FILE_MAX_BYTES, &bytes);
    if (ok && bytes.digest.present) {
      ok = config_blob_equal(&bytes, &completion.menu);
      completing = true;
    }
    config_blob_destroy(&bytes);
    if (!ok) {
      fputs("Recorded staged menu differs; explicit manual disposition required.\n", stderr);
      goto done;
    }
  }
  if (completing && !completion.available) {
    fputs("Pending cleanup and changed selection need explicit manual disposition; nothing written.\n", stderr);
    goto done;
  }
  if (completion_only && !completing) {
    fputs("No provable pending activation/cleanup remains; prepare a fresh Update instead.\n", stderr);
    goto done;
  }
  if (!retire(&mount) || handle_close(disk.handle) != CALL_OK) {
    goto done;
  }
  disk.handle = HANDLE_INVALID;
  if (completing) {
    uint64_t required = install_recovery_space(&recovery, cluster_bytes);
    fprintf(stderr, "Pending activation: %s. Reuse attempt %s; readiness and retention stay unchanged.\n"
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
    goto admit;
  }
  const char *defaults[CONFIG_LUA_COUNT] = {"config/installed.lua", "config/session.lua", "config/network.lua"};
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (!install_snapshot_file(boot, defaults[i], CONFIG_FILE_MAX_BYTES, &archive[i]) || !archive[i].digest.present) {
      goto done;
    }
  }
  if (!install_snapshot_file(boot, "config/catalog.lua", CONFIG_FILE_MAX_BYTES, &catalog) ||
      !catalog.digest.present) {
    goto done;
  }
  struct system_info_identity identity;
  if (system_info_get_identity(bootstrap_capability("system_info"), &identity) != CALL_OK) {
    goto done;
  }
  if (!config_revision_valid(identity.build_revision)) {
    fputs("Actual kernel revision unavailable; readiness cannot be bound.\n", stderr);
    goto done;
  }
  strcpy(revision, identity.build_revision);
  struct install_configuration_input input = {.current = current, .record = &record,
    .archive = archive, .catalog = &catalog, .pool_override = &pool,
    .kernel_revision = revision, .program_revision = revision};
  if (!install_configuration_prepare(&input, &plan) || !ready_preview(&plan, &record)) {
    goto done;
  }
  struct install_source source_archive = {.handle = bootstrap_capability("boot_archive")};
  request.sources[0] = (struct install_source){.handle = bootstrap_capability("boot_kernel")};
  if (file_size(request.sources[0].handle, &request.sources[0].bytes) != CALL_OK ||
      file_size(source_archive.handle, &source_archive.bytes) != CALL_OK ||
      !install_programs_list(boot, &programs) || !install_archive_filter(&source_archive, INSTALL_POOL_PARTITION, &programs, &filtered)) {
    goto done;
  }
  request.sources[1] = filtered;
  snprintf(revision_text, sizeof(revision_text), "%s\n", revision);
  request.sources[2] = (struct install_source){.handle = HANDLE_INVALID,
    .bytes = strlen(revision_text), .memory = (uint8_t *)revision_text};
  uint64_t now;
  uint8_t random_bytes[16];
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - RANDOM_MAX_WAIT_NS ||
      random_read(random, random_bytes, sizeof(random_bytes), now + RANDOM_MAX_WAIT_NS) != CALL_OK) {
    goto done;
  }
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 16; ++i) {
    attempt[2 * i] = hex[random_bytes[i] >> 4];
    attempt[2 * i + 1] = hex[random_bytes[i] & 15];
  }
  attempt[32] = 0;
  struct config_boot_binding *binding = &request.binding;
  binding->present = true;
  strcpy(binding->kernel_revision, revision);
  strcpy(binding->program_revision, revision);
  strcpy(binding->attempt, attempt);
  binding->archive_catalog = catalog.digest;
  struct config_boot_payload *payloads[] = {&binding->kernel, &binding->archive, &binding->revision};
  const char *names[] = {"kernel.elf", "boot.arc", "revision.txt"};
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    if (!payload_path(payloads[i], revision, attempt, names[i]) ||
        !install_boot_payload_digest(&request.sources[i], &payloads[i]->digest)) {
      goto done;
    }
  }
  struct config_record previous = {0};
  bool previous_valid = record.digest.present && config_record_parse(record.bytes, record.size, &previous) && previous.committed;
  const struct install_boot_entry_paths previous_paths = {
    .kernel = previous.boot.kernel.path, .archive = previous.boot.archive.path};
  const struct install_boot_entry_paths *previous_pair = previous_valid && previous.boot.present ?
    &previous_paths : NULL;
  const struct install_boot_entry_paths next_paths = {
    .kernel = binding->kernel.path, .archive = binding->archive.path};
  bool adoption, pending;
  if (!install_boot_entry_plan(&current[CONFIG_LIMINE], previous_pair,
        planned_facts.disk_guid, &next_paths, legacy_pair, &plan.after[CONFIG_LIMINE], &adoption, &pending)) {
    goto done;
  }
  for (;;) {
    if (verbose) {
      install_configuration_review(&plan, stderr);
    } else {
      install_configuration_summary(&plan, stderr);
    }
    uint64_t payload_required = required_capacity(&plan, binding, cluster_bytes);
    uint64_t rescue_required = install_recovery_space(&recovery, cluster_bytes);
    uint64_t required = payload_required > UINT64_MAX - rescue_required ? UINT64_MAX :
        payload_required + rescue_required;
    fprintf(stderr, "Scratch staging requires at most %llu bytes; inspected free %llu (includes namespace reserve).\n",
        (unsigned long long)required, (unsigned long long)available_bytes);
    fprintf(stderr, "Immutable rescue: %s; independent lower-priority /boot/limine.conf.\n",
        recovery.present ? "verify existing pinned pair" : "provision small non-game pair first");
    fprintf(stderr, "Boot payload attempt: %s; %llu bytes kernel, %llu bytes archive.\n"
        "Boot entry: %s%s.\n", attempt, (unsigned long long)binding->kernel.digest.size,
        (unsigned long long)binding->archive.digest.size, pending ? "manual adoption; activation pending" : "owned switch last",
        adoption ? "; legacy adoption pre-selected" : "");
    if (verbose) {
      install_boot_entry_print(&next_paths, planned_facts.disk_guid, stderr);
    }
    if (ferror(stderr)) {
      goto done;
    }
    enum { REVIEW_CONFIRM, REVIEW_CANCEL, REVIEW_PLAN, REVIEW_SOURCE, REVIEW_ADOPT, REVIEW_CHANGE };
    const char *const actions[] = {
      "Confirm publication", "Cancel", "Show full plan", "Choose configuration source",
      "Adopt configuration file", "Change configuration choice",
    };
    size_t action;
    if (!choose(terminal, "Review: ", actions, sizeof(actions) / sizeof(actions[0]),
        REVIEW_CONFIRM, &action, &result)) {
      goto done;
    }
    if (action == REVIEW_CANCEL) {
      fputs("Cancelled; nothing written.\n", stderr);
      result = EXIT_SUCCESS;
      goto done;
    }
    if (action == REVIEW_PLAN) {
      install_configuration_review(&plan, stderr);
      install_boot_entry_print(&next_paths, planned_facts.disk_guid, stderr);
      continue;
    }
    if (action == REVIEW_CONFIRM) {
      if (!plan.applicable || required > available_bytes) {
        fputs("Plan is not applicable or lacks staging capacity; nothing written.\n", stderr);
        continue;
      }
      if (!confirm_word(terminal, "confirm")) {
        fputs("Publication not confirmed; nothing written.\n", stderr);
        goto done;
      }
      break;
    }
    bool ok = false;
    if (action == REVIEW_SOURCE) {
      const char *const sources[] = {"Archive defaults", "ESP configuration", "Back"};
      size_t source;
      if (!choose(terminal, "Configuration source: ", sources, 3,
          plan.source == CONFIG_ACTIVE_ESP ? 1 : 0, &source, &result)) {
        goto done;
      }
      if (source == 2) {
        continue;
      }
      ok = install_configuration_source(&plan,
          source == 0 ? CONFIG_ACTIVE_ARCHIVE : CONFIG_ACTIVE_ESP, true);
    } else if (action == REVIEW_ADOPT) {
      const char *const files[] = {"Boot configuration", "Session configuration", "Network configuration", "Back"};
      size_t file;
      if (!choose(terminal, "Adopt owner file: ", files, CONFIG_LUA_COUNT + 1, 0, &file, &result)) {
        goto done;
      }
      if (file == CONFIG_LUA_COUNT) {
        continue;
      }
      ok = install_configuration_adopt(&plan, file, true);
    } else if (action == REVIEW_CHANGE) {
      if (!plan.delta.count) {
        fputs("No configuration changes to select.\n", stderr);
        continue;
      }
      const char *changes[CONFIG_PLAN_CHANGE_LIMIT + 1];
      for (size_t i = 0; i < plan.delta.count; ++i) {
        changes[i] = plan.delta.changes[i].entry->id;
      }
      changes[plan.delta.count] = "Back";
      size_t change;
      if (!choose(terminal, "Configuration change: ", changes, plan.delta.count + 1,
          0, &change, &result)) {
        goto done;
      }
      if (change == plan.delta.count) {
        continue;
      }
      config_delta_plan_review(&plan.delta, change, stderr);
      const char *const choices[] = {"Keep", "Apply", "Adopt", "Remove", "Back"};
      size_t choice;
      if (!choose(terminal, "Change action: ", choices, CONFIG_CHOICE_REMOVE + 2,
          plan.delta.changes[change].choice, &choice, &result)) {
        goto done;
      }
      if (choice == CONFIG_CHOICE_REMOVE + 1) {
        continue;
      }
      ok = install_configuration_choose(&plan, change, choice);
    }
    if (!ok) {
      fputs("Choice refused; plan retained.\n", stderr);
    }
    /* Rebuilding config choices copies the original Limine snapshot. */
    if (!install_boot_entry_plan(&current[CONFIG_LIMINE], previous_pair,
          planned_facts.disk_guid, &next_paths, legacy_pair, &plan.after[CONFIG_LIMINE], &adoption, &pending)) {
      goto done;
    }
  }
  if (!install_battery_check(bootstrap_capability("system_info"), stderr)) {
    goto done;
  }
  enum call_status handoff_status;
  if (!config_handoff_create(workspace, &plan.selection,
        &handoff, &handoff_status)) {
    fprintf(stderr, "Private configuration FILE handoff refused (status %u); nothing written.\n", handoff_status);
    goto done;
  }
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    struct config_handoff_bytes bytes = {0};
    bool valid = config_handoff_read(handoff.files[i], &bytes, &handoff_status) &&
        bytes.size == plan.selection.sizes[i] &&
        !memcmp(bytes.bytes, plan.selection.bytes[i], bytes.size);
    config_handoff_bytes_free(&bytes);
    if (!valid) {
      fprintf(stderr, "Frozen configuration FILE verification refused (status %u); nothing written.\n", handoff_status);
      goto done;
    }
  }
  fputs("Frozen configuration handoff: three private unlinked native READ-only FILEs verified.\n", stderr);
  request.legacy_active = adoption;
  memcpy(request.legacy_expected, legacy_digests, sizeof(legacy_digests));
  binding->source = plan.source;
  binding->legacy_override = pool.digest;
  binding->legacy_disposition = plan.pool_disposition;
admit:
  if (!install_battery_check(bootstrap_capability("system_info"), stderr)) goto done;
  install_progress_begin("Scoped exclusive revalidation (no owned writes yet)", INSTALL_PROGRESS_RAW_READ, NULL);
  enum call_status status = disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk.handle);
  if (status == CALL_OK) status = disk_get_info(disk.handle, &disk.info);
  if (status == CALL_OK) status = disks_open_owned_esp(disks, &target_selection,
      MOUNT_ACCESS_READ_WRITE, WRITE_RIGHTS, &mount);
  struct config_blob current_pool = {0};
  if (status != CALL_OK ||
      !install_pool_boot_snapshot_at(&disk, pool_partition, &current_pool) || !config_blob_equal(&pool, &current_pool)) {
    config_blob_destroy(&current_pool);
    fprintf(stderr, "Scoped admission/stale input refused (status %u); nothing staged.\n", status);
    goto done;
  }
  config_blob_destroy(&current_pool);
  if (mount_observe(mount.control, &request.expected_target) != CALL_OK ||
      !install_snapshot_facts(mount.control, &request.facts) ||
      !install_snapshot_same_target(&planned_facts, &request.facts)) goto done;
  uint64_t fresh_available, fresh_cluster;
  uint64_t required = completing ? 0 : required_capacity(&plan, &request.binding, cluster_bytes);
  uint64_t rescue_required = install_recovery_space(&recovery, cluster_bytes);
  if (required > UINT64_MAX - rescue_required ||
      !install_snapshot_capacity(&disk, &planned_facts, &fresh_available, &fresh_cluster) ||
      fresh_cluster != cluster_bytes || required + rescue_required > fresh_available) {
    fputs("Staging capacity changed or unavailable; no owned files changed.\n", stderr);
    goto done;
  }
  request.esp_root = mount.root;
  request.view.control = mount.control;
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    struct config_blob fresh = {0};
    bool same = install_snapshot_file(mount.root, paths[i], CONFIG_FILE_MAX_BYTES, &fresh) &&
        config_blob_equal(&fresh, &current[i]);
    config_blob_destroy(&fresh);
    if (!same) {
      fputs("Writable admission found stale configuration; no owned files changed.\n", stderr);
      goto done;
    }
  }
  struct config_blob fresh_record = {0};
  bool same_record = install_snapshot_file(mount.root, "EFI/pyxis/config/config.record", CONFIG_RECORD_MAX_BYTES,
      &fresh_record) && config_blob_equal(&fresh_record, &record);
  config_blob_destroy(&fresh_record);
  if (!same_record || (!completing && !install_recovery_resolved(mount.root, &record))) {
    goto done;
  }
  if (request.legacy_active) {
    struct config_storage_report legacy_report = {0};
    if (!install_publication_legacy_check(mount.root, workspace, legacy_digests, &legacy_report)) {
      fputs("Legacy ownership evidence changed at writable admission; no owned files changed.\n", stderr);
      goto done;
    }
  }
  if (!install_recovery_menu_review(mount.root, &record, &request.facts) ||
      !install_recovery_provision(mount.root, mount.control, workspace, &recovery)) {
    fputs("Static fallback not verified; no readiness/menu publication.\n", stderr);
    goto done;
  }
  /* Static rescue is durable before any readiness/menu publication. */
  if (!parent_open(mount.root, "EFI/pyxis/config", true, &request.view.config_parent) ||
      (current[CONFIG_LIMINE].digest.present &&
       !parent_open(mount.root, "boot/limine", false, &request.view.limine_parent))) {
    goto done;
  }
  if (completing) {
    struct install_pending_request resume = {.esp_root = mount.root, .workspace = workspace,
      .view = request.view, .facts = request.facts, .expected_target = request.expected_target,
      .record = &record, .current = current, .previous_limine = &previous_limine, .fallback_ready = true};
    memcpy(resume.legacy, legacy_digests, sizeof(resume.legacy));
    struct config_storage_report report = {0};
    bool ok = install_pending_complete(&resume, &report);
    request.binding = completion.record.boot;
    publication_result(&request, &report, NULL, ok);
    if (ok) fputs("Pending activation completed. Build and Review a fresh plan for another Update.\n", stderr);
    result = ok ? EXIT_SUCCESS : EXIT_FAILURE;
    goto done;
  }
  request.expected = current;
  request.expected_record = &record;
  struct config_blob selected[CONFIG_ALL_FILES];
  memcpy(selected, plan.after, sizeof(selected));
  if (plan.source == CONFIG_ACTIVE_ARCHIVE) {
    memset(selected, 0, sizeof(selected[0]) * CONFIG_SET_FILES);
  }
  request.selected = selected;
  request.transaction = attempt;
  request.revision = plan.catalog->revision;
  struct config_storage_report report = {0};
  bool ok = install_publication_publish(&request, &report);
  publication_result(&request, &report, previous_valid ? &previous.previous_boot : NULL, ok);
  if (report.committed && pending) {
    install_boot_entry_print(&next_paths, planned_facts.disk_guid, stderr);
  }
  result = ok ? EXIT_SUCCESS : EXIT_FAILURE;
done:
  if (!config_handoff_close(&handoff)) {
    result = EXIT_FAILURE;
  }
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
  install_configuration_free(&plan);
  install_programs_destroy(&programs);
  free(filtered.memory);
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    config_blob_destroy(&current[i]);
  }
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    config_blob_destroy(&archive[i]);
  }
  config_blob_destroy(&record);
  config_blob_destroy(&catalog);
  config_blob_destroy(&pool);
  install_progress_finish();
  if (crypto) {
    hash_close();
  }
  return result;
}

int install_publication_scratch(struct terminal *terminal, bool verbose)
{
  return publication_frontend(terminal, verbose, 0, 0, 0, NULL);
}

int install_publication_complete(struct terminal *terminal, bool verbose, uint64_t disk,
    uint64_t esp, uint64_t pool, const char *guid)
{
  if (!disk || !esp || !pool || !guid) {
    return EXIT_FAILURE;
  }
  return publication_frontend(terminal, verbose, disk, esp, pool, guid);
}
