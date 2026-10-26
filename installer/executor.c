#include "plan.h"
#include "battery.h"
#include "packages.h"
#include "../common/bootstrap_environment.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <string.h>

#define EXECUTION_CLEANUP_NS UINT64_C(30000000000)
#define COEXIST_ESP_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)
#define PACKAGE_CONFIG_MAX_BYTES (64u * 1024u)

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool valid = handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return valid;
}

static bool release_raw(struct install_disk *disk, bool *claimed)
{
  if (!*claimed) {
    return true;
  }
  enum call_status status = disk_release(disk->handle);
  /* RELEASE ends the claim even on failure; never retry its failed flush. */
  *claimed = false;
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: raw release failed (status %u); progress may be partial\n", status);
  }
  return status == CALL_OK;
}

static bool finish_disk(struct install_disk *disk, bool *claimed)
{
  bool ok = release_raw(disk, claimed);
  if (!close_handle(&disk->handle)) {
    fputs("installer: disk handle close found invalid bookkeeping\n", stderr);
    ok = false;
  }
  return ok;
}

bool install_plan_finish_execution(struct install_plan *plan)
{
  if (!plan) {
    return false;
  }
  bool ok = true;
  if (plan->payloads) {
    ok = install_esp_payload_close(plan->payloads) && ok;
    plan->payloads = NULL;
  }
  if (plan->classic_files) {
    ok = install_esp_classic_close(plan->classic_files) && ok;
    plan->classic_files = NULL;
  }
  if (!ok) {
    fputs("installer: staged EFI reference cleanup was not confirmed\n", stderr);
  }
  enum call_status status = CALL_OK;
  if (plan->view.control != HANDLE_INVALID) {
    if (!plan->cleanup_deadline_ns) {
      uint64_t now;
      status = clock_now(plan->clock, &now);
      if (status == CALL_OK) {
        if (now > UINT64_MAX - EXECUTION_CLEANUP_NS) {
          status = CALL_LIMIT;
        } else {
          plan->cleanup_deadline_ns = now + EXECUTION_CLEANUP_NS;
        }
      }
    }
    if (status == CALL_OK) {
      status = install_esp_view_close_and_retire(&plan->view, plan->clock,
          plan->cleanup_deadline_ns);
    }
  } else {
    status = install_esp_view_close_and_retire(&plan->view, plan->clock,
        plan->cleanup_deadline_ns);
  }
  if (status != CALL_OK) {
    fprintf(stderr, "installer: EFI view cleanup failed (status %u); ownership retained\n", status);
    ok = false;
  }
  ok = finish_disk(&plan->raw_disk, &plan->raw_claimed) && ok;
  ok = finish_disk(&plan->disk, &plan->disk_claimed) && ok;
  return ok;
}

static bool system_verify(const struct install_plan *plan)
{
  handle_t root = HANDLE_INVALID, marker = HANDLE_INVALID;
  bool ok = disk_open_volume(plan->disk.handle, plan->target.pool.partition, "system",
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &root) == CALL_OK;
  if (ok && !plan->record.updating) {
    uint64_t size;
    ok = directory_lookup(root, "SAFE_TO_WIPE", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &marker) == CALL_OK && file_size(marker, &size) == CALL_OK && size == 0;
  }
  ok = close_handle(&marker) && ok;
  if (ok && !plan->record.coexist) {
    char bytes[sizeof(INSTALL_CLASSIC_MARKER_BYTES)];
    uint64_t size;
    size_t read = 0;
    ok = directory_lookup(root, INSTALL_CLASSIC_MARKER_NAME, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &marker) == CALL_OK && file_size(marker, &size) == CALL_OK &&
        size == sizeof(INSTALL_CLASSIC_MARKER_BYTES) - 1 &&
        file_read(marker, 0, bytes, sizeof(bytes), &read) == CALL_OK && read == size &&
        !memcmp(bytes, INSTALL_CLASSIC_MARKER_BYTES, (size_t)size);
    ok = close_handle(&marker) && ok;
  }
  ok = close_handle(&root) && ok;
  return ok;
}

