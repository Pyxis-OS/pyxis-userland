#include "installer.h"

#include <pyxis_fs/npfs.h>
#include <stdlib.h>
#include <string.h>

struct pool_metadata {
  uint8_t header[NPFS_BLOCK_SIZE];
  uint8_t volume[NPFS_BLOCK_SIZE];
  uint8_t inodes[NPFS_BLOCK_SIZE];
  uint8_t directory[NPFS_BLOCK_SIZE];
  uint8_t controls[2][NPFS_BLOCK_SIZE];
  uint8_t bitmap[NPFS_BLOCK_SIZE];
};

static bool bootstrap_blocks(const struct npfs_header *header, uint64_t blocks[2])
{
  uint64_t next = 1;
  unsigned count = 0;
  while (next < header->pool_blocks - 1 && count < 2) {
    if (next >= header->bitmap_start &&
        next - header->bitmap_start < header->bitmap_blocks) {
      next = header->bitmap_start + header->bitmap_blocks;
    } else if (next >= header->volume_start &&
        next - header->volume_start < header->volume_blocks) {
      next = header->volume_start + header->volume_blocks;
    } else if (next >= header->journal_start &&
        next - header->journal_start < header->journal_blocks) {
      next = header->journal_start + header->journal_blocks;
    } else {
      blocks[count++] = next++;
    }
  }
  return count == 2;
}

static void bitmap_mark(uint8_t *bytes, uint64_t base, uint64_t begin, uint64_t end)
{
  uint64_t limit = base + NPFS_BITMAP_BITS;
  if (begin < base) {
    begin = base;
  }
  if (end > limit) {
    end = limit;
  }
  if (begin >= end) {
    return;
  }
  while (begin < end && begin % 8) {
    bytes[(begin - base) / 8] |= (uint8_t)(1u << (begin % 8));
    begin++;
  }
  uint64_t whole_end = end - end % 8;
  if (begin < whole_end) {
    memset(bytes + (begin - base) / 8, 0xff, (size_t)((whole_end - begin) / 8));
    begin = whole_end;
  }
  while (begin < end) {
    bytes[(begin - base) / 8] |= (uint8_t)(1u << (begin % 8));
    begin++;
  }
}

static struct npfs_inode new_inode(uint16_t kind, uint64_t parent,
    int64_t created_ns, bool created_valid)
{
  struct npfs_inode inode = {
    .kind = kind,
    .mapping = NPFS_MAPPING_POINTERS,
    .parent = parent,
  };
  if (created_valid) {
    inode.flags = NPFS_TIME_CREATED_VALID | NPFS_TIME_MODIFIED_VALID;
    inode.created_ns = created_ns;
    inode.modified_ns = created_ns;
  }
  return inode;
}

static bool prepare_metadata(const struct npfs_header *header,
    const uint8_t volume_id[NPFS_ID_SIZE], const uint64_t blocks[2],
    int64_t created_ns, bool created_valid, struct pool_metadata *metadata)
{
  static const char volume_name[] = "system";
  static const char marker_name[] = "SAFE_TO_WIPE";
  struct npfs_volume volume = {
    .state = NPFS_VOLUME_LIVE,
    .mapping = NPFS_MAPPING_POINTERS,
    .name_length = sizeof(volume_name) - 1,
    .root_inode = 1,
    .inode_bytes = NPFS_BLOCK_SIZE,
    .pointers = {blocks[0]},
  };
  memcpy(volume.id, volume_id, sizeof(volume.id));
  memcpy(volume.name, volume_name, volume.name_length);
  struct npfs_inode root = new_inode(NPFS_INODE_DIRECTORY, 1, created_ns, created_valid);
  root.size = NPFS_BLOCK_SIZE;
  root.pointers[0] = blocks[1];
  struct npfs_inode marker = new_inode(NPFS_INODE_FILE, 0, created_ns, created_valid);
  struct npfs_dirent entry = {
    .inode = 2,
    .record_length = (NPFS_DIRENT_HEADER_SIZE + sizeof(marker_name) - 1 + 7) & ~7u,
    .name_length = sizeof(marker_name) - 1,
  };
  memcpy(entry.name, marker_name, entry.name_length);
  struct npfs_dirent unused = {.record_length = NPFS_BLOCK_SIZE - entry.record_length};
  struct npfs_control control = {.state = NPFS_JOURNAL_EMPTY};
  memcpy(control.pool_id, header->pool_id, sizeof(control.pool_id));

  if (npfs_header_encode(header, metadata->header) != NPFS_OK ||
      npfs_volume_encode(header, &volume, metadata->volume) != NPFS_OK ||
      npfs_inode_encode(header, &root, metadata->inodes + NPFS_INODE_SIZE) != NPFS_OK ||
      npfs_inode_encode(header, &marker, metadata->inodes + 2 * NPFS_INODE_SIZE) != NPFS_OK ||
      npfs_dirent_encode(header, &entry, metadata->directory) != NPFS_OK ||
      npfs_dirent_encode(header, &unused, metadata->directory + entry.record_length) != NPFS_OK ||
      npfs_control_encode(header, &control, metadata->controls[0]) != NPFS_OK) {
    return false;
  }
  control.sequence = 1;
  return npfs_control_encode(header, &control, metadata->controls[1]) == NPFS_OK;
}

