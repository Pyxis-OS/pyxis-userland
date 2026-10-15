#include "plan.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

static bool marker_verify(const struct install_disk *disk)
{
  handle_t root = HANDLE_INVALID, marker = HANDLE_INVALID;
  bool ok = disk_open_volume(disk->handle, 2, "system",
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &root) == CALL_OK &&
      directory_lookup(root, "SAFE_TO_WIPE", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &marker) == CALL_OK;
  uint64_t size;
  if (ok) {
    ok = file_size(marker, &size) == CALL_OK && size == 0;
  }
  if (marker != HANDLE_INVALID) {
    ok = handle_close(marker) == 0 && ok;
  }
  if (root != HANDLE_INVALID) {
    ok = handle_close(root) == 0 && ok;
  }
  return ok;
}

static bool system_verify(const struct install_disk *disk)
{
  handle_t root = HANDLE_INVALID;
  if (disk_open_volume(disk->handle, 2, "system", DIRECTORY_RIGHT_LOOKUP, &root) != CALL_OK) {
    return false;
  }
  return handle_close(root) == 0;
}

bool install_plan_execute(struct install_plan *plan, const char *consent)
{
  bool updating = plan->record.updating;
  if (!plan->ready || !consent || strcmp(consent, updating ? "update" : "wipe")) {
    fputs("installer: plan was not confirmed; nothing was written\n", stderr);
    return false;
  }
  struct install_disk *disk = &plan->disk;
  const struct install_layout *layout = &plan->record.layout;
  const char *revision = plan->record.revision;
  bool claimed = false, attempted_write = false, esp_attempted = false;
  int result = EXIT_FAILURE;
  install_progress_begin("Claiming reviewed target and refusing stale inputs", INSTALL_PROGRESS_NONE, NULL);
  enum call_status status = disks_open(plan->disks, disk->info.id,
      DISK_ACCESS_READ_WRITE, &disk->handle);
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: cannot claim disk (status %u); nothing was written\n", status);
    goto done;
  }
  claimed = true;
  if (!install_plan_revalidate(plan)) {
    install_progress_pause();
    fputs("installer: plan is stale or cannot be revalidated; nothing was written.\n"
        "Build and review a new plan; this one was not amended.\n", stderr);
    goto done;
  }
  bool created_valid = false;
  int64_t created_ns = 0;
  if (!updating) {
    struct clock_wall_reading wall;
    created_valid = clock_wall_now(plan->clock, &wall) == CALL_OK;
    created_ns = created_valid ? npfs_timestamp(wall.seconds, wall.nanoseconds) : 0;
  }
  /* Programs first, then the ESP: an interruption before the ESP switch leaves
   * the old kernel booting the old revision's programs. */
  attempted_write = true;
  if (!updating) {
    if (!install_pool_format(disk, layout, &plan->header, plan->record.volume_id, created_ns, created_valid)) {
      goto done;
    }
    if (!install_gpt_write(disk, layout)) {
      goto done;
    }
    install_progress_begin_major("Flushing pool and GPT", INSTALL_PROGRESS_NONE, NULL);
    status = disk_flush(disk->handle);
    if (status != CALL_OK) {
      install_progress_pause();
      fprintf(stderr, "installer: pool sync failed (status %u)\n", status);
      goto done;
    }
  }
  install_progress_begin("Releasing raw claim and rescanning GPT", INSTALL_PROGRESS_NONE, NULL);
  status = disk_release(disk->handle);
  claimed = false;
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: raw release/GPT rescan failed (status %u)\n", status);
    goto done;
  }
  install_progress_begin("Checking GPT read-back", INSTALL_PROGRESS_NONE, NULL);
  struct disk_info verified;
  if (disk_get_info(disk->handle, &verified) != CALL_OK || verified.gpt_status != DISK_GPT_HEALTHY ||
      memcmp(verified.gpt_guid, layout->disk_guid, 16)) {
    install_progress_pause();
    fputs("installer: GPT read-back failed\n", stderr);
    goto done;
  }
  /* Update adds home to pools made before it existed and never opens it. */
  install_progress_begin("Preparing home volume", INSTALL_PROGRESS_NONE, NULL);
  status = disk_create_volume(disk->handle, INSTALL_POOL_PARTITION, INSTALL_HOME_VOLUME);
  install_progress_finish();
  if (status == CALL_OK) {
    install_progress_detail("Created the home volume.");
  } else if (status != CALL_ALREADY_EXISTS) {
    install_progress_pause();
    fprintf(stderr, "installer: cannot create the home volume (status %u)\n", status);
    goto done;
  }
  if (!install_programs_write(disk, layout->pool_partition, plan->boot, &plan->programs, revision,
        &plan->manifest, plan->manifest_text, plan->manifest_bytes)) {
    goto done;
  }
  install_progress_begin("Claiming EFI partition", INSTALL_PROGRESS_NONE, NULL);
  status = disk_claim(disk->handle, INSTALL_ESP_PARTITION);
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: cannot claim the EFI partition (status %u)\n", status);
    goto done;
  }
  claimed = true;
  esp_attempted = true;
  if (!install_esp_write(plan->esp)) {
    goto done;
  }
  install_progress_begin_major("Flushing EFI boot files", INSTALL_PROGRESS_NONE, NULL);
  status = disk_flush(disk->handle);
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: final sync failed (status %u)\n", status);
    goto done;
  }
  install_progress_begin("Releasing EFI partition and rescanning GPT", INSTALL_PROGRESS_NONE, NULL);
  status = disk_release(disk->handle);
  claimed = false;
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: EFI partition release failed (status %u)\n", status);
    goto done;
  }
  if (!install_esp_verify(plan->esp)) {
    install_progress_pause();
    fputs("installer: read-back check failed\n", stderr);
    goto done;
  }
  install_progress_begin(updating ? "Reopening system read-only" :
      "Reopening system read-only and checking its marker", INSTALL_PROGRESS_NONE, NULL);
  if (updating ? !system_verify(disk) : !marker_verify(disk)) {
    install_progress_pause();
    fputs("installer: read-back check failed\n", stderr);
    goto done;
  }
  /* Keep the new revision and the one the disk booted until now. When that
   * one is unknown, as after damaged boot files, nothing is removed. */
  install_progress_finish();
  const char *previous = updating ? plan->record.previous : NULL;
  if (updating && !strcmp(previous, INSTALL_UNKNOWN_REVISION)) {
    fputs("Older program directories were kept: the previous revision is unknown.\n", stderr);
  } else {
    if (!install_programs_cleanup(disk, layout->pool_partition, revision, previous)) {
      install_progress_pause();
      fputs("installer: old program directories could not all be removed\n", stderr);
    }
  }
  install_progress_finish();
  result = EXIT_SUCCESS;

