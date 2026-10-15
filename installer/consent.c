#include "installer.h"
#include "inventory.h"
#include "digest.h"

#include <directory.h>
#include <pyxis_fs/npfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPT_HEADER_BYTES 92u
#define GPT_ENTRY_BYTES 128u
#define GPT_RESERVED_ATTRIBUTES UINT64_C(0x0000fffffffffff8)

enum inspection { INSPECT_OK, INSPECT_DAMAGED, INSPECT_FATAL };
enum gpt_state { GPT_ABSENT, GPT_VALID, GPT_DAMAGED, GPT_UNSUPPORTED, GPT_FATAL };

struct gpt_copy {
  enum gpt_state state;
  uint32_t header_bytes, count, entry_bytes, table_bytes;
  uint64_t first, last;
  uint8_t guid[16];
  uint8_t table[INSTALL_GPT_TABLE_MAX_BYTES];
};

struct overlay_entry {
  uint64_t home;
  uint32_t kind, index, crc;
};

struct pool_view {
  const struct install_disk *disk;
  uint64_t offset, blocks;
  struct npfs_header header;
  struct npfs_control control;
  bool control_degraded;
  struct overlay_entry *overlay;
  uint32_t overlay_count;
  uint8_t scratch[3][NPFS_BLOCK_SIZE];
};

static bool zero_bytes(const uint8_t *bytes, size_t length)
{
  for (size_t i = 0; i < length; i++) {
    if (bytes[i]) {
      return false;
    }
  }
  return true;
}

static enum gpt_state read_gpt(const struct install_disk *disk, unsigned copy,
    uint8_t *block, struct gpt_copy *gpt)
{
  install_progress_begin(copy ? "Inspecting backup GPT metadata" :
      "Inspecting primary GPT metadata", INSTALL_PROGRESS_RAW_READ, NULL);
  uint64_t last = disk->info.block_count - 1;
  uint64_t location = copy ? last : 1;
  size_t block_bytes = (size_t)disk->info.block_size;
  if (!install_read(disk, location * block_bytes, block, block_bytes)) {
    return GPT_FATAL;
  }
  if (memcmp(block, "EFI PART", 8)) {
    return GPT_ABSENT;
  }
  uint32_t header_bytes = npfs_get_u32(block + 12);
  if (header_bytes < GPT_HEADER_BYTES || header_bytes > block_bytes) {
    return GPT_DAMAGED;
  }
  uint32_t crc = npfs_get_u32(block + 16);
  npfs_put_u32(block + 16, 0);
  if (install_crc32(block, header_bytes) != crc) {
    return GPT_DAMAGED;
  }
  if (npfs_get_u32(block + 8) != UINT32_C(0x00010000)) {
    return GPT_UNSUPPORTED;
  }
  if (npfs_get_u32(block + 20) ||
      !zero_bytes(block + GPT_HEADER_BYTES, block_bytes - GPT_HEADER_BYTES) ||
      npfs_get_u64(block + 24) != location || npfs_get_u64(block + 32) != (copy ? 1 : last)) {
    return GPT_DAMAGED;
  }
  gpt->header_bytes = header_bytes;
  gpt->first = npfs_get_u64(block + 40);
  gpt->last = npfs_get_u64(block + 48);
  memcpy(gpt->guid, block + 56, 16);
  uint64_t table_lba = npfs_get_u64(block + 72);
  gpt->count = npfs_get_u32(block + 80);
  gpt->entry_bytes = npfs_get_u32(block + 84);
  uint32_t table_crc = npfs_get_u32(block + 88);
  if (zero_bytes(gpt->guid, 16) || !gpt->count || gpt->entry_bytes < GPT_ENTRY_BYTES ||
      (gpt->entry_bytes & (gpt->entry_bytes - 1))) {
    return GPT_DAMAGED;
  }
  uint64_t table_bytes = (uint64_t)gpt->count * gpt->entry_bytes;
  uint64_t table_blocks = (table_bytes + block_bytes - 1) / block_bytes;
  uint64_t reserved = table_blocks;
  if (reserved < 16384 / block_bytes) {
    reserved = 16384 / block_bytes;
  }
  if (reserved >= last || gpt->first < 2 + reserved || gpt->first > gpt->last ||
      gpt->last >= last - reserved || table_lba >= last || table_blocks > last - table_lba ||
      (copy ? table_lba <= gpt->last : (table_lba < 2 || table_lba + table_blocks > gpt->first))) {
    return GPT_DAMAGED;
  }
  if (gpt->count > INSTALL_GPT_MAX_PARTITIONS || table_bytes > INSTALL_GPT_TABLE_MAX_BYTES) {
    return GPT_UNSUPPORTED;
  }
  gpt->table_bytes = (uint32_t)table_bytes;
  if (!install_read(disk, table_lba * block_bytes, gpt->table, (size_t)table_blocks * block_bytes)) {
    return GPT_FATAL;
  }
  if (install_crc32(gpt->table, gpt->table_bytes) != table_crc) {
    return GPT_DAMAGED;
  }
  for (uint32_t i = 0; i < gpt->count; i++) {
    const uint8_t *entry = gpt->table + (size_t)i * gpt->entry_bytes;
    if (!zero_bytes(entry + GPT_ENTRY_BYTES, gpt->entry_bytes - GPT_ENTRY_BYTES)) {
      return GPT_DAMAGED;
    }
    if (zero_bytes(entry, 16)) {
      continue;
    }
    uint64_t first = npfs_get_u64(entry + 32), end = npfs_get_u64(entry + 40);
    if (zero_bytes(entry + 16, 16) || first > end || first < gpt->first || end > gpt->last) {
      return GPT_DAMAGED;
    }
    if (npfs_get_u64(entry + 48) & GPT_RESERVED_ATTRIBUTES) {
      return GPT_UNSUPPORTED;
    }
    for (uint32_t j = 0; j < i; j++) {
      const uint8_t *other = gpt->table + (size_t)j * gpt->entry_bytes;
      if (!zero_bytes(other, 16) && (!memcmp(entry + 16, other + 16, 16) ||
          (first <= npfs_get_u64(other + 40) && npfs_get_u64(other + 32) <= end))) {
        return GPT_DAMAGED;
      }
    }
  }
  return GPT_VALID;
}

static bool protective_mbr(const uint8_t *block, const struct install_disk *disk)
{
  if (npfs_get_u16(block + 510) != UINT16_C(0xaa55)) {
    return false;
  }
  unsigned protective = 0;
  uint64_t count = disk->info.block_count - 1;
  uint32_t expected = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
  for (unsigned i = 0; i < 4; i++) {
    const uint8_t *entry = block + 446 + i * 16;
    if (entry[4] == 0xee) {
      protective++;
      if (npfs_get_u32(entry + 8) != 1 || npfs_get_u32(entry + 12) != expected) {
        return false;
      }
    } else if (!zero_bytes(entry, 16)) {
      return false;
    }
  }
  return protective == 1 && zero_bytes(block + 512, (size_t)disk->info.block_size - 512);
}

static bool gpt_agree(const struct gpt_copy *a, const struct gpt_copy *b)
{
  return a->header_bytes == b->header_bytes && a->first == b->first && a->last == b->last &&
    a->count == b->count && a->entry_bytes == b->entry_bytes && !memcmp(a->guid, b->guid, 16) &&
    !memcmp(a->table, b->table, a->table_bytes);
}