static bool classic_origin_prepare(const struct install_plan *plan)
{
  if (plan->record.updating || plan->record.coexist) {
    return true;
  }
  handle_t system = HANDLE_INVALID, marker = HANDLE_INVALID;
  enum call_status status = disk_open_volume(plan->disk.handle, plan->target.pool.partition,
      "system", DIRECTORY_CONTENT_RIGHTS, &system);
  if (status == CALL_OK) {
    status = directory_create(system, INSTALL_CLASSIC_MARKER_NAME, DIRECTORY_KIND_FILE,
        FILE_RIGHT_WRITE, &marker);
  }
  size_t written = 0;
  if (status == CALL_OK) {
    status = file_write(marker, 0, INSTALL_CLASSIC_MARKER_BYTES,
        sizeof(INSTALL_CLASSIC_MARKER_BYTES) - 1, &written);
    if (status == CALL_OK && written != sizeof(INSTALL_CLASSIC_MARKER_BYTES) - 1) {
      status = CALL_IO;
    }
  }
  if (status == CALL_OK) {
    status = file_sync(marker);
  }
  if (status == CALL_OK) {
    status = directory_sync(system);
  }
  bool ok = status == CALL_OK;
  ok = close_handle(&marker) && ok;
  ok = close_handle(&system) && ok;
  if (!ok) {
    fprintf(stderr, "installer: Classic origin marker preparation failed (status %u)\n", status);
  }
  return ok;
}

/* Copies NAME from SOURCE_DIR into TARGET_DIR unless TARGET_DIR already has it;
 * an existing file is never replaced or checked. */
static enum call_status seed_file(handle_t source_dir, handle_t target_dir, const char *name,
    uint64_t limit)
{
  handle_t source = HANDLE_INVALID, target = HANDLE_INVALID;
  enum call_status status = directory_lookup(target_dir, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &target);
  if (status == CALL_OK) {
    return close_handle(&target) ? CALL_OK : CALL_IO;
  }
  if (status != CALL_NOT_FOUND) {
    return status;
  }
  status = directory_lookup(source_dir, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &source);
  uint64_t size = 0;
  if (status == CALL_OK) status = file_size(source, &size);
  if (status == CALL_OK && size > limit) status = CALL_LIMIT;
  if (status == CALL_OK) {
    status = directory_create(target_dir, name, DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, &target);
  }
  char buffer[512];
  for (uint64_t offset = 0; status == CALL_OK && offset < size;) {
    size_t wanted = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer), read = 0;
    status = file_read(source, offset, buffer, wanted, &read);
    if (status == CALL_OK && (!read || read > wanted)) status = CALL_IO;
    for (size_t written = 0; status == CALL_OK && written < read;) {
      size_t count = 0;
      status = file_write(target, offset + written, buffer + written, read - written, &count);
      if (status == CALL_OK && (!count || count > read - written)) status = CALL_IO;
      written += count;
    }
    offset += read;
  }
  if (status == CALL_OK) status = file_sync(target);
  if (!close_handle(&target) && status == CALL_OK) status = CALL_IO;
  if (!close_handle(&source) && status == CALL_OK) status = CALL_IO;
  return status;
}

/* Seeds missing owner configuration in system://config/: packages.lua on every
 * run, and on a new install the initial boot configuration from
 * boot://share/installer/system/ (decision 27). Afterwards these files belong
 * to the administrator, so Update never replaces one, even when the new
 * release would reject its contents. */
static bool system_config_prepare(const struct install_plan *plan)
{
  static const char *const initial[] = {
    "boot.lua", "session.lua", "network.lua", "init-installed", "init-services",
  };
  const uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES;
  handle_t system = HANDLE_INVALID, config = HANDLE_INVALID;
  handle_t source_config = HANDLE_INVALID, share = HANDLE_INVALID;
  handle_t installer = HANDLE_INVALID, source_system = HANDLE_INVALID;
  enum call_status status = disk_open_volume(plan->disk.handle, plan->target.pool.partition,
      "system", DIRECTORY_CONTENT_RIGHTS, &system);
  if (status == CALL_OK) {
    status = directory_create(system, "config", DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_CONTENT_RIGHTS, &config);
    if (status == CALL_ALREADY_EXISTS) {
      status = directory_lookup(system, "config", DIRECTORY_KIND_DIRECTORY,
          DIRECTORY_CONTENT_RIGHTS, &config);
    }
  }
  if (status == CALL_OK) {
    status = directory_lookup(plan->boot, "config", DIRECTORY_KIND_DIRECTORY,
        directory_rights, &source_config);
  }
  if (status == CALL_OK) {
    status = seed_file(source_config, config, "packages.lua", PACKAGE_CONFIG_MAX_BYTES);
  }
  if (status == CALL_OK && !plan->record.updating) {
    status = directory_lookup(plan->boot, "share", DIRECTORY_KIND_DIRECTORY,
        directory_rights, &share);
    if (status == CALL_OK) {
      status = directory_lookup(share, "installer", DIRECTORY_KIND_DIRECTORY,
          directory_rights, &installer);
    }
    if (status == CALL_OK) {
      status = directory_lookup(installer, "system", DIRECTORY_KIND_DIRECTORY,
          directory_rights, &source_system);
    }
    for (size_t i = 0; status == CALL_OK && i < sizeof(initial) / sizeof(*initial); ++i) {
      status = seed_file(source_system, config, initial[i], UINT64_MAX);
    }
  }
  if (status == CALL_OK) status = directory_sync(config);
  if (status == CALL_OK) status = directory_sync(system);
  bool ok = status == CALL_OK;
  ok = close_handle(&source_system) && ok;
  ok = close_handle(&installer) && ok;
  ok = close_handle(&share) && ok;
  ok = close_handle(&source_config) && ok;
  ok = close_handle(&config) && ok;
  ok = close_handle(&system) && ok;
  if (!ok) {
    fprintf(stderr, "installer: system configuration preparation failed (status %u)\n", status);
  }
  return ok;
}