done:
  install_progress_finish();
  if (claimed) {
    install_progress_begin("Releasing operation raw claim", INSTALL_PROGRESS_NONE, NULL);
    status = disk_release(disk->handle);
    if (status != CALL_OK) {
      install_progress_pause();
      fprintf(stderr, "installer: cleanup release failed (status %u)\n", status);
      result = EXIT_FAILURE;
    }
  }
  if (disk->handle != HANDLE_INVALID) {
    if (handle_close(disk->handle) != 0) {
      result = EXIT_FAILURE;
    }
    disk->handle = HANDLE_INVALID;
  }
  install_progress_finish();
  if (attempted_write && result != EXIT_SUCCESS) {
    if (updating && !esp_attempted) {
      if (strcmp(plan->record.previous, INSTALL_UNKNOWN_REVISION) &&
          strcmp(plan->record.previous, revision)) {
        fputs("Update failed before the EFI boot files changed; the disk still boots its\n"
            "previous revision. Boot the live image again, then choose Update.\n", stderr);
      } else {
        fputs("Update failed before the EFI boot files changed; its program revision may\n"
            "be partially rewritten. Boot the live image again, then choose Update.\n", stderr);
      }
    } else if (updating) {
      fputs("Update failed; the EFI boot files may be partially rebuilt.\n"
          "Boot the live image again, then choose Update to rebuild this disk's boot files.\n", stderr);
    } else {
      fputs("Installation failed; the disk may be partially rebuilt.\n"
          "Boot the live image again, then run the installer and choose Read the room to reinstall this disk.\n", stderr);
    }
  }
  if (result == EXIT_SUCCESS) {
    if (updating) {
      fputs("updated\nBoot from this disk after removing the live medium.\n", stderr);
    } else {
      fputs("installed\nBoot from this disk after removing the install medium.\n"
          "Deleting system://SAFE_TO_WIPE marks this installation as final.\n", stderr);
    }
  }
  return result == EXIT_SUCCESS;
}