bool install_gpt_inspect(const struct install_disk *disk, struct install_gpt *gpt,
    const char **reason)
{
  if (reason) {
    *reason = "unsupported disk geometry or missing GPT projection output";
  }
  if (!gpt) {
    return false;
  }
  memset(gpt, 0, sizeof(*gpt));
  if (!disk || disk->info.block_size < 512 || disk->info.block_size > NPFS_BLOCK_SIZE ||
      (disk->info.block_size & (disk->info.block_size - 1)) || disk->info.block_count < 3 ||
      disk->info.block_count > UINT64_MAX / disk->info.block_size ||
      disk->bytes != disk->info.block_count * disk->info.block_size) {
    return false;
  }
  struct gpt_copy *maps = calloc(2, sizeof(*maps));
  uint8_t *block = malloc(NPFS_BLOCK_SIZE);
  const char *failure = "out of memory during GPT inspection";
  bool success = false;
  if (!maps || !block) {
    goto done;
  }
  failure = "I/O failure during GPT inspection";
  install_progress_begin("Inspecting disk protective MBR", INSTALL_PROGRESS_RAW_READ, NULL);
  if (!install_read(disk, 0, block, (size_t)disk->info.block_size)) {
    goto done;
  }
  bool mbr = protective_mbr(block, disk);
  for (unsigned copy = 0; copy < 2; ++copy) {
    maps[copy].state = read_gpt(disk, copy, block, &maps[copy]);
    if (maps[copy].state == GPT_FATAL) {
      goto done;
    }
    if (maps[copy].state == GPT_UNSUPPORTED) {
      failure = "unsupported GPT format or inspection capacity";
      goto done;
    }
  }
  if (!mbr || maps[0].state != GPT_VALID || maps[1].state != GPT_VALID) {
    failure = "no healthy validated protective GPT";
    goto done;
  }
  if (!gpt_agree(&maps[0], &maps[1])) {
    failure = "GPT copies disagree";
    goto done;
  }
  const struct gpt_copy *map = &maps[0];
  gpt->first_usable = map->first;
  gpt->last_usable = map->last;
  gpt->slot_count = map->count;
  memcpy(gpt->disk_guid, map->guid, sizeof(gpt->disk_guid));
  for (uint32_t i = 0; i < map->count; ++i) {
    const uint8_t *entry = map->table + (size_t)i * map->entry_bytes;
    if (zero_bytes(entry, 16)) {
      continue;
    }
    struct install_gpt_partition *partition = &gpt->partitions[gpt->partition_count++];
    partition->entry = (uint64_t)i + 1;
    partition->first_block = npfs_get_u64(entry + 32);
    partition->block_count = npfs_get_u64(entry + 40) - partition->first_block + 1;
    partition->attributes = npfs_get_u64(entry + 48);
    memcpy(partition->type_guid, entry, sizeof(partition->type_guid));
    memcpy(partition->guid, entry + 16, sizeof(partition->guid));
    for (unsigned unit = 0; unit < INSTALL_GPT_NAME_UNITS; ++unit) {
      partition->name[unit] = npfs_get_u16(entry + 56 + unit * 2);
    }
  }
  success = true;
done:
  install_progress_finish();
  free(block);
  free(maps);
  if (reason) {
    *reason = success ? NULL : failure;
  }
  return success;
}

static enum inspection raw_pool_read(struct pool_view *pool, uint64_t block, uint8_t *bytes)
{
  if (block >= pool->blocks) {
    return INSPECT_DAMAGED;
  }
  return install_read(pool->disk, pool->offset + block * NPFS_BLOCK_SIZE, bytes, NPFS_BLOCK_SIZE) ?
    INSPECT_OK : INSPECT_FATAL;
}

static enum inspection validate_image(struct pool_view *pool, uint64_t home, uint32_t kind,
    const uint8_t *bytes)
{
  if (kind == NPFS_METADATA_BITMAP) {
    uint64_t base = (home - pool->header.bitmap_start) * NPFS_BITMAP_BITS;
    for (unsigned bit = 0; bit < NPFS_BITMAP_BITS; bit++) {
      uint64_t number = base + bit;
      if ((number >= pool->blocks || !npfs_data_block_valid(&pool->header, number)) &&
          !(bytes[bit / 8] & (1u << (bit % 8)))) {
        return INSPECT_DAMAGED;
      }
    }
  } else if (kind == NPFS_METADATA_VOLUMES) {
    for (unsigned i = 0; i < NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE; i++) {
      struct npfs_volume volume;
      if (npfs_volume_decode(&pool->header, bytes + i * NPFS_VOLUME_SIZE, &volume) != NPFS_OK) {
        return INSPECT_DAMAGED;
      }
    }
  } else if (kind == NPFS_METADATA_INODES) {
    for (unsigned i = 0; i < NPFS_BLOCK_SIZE / NPFS_INODE_SIZE; i++) {
      struct npfs_inode inode;
      if (npfs_inode_decode(&pool->header, bytes + i * NPFS_INODE_SIZE, &inode) != NPFS_OK) {
        return INSPECT_DAMAGED;
      }
    }
  } else if (kind == NPFS_METADATA_DIRECTORY) {
    size_t offset = 0;
    while (offset < NPFS_BLOCK_SIZE) {
      struct npfs_dirent entry;
      if (npfs_dirent_decode(&pool->header, bytes + offset, NPFS_BLOCK_SIZE - offset, &entry) != NPFS_OK) {
        return INSPECT_DAMAGED;
      }
      offset += entry.record_length;
    }
  } else if (kind == NPFS_METADATA_INDIRECT) {
    for (unsigned i = 0; i < NPFS_INDIRECT_COUNT; i++) {
      uint64_t pointer = npfs_get_u64(bytes + i * 8);
      if (pointer && !npfs_data_block_valid(&pool->header, pointer)) {
        return INSPECT_DAMAGED;
      }
    }
  } else {
    return INSPECT_DAMAGED;
  }
  return INSPECT_OK;
}

static int compare_overlay(const void *left, const void *right)
{
  const struct overlay_entry *a = left, *b = right;
  return (a->home > b->home) - (a->home < b->home);
}

static enum inspection load_overlay(struct pool_view *pool)
{
  if (pool->control.state == NPFS_JOURNAL_EMPTY) {
    return INSPECT_OK;
  }
  if (pool->control.sequence == UINT64_MAX || npfs_features_check(&pool->header, true) != NPFS_OK) {
    return INSPECT_DAMAGED;
  }
  size_t count = pool->control.image_count;
  if (count > SIZE_MAX / sizeof(*pool->overlay)) {
    return INSPECT_FATAL;
  }
  pool->overlay = calloc(count, sizeof(*pool->overlay));
  if (!pool->overlay) {
    return INSPECT_FATAL;
  }
  uint32_t crc = npfs_payload_begin(&pool->control);
  size_t index = 0;
  install_progress_begin("Validating committed journal descriptors", INSTALL_PROGRESS_RAW_READ,
      "pages checked");
  install_progress_items_total(pool->control.descriptor_blocks);
  for (uint32_t page = 0; page < pool->control.descriptor_blocks; page++) {
    enum inspection status = raw_pool_read(pool, pool->header.journal_start + 2 + page, pool->scratch[0]);
    if (status != INSPECT_OK) {
      return status;
    }
    crc = npfs_crc_update(crc, pool->scratch[0], NPFS_BLOCK_SIZE);
    for (unsigned slot = 0; slot < NPFS_DESCRIPTORS_PER_BLOCK; slot++) {
      const uint8_t *bytes = pool->scratch[0] + slot * NPFS_DESCRIPTOR_SIZE;
      if (index == count) {
        if (!zero_bytes(bytes, NPFS_DESCRIPTOR_SIZE)) {
          return INSPECT_DAMAGED;
        }
        continue;
      }
      struct npfs_descriptor descriptor;
      if (npfs_descriptor_decode(&pool->header, bytes, &descriptor) != NPFS_OK) {
        return INSPECT_DAMAGED;
      }
      pool->overlay[index] = (struct overlay_entry){
        .home = descriptor.home, .kind = descriptor.kind, .index = (uint32_t)index,
      };
      index++;
    }
    install_progress_advance(0, 1);
  }
  install_progress_begin("Validating committed journal images", INSTALL_PROGRESS_RAW_READ,
      "images checked");
  install_progress_items_total(count);
  for (size_t i = 0; i < count; i++) {
    enum inspection status = raw_pool_read(pool,
      pool->header.journal_start + 2 + pool->control.descriptor_blocks + i, pool->scratch[0]);
    if (status != INSPECT_OK) {
      return status;
    }
    crc = npfs_crc_update(crc, pool->scratch[0], NPFS_BLOCK_SIZE);
    status = validate_image(pool, pool->overlay[i].home, pool->overlay[i].kind, pool->scratch[0]);
    if (status != INSPECT_OK) {
      return status;
    }
    pool->overlay[i].crc = npfs_crc32c(pool->scratch[0], NPFS_BLOCK_SIZE);
    install_progress_advance(0, 1);
  }
  install_progress_begin("Checking committed journal checksum and descriptor uniqueness",
      INSTALL_PROGRESS_NONE, NULL);
  if (index != count || npfs_crc_finish(crc) != pool->control.payload_crc) {
    return INSPECT_DAMAGED;
  }
  qsort(pool->overlay, count, sizeof(*pool->overlay), compare_overlay);
  for (size_t i = 1; i < count; i++) {
    if (pool->overlay[i - 1].home == pool->overlay[i].home) {
      return INSPECT_DAMAGED;
    }
  }
  /* No overlay read is possible until the entire committed payload is valid. */
  pool->overlay_count = (uint32_t)count;
  return INSPECT_OK;
}