static bool write_block(const struct install_disk *disk, const struct install_layout *layout,
    uint64_t block, const uint8_t bytes[NPFS_BLOCK_SIZE])
{
  return install_write(disk, layout->pool_start + block * NPFS_BLOCK_SIZE,
      bytes, NPFS_BLOCK_SIZE);
}

bool install_pool_format(const struct install_disk *disk,
    const struct install_layout *layout, const struct npfs_header *header,
    const uint8_t volume_id[16], int64_t created_ns, bool created_valid)
{
  if (!disk || !layout || !header || !volume_id || disk->handle == HANDLE_INVALID ||
      disk->info.preparation != DISK_READY ||
      disk->info.block_size < 512 || disk->info.block_size > NPFS_BLOCK_SIZE ||
      (disk->info.block_size & (disk->info.block_size - 1)) ||
      !(disk->info.flags & DISK_FLAG_WRITABLE) ||
      !(disk->info.flags & DISK_FLAG_FLUSH_SUPPORTED) ||
      (disk->info.flags & DISK_FLAG_WRITE_FAILED) ||
      disk->info.block_count > UINT64_MAX / disk->info.block_size ||
      disk->bytes != disk->info.block_count * disk->info.block_size ||
      layout->pool_start % disk->info.block_size ||
      layout->pool_bytes % disk->info.block_size ||
      layout->pool_start > disk->bytes ||
      layout->pool_bytes > disk->bytes - layout->pool_start ||
      header->pool_blocks != layout->pool_bytes / NPFS_BLOCK_SIZE ||
      npfs_header_validate(header) != NPFS_OK || npfs_features_check(header, true) != NPFS_OK ||
      !npfs_id_valid(volume_id)) {
    return false;
  }
  uint64_t blocks[2];
  if (!bootstrap_blocks(header, blocks)) {
    return false;
  }
  struct pool_metadata *metadata = calloc(1, sizeof(*metadata));
  if (!metadata) {
    return false;
  }
  bool ok = prepare_metadata(header, volume_id, blocks, created_ns, created_valid, metadata);
  if (!ok) {
    goto done;
  }

  for (uint64_t index = 0; index < header->bitmap_blocks; index++) {
    uint64_t base = index * NPFS_BITMAP_BITS;
    memset(metadata->bitmap, 0, NPFS_BLOCK_SIZE);
    bitmap_mark(metadata->bitmap, base, 0, 1);
    bitmap_mark(metadata->bitmap, base, header->bitmap_start,
        header->bitmap_start + header->bitmap_blocks);
    bitmap_mark(metadata->bitmap, base, header->volume_start,
        header->volume_start + header->volume_blocks);
    bitmap_mark(metadata->bitmap, base, header->journal_start,
        header->journal_start + header->journal_blocks);
    bitmap_mark(metadata->bitmap, base, blocks[0], blocks[0] + 1);
    bitmap_mark(metadata->bitmap, base, blocks[1], blocks[1] + 1);
    bitmap_mark(metadata->bitmap, base, header->pool_blocks - 1, base + NPFS_BITMAP_BITS);
    if (!write_block(disk, layout, header->bitmap_start + index, metadata->bitmap)) {
      ok = false;
      goto done;
    }
  }
  memset(metadata->bitmap, 0, NPFS_BLOCK_SIZE);
  for (uint64_t index = 0; index < header->volume_blocks; index++) {
    const uint8_t *bytes = index == 0 ? metadata->volume : metadata->bitmap;
    if (!write_block(disk, layout, header->volume_start + index, bytes)) {
      ok = false;
      goto done;
    }
  }
  ok = write_block(disk, layout, blocks[0], metadata->inodes) &&
      write_block(disk, layout, blocks[1], metadata->directory) &&
      write_block(disk, layout, header->journal_start, metadata->controls[0]) &&
      write_block(disk, layout, header->journal_start + 1, metadata->controls[1]);
  /* Keep the new headers last; the caller flushes before releasing the raw claim. */
  if (ok) {
    ok = write_block(disk, layout, header->pool_blocks - 1, metadata->header) &&
        write_block(disk, layout, 0, metadata->header);
  }

done:
  free(metadata);
  return ok;
}