static bool flush_raw(const struct install_disk *disk)
{
  enum call_status status = disk_flush(disk->handle);
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: raw sync failed (status %u)\n", status);
  }
  return status == CALL_OK;
}

static bool open_pool(struct install_plan *plan)
{
  enum call_status status = disks_open_pool(plan->disks, &plan->target.pool,
      &plan->disk.handle);
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: selected pool admission failed (status %u)\n", status);
    return false;
  }
  plan->disk_claimed = true;
  return release_raw(&plan->disk, &plan->disk_claimed);
}

static const struct mount_state_reply *reviewed_esp(const struct install_plan *plan)
{
  if (!plan->record.coexist && (!plan->record.updating || plan->record.esp_rebuild)) {
    return NULL;
  }
  for (size_t i = 0; i < plan->target.review.esp_count; ++i) {
    const struct install_review_esp *esp = &plan->target.review.esps[i];
    if (esp->available && esp->identity_matches &&
        esp->selection.id == plan->target.esp.id &&
        esp->selection.partition == plan->target.esp.partition) {
      return &esp->observed;
    }
  }
  return NULL;
}

static bool stage_esp(struct install_plan *plan)
{
#if INSTALLER_PUBLICATION_SCRATCH
  if (install_plan_publication(plan)) {
    return install_installed_publish(plan);
  }
#endif
  enum call_status status = install_esp_view_open(&plan->view, plan->disks,
      &plan->target.esp, reviewed_esp(plan),
      plan->record.coexist ? COEXIST_ESP_RIGHTS : INSTALL_ESP_VIEW_RIGHTS);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: selected EFI view admission failed (status %u)\n", status);
    return false;
  }
  struct install_esp_payload_report report = {0};
  if (plan->record.coexist) {
    struct install_esp_payload_request *request = &plan->payload_request;
    request->root = plan->view.root;
    request->control = plan->view.control;
    request->expected = plan->view.expected;
    if (!install_esp_payload_stage(request, &plan->payloads, &report)) {
      fprintf(stderr, "installer: EFI payload stage failed (status %u, phase %u, artifact %s)\n",
          report.status, report.phase, report.artifact);
      return false;
    }
  } else {
    struct install_esp_activation_request *request = &plan->classic_request;
    request->root = plan->view.root;
    request->control = plan->view.control;
    request->expected = plan->view.expected;
    if (!install_esp_classic_stage(request, &plan->classic_files, &status)) {
      fprintf(stderr, "installer: classic EFI stage failed (status %u)\n", status);
      return false;
    }
  }
  status = install_esp_view_begin_checked(&plan->view);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: EFI verification interval failed (status %u)\n", status);
    return false;
  }
  bool verified;
  if (plan->record.coexist) {
    verified = install_esp_payload_verify(plan->payloads, &report);
    status = report.status;
  } else {
    verified = install_esp_classic_verify(plan->classic_files, &status);
    if (verified) {
      verified = install_esp_activate_classic(&plan->classic_request, &status);
    }
  }
  if (!verified) {
    fprintf(stderr, "installer: EFI verification/publication failed (status %u)\n", status);
    return false;
  }
  status = install_esp_view_end_checked(&plan->view, plan->clock);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: EFI interval end failed (status %u)\n", status);
  }
  return status == CALL_OK;
}