static enum inspection pool_read(struct pool_view *pool, uint64_t home, uint32_t kind, uint8_t *bytes)
{
  size_t low = 0, high = pool->overlay_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (pool->overlay[middle].home < home) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  enum inspection status;
  if (low < pool->overlay_count && pool->overlay[low].home == home) {
    const struct overlay_entry *entry = &pool->overlay[low];
    if (entry->kind != kind) {
      return INSPECT_DAMAGED;
    }
    status = raw_pool_read(pool,
      pool->header.journal_start + 2 + pool->control.descriptor_blocks + entry->index, bytes);
    if (status == INSPECT_OK && npfs_crc32c(bytes, NPFS_BLOCK_SIZE) != entry->crc) {
      return INSPECT_DAMAGED;
    }
  } else {
    status = raw_pool_read(pool, home, bytes);
  }
  return status == INSPECT_OK ? validate_image(pool, home, kind, bytes) : status;
}

static enum inspection select_pool(struct pool_view *pool, bool *degraded,
    bool writable, const char **reason)
{
  install_progress_begin("Inspecting pool header copies", INSTALL_PROGRESS_RAW_READ, NULL);
  if (reason) {
    *reason = "npfs pool headers are damaged or incompatible";
  }
  enum inspection status = raw_pool_read(pool, 0, pool->scratch[0]);
  if (status != INSPECT_OK) {
    return status;
  }
  status = raw_pool_read(pool, pool->blocks - 1, pool->scratch[1]);
  if (status != INSPECT_OK) {
    return status;
  }
  struct npfs_header headers[2];
  enum npfs_status decoded[2] = {
    npfs_header_decode(pool->scratch[0], &headers[0]),
    npfs_header_decode(pool->scratch[1], &headers[1]),
  };
  for (unsigned i = 0; i < 2; ++i) {
    if (decoded[i] == NPFS_OK && headers[i].pool_blocks != pool->blocks) {
      decoded[i] = NPFS_CORRUPT;
    }
  }
  if (decoded[0] == NPFS_UNSUPPORTED || decoded[1] == NPFS_UNSUPPORTED ||
      (decoded[0] != NPFS_OK && decoded[1] != NPFS_OK) ||
      (decoded[0] == NPFS_OK && decoded[1] == NPFS_OK && memcmp(pool->scratch[0], pool->scratch[1], NPFS_BLOCK_SIZE))) {
    return INSPECT_DAMAGED;
  }
  *degraded |= decoded[0] != NPFS_OK || decoded[1] != NPFS_OK;
  pool->header = headers[decoded[0] == NPFS_OK ? 0 : 1];
  if (npfs_features_check(&pool->header, writable) != NPFS_OK) {
    if (reason) {
      *reason = "npfs pool features are incompatible with the live kernel";
    }
    return INSPECT_DAMAGED;
  }
  if (reason) {
    *reason = "npfs journal controls are damaged or incompatible";
  }
  struct npfs_control controls[2];
  bool valid[2];
  install_progress_begin("Inspecting pool journal controls", INSTALL_PROGRESS_RAW_READ, NULL);
  for (unsigned i = 0; i < 2; i++) {
    status = raw_pool_read(pool, pool->header.journal_start + i, pool->scratch[i]);
    if (status != INSPECT_OK) {
      return status;
    }
    enum npfs_status result = npfs_control_decode(&pool->header, pool->scratch[i], &controls[i]);
    valid[i] = result == NPFS_OK;
    if (!valid[i] && npfs_control_checksum_valid(pool->scratch[i])) {
      return INSPECT_DAMAGED;
    }
  }
  if ((!valid[0] && !valid[1]) || (valid[0] && valid[1] && controls[0].sequence == controls[1].sequence &&
      memcmp(pool->scratch[0], pool->scratch[1], NPFS_BLOCK_SIZE))) {
    return INSPECT_DAMAGED;
  }
  unsigned selected = valid[1] && (!valid[0] || controls[1].sequence > controls[0].sequence) ? 1 : 0;
  pool->control = controls[selected];
  pool->control_degraded = !valid[0] || !valid[1];
  return INSPECT_OK;
}

static enum inspection open_pool(struct pool_view *pool, bool *degraded)
{
  enum inspection status = select_pool(pool, degraded, false, NULL);
  return status == INSPECT_OK ? load_overlay(pool) : status;
}

static enum inspection allocated_pointer(struct pool_view *pool, uint64_t block)
{
  if (!npfs_data_block_valid(&pool->header, block)) {
    return INSPECT_DAMAGED;
  }
  enum inspection status = pool_read(pool, pool->header.bitmap_start + block / NPFS_BITMAP_BITS,
    NPFS_METADATA_BITMAP, pool->scratch[2]);
  if (status != INSPECT_OK) {
    return status;
  }
  unsigned bit = (unsigned)(block % NPFS_BITMAP_BITS);
  return pool->scratch[2][bit / 8] & (1u << (bit % 8)) ? INSPECT_OK : INSPECT_DAMAGED;
}

static enum inspection mapping_read_kind(struct pool_view *pool, const uint64_t *pointers,
    uint64_t logical, bool sparse, uint64_t *leaf)
{
  struct npfs_map_path path;
  if (npfs_map_path(logical, &path) != NPFS_OK) {
    return INSPECT_DAMAGED;
  }
  uint64_t block = pointers[path.slot], ancestors[3];
  for (unsigned depth = 0; depth < path.depth; depth++) {
    if (!block) {
      if (sparse) {
        *leaf = 0;
        return INSPECT_OK;
      }
      return INSPECT_DAMAGED;
    }
    for (unsigned i = 0; i < depth; i++) {
      if (ancestors[i] == block) {
        return INSPECT_DAMAGED;
      }
    }
    ancestors[depth] = block;
    enum inspection status = allocated_pointer(pool, block);
    if (status != INSPECT_OK) {
      return status;
    }
    status = pool_read(pool, block, NPFS_METADATA_INDIRECT, pool->scratch[1]);
    if (status != INSPECT_OK) {
      return status;
    }
    block = npfs_get_u64(pool->scratch[1] + path.index[depth] * 8);
  }
  for (unsigned i = 0; i < path.depth; i++) {
    if (block == ancestors[i]) {
      return INSPECT_DAMAGED;
    }
  }
  if (!block && sparse) {
    *leaf = 0;
    return INSPECT_OK;
  }
  enum inspection status = allocated_pointer(pool, block);
  if (status == INSPECT_OK) {
    *leaf = block;
  }
  return status;
}

static enum inspection mapping_read(struct pool_view *pool, const uint64_t *pointers,
    uint64_t logical, uint64_t *leaf)
{
  return mapping_read_kind(pool, pointers, logical, false, leaf);
}

static enum inspection read_inode(struct pool_view *pool, const struct npfs_volume *volume,
    uint64_t number, struct npfs_inode *inode)
{
  if (number >= volume->inode_bytes / NPFS_INODE_SIZE) {
    return INSPECT_DAMAGED;
  }
  uint64_t offset = number * NPFS_INODE_SIZE, leaf;
  enum inspection status = mapping_read(pool, volume->pointers, offset / NPFS_BLOCK_SIZE, &leaf);
  if (status != INSPECT_OK) {
    return status;
  }
  status = pool_read(pool, leaf, NPFS_METADATA_INODES, pool->scratch[1]);
  if (status != INSPECT_OK) {
    return status;
  }
  return npfs_inode_decode(&pool->header, pool->scratch[1] + offset % NPFS_BLOCK_SIZE, inode) == NPFS_OK ?
    INSPECT_OK : INSPECT_DAMAGED;
}

/* This inspects the root marker path, not global inode ownership, reachability,
 * file contents, or all directory names. A missing marker differs from damage. */
