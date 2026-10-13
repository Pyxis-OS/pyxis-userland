#include "publication_snapshot.h"
#include "digest.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <pyxis_fs/fat32.h>
#include <stdlib.h>
#include <string.h>

bool install_snapshot_file(handle_t root, const char *path, size_t limit,
    struct config_blob *blob)
{
  handle_t parent = root, file = HANDLE_INVALID;
  const char *cursor = path;
  bool owned = false, ok = false;
  enum call_status status = CALL_BAD_REQUEST;
  for (;;) {
    const char *slash = strchr(cursor, '/');
    size_t size = slash ? (size_t)(slash - cursor) : strlen(cursor);
    char name[256];
    if (!size || size >= sizeof(name)) {
      break;
    }
    memcpy(name, cursor, size);
    name[size] = '\0';
    handle_t next = HANDLE_INVALID;
    status = directory_lookup(parent, name, slash ? DIRECTORY_KIND_DIRECTORY : DIRECTORY_KIND_FILE,
        slash ? DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES : FILE_RIGHT_READ, &next);
    if (owned) {
      if (handle_close(parent) != CALL_OK) {
        if (next != HANDLE_INVALID) {
          handle_close(next);
        }
        owned = false;
        break;
      }
      owned = false;
    }
    if (status != CALL_OK) {
      if (status == CALL_NOT_FOUND) {
        config_blob_destroy(blob);
        ok = true;
      }
      break;
    }
    if (!slash) {
      file = next;
      uint64_t size = 0, final_size;
      ok = file_size(file, &size) == CALL_OK && size <= limit;
      unsigned char *bytes = ok ? malloc((size_t)size + 1) : NULL;
      ok &= bytes != NULL;
      for (size_t offset = 0; ok && offset < size;) {
        size_t count = 0;
        status = file_read(file, offset, bytes + offset, (size_t)size - offset, &count);
        ok = status == CALL_OK && count && count <= size - offset;
        offset += ok ? count : 0;
      }
      ok = ok && file_size(file, &final_size) == CALL_OK && final_size == size &&
          config_blob_set(blob, bytes, (size_t)size);
      free(bytes);
      ok = handle_close(file) == CALL_OK && ok;
      break;
    }
    parent = next;
    owned = true;
    cursor = slash + 1;
  }
  if (owned) {
    ok = handle_close(parent) == CALL_OK && ok;
  }
  return ok;
}

bool install_snapshot_facts(handle_t control, struct config_target_facts *facts)
{
  struct mount_state_reply state;
  memset(facts, 0, sizeof(*facts));
  if (mount_observe(control, &state) != CALL_OK ||
      !(state.flags & MOUNT_STATE_IDENTITY_VALID) ||
      (state.flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_FAILED))) {
    return false;
  }
  *facts = (struct config_target_facts){
    .valid = true, .domain = state.domain, .device = state.device, .partition = state.partition,
    .first_block = state.first_block, .block_count = state.block_count,
    .device_block_size = state.device_block_size, .device_block_count = state.device_block_count,
    .partition_attributes = state.partition_attributes,
  };
  memcpy(facts->disk_guid, state.disk_guid, 16);
  memcpy(facts->partition_guid, state.partition_guid, 16);
  memcpy(facts->partition_type, state.partition_type, 16);
  return config_target_valid(facts);
}

bool install_snapshot_same_target(const struct config_target_facts *left,
    const struct config_target_facts *right)
{
  return config_target_valid(left) && config_target_valid(right) &&
      left->partition == right->partition && left->first_block == right->first_block &&
      left->block_count == right->block_count && left->device_block_size == right->device_block_size &&
      left->device_block_count == right->device_block_count &&
      left->partition_attributes == right->partition_attributes &&
      !memcmp(left->disk_guid, right->disk_guid, 16) &&
      !memcmp(left->partition_guid, right->partition_guid, 16) &&
      !memcmp(left->partition_type, right->partition_type, 16);
}

