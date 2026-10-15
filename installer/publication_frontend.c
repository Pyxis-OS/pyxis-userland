#include "publication_frontend.h"
#include "publication_snapshot.h"
#include "configuration.h"
#include "publication.h"
#include "boot_entry.h"
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

static bool line(struct terminal *terminal, const char *prompt, char *text, size_t size)
{
  struct term_line_result result = term_read_line(terminal, prompt, text, size);
  return result.status == TERM_LINE_OK && !result.limit_reached;
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
    enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
        WRITE_RIGHTS, &next);
    if (create && status == CALL_NOT_FOUND) {
      status = directory_create(parent, name, DIRECTORY_KIND_DIRECTORY, WRITE_RIGHTS, &next);
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
      fprintf(stderr, "installer publication: view retirement refused (status %u); no revocation\n", status);
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
  bool ok = config_readiness_choose(archive, &catalog, plan->kernel_revision, plan->program_revision,
      &completion, esp, &current_catalog, true, false, &frozen, &reason);
  fprintf(stderr, "Frozen handoff preview: %s (%s); installed loader unchanged.\n",
      ok && frozen.from_esp ? "ESP" : "archive", reason ? reason : "unavailable");
  config_selection_free(&frozen);
  return ok;
}

static void configuration_summary(const struct install_configuration *plan, FILE *output)
{
  size_t required = 0, selected = 0;
  for (size_t i = 0; i < plan->delta.count; ++i) {
    required += plan->delta.changes[i].required;
    selected += plan->delta.changes[i].choice != CONFIG_CHOICE_KEEP;
  }
  fprintf(output, "Configuration source: %s; %s.\nReason: %s.\n",
      plan->source == CONFIG_ACTIVE_ESP ? "complete ESP set" : "whole archive",
      plan->applicable ? "applicable" : "blocked", plan->reason ? plan->reason : "not prepared");
  fprintf(output, "ESP deltas: %zu required, %zu selected / %zu total%s.\n", required,
      selected, plan->delta.count, plan->source == CONFIG_ACTIVE_ARCHIVE ? "; inactive" : "");
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (plan->needs_adoption[i]) {
      fprintf(output, "%s owner bytes: %s%s.\n", config_storage_file_name(i),
          plan->adopted[i] ? "adoption selected" : "Keep; adoption required for ESP",
          plan->source == CONFIG_ACTIVE_ARCHIVE ? "; retained inactive" : "");
    }
    const struct config_plan_file *file = &plan->delta.files[i];
    if (file->print_only && file->relevant) {
      fprintf(output, "%s: %s.\n", config_storage_file_name(i),
          file->reason ? file->reason : "printed changes only");
    }
  }
  if (plan->pool_override.digest.present) {
    fprintf(output, "Pool boot override: %s; original kept as inactive recovery after publication.\n",
        plan->source == CONFIG_ACTIVE_ARCHIVE ? "migration inactive; archive selected" :
        plan->migrate_pool ? "migration selected" : plan->pool_valid ? "migration not selected" :
        "unsupported/invalid; printed proposals only");
    if (plan->pool_conflict) {
      fputs("Pool/ESP boot inputs differ; no silent merge.\n", output);
    }
  }
  fputs("CPU/volume/timezone/network: not checked. Details and numbered choices: plan.\n", output);
}