static enum inspection root_marker(struct pool_view *pool, const struct npfs_volume *volume, bool *marked)
{
  *marked = false;
  struct npfs_inode root, reserved;
  enum inspection status = read_inode(pool, volume, 0, &reserved);
  if (status != INSPECT_OK) {
    return status;
  }
  if (reserved.kind != NPFS_INODE_FREE) {
    return INSPECT_DAMAGED;
  }
  status = read_inode(pool, volume, 1, &root);
  if (status != INSPECT_OK) {
    return status;
  }
  if (root.kind != NPFS_INODE_DIRECTORY || root.parent != 1 || root.cleanup) {
    return INSPECT_DAMAGED;
  }
  uint64_t marker = 0;
  for (uint64_t logical = 0; logical < root.size / NPFS_BLOCK_SIZE; logical++) {
    uint64_t leaf;
    status = mapping_read(pool, root.pointers, logical, &leaf);
    if (status != INSPECT_OK) {
      return status;
    }
    status = pool_read(pool, leaf, NPFS_METADATA_DIRECTORY, pool->scratch[0]);
    if (status != INSPECT_OK) {
      return status;
    }
    size_t offset = 0;
    while (offset < NPFS_BLOCK_SIZE) {
      struct npfs_dirent entry;
      if (npfs_dirent_decode(&pool->header, pool->scratch[0] + offset, NPFS_BLOCK_SIZE - offset, &entry) != NPFS_OK) {
        return INSPECT_DAMAGED;
      }
      offset += entry.record_length;
      if (!entry.inode) {
        continue;
      }
      if (entry.inode == 1 || entry.inode >= volume->inode_bytes / NPFS_INODE_SIZE) {
        return INSPECT_DAMAGED;
      }
      if (entry.name_length == sizeof("SAFE_TO_WIPE") - 1 &&
          !memcmp(entry.name, "SAFE_TO_WIPE", sizeof("SAFE_TO_WIPE") - 1)) {
        if (marker) {
          return INSPECT_DAMAGED;
        }
        marker = entry.inode;
      }
    }
  }
  if (marker) {
    struct npfs_inode inode;
    status = read_inode(pool, volume, marker, &inode);
    if (status != INSPECT_OK) {
      return status;
    }
    if (inode.kind == NPFS_INODE_FREE || (inode.cleanup & NPFS_CLEANUP_DETACHED) ||
        inode.cleanup_next >= volume->inode_bytes / NPFS_INODE_SIZE ||
        (inode.kind == NPFS_INODE_DIRECTORY && inode.parent != 1)) {
      return INSPECT_DAMAGED;
    }
    *marked = inode.kind == NPFS_INODE_FILE;
  }
  return INSPECT_OK;
}

static bool append_label(struct install_consent *consent, unsigned partition, const struct npfs_volume *volume)
{
  char prefix[40];
  int prefix_bytes = snprintf(prefix, sizeof(prefix), "partition %u: \"", partition);
  if (prefix_bytes < 0 || (size_t)prefix_bytes >= sizeof(prefix)) {
    return false;
  }
  size_t existing = consent->volume_labels ? strlen(consent->volume_labels) : 0;
  size_t extra = (size_t)prefix_bytes + (size_t)volume->name_length * 4 + 3;
  if (existing > SIZE_MAX - extra) {
    return false;
  }
  char *labels = realloc(consent->volume_labels, existing + extra);
  if (!labels) {
    return false;
  }
  consent->volume_labels = labels;
  char *next = labels + existing;
  memcpy(next, prefix, (size_t)prefix_bytes);
  next += prefix_bytes;
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < volume->name_length; i++) {
    unsigned ch = volume->name[i];
    if (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') {
      *next++ = (char)ch;
    } else {
      *next++ = '\\';
      *next++ = 'x';
      *next++ = hex[ch >> 4];
      *next++ = hex[ch & 15];
    }
  }
  *next++ = '"';
  *next++ = '\n';
  *next = 0;
  return true;
}

static enum inspection inspect_volumes(struct pool_view *pool, unsigned partition,
    struct install_consent *consent, bool *all_marked)
{
  install_progress_begin("Inspecting consent volume catalog", INSTALL_PROGRESS_RAW_READ,
      "pages checked");
  install_progress_items_total(NPFS_VOLUME_TABLE_BLOCKS);
  struct npfs_volume *volumes = calloc(NPFS_VOLUME_COUNT, sizeof(*volumes));
  if (!volumes) {
    return INSPECT_FATAL;
  }
  enum inspection result = INSPECT_OK;
  unsigned live = 0, marked = 0;
  bool readable = true;
  for (unsigned page = 0; page < NPFS_VOLUME_TABLE_BLOCKS; page++) {
    result = pool_read(pool, pool->header.volume_start + page, NPFS_METADATA_VOLUMES, pool->scratch[0]);
    if (result != INSPECT_OK) {
      goto done;
    }
    for (unsigned slot = 0; slot < NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE; slot++) {
      unsigned index = page * (NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE) + slot;
      if (npfs_volume_decode(&pool->header, pool->scratch[0] + slot * NPFS_VOLUME_SIZE, &volumes[index]) != NPFS_OK) {
        result = INSPECT_DAMAGED;
        goto done;
      }
      if (volumes[index].state != NPFS_VOLUME_LIVE) {
        continue;
      }
      for (unsigned j = 0; j < index; j++) {
        if (volumes[j].state == NPFS_VOLUME_LIVE &&
            (!memcmp(volumes[index].id, volumes[j].id, NPFS_ID_SIZE) ||
             (volumes[index].name_length == volumes[j].name_length &&
              !memcmp(volumes[index].name, volumes[j].name, volumes[j].name_length)))) {
          result = INSPECT_DAMAGED;
          goto done;
        }
      }
      if (!append_label(consent, partition, &volumes[index])) {
        result = INSPECT_FATAL;
        goto done;
      }
      live++;
      consent->volumes++;
    }
    install_progress_advance(0, 1);
  }
  install_progress_begin("Inspecting volume consent markers", INSTALL_PROGRESS_RAW_READ,
      "volumes inspected");
  install_progress_items_total(live);
  for (unsigned i = 0; i < NPFS_VOLUME_COUNT; i++) {
    if (volumes[i].state != NPFS_VOLUME_LIVE) {
      continue;
    }
    bool has_marker;
    result = root_marker(pool, &volumes[i], &has_marker);
    if (result == INSPECT_FATAL) {
      goto done;
    }
    if (result == INSPECT_DAMAGED) {
      readable = false;
    } else if (has_marker) {
      marked++;
    }
    install_progress_advance(0, 1);
  }
  consent->final_pool |= readable && live && !marked;
  *all_marked = readable && live && marked == live;
  result = readable ? INSPECT_OK : INSPECT_DAMAGED;
done:
  free(volumes);
  return result;
}

static enum inspection inspect_partition(const struct install_disk *disk, uint64_t first,
    uint64_t last, unsigned number, struct install_consent *consent, bool *all_marked)
{
  install_progress_begin("Inspecting partition pool signatures", INSTALL_PROGRESS_RAW_READ, NULL);
  uint64_t offset = first * disk->info.block_size;
  uint64_t bytes = (last - first + 1) * disk->info.block_size;
  *all_marked = true;
  uint8_t signature[8];
  if (!install_read(disk, offset, signature, sizeof(signature))) {
    return INSPECT_FATAL;
  }
  bool recognized = !memcmp(signature, "PYXISNFS", sizeof(signature));
  uint64_t blocks = bytes / NPFS_BLOCK_SIZE;
  if (blocks) {
    if (!install_read(disk, offset + (blocks - 1) * NPFS_BLOCK_SIZE, signature, sizeof(signature))) {
      return INSPECT_FATAL;
    }
    recognized |= !memcmp(signature, "PYXISNFS", sizeof(signature));
  }
  if (!recognized) {
    return INSPECT_OK;
  }
  consent->pools++;
  *all_marked = false;
  if (blocks < 2) {
    return INSPECT_DAMAGED;
  }
  struct pool_view *pool = calloc(1, sizeof(*pool));
  if (!pool) {
    return INSPECT_FATAL;
  }
  pool->disk = disk;
  pool->offset = offset;
  pool->blocks = blocks;
  enum inspection result = open_pool(pool, &consent->degraded);
  if (result == INSPECT_OK) {
    result = inspect_volumes(pool, number, consent, all_marked);
  }
  free(pool->overlay);
  free(pool);
  return result;
}

void install_consent_destroy(struct install_consent *consent)
{
  if (consent) {
    free(consent->volume_labels);
    memset(consent, 0, sizeof(*consent));
  }
}