static bool read_sectors(void *context, uint64_t sector, uint32_t count, void *bytes)
{
  const struct install_disk *disk = context;
  return sector <= disk->info.block_count && count <= disk->info.block_count - sector &&
      install_read(disk, sector * disk->info.block_size, bytes,
        (size_t)count * (size_t)disk->info.block_size);
}

bool install_snapshot_capacity(const struct install_disk *disk,
    const struct config_target_facts *facts, uint64_t *free_bytes)
{
  if (!free_bytes) {
    return false;
  }
  *free_bytes = 0;
  if (!disk || !config_target_valid(facts) || facts->partition > UINT32_MAX ||
      disk->info.id != facts->device ||
      disk->info.block_size != facts->device_block_size ||
      disk->info.block_count != facts->device_block_count ||
      disk->info.block_count > UINT64_MAX / disk->info.block_size ||
      memcmp(disk->info.gpt_guid, facts->disk_guid, 16)) {
    return false;
  }
  struct fat32_source source = {.read = read_sectors, .context = (void *)disk,
    .sectors = disk->info.block_count, .sector_bytes = (uint32_t)disk->info.block_size,
    .max_read_sectors = DISK_IO_MAX_BYTES / disk->info.block_size};
  struct fat32_workspace work = {.bytes = fat32_open_workspace_size(),
    .step_limit = UINT64_C(16000000), .sector_limit = UINT64_C(16000000),
    .directory_depth_limit = 16};
  work.memory = malloc(work.bytes);
  struct fat32_volume volume;
  bool ok = work.memory && fat32_open(&source, (uint32_t)facts->partition, &work, &volume) == FAT32_OK &&
      volume.partition_first == facts->first_block && volume.partition_sectors == facts->block_count &&
      volume.partition_attributes == facts->partition_attributes &&
      !memcmp(volume.disk_guid, facts->disk_guid, 16) &&
      !memcmp(volume.partition_guid, facts->partition_guid, 16) &&
      !memcmp(volume.partition_type, facts->partition_type, 16);
  size_t bytes;
  if (ok) {
    ok = fat32_workspace_size(&volume, 16, &bytes) == FAT32_OK;
    if (ok && bytes > work.bytes) {
      free(work.memory);
      work.bytes = bytes;
      work.memory = malloc(bytes);
      ok = work.memory != NULL;
    }
  }
  struct fat32_check_report report;
  if (ok) {
    ok = fat32_check(&volume, &work, &report) == FAT32_OK;
  }
  if (ok) {
    uint64_t cluster_bytes = (uint64_t)volume.sectors_per_cluster * volume.source.sector_bytes;
    ok = cluster_bytes && report.free_clusters <= UINT64_MAX / cluster_bytes;
    if (ok) {
      *free_bytes = report.free_clusters * cluster_bytes;
    }
  }
  free(work.memory);
  return ok;
}

struct compare_bytes {
  const struct config_blob *expected;
  size_t offset;
};

static bool compare_sink(void *context, const void *bytes, size_t length)
{
  struct compare_bytes *compare = context;
  if (compare->offset > compare->expected->size ||
      length > compare->expected->size - compare->offset ||
      memcmp(bytes, compare->expected->bytes + compare->offset, length)) {
    return false;
  }
  compare->offset += length;
  return true;
}

