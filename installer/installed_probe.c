#include "installed_publication.h"
#include "boot_entry.h"

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