bool install_scan_consent(const struct install_disk *disk, bool room, struct install_consent *consent)
{
  memset(consent, 0, sizeof(*consent));
  consent->reason = "disk is not writable with reliable flush support";
  uint64_t required = DISK_FLAG_WRITABLE | DISK_FLAG_FLUSH_SUPPORTED;
  if (disk->info.preparation != DISK_READY || (disk->info.flags & required) != required ||
      (disk->info.flags & (DISK_FLAG_WRITE_FAILED | DISK_FLAG_MOUNTED))) {
    return true;
  }
  /* CLAIMED is intentionally allowed: the owner repeats this scan on its own
   * exclusive handle immediately before mutation. Inspection is not a snapshot. */
  if (disk->info.block_size < 512 || disk->info.block_size > NPFS_BLOCK_SIZE ||
      (disk->info.block_size & (disk->info.block_size - 1)) ||
      disk->info.block_count < 3 || disk->info.block_count > UINT64_MAX / disk->info.block_size ||
      disk->bytes != disk->info.block_count * disk->info.block_size) {
    consent->reason = "unsupported disk geometry";
    return false;
  }
  struct gpt_copy *maps = calloc(2, sizeof(*maps));
  uint8_t *block = malloc(NPFS_BLOCK_SIZE);
  if (!maps || !block) {
    free(maps);
    free(block);
    consent->reason = "out of memory during consent inspection";
    return false;
  }
  bool success = false;
  consent->reason = "I/O failure during consent inspection";
  install_progress_begin("Inspecting disk protective MBR", INSTALL_PROGRESS_RAW_READ, NULL);
  if (!install_read(disk, 0, block, (size_t)disk->info.block_size)) {
    goto done;
  }
  bool mbr = protective_mbr(block, disk);
  bool empty_mbr = zero_bytes(block, (size_t)disk->info.block_size);
  for (unsigned i = 0; i < 2; i++) {
    maps[i].state = read_gpt(disk, i, block, &maps[i]);
    if (maps[i].state == GPT_FATAL) {
      goto done;
    }
    if (maps[i].state == GPT_UNSUPPORTED) {
      consent->reason = "unsupported GPT format or inspection capacity";
      goto done;
    }
  }
  bool primary = maps[0].state == GPT_VALID, backup = maps[1].state == GPT_VALID;
  bool blank = empty_mbr && maps[0].state == GPT_ABSENT && maps[1].state == GPT_ABSENT;
  bool ambiguous = primary && backup && !gpt_agree(&maps[0], &maps[1]);
  bool valid_gpt = mbr && (primary || backup) && !ambiguous;
  consent->degraded = valid_gpt && primary != backup;
  if (valid_gpt) {
    memcpy(consent->existing_guid, maps[primary ? 0 : 1].guid, 16);
  }
  bool all_marked = true, damaged_pool = false, empty_pool = false;
  /* Both valid conflicting maps are inspected. Identical extents are scanned
   * once; overlapping but different extents are independently inspected. */
  for (unsigned copy = 0; copy < 2; copy++) {
    if (maps[copy].state != GPT_VALID || (copy && primary && !ambiguous)) {
      continue;
    }
    for (uint32_t i = 0; i < maps[copy].count; i++) {
      const uint8_t *entry = maps[copy].table + (size_t)i * maps[copy].entry_bytes;
      if (zero_bytes(entry, 16)) {
        continue;
      }
      uint64_t first = npfs_get_u64(entry + 32), last = npfs_get_u64(entry + 40);
      bool duplicate = false;
      if (copy && primary) {
        for (uint32_t j = 0; j < maps[0].count; j++) {
          const uint8_t *other = maps[0].table + (size_t)j * maps[0].entry_bytes;
          if (!zero_bytes(other, 16) && first == npfs_get_u64(other + 32) && last == npfs_get_u64(other + 40)) {
            duplicate = true;
            break;
          }
        }
      }
      if (duplicate) {
        continue;
      }
      bool marked;
      uint32_t previous_pools = consent->pools, previous_volumes = consent->volumes;
      enum inspection status = inspect_partition(disk, first, last, i + 1, consent, &marked);
      if (status == INSPECT_FATAL) {
        consent->reason = "I/O or allocation failure during consent inspection";
        goto done;
      }
      damaged_pool |= status == INSPECT_DAMAGED;
      empty_pool |= status == INSPECT_OK && consent->pools > previous_pools &&
        consent->volumes == previous_volumes;
      all_marked &= marked && status == INSPECT_OK;
    }
  }
  if (consent->final_pool) {
    consent->reason = "readable nonempty npfs pool has no SAFE_TO_WIPE markers";
  } else if (room) {
    consent->eligible = true;
    if (damaged_pool) {
      consent->reason = "npfs pool, damaged: consent unreadable";
    } else if (blank) {
      consent->reason = "blank disk: no partition table";
    } else if (!valid_gpt) {
      consent->reason = ambiguous ? "unrecognized layout: GPT copies disagree" :
        "unrecognized layout: no validated protective GPT";
    } else if (!consent->pools) {
      consent->reason = "foreign GPT disk: no npfs pools";
    } else if (empty_pool) {
      consent->reason = "npfs pool, empty: no live volumes";
    } else if (!all_marked) {
      consent->reason = "npfs pool, partially marked: some live volumes lack a regular SAFE_TO_WIPE";
    } else {
      consent->reason = "npfs pool, marked: every live volume has a regular SAFE_TO_WIPE";
    }
  } else if (!valid_gpt) {
    consent->reason = blank ? "blank disk: use Read the room" :
      (ambiguous ? "GPT copies disagree" : "no validated protective GPT");
  } else if (!consent->pools) {
    consent->reason = "no npfs pools found";
  } else if (!all_marked) {
    consent->reason = "every npfs pool must be nonempty and every live volume must have a regular SAFE_TO_WIPE";
  } else {
    consent->eligible = true;
    consent->reason = "all live npfs volumes have regular SAFE_TO_WIPE markers";
  }
  success = true;
done:
  install_progress_finish();
  free(block);
  free(maps);
  return success;
}

static bool update_layout(const struct install_disk *disk, const struct gpt_copy *map,
    struct install_layout *layout)
{
  static const uint8_t types[2][16] = {
    {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b},
    {0xa3, 0x94, 0x81, 0x1a, 0x07, 0x8a, 0xff, 0x4d,
     0x83, 0x0e, 0x4c, 0xb4, 0xed, 0x7a, 0xac, 0x00},
  };
  if (map->count < 2 || !install_layout_plan(disk, layout)) {
    return false;
  }
  const uint64_t starts[2] = {layout->esp_start, layout->pool_start};
  const uint64_t sizes[2] = {layout->esp_bytes, layout->pool_bytes};
  for (uint32_t index = 0; index < map->count; index++) {
    const uint8_t *entry = map->table + (size_t)index * map->entry_bytes;
    if (index >= 2) {
      if (!zero_bytes(entry, 16)) {
        return false;
      }
      continue;
    }
    if (memcmp(entry, types[index], 16) ||
        npfs_get_u64(entry + 32) != starts[index] / disk->info.block_size ||
        npfs_get_u64(entry + 40) != (starts[index] + sizes[index]) / disk->info.block_size - 1) {
      return false;
    }
    memcpy(index ? layout->pool_guid : layout->esp_guid, entry + 16, 16);
  }
  memcpy(layout->disk_guid, map->guid, 16);
  return true;
}

static enum inspection update_catalog(struct pool_view *pool, const char *name,
    struct npfs_volume *selected, bool *found)
{
  install_progress_begin("Inspecting update volume catalog", INSTALL_PROGRESS_RAW_READ,
      "pages checked");
  install_progress_items_total(NPFS_VOLUME_TABLE_BLOCKS);
  struct npfs_volume *volumes = calloc(NPFS_VOLUME_COUNT, sizeof(*volumes));
  if (!volumes) {
    return INSPECT_FATAL;
  }
  *found = false;
  enum inspection status = INSPECT_OK;
  for (unsigned page = 0; page < NPFS_VOLUME_TABLE_BLOCKS; page++) {
    status = pool_read(pool, pool->header.volume_start + page,
        NPFS_METADATA_VOLUMES, pool->scratch[0]);
    if (status != INSPECT_OK) {
      goto done;
    }
    for (unsigned slot = 0; slot < NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE; slot++) {
      unsigned index = page * (NPFS_BLOCK_SIZE / NPFS_VOLUME_SIZE) + slot;
      struct npfs_volume *volume = &volumes[index];
      if (npfs_volume_decode(&pool->header,
          pool->scratch[0] + slot * NPFS_VOLUME_SIZE, volume) != NPFS_OK) {
        status = INSPECT_DAMAGED;
        goto done;
      }
      if (volume->state != NPFS_VOLUME_LIVE) {
        continue;
      }
      for (unsigned earlier = 0; earlier < index; earlier++) {
        const struct npfs_volume *other = &volumes[earlier];
        if (other->state == NPFS_VOLUME_LIVE &&
            (!memcmp(volume->id, other->id, NPFS_ID_SIZE) ||
             (volume->name_length == other->name_length &&
              !memcmp(volume->name, other->name, volume->name_length)))) {
          status = INSPECT_DAMAGED;
          goto done;
        }
      }
      if (volume->name_length == strlen(name) &&
          !memcmp(volume->name, name, volume->name_length)) {
        *selected = *volume;
        *found = true;
      }
    }
    install_progress_advance(0, 1);
  }
done:
  free(volumes);
  return status;
}

