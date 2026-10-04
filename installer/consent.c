#include "installer.h"

#include <pyxis_fs/npfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPT_TABLE_BYTES 65536u
#define GPT_ENTRIES 256u
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
  uint8_t table[GPT_TABLE_BYTES];
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
  if (gpt->count > GPT_ENTRIES || table_bytes > GPT_TABLE_BYTES) {
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
  }
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
  }
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

static enum inspection open_pool(struct pool_view *pool, bool *degraded)
{
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
  if (npfs_features_check(&pool->header, false) != NPFS_OK) {
    return INSPECT_DAMAGED;
  }
  struct npfs_control controls[2];
  bool valid[2];
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
  return load_overlay(pool);
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

static enum inspection mapping_read(struct pool_view *pool, const uint64_t *pointers,
    uint64_t logical, uint64_t *leaf)
{
  struct npfs_map_path path;
  if (npfs_map_path(logical, &path) != NPFS_OK) {
    return INSPECT_DAMAGED;
  }
  uint64_t block = pointers[path.slot], ancestors[3];
  for (unsigned depth = 0; depth < path.depth; depth++) {
    if (!block) {
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
  enum inspection status = allocated_pointer(pool, block);
  if (status == INSPECT_OK) {
    *leaf = block;
  }
  return status;
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
  }
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
  if (!install_read(disk, 0, block, (size_t)disk->info.block_size)) {
    goto done;
  }
  bool mbr = protective_mbr(block, disk);
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
  bool ambiguous = primary && backup && !gpt_agree(&maps[0], &maps[1]);
  bool valid_gpt = mbr && (primary || backup) && !ambiguous;
  consent->degraded = valid_gpt && primary != backup;
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
    consent->reason = ambiguous ? "GPT copies disagree" : "no validated protective GPT";
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
  free(block);
  free(maps);
  return success;
}