/* Removes program copies after a confirmed Update (installer.h). */
static void cleanup_programs(struct install_plan *plan)
{
  bool updating = plan->record.updating;
  const char *previous = updating ? plan->record.previous : NULL;
#if INSTALLER_PUBLICATION_SCRATCH
  if (install_plan_publication(plan) && updating) {
    if (!plan->publication_report.boot_activated) {
      fputs("Older program directories kept: boot activation is pending.\n", stderr);
      return;
    }
    struct config_record outgoing = {0};
    const struct config_blob *record = &plan->installed.record;
    if (record->digest.present &&
        (!config_record_parse(record->bytes, record->size, &outgoing) ||
         (strcmp(previous, INSTALL_UNKNOWN_REVISION) &&
          strcmp(previous, outgoing.boot.program_revision)))) {
      fputs("Older program directories kept: installed and boot revisions differ.\n", stderr);
      return;
    }
  }
#endif
  if (updating && !strcmp(previous, INSTALL_UNKNOWN_REVISION)) {
    fputs("Older program directories were kept: the previous revision is unknown.\n", stderr);
  } else if (!install_bin_cleanup(&plan->disk, plan->target.pool.partition, previous)) {
    fputs("installer: old program directory cleanup failed\n", stderr);
  }
}

bool install_plan_execute(struct install_plan *plan, const char *consent)
{
  if (!plan || !plan->ready || !consent || strcmp(consent,
        plan->record.updating ? "update" : plan->record.coexist ? "append" : "wipe") ||
      plan->disk.handle != HANDLE_INVALID || plan->disk_claimed ||
      plan->raw_disk.handle != HANDLE_INVALID || plan->raw_claimed || plan->view.attempted ||
      plan->payloads || plan->classic_files) {
    fputs("installer: plan was not confirmed or has pending execution ownership; nothing was written\n",
        stderr);
    return false;
  }
  if (!install_battery_check(bootstrap_capability("system_info"), stderr)) {
    plan->ready = false;
    return false;
  }
  install_progress_begin("Revalidating the reviewed target and frozen inputs", INSTALL_PROGRESS_NONE, NULL);
  if (!install_plan_revalidate(plan)) {
    plan->ready = false;
    install_progress_pause();
    fputs("installer: plan is stale or cannot be revalidated; nothing was written.\n"
        "Build and review a new plan; this one was not amended.\n", stderr);
    return false;
  }
  plan->ready = false;
  bool ok = false, raw_cleanup_failed = false;
  bool updating = plan->record.updating, coexist = plan->record.coexist;
  struct install_disk *disk = &plan->disk, *raw = &plan->raw_disk;
  const struct install_layout *layout = &plan->record.layout;
  enum call_status status;
  bool created_valid = false;
  int64_t created_ns = 0;
  if (!updating) {
    struct clock_wall_reading wall;
    created_valid = clock_wall_now(plan->clock, &wall) == CALL_OK;
    created_ns = created_valid ? npfs_timestamp(wall.seconds, wall.nanoseconds) : 0;
  }
  if (!updating && coexist) {
    struct disk_append_reply reply;
    status = disk_append_pool(plan->target.review_handle, &plan->target.pool, &reply);
    if (status != CALL_OK) {
      fprintf(stderr, "installer: pool append failed (status %u, phase %llu, effects possible %s, "
          "disk %llu, GPT entry %llu, first block %llu, blocks %llu, failed block %llu)\n",
          status, (unsigned long long)reply.phase,
          reply.flags & DISK_APPEND_EFFECTS_POSSIBLE ? "yes" : "unknown or none",
          (unsigned long long)plan->target.pool.id,
          (unsigned long long)plan->target.pool.partition,
          (unsigned long long)plan->target.pool.first_block,
          (unsigned long long)plan->target.pool.block_count,
          (unsigned long long)reply.failed_block);
      goto done;
    }
    disk->handle = reply.disk;
    plan->disk_claimed = true;
    if (!install_pool_format(disk, layout, &plan->header, plan->record.volume_id,
          created_ns, created_valid) || !flush_raw(disk) ||
        !release_raw(disk, &plan->disk_claimed)) {
      goto done;
    }
  } else {
    if (!updating) {
      status = disk_begin_erase(plan->target.review_handle, "wipe", &raw->handle);
      if (status != CALL_OK) {
        fprintf(stderr, "installer: reviewed erase writer admission failed (status %u)\n", status);
        goto done;
      }
      plan->raw_claimed = true;
      if (!install_pool_format(raw, layout, &plan->header, plan->record.volume_id,
            created_ns, created_valid) || !install_gpt_write(raw, layout) ||
          !plan->esp || !install_esp_write(plan->esp) || !flush_raw(raw)) {
        goto done;
      }
      if (!finish_disk(raw, &plan->raw_claimed)) {
        raw_cleanup_failed = true;
        goto done;
      }
    }
    if (!open_pool(plan)) {
      goto done;
    }
  }
#if INSTALLER_PUBLICATION_SCRATCH
  if (updating && plan->record.esp_rebuild) {
    status = disk_begin_esp_rebuild(plan->target.rebuild_review, "update", &raw->handle);
    if (status != CALL_OK) goto done;
    plan->raw_claimed = true;
    if (!plan->esp || !install_esp_write(plan->esp) || !flush_raw(raw) ||
        !finish_disk(raw, &plan->raw_claimed)) {
      raw_cleanup_failed = true;
      goto done;
    }
  }
  if (install_plan_publication(plan) && !install_installed_admit(plan)) goto done;
#endif
  install_progress_begin("Preparing the selected home volume", INSTALL_PROGRESS_NONE, NULL);
  status = disk_create_volume(disk->handle, plan->target.pool.partition, INSTALL_HOME_VOLUME);
  if (status != CALL_OK && status != CALL_ALREADY_EXISTS) {
    fprintf(stderr, "installer: home volume preparation failed (status %u)\n", status);
    goto done;
  }
  /* Update creates it on systems installed before packages; pkg fills it. */
  install_progress_begin("Preparing the packages volume", INSTALL_PROGRESS_NONE, NULL);
  status = disk_create_volume(disk->handle, plan->target.pool.partition, INSTALL_PACKAGES_VOLUME);
  if (status != CALL_OK && status != CALL_ALREADY_EXISTS) {
    fprintf(stderr, "installer: packages volume preparation failed (status %u)\n", status);
    goto done;
  }
  install_progress_begin("Preparing private HTTP backing", INSTALL_PROGRESS_NONE, NULL);
  status = disk_create_volume(disk->handle, plan->target.pool.partition, INSTALL_HTTP_WORK_VOLUME);
  if (status != CALL_OK && status != CALL_ALREADY_EXISTS) {
    fprintf(stderr, "installer: HTTP backing volume preparation failed (status %u)\n", status);
    goto done;
  }
  if (!system_config_prepare(plan) || !classic_origin_prepare(plan)) goto done;
  if (!system_verify(plan)) {
    goto done;
  }
  /* Update keeps the installed packages; pkg upgrades them later. */
  if (!updating && !install_packages_base(disk->handle, plan->target.pool.partition)) {
    goto done;
  }
#if !INSTALLER_PUBLICATION_SCRATCH
  if (updating && plan->record.esp_rebuild) {
    status = disk_begin_esp_rebuild(plan->target.rebuild_review, "update", &raw->handle);
    if (status != CALL_OK) {
      fprintf(stderr, "installer: reviewed EFI rebuild admission failed (status %u)\n", status);
      goto done;
    }
    plan->raw_claimed = true;
    if (!plan->esp || !install_esp_write(plan->esp) || !flush_raw(raw)) {
      goto done;
    }
    if (!finish_disk(raw, &plan->raw_claimed)) {
      raw_cleanup_failed = true;
      goto done;
    }
  }
#endif
  if (!stage_esp(plan)) {
    goto done;
  }
  cleanup_programs(plan);
  ok = true;

done:
  install_progress_finish();
  if (raw_cleanup_failed) {
    /* No EFI view was opened; do not retry the raw handle's failed close. */
    finish_disk(disk, &plan->disk_claimed);
  } else if (!install_plan_finish_execution(plan)) {
    ok = false;
  }
  if (!ok) {
    fputs("Apply failed; the target may contain partial changes. No mutation was retried.\n"
        "Inspect the target before preparing and reviewing another attempt.\n", stderr);
    return false;
  }
#if INSTALLER_PUBLICATION_SCRATCH
  if (install_plan_publication(plan)) {
    fputs(updating ? "updated\n" : "installed\n", stderr);
    fputs("Boot record and owned menu activation verified.\n", stderr);
  } else
#endif
  {
    if (coexist) {
      fputs("Coexistence payload staged, synchronized and verified. Existing boot menu unchanged.\n"
          "Review and add these entries to that menu before booting the new payload:\n", stderr);
      if (fwrite(plan->entry.bytes, 1, plan->entry.size, stderr) != plan->entry.size ||
          fflush(stderr) != 0) {
        return false;
      }
    } else {
      fputs(updating ? "updated\n" : "installed\n", stderr);
      fputs("Boot from this disk after removing the live medium.\n", stderr);
    }
  }
  if (!updating) {
    fputs("Deleting system://SAFE_TO_WIPE marks this installation as final.\n", stderr);
  }
  return true;
}