/* Match the inode and cleanup checks performed when the kernel opens a volume.
 * This does not inspect every file's contents or establish global ownership. */
static enum inspection update_system(struct pool_view *pool, const struct npfs_volume *volume)
{
  uint64_t count = volume->inode_bytes / NPFS_INODE_SIZE;
  uint64_t cleanup_count = 0;
  install_progress_begin("Inspecting system inode metadata", INSTALL_PROGRESS_RAW_READ,
      "inodes checked");
  install_progress_items_total(count);
  for (uint64_t first = 0; first < count;) {
    uint64_t before = first;
    uint64_t leaf;
    enum inspection status = mapping_read(pool, volume->pointers,
        first / (NPFS_BLOCK_SIZE / NPFS_INODE_SIZE), &leaf);
    if (status != INSPECT_OK) {
      return status;
    }
    status = pool_read(pool, leaf, NPFS_METADATA_INODES, pool->scratch[0]);
    if (status != INSPECT_OK) {
      return status;
    }
    for (unsigned slot = 0; slot < NPFS_BLOCK_SIZE / NPFS_INODE_SIZE && first < count;
        slot++, first++) {
      struct npfs_inode inode;
      if (npfs_inode_decode(&pool->header,
          pool->scratch[0] + slot * NPFS_INODE_SIZE, &inode) != NPFS_OK ||
          (!first && inode.kind != NPFS_INODE_FREE) ||
          (first == 1 && (inode.kind != NPFS_INODE_DIRECTORY || inode.parent != 1 ||
           (inode.cleanup & NPFS_CLEANUP_DETACHED)))) {
        return INSPECT_DAMAGED;
      }
      if (inode.cleanup) {
        cleanup_count++;
      }
    }
    install_progress_advance(0, first - before);
  }
  install_progress_begin("Inspecting system cleanup chain", INSTALL_PROGRESS_RAW_READ,
      "inodes checked");
  uint64_t next = volume->cleanup_head, walked = 0;
  while (next) {
    struct npfs_inode inode;
    if (++walked >= count) {
      return INSPECT_DAMAGED;
    }
    enum inspection status = read_inode(pool, volume, next, &inode);
    if (status != INSPECT_OK) {
      return status;
    }
    if (!inode.cleanup) {
      return INSPECT_DAMAGED;
    }
    next = inode.cleanup_next;
    install_progress_advance(0, 1);
  }
  return walked == cleanup_count ? INSPECT_OK : INSPECT_DAMAGED;
}

bool install_scan_update(const struct install_disk *disk, struct install_update *update)
{
  memset(update, 0, sizeof(*update));
  update->reason = "disk is not writable with reliable flush support";
  uint64_t required = DISK_FLAG_WRITABLE | DISK_FLAG_FLUSH_SUPPORTED;
  if (disk->info.preparation != DISK_READY || (disk->info.flags & required) != required ||
      (disk->info.flags & (DISK_FLAG_WRITE_FAILED | DISK_FLAG_MOUNTED))) {
    return true;
  }
  if ((disk->info.block_size != 512 && disk->info.block_size != NPFS_BLOCK_SIZE) ||
      disk->info.block_count < 3 || disk->info.block_count > UINT64_MAX / disk->info.block_size ||
      disk->bytes != disk->info.block_count * disk->info.block_size) {
    update->reason = "unsupported disk geometry";
    return false;
  }
  struct gpt_copy *maps = calloc(2, sizeof(*maps));
  struct pool_view *pool = calloc(1, sizeof(*pool));
  uint8_t *block = malloc(NPFS_BLOCK_SIZE);
  bool success = false;
  if (!maps || !pool || !block) {
    update->reason = "out of memory during update inspection";
    goto done;
  }
  update->reason = "I/O failure during update inspection";
  install_progress_begin("Inspecting disk protective MBR", INSTALL_PROGRESS_RAW_READ, NULL);
  if (!install_read(disk, 0, block, (size_t)disk->info.block_size)) {
    goto done;
  }
  bool mbr = protective_mbr(block, disk);
  bool empty_mbr = zero_bytes(block, (size_t)disk->info.block_size);
  for (unsigned copy = 0; copy < 2; copy++) {
    maps[copy].state = read_gpt(disk, copy, block, &maps[copy]);
    if (maps[copy].state == GPT_FATAL) {
      goto done;
    }
    if (maps[copy].state == GPT_UNSUPPORTED) {
      update->reason = "unsupported GPT format or inspection capacity";
      goto done;
    }
  }
  success = true;
  if (empty_mbr && maps[0].state == GPT_ABSENT && maps[1].state == GPT_ABSENT) {
    update->reason = "blank disk: no installation";
    goto done;
  }
  if (!mbr || maps[0].state != GPT_VALID || maps[1].state != GPT_VALID) {
    update->reason = "no healthy validated protective GPT";
    goto done;
  }
  if (!gpt_agree(&maps[0], &maps[1])) {
    update->reason = "GPT copies disagree";
    goto done;
  }
  if (!update_layout(disk, &maps[0], &update->layout)) {
    update->reason = "foreign GPT disk: layout is not an installer layout";
    goto done;
  }
  pool->disk = disk;
  pool->offset = update->layout.pool_start;
  pool->blocks = update->layout.pool_bytes / NPFS_BLOCK_SIZE;
  enum inspection status = select_pool(pool, &update->degraded, true, &update->reason);
  if (status != INSPECT_OK) {
    goto inspection_failed;
  }
  update->degraded |= pool->control_degraded;
  if (pool->control.state != NPFS_JOURNAL_EMPTY) {
    update->reason = "npfs journal committed: boot the installed system once to recover it, then update";
    goto done;
  }
  if (npfs_journal_capacity(pool->header.journal_blocks) < MOUNT_NPFS_MIN_JOURNAL_IMAGES) {
    update->reason = "npfs journal is too small for the live kernel's writable mount";
    goto done;
  }
  update->reason = "npfs allocation bitmap is damaged";
  install_progress_begin("Inspecting allocation bitmap", INSTALL_PROGRESS_RAW_READ,
      "pages checked");
  install_progress_items_total(pool->header.bitmap_blocks);
  for (uint64_t page = 0; page < pool->header.bitmap_blocks; page++) {
    status = pool_read(pool, pool->header.bitmap_start + page,
        NPFS_METADATA_BITMAP, pool->scratch[0]);
    if (status != INSPECT_OK) {
      goto inspection_failed;
    }
    uint64_t base = page * NPFS_BITMAP_BITS;
    uint64_t count = pool->blocks - base;
    if (count > NPFS_BITMAP_BITS) {
      count = NPFS_BITMAP_BITS;
    }
    for (uint64_t bit = 0; bit < count; bit++) {
      if (!(pool->scratch[0][bit / 8] & (1u << (bit % 8)))) {
        update->free_blocks++;
      }
    }
    install_progress_advance(0, 1);
  }
  struct npfs_volume system;
  bool found;
  update->reason = "npfs volume catalog is damaged or incompatible";
  status = update_catalog(pool, "system", &system, &found);
  if (status != INSPECT_OK) {
    goto inspection_failed;
  }
  if (!found) {
    update->reason = "npfs pool has no live system volume";
    goto done;
  }
  update->reason = "npfs system volume cannot be opened by the live kernel";
  status = update_system(pool, &system);
  if (status != INSPECT_OK) {
    goto inspection_failed;
  }
  update->eligible = true;
  update->reason = "installer layout and system pool recognized";
  goto done;
inspection_failed:
  if (status == INSPECT_FATAL) {
    update->reason = "I/O or allocation failure during update inspection";
    success = false;
  }
done:
  install_progress_finish();
  free(block);
  free(pool);
  free(maps);
  return success;
}

