#include "installed_publication.h"
#include "pending.h"

#include <pyxis_fs/fat32.h>
#include <stdlib.h>
#include <string.h>

#if INSTALLER_PUBLICATION_SCRATCH

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
  if (!read.bytes) {
    fprintf(stderr, "installer: boot record read %s failed (FAT32 status %u)\n", path, FAT32_NO_MEMORY);
    return FAT32_NO_MEMORY;
  }
  enum fat32_status status = fat32_read(volume, path, strlen(path), work, probe_sink, &read);
  if (status == FAT32_OK && !config_blob_set(blob, read.bytes, read.size)) status = FAT32_NO_MEMORY;
  free(read.bytes);
  if (status != FAT32_OK && status != FAT32_NOT_FOUND) {
    fprintf(stderr, "installer: boot record read %s failed (FAT32 status %u)\n", path, status);
  }
  return status;
}

static bool optional_file(const struct fat32_volume *volume, struct fat32_workspace *work,
    const char *path, size_t limit, struct config_blob *blob)
{
  enum fat32_status status = probe_file(volume, work, path, limit, blob);
  if (status == FAT32_NOT_FOUND) {
    config_blob_destroy(blob);
    return true;
  }
  return status == FAT32_OK;
}

static bool retained_cleanup(const struct fat32_volume *volume, struct fat32_workspace *work,
    const struct install_pending_plan *pending, bool *retained)
{
  const struct config_record *record = &pending->record;
  char path[256];
  struct config_blob artifact = {0};
  bool ok = true;
  *retained = false;
  const struct config_recovery_file *obsolete = &record->obsolete_menu;
  if (obsolete->digest.present) {
    snprintf(path, sizeof(path), "/boot/limine/%s", obsolete->name);
    ok = optional_file(volume, work, path, CONFIG_FILE_MAX_BYTES, &artifact);
    if (ok && artifact.digest.present) {
      ok = artifact.digest.size == obsolete->digest.size &&
          !strcmp(artifact.digest.sha256, obsolete->digest.sha256);
      *retained = true;
    }
    config_blob_destroy(&artifact);
  }
  char staged[CONFIG_ARTIFACT_NAME_BYTES];
  ok = ok && config_record_artifact_name(record->transaction, CONFIG_MENU_NAME, "new", staged);
  if (ok) {
    snprintf(path, sizeof(path), "/boot/limine/%s", staged);
    ok = optional_file(volume, work, path, CONFIG_FILE_MAX_BYTES, &artifact);
    if (ok && artifact.digest.present) {
      ok = config_blob_equal(&artifact, &pending->menu);
      *retained = true;
    }
  }
  config_blob_destroy(&artifact);
  return ok;
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
  if (!work.memory) {
    *reason = "boot record inspection allocation failed";
    return INSTALL_INSTALLED_REFUSED;
  }
  struct fat32_volume volume;
  enum fat32_status status = fat32_open(&source, (uint32_t)layout->esp_partition, &work, &volume);
  if (status != FAT32_OK) {
    free(work.memory);
    if (status == FAT32_IO || status == FAT32_NO_MEMORY || status == FAT32_LIMIT) {
      *reason = "ESP inspection failed or exceeded its bound";
      return INSTALL_INSTALLED_REFUSED;
    }
    return INSTALL_INSTALLED_ABSENT;
  }
  if (volume.partition_first != layout->esp_start / disk->info.block_size ||
      volume.partition_sectors != layout->esp_bytes / disk->info.block_size ||
      memcmp(volume.disk_guid, layout->disk_guid, 16) ||
      memcmp(volume.partition_guid, layout->esp_guid, 16)) {
    free(work.memory);
    *reason = "ESP identity differs from the reviewed layout";
    return INSTALL_INSTALLED_REFUSED;
  }
  struct config_blob record = {0}, menu = {0}, previous = {0};
  struct config_record parsed = {0};
  struct install_pending_plan pending = {0};
  enum install_installed_probe result = INSTALL_INSTALLED_REFUSED;
  *reason = "boot record inspection failed or exceeded its bound";
  size_t bytes;
  if (fat32_workspace_size(&volume, 16, &bytes) != FAT32_OK) {
    goto done;
  }
  if (bytes > work.bytes) {
    free(work.memory);
    work.bytes = bytes;
    work.memory = malloc(bytes);
    if (!work.memory) {
      goto done;
    }
  }
  status = probe_file(&volume, &work, "/EFI/pyxis/config/config.record", CONFIG_RECORD_MAX_BYTES, &record);
  if (status == FAT32_NOT_FOUND) {
    result = INSTALL_INSTALLED_ABSENT;
    *reason = NULL;
    goto done;
  }
  /* Decision 28: a record from before this format is refused, never read;
   * that machine reinstalls. */
  *reason = "the boot record is malformed or from an earlier release; back up home and reinstall";
  if (status != FAT32_OK || !config_record_parse(record.bytes, record.size, &parsed)) {
    goto done;
  }
  if (!parsed.committed) {
    *reason = "an earlier Update stopped before its boot record committed; the previous entries "
        "still boot; see Interrupted Update in the installer documentation";
    goto done;
  }
  strcpy(revision, parsed.boot.program_revision);
  *reason = "the recorded boot menu is unavailable; manual disposition required";
  char path[256];
  if (!optional_file(&volume, &work, "/boot/limine/" CONFIG_MENU_NAME, CONFIG_FILE_MAX_BYTES,
      &menu)) {
    goto done;
  }
  if (parsed.previous_menu.digest.present) {
    snprintf(path, sizeof(path), "/boot/limine/%s", parsed.previous_menu.name);
    if (!optional_file(&volume, &work, path, CONFIG_FILE_MAX_BYTES, &previous)) {
      goto done;
    }
  }
  if (!install_pending_inspect(&record, &menu, &previous, layout->disk_guid,
      layout->pool_partition, &pending)) {
    goto done;
  }
  if (pending.state != INSTALL_PENDING_MENU_CURRENT) {
    /* Discovery only routes this inherited record to a fresh completion Review.
     * Ambiguous menus confer no write authority. */
    result = INSTALL_INSTALLED_PENDING;
    *reason = "the committed boot record has pending or ambiguous activation; fresh completion Review required";
    goto done;
  }
  bool retained;
  *reason = "retained recovery differs from its committed record or cannot be inspected; manual disposition required";
  if (!retained_cleanup(&volume, &work, &pending, &retained)) {
    goto done;
  }
  result = retained ? INSTALL_INSTALLED_PENDING : INSTALL_INSTALLED_READY;
  *reason = retained ? "the committed boot record has retained recovery; fresh completion Review required" : NULL;
done:
  install_pending_destroy(&pending);
  config_blob_destroy(&record);
  config_blob_destroy(&menu);
  config_blob_destroy(&previous);
  free(work.memory);
  return result;
}
#endif