bool install_snapshot_revalidate(const struct install_disk *disk,
    const struct config_target_facts *facts, const struct config_blob current[CONFIG_ALL_FILES],
    const struct config_blob *record)
{
  if (!config_target_valid(facts) || facts->partition > UINT32_MAX ||
      disk->info.block_size != facts->device_block_size ||
      disk->info.block_count != facts->device_block_count ||
      memcmp(disk->info.gpt_guid, facts->disk_guid, 16)) {
    return false;
  }
  struct fat32_source source = {.read = read_sectors, .context = (void *)disk,
    .sectors = disk->info.block_count, .sector_bytes = (uint32_t)disk->info.block_size,
    .max_read_sectors = DISK_IO_MAX_BYTES / disk->info.block_size};
  struct fat32_workspace work = {.bytes = fat32_open_workspace_size(),
    .step_limit = UINT64_C(16000000), .sector_limit = UINT64_C(16000000),
    .directory_depth_limit = 16};
  work.memory = malloc(work.bytes);
  struct fat32_volume volume;
  bool ok = work.memory && fat32_open(&source, (uint32_t)facts->partition, &work, &volume) == FAT32_OK &&
      volume.partition_first == facts->first_block && volume.partition_sectors == facts->block_count &&
      volume.partition_attributes == facts->partition_attributes &&
      !memcmp(volume.disk_guid, facts->disk_guid, 16) &&
      !memcmp(volume.partition_guid, facts->partition_guid, 16) &&
      !memcmp(volume.partition_type, facts->partition_type, 16);
  size_t bytes;
  if (ok) {
    ok = fat32_workspace_size(&volume, 16, &bytes) == FAT32_OK;
    if (ok && bytes > work.bytes) {
      free(work.memory);
      work.bytes = bytes;
      work.memory = malloc(bytes);
      ok = work.memory != NULL;
    }
  }
  const char *paths[CONFIG_ALL_FILES + 1] = {
    "/EFI/pyxis/config/boot.lua", "/EFI/pyxis/config/session.lua",
    "/EFI/pyxis/config/network.lua", "/EFI/pyxis/config/catalog.lua",
    "/boot/limine/limine.conf", "/EFI/pyxis/config/config.record",
  };
  for (unsigned i = 0; ok && i <= CONFIG_ALL_FILES; ++i) {
    const struct config_blob *expected = i == CONFIG_ALL_FILES ? record : &current[i];
    struct compare_bytes compare = {.expected = expected};
    enum fat32_status status = fat32_read(&volume, paths[i], strlen(paths[i]), &work, compare_sink, &compare);
    ok = expected->digest.present ? status == FAT32_OK && compare.offset == expected->size :
        status == FAT32_NOT_FOUND;
  }
  free(work.memory);
  return ok;
}


struct hash_bytes {
  struct install_digest digest;
  uint64_t size;
};

static bool hash_sink(void *context, const void *bytes, size_t length)
{
  struct hash_bytes *hash = context;
  if (length > UINT64_MAX - hash->size || !install_digest_update(&hash->digest, bytes, length)) {
    return false;
  }
  hash->size += length;
  return true;
}

bool install_snapshot_payload_revalidate(const struct install_disk *disk,
    const struct config_target_facts *facts, const char *path,
    const struct config_file_digest *digest)
{
  struct fat32_source source = {.read = read_sectors, .context = (void *)disk,
    .sectors = disk->info.block_count, .sector_bytes = (uint32_t)disk->info.block_size,
    .max_read_sectors = DISK_IO_MAX_BYTES / disk->info.block_size};
  struct fat32_workspace work = {.bytes = fat32_open_workspace_size(),
    .step_limit = UINT64_C(16000000), .sector_limit = UINT64_C(16000000), .directory_depth_limit = 16};
  work.memory = malloc(work.bytes);
  struct fat32_volume volume;
  bool ok = work.memory && fat32_open(&source, facts->partition, &work, &volume) == FAT32_OK &&
      !memcmp(volume.disk_guid, facts->disk_guid, 16) &&
      !memcmp(volume.partition_guid, facts->partition_guid, 16);
  size_t bytes;
  if (ok) {
    ok = fat32_workspace_size(&volume, 16, &bytes) == FAT32_OK;
    if (ok && bytes > work.bytes) {
      free(work.memory);
      work.bytes = bytes;
      work.memory = malloc(bytes);
      ok = work.memory != NULL;
    }
  }
  struct hash_bytes hash = {0};
  char actual[65];
  ok = ok && digest->present && install_digest_begin(&hash.digest) &&
      fat32_read(&volume, path, strlen(path), &work, hash_sink, &hash) == FAT32_OK &&
      hash.size == digest->size && install_digest_finish(&hash.digest, actual) && !strcmp(actual, digest->sha256);
  install_digest_abort(&hash.digest);
  free(work.memory);
  return ok;
}