struct prior_directory {
  struct npfs_inode inode;
  uint64_t number, position, page_number;
  size_t records, path_bytes;
  bool page_loaded;
  uint8_t page[NPFS_BLOCK_SIZE];
};

static enum inspection prior_next(struct pool_view *pool, const struct npfs_volume *volume,
    struct prior_directory *directory, struct npfs_dirent *entry, bool *end)
{
  *end = false;
  while (directory->position < directory->inode.size) {
    uint64_t logical = directory->position / NPFS_BLOCK_SIZE;
    if (!directory->page_loaded || directory->page_number != logical) {
      uint64_t leaf;
      enum inspection status = mapping_read(pool, directory->inode.pointers, logical, &leaf);
      if (status != INSPECT_OK) {
        return status;
      }
      status = pool_read(pool, leaf, NPFS_METADATA_DIRECTORY, directory->page);
      if (status != INSPECT_OK) {
        return status;
      }
      directory->page_number = logical;
      directory->page_loaded = true;
    }
    size_t offset = (size_t)(directory->position % NPFS_BLOCK_SIZE);
    /* Free records consume inspection work too, even in an empty namespace. */
    if (++directory->records > INSTALL_TREE_MAX_ENTRIES ||
        npfs_dirent_decode(&pool->header, directory->page + offset,
          NPFS_BLOCK_SIZE - offset, entry) != NPFS_OK) {
      return INSPECT_DAMAGED;
    }
    directory->position += entry->record_length;
    if (!entry->inode) {
      continue;
    }
    if (entry->inode == 1 || entry->inode >= volume->inode_bytes / NPFS_INODE_SIZE) {
      return INSPECT_DAMAGED;
    }
    return INSPECT_OK;
  }
  *end = true;
  return INSPECT_OK;
}

static enum inspection prior_linked_inode(struct pool_view *pool,
    const struct npfs_volume *volume, uint64_t number, uint64_t parent,
    struct npfs_inode *inode)
{
  enum inspection status = read_inode(pool, volume, number, inode);
  if (status != INSPECT_OK) {
    return status;
  }
  if (inode->kind == NPFS_INODE_FREE || (inode->cleanup & NPFS_CLEANUP_DETACHED) ||
      inode->cleanup_next >= volume->inode_bytes / NPFS_INODE_SIZE ||
      (inode->kind == NPFS_INODE_DIRECTORY && inode->parent != parent)) {
    return INSPECT_DAMAGED;
  }
  return INSPECT_OK;
}

static enum inspection prior_lookup(struct pool_view *pool, const struct npfs_volume *volume,
    uint64_t parent, const struct npfs_inode *directory, const char *name,
    uint64_t *number, struct npfs_inode *inode)
{
  struct prior_directory *cursor = calloc(1, sizeof(*cursor));
  if (!cursor) {
    return INSPECT_FATAL;
  }
  cursor->inode = *directory;
  cursor->number = parent;
  *number = 0;
  size_t length = strlen(name);
  enum inspection status = INSPECT_OK;
  install_progress_begin("Inspecting previous revision directory", INSTALL_PROGRESS_RAW_READ, "entries");
  for (;;) {
    struct npfs_dirent entry;
    bool end;
    status = prior_next(pool, volume, cursor, &entry, &end);
    if (status != INSPECT_OK || end) {
      break;
    }
    install_progress_advance(0, 1);
    if (entry.name_length != length || memcmp(entry.name, name, length)) {
      continue;
    }
    if (*number) {
      status = INSPECT_DAMAGED;
      break;
    }
    *number = entry.inode;
    status = prior_linked_inode(pool, volume, *number, parent, inode);
    if (status != INSPECT_OK) {
      break;
    }
  }
  install_progress_finish();
  free(cursor);
  return status;
}

static enum inspection prior_file_block(struct pool_view *pool, const struct npfs_inode *inode,
    uint64_t logical, uint8_t bytes[NPFS_BLOCK_SIZE])
{
  uint64_t leaf;
  enum inspection status = mapping_read_kind(pool, inode->pointers, logical, true, &leaf);
  if (status != INSPECT_OK) {
    return status;
  }
  if (!leaf) {
    memset(bytes, 0, NPFS_BLOCK_SIZE);
    return INSPECT_OK;
  }
  return raw_pool_read(pool, leaf, bytes);
}

static enum inspection prior_hash_file(struct pool_view *pool, const struct npfs_inode *inode,
    char sha256[65])
{
  struct install_digest digest = {0};
  if (!install_digest_begin(&digest)) {
    return INSPECT_FATAL;
  }
  enum inspection status = INSPECT_OK;
  for (uint64_t offset = 0; offset < inode->size;) {
    status = prior_file_block(pool, inode, offset / NPFS_BLOCK_SIZE, pool->scratch[0]);
    if (status != INSPECT_OK) {
      break;
    }
    size_t length = inode->size - offset < NPFS_BLOCK_SIZE ?
        (size_t)(inode->size - offset) : NPFS_BLOCK_SIZE;
    if (!install_digest_update(&digest, pool->scratch[0], length)) {
      status = INSPECT_FATAL;
      break;
    }
    install_progress_advance(length, 0);
    offset += length;
  }
  if (status == INSPECT_OK && !install_digest_finish(&digest, sha256)) {
    status = INSPECT_FATAL;
  }
  install_digest_abort(&digest);
  return status;
}