int install_publication_scratch(struct terminal *terminal, bool verbose)
{
  handle_t disks = bootstrap_capability("disks"), boot = bootstrap_capability("boot");
  handle_t clock = bootstrap_capability("clock"), random = bootstrap_capability("random");
  struct install_disk disk = {.handle = HANDLE_INVALID};
  struct mount_fat32_reply mount = {.root = HANDLE_INVALID, .control = HANDLE_INVALID};
  struct config_blob current[CONFIG_ALL_FILES] = {0}, record = {0}, archive[CONFIG_LUA_COUNT] = {0};
  struct config_blob catalog = {0}, pool = {0};
  struct install_configuration plan = {0};
  struct config_handoff handoff = {0};
  struct install_programs programs = {0};
  struct install_source filtered = {.handle = HANDLE_INVALID};
  struct install_publication_request request = {.esp_root = HANDLE_INVALID,
    .view = {.config_parent = HANDLE_INVALID, .limine_parent = HANDLE_INVALID, .control = HANDLE_INVALID, .clock = clock}};
  struct config_target_facts planned_facts = {0};
  struct config_file_digest legacy_digests[INSTALL_BOOT_LEGACY_PAYLOADS] = {0};
  bool legacy_pair = false;
  uint64_t available_bytes = 0, cluster_bytes = 0;
  int result = EXIT_FAILURE;
  bool crypto = hash_init(clock, random), claimed = false;
  char text[128], revision[65], attempt[33], revision_text[66];
  if (!crypto) {
    goto done;
  }
  fputs("SCRATCH configuration publication. Disposable GPT/FAT32 images only.\n"
      "No installed activation, program installation, formatting or firmware changes.\n", stderr);
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
  uint64_t id, partition;
  if (!line(terminal, "Disposable disk ID: ", text, sizeof(text)) || !number(text, &id) ||
      !line(terminal, "ESP partition number: ", text, sizeof(text)) || !number(text, &partition) ||
      disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk.handle) != CALL_OK ||
      disk_get_info(disk.handle, &disk.info) != CALL_OK ||
      disk.info.block_count > UINT64_MAX / disk.info.block_size) {
    goto done;
  }
  disk.bytes = disk.info.block_count * disk.info.block_size;
  install_progress_plain_stream(clock, stderr, bootstrap_capability("installer_diagnostics"), verbose);
  if (!install_pool_boot_snapshot(&disk, &pool) ||
      disk_open_fat32(disk.handle, partition, MOUNT_ACCESS_READ_ONLY, READ_RIGHTS, &mount) != CALL_OK ||
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
  /* Evidence remains bound to this planning view and is rechecked under the
   * initial raw claim and again after writable admission. */
  legacy_pair = install_boot_legacy_certify(mount.root, legacy_digests);
  if (!install_snapshot_capacity(&disk, &planned_facts, &available_bytes, &cluster_bytes)) {
    fputs("Capacity inspection unavailable; nothing written.\n", stderr);
    goto done;
  }
  if (!retire(&mount) || handle_close(disk.handle) != CALL_OK) {
    goto done;
  }
  disk.handle = HANDLE_INVALID;
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
      !install_programs_list(boot, &programs) || !install_archive_filter(&source_archive, &programs, &filtered)) {
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
      configuration_summary(&plan, stderr);
    }
    uint64_t required = required_capacity(&plan, binding, cluster_bytes);
    fprintf(stderr, "Scratch staging requires at most %llu bytes; inspected free %llu (includes namespace reserve).\n",
        (unsigned long long)required, (unsigned long long)available_bytes);
    fprintf(stderr, "Boot payload attempt: %s; %llu bytes kernel, %llu bytes archive.\n"
        "Boot entry: %s%s.\n", attempt, (unsigned long long)binding->kernel.digest.size,
        (unsigned long long)binding->archive.digest.size, pending ? "manual adoption; activation pending" : "owned switch last",
        adoption ? "; legacy adoption pre-selected" : "");
    if (verbose) {
      install_boot_entry_print(&next_paths, planned_facts.disk_guid, stderr);
    }
    fputs("Review actions: confirm, cancel, plan, source archive, source esp, adopt boot|session|network,\n"
        "or change NUMBER keep|apply|adopt|remove.\n", stderr);
    if (ferror(stderr)) {
      goto done;
    }
    if (!line(terminal, "Review: ", text, sizeof(text)) || !strcmp(text, "cancel")) {
      fputs("Cancelled; nothing written.\n", stderr);
      result = EXIT_SUCCESS;
      goto done;
    }
    if (!strcmp(text, "plan")) {
      install_configuration_review(&plan, stderr);
      install_boot_entry_print(&next_paths, planned_facts.disk_guid, stderr);
      continue;
    }
    if (!strcmp(text, "confirm")) {
      if (!plan.applicable || required > available_bytes) {
        fputs("Plan is not applicable or lacks staging capacity; nothing written.\n", stderr);
        continue;
      }
      break;
    }
    bool ok = false;
    if (!strcmp(text, "source archive") || !strcmp(text, "source esp")) {
      ok = install_configuration_source(&plan,
          !strcmp(text, "source archive") ? CONFIG_ACTIVE_ARCHIVE : CONFIG_ACTIVE_ESP, true);
    } else if (!strncmp(text, "adopt ", 6)) {
      for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
        const char *file_names[] = {"boot", "session", "network"};
        if (!strcmp(text + 6, file_names[i])) {
          ok = install_configuration_adopt(&plan, i, true);
        }
      }
    } else if (!strncmp(text, "change ", 7)) {
      char *separator = strchr(text + 7, ' ');
      uint64_t index;
      if (separator) {
        *separator++ = 0;
        if (number(text + 7, &index) && index && index <= plan.delta.count) {
          for (unsigned choice = 0; choice <= CONFIG_CHOICE_REMOVE; ++choice) {
            if (!strcmp(separator, config_plan_choice_name(choice))) {
              ok = install_configuration_choose(&plan, index - 1, choice);
            }
          }
        }
      }
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
  enum call_status handoff_status;
  if (!config_handoff_create(bootstrap_capability("config_snapshot_parent"), &plan.selection,
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
  install_progress_begin("Exclusive initial revalidation (no writes yet)", INSTALL_PROGRESS_RAW_READ, NULL);
  enum call_status status = disks_open(disks, id, DISK_ACCESS_READ_WRITE, &disk.handle);
  claimed = status == CALL_OK;
  struct config_blob current_pool = {0};
  if (!claimed || disk_get_info(disk.handle, &disk.info) != CALL_OK ||
      !install_snapshot_revalidate(&disk, &planned_facts, current, &record) ||
      !install_pool_boot_snapshot(&disk, &current_pool) || !config_blob_equal(&pool, &current_pool)) {
    config_blob_destroy(&current_pool);
    fputs("Stale/unavailable input; nothing staged; no retry.\n", stderr);
    goto done;
  }
  config_blob_destroy(&current_pool);
  uint64_t fresh_available, fresh_cluster;
  if (!install_snapshot_capacity(&disk, &planned_facts, &fresh_available, &fresh_cluster) ||
      fresh_cluster != cluster_bytes || required_capacity(&plan, binding, fresh_cluster) > fresh_available) {
    fputs("Staging capacity changed or unavailable; nothing written.\n", stderr);
    goto done;
  }
  const char *legacy_paths[] = {"/boot/caelum.elf", "/boot/initrd.cpio", "/boot/revision"};
  for (unsigned i = 0; legacy_pair && i < 3; ++i) {
    if (!install_snapshot_payload_revalidate(&disk, &planned_facts, legacy_paths[i], &legacy_digests[i])) {
      fputs("Legacy boot ownership evidence changed; nothing staged.\n", stderr);
      goto done;
    }
  }
  if (disk_release(disk.handle) != CALL_OK) {
    claimed = false;
    goto done;
  }
  claimed = false;
  status = disk_open_fat32(disk.handle, partition, MOUNT_ACCESS_READ_WRITE, WRITE_RIGHTS, &mount);
  if (status != CALL_OK || mount_observe(mount.control, &request.expected_target) != CALL_OK ||
      !install_snapshot_facts(mount.control, &request.facts) ||
      !install_snapshot_same_target(&planned_facts, &request.facts)) {
    fprintf(stderr, "Writable view refused (status %u); no publication.\n", status);
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
  if (!same_record) {
    goto done;
  }
  if (request.legacy_active) {
    struct config_storage_report legacy_report = {0};
    if (!install_publication_legacy_check(mount.root, legacy_digests, &legacy_report)) {
      fputs("Legacy ownership evidence changed at writable admission; no owned files changed.\n", stderr);
      goto done;
    }
  }
  /* Creating missing parents is the first owned mutation after initial checks. */
  if (!parent_open(mount.root, "EFI/pyxis/config", true, &request.view.config_parent) ||
      (current[CONFIG_LIMINE].digest.present &&
       !parent_open(mount.root, "boot/limine", false, &request.view.limine_parent))) {
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
  fprintf(stderr, "Scratch publication: status %u, phase %u, readiness %s, boot %s.\n",
      report.status, report.phase, report.committed ? "committed" : "not confirmed",
      report.boot_activated ? "entry confirmed" : report.committed ?
      "staged, boot activation pending" : "entry unchanged; staging/readiness incomplete");
  if (report.committed && pending) {
    install_boot_entry_print(&next_paths, planned_facts.disk_guid, stderr);
  }
  if (!ok) {
    fputs("Stopped; retain artifacts for manual disposition. No rollback, retry or reboot offer.\n", stderr);
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
    fputs("Scratch publication cleanup unconfirmed; view was not revoked or retried.\n", stderr);
    result = EXIT_FAILURE;
  }
  if (disk.handle != HANDLE_INVALID) {
    if (claimed) {
      if (disk_release(disk.handle) != CALL_OK) {
        result = EXIT_FAILURE;
      }
    }
    if (handle_close(disk.handle) != CALL_OK) {
      result = EXIT_FAILURE;
    }
  }
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