static int compare_prior_inodes(const void *left, const void *right)
{
  uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static enum inspection prior_tree(struct pool_view *pool, const struct npfs_volume *volume,
    uint64_t root_number, const struct npfs_inode *root, struct install_manifest *manifest)
{
  struct prior_directory *levels = calloc(INSTALL_TREE_MAX_DEPTH + 1, sizeof(*levels));
  uint64_t *seen = malloc((INSTALL_TREE_MAX_ENTRIES + 1) * sizeof(*seen));
  char *path = malloc((INSTALL_TREE_MAX_DEPTH + 1) * (NPFS_NAME_MAX + 1));
  if (!levels || !seen || !path) {
    free(levels);
    free(seen);
    free(path);
    return INSPECT_FATAL;
  }
  levels[0].inode = *root;
  levels[0].number = root_number;
  seen[0] = root_number;
  path[0] = '\0';
  unsigned depth = 0;
  size_t entries = 0;
  enum inspection status = INSPECT_OK;
  install_progress_begin_major("Reading and hashing previous program inventory", INSTALL_PROGRESS_WORK, "entries");
  for (;;) {
    struct prior_directory *level = &levels[depth];
    struct npfs_dirent entry;
    bool end;
    status = prior_next(pool, volume, level, &entry, &end);
    if (status != INSPECT_OK) {
      break;
    }
    if (end) {
      if (!depth) {
        break;
      }
      --depth;
      path[levels[depth].path_bytes] = '\0';
      continue;
    }
    if (++entries > INSTALL_TREE_MAX_ENTRIES) {
      status = INSPECT_DAMAGED;
      break;
    }
    seen[entries] = entry.inode;
    struct npfs_inode inode;
    status = prior_linked_inode(pool, volume, entry.inode, level->number, &inode);
    if (status != INSPECT_OK) {
      break;
    }
    if (!depth && entry.name_length == sizeof(INSTALL_MANIFEST_NAME) - 1 &&
        !memcmp(entry.name, INSTALL_MANIFEST_NAME, entry.name_length)) {
      continue;
    }
    for (unsigned ancestor = 0; ancestor <= depth; ancestor++) {
      if (entry.inode == levels[ancestor].number) {
        status = INSPECT_DAMAGED;
        break;
      }
    }
    if (status != INSPECT_OK) {
      break;
    }
    size_t length = level->path_bytes;
    if (length) {
      path[length++] = '/';
    }
    memcpy(path + length, entry.name, entry.name_length);
    length += entry.name_length;
    path[length] = '\0';
    char sha256[65];
    uint64_t kind, bytes;
    if (inode.kind == NPFS_INODE_FILE) {
      kind = DIRECTORY_KIND_FILE;
      bytes = inode.size;
      status = prior_hash_file(pool, &inode, sha256);
    } else {
      kind = DIRECTORY_KIND_DIRECTORY;
      bytes = 0;
      if (depth == INSTALL_TREE_MAX_DEPTH) {
        status = INSPECT_DAMAGED;
      }
    }
    if (status != INSPECT_OK) {
      break;
    }
    enum install_manifest_status added = install_manifest_add(manifest, path, kind, bytes,
        kind == DIRECTORY_KIND_FILE ? sha256 : NULL);
    if (added != INSTALL_MANIFEST_OK) {
      status = added == INSTALL_MANIFEST_NO_MEMORY ? INSPECT_FATAL : INSPECT_DAMAGED;
      break;
    }
    if (kind == DIRECTORY_KIND_DIRECTORY) {
      ++depth;
      levels[depth] = (struct prior_directory){
        .inode = inode, .number = entry.inode, .path_bytes = length,
      };
    } else {
      path[level->path_bytes] = '\0';
    }
    install_progress_advance(0, 1);
  }
  if (status == INSPECT_OK) {
    install_progress_begin("Validating previous inventory structure", INSTALL_PROGRESS_NONE, NULL);
    qsort(seen, entries + 1, sizeof(*seen), compare_prior_inodes);
    for (size_t i = 1; i <= entries; i++) {
      if (seen[i - 1] == seen[i]) {
        status = INSPECT_DAMAGED;
        break;
      }
    }
  }
  if (status == INSPECT_OK) {
    enum install_manifest_status sealed = install_manifest_seal(manifest);
    if (sealed != INSTALL_MANIFEST_OK) {
      status = sealed == INSTALL_MANIFEST_NO_MEMORY ? INSPECT_FATAL : INSPECT_DAMAGED;
    }
  }
  install_progress_finish();
  free(path);
  free(seen);
  free(levels);
  return status;
}

static enum inspection prior_manifest(struct pool_view *pool, const struct npfs_inode *inode,
    const char *revision, struct install_manifest *manifest, bool *valid)
{
  *valid = false;
  if (inode->kind != NPFS_INODE_FILE || inode->size > INSTALL_MANIFEST_MAX_BYTES) {
    return INSPECT_OK;
  }
  size_t length = (size_t)inode->size;
  uint8_t *text = malloc(length ? length : 1);
  if (!text) {
    return INSPECT_FATAL;
  }
  install_progress_begin("Reading previous revision manifest", INSTALL_PROGRESS_RAW_READ, NULL);
  enum inspection status = INSPECT_OK;
  for (size_t offset = 0; offset < length;) {
    status = prior_file_block(pool, inode, offset / NPFS_BLOCK_SIZE, pool->scratch[0]);
    if (status != INSPECT_OK) {
      break;
    }
    size_t bytes = length - offset < NPFS_BLOCK_SIZE ? length - offset : NPFS_BLOCK_SIZE;
    memcpy(text + offset, pool->scratch[0], bytes);
    offset += bytes;
  }
  if (status == INSPECT_OK) {
    install_progress_begin("Parsing previous revision manifest", INSTALL_PROGRESS_NONE, NULL);
    enum install_manifest_status parsed = install_manifest_parse(text, length, manifest);
    if (parsed == INSTALL_MANIFEST_NO_MEMORY) {
      status = INSPECT_FATAL;
    } else if (parsed == INSTALL_MANIFEST_OK) {
      *valid = !strcmp(manifest->revision, revision);
      if (!*valid) {
        install_manifest_destroy(manifest);
      }
    }
  }
  free(text);
  return status;
}

void install_prior_destroy(struct install_prior *prior)
{
  install_manifest_destroy(&prior->inventory);
  memset(prior, 0, sizeof(*prior));
}

bool install_prior_read(const struct install_disk *disk, const struct install_update *update,
    bool inventory_legacy, struct install_prior *prior)
{
  *prior = (struct install_prior){
    .state = INSTALL_PRIOR_UNKNOWN, .reason = "previous revision is unknown",
  };
  if (!update->eligible || update->layout.pool_start > disk->bytes ||
      update->layout.pool_bytes > disk->bytes - update->layout.pool_start ||
      update->layout.pool_bytes % NPFS_BLOCK_SIZE) {
    prior->reason = "prior inventory requires an admitted update target";
    return false;
  }
  struct pool_view *pool = calloc(1, sizeof(*pool));
  if (!pool) {
    prior->reason = "out of memory during prior inventory inspection";
    return false;
  }
  pool->disk = disk;
  pool->offset = update->layout.pool_start;
  pool->blocks = update->layout.pool_bytes / NPFS_BLOCK_SIZE;
  bool success = false, degraded = false;
  enum inspection status = select_pool(pool, &degraded, true, &prior->reason);
  if (status != INSPECT_OK) {
    goto failed;
  }
  if (pool->control.state != NPFS_JOURNAL_EMPTY) {
    prior->reason = "prior inventory requires an empty journal";
    goto done;
  }
  struct npfs_volume bin;
  bool found;
  status = update_catalog(pool, INSTALL_BIN_VOLUME, &bin, &found);
  if (status != INSPECT_OK) {
    goto failed;
  }
  if (!found) {
    enum install_manifest_status initialized = install_manifest_init(&prior->inventory,
        INSTALL_UNKNOWN_REVISION);
    if (initialized == INSTALL_MANIFEST_OK) {
      initialized = install_manifest_seal(&prior->inventory);
    }
    if (initialized != INSTALL_MANIFEST_OK) {
      status = initialized == INSTALL_MANIFEST_NO_MEMORY ? INSPECT_FATAL : INSPECT_DAMAGED;
      goto failed;
    }
    prior->state = INSTALL_PRIOR_EMPTY;
    prior->reason = "no bin volume; no installed program inventory";
    success = true;
    goto done;
  }
  if (!strcmp(update->revision, INSTALL_UNKNOWN_REVISION) ||
      !npfs_name_valid((const uint8_t *)update->revision, strlen(update->revision))) {
    prior->reason = "previous revision is unknown or is not a valid directory name";
    success = true;
    goto done;
  }
  status = update_system(pool, &bin);
  if (status != INSPECT_OK) {
    goto failed;
  }
  struct npfs_inode root, revision_root, manifest_inode;
  status = read_inode(pool, &bin, 1, &root);
  if (status != INSPECT_OK) {
    goto failed;
  }
  uint64_t revision_number, manifest_number;
  status = prior_lookup(pool, &bin, 1, &root, update->revision,
      &revision_number, &revision_root);
  if (status != INSPECT_OK) {
    goto failed;
  }
  if (!revision_number) {
    prior->reason = "previous program revision directory is absent";
    success = true;
    goto done;
  }
  if (revision_root.kind != NPFS_INODE_DIRECTORY) {
    status = INSPECT_DAMAGED;
    goto failed;
  }
  status = prior_lookup(pool, &bin, revision_number, &revision_root, INSTALL_MANIFEST_NAME,
      &manifest_number, &manifest_inode);
  if (status != INSPECT_OK) {
    goto failed;
  }
  bool valid = false;
  if (manifest_number) {
    status = prior_manifest(pool, &manifest_inode, update->revision, &prior->inventory, &valid);
    if (status != INSPECT_OK) {
      goto failed;
    }
  }
  if (valid) {
    prior->state = INSTALL_PRIOR_MANIFEST;
    prior->reason = "previous recorded revision manifest";
    success = true;
    goto done;
  }
  prior->needs_legacy = true;
  prior->reason = manifest_number ? "previous revision manifest is invalid" :
      "previous revision manifest is missing";
  if (!inventory_legacy) {
    success = true;
    goto done;
  }
  enum install_manifest_status initialized = install_manifest_init(&prior->inventory,
      update->revision);
  if (initialized != INSTALL_MANIFEST_OK) {
    status = initialized == INSTALL_MANIFEST_NO_MEMORY ? INSPECT_FATAL : INSPECT_DAMAGED;
    goto failed;
  }
  status = prior_tree(pool, &bin, revision_number, &revision_root, &prior->inventory);
  if (status != INSPECT_OK) {
    goto failed;
  }
  prior->state = INSTALL_PRIOR_SYNTHESIZED;
  prior->needs_legacy = false;
  prior->reason = "previous revision read and hashed without mounting";
  success = true;
  goto done;
failed:
  prior->reason = status == INSPECT_FATAL ? "I/O, allocation or hashing failure during prior inventory" :
      "prior inventory metadata is damaged or exceeds inspection bounds";
done:
  install_progress_finish();
  free(pool);
  if (!success) {
    install_manifest_destroy(&prior->inventory);
    prior->needs_legacy = false;
  }
  return success;
}
