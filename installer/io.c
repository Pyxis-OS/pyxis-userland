#include "installer.h"

#include <file.h>
#include <pyxis_fs/npfs.h>
#include <stdio.h>
#include <string.h>

void npfs_memory_copy(void *destination, const void *source, size_t length)
{
  memcpy(destination, source, length);
}

void npfs_memory_zero(void *destination, size_t length)
{
  memset(destination, 0, length);
}

static bool range_valid(const struct install_disk *disk, uint64_t offset, uint64_t length)
{
  return disk->info.block_size != 0 && disk->info.block_size <= DISK_IO_MAX_BYTES &&
      offset <= disk->bytes && length <= disk->bytes - offset;
}

bool install_read(const struct install_disk *disk, uint64_t offset, void *bytes, size_t length)
{
  if (!range_valid(disk, offset, length)) {
    return false;
  }
  uint8_t *destination = bytes;
  uint8_t block[DISK_IO_MAX_BYTES];
  size_t alignment = disk->info.block_size;
  while (length) {
    size_t skip = offset % alignment;
    size_t count;
    enum call_status status;
    if (skip || length < alignment) {
      status = disk_read(disk->handle, offset - skip, block, alignment);
      count = alignment - skip;
      if (count > length) {
        count = length;
      }
      if (status == CALL_OK) {
        memcpy(destination, block + skip, count);
      }
    } else {
      count = length < DISK_IO_MAX_BYTES ? length : DISK_IO_MAX_BYTES;
      count -= count % alignment;
      status = disk_read(disk->handle, offset, destination, count);
    }
    if (status != CALL_OK) {
      fprintf(stderr, "installer: disk read failed at byte %llu (status %u)\n",
          (unsigned long long)offset, status);
      return false;
    }
    destination += count;
    offset += count;
    length -= count;
  }
  return true;
}

bool install_write(const struct install_disk *disk, uint64_t offset,
    const void *bytes, size_t length)
{
  if (!range_valid(disk, offset, length) || offset % disk->info.block_size ||
      length % disk->info.block_size) {
    return false;
  }
  const uint8_t *source = bytes;
  while (length) {
    size_t count = length < DISK_IO_MAX_BYTES ? length : DISK_IO_MAX_BYTES;
    enum call_status status = disk_write(disk->handle, offset, source, count);
    if (status != CALL_OK) {
      fprintf(stderr, "installer: disk write failed at byte %llu (status %u); stopping\n",
          (unsigned long long)offset, status);
      return false;
    }
    source += count;
    offset += count;
    length -= count;
  }
  return true;
}

bool install_zero(const struct install_disk *disk, uint64_t offset, uint64_t length)
{
  const uint8_t bytes[DISK_IO_MAX_BYTES] = {0};
  if (!range_valid(disk, offset, length) || offset % disk->info.block_size ||
      length % disk->info.block_size) {
    return false;
  }
  while (length) {
    size_t count = length < sizeof(bytes) ? (size_t)length : sizeof(bytes);
    if (!install_write(disk, offset, bytes, count)) {
      return false;
    }
    offset += count;
    length -= count;
  }
  return true;
}

bool install_source_read(const struct install_source *source, uint64_t offset,
    void *bytes, size_t length)
{
  if (offset > source->bytes || length > source->bytes - offset) {
    return false;
  }
  if (source->memory) {
    memcpy(bytes, source->memory + offset, length);
    return true;
  }
  uint8_t *destination = bytes;
  while (length) {
    size_t count = 0;
    enum call_status status = file_read(source->handle, offset, destination, length, &count);
    if (status != CALL_OK || count == 0 || count > length) {
      fprintf(stderr, "installer: source read failed at byte %llu (status %u)\n",
          (unsigned long long)offset, status);
      return false;
    }
    offset += count;
    destination += count;
    length -= count;
  }
  return true;
}

uint32_t install_crc32(const void *bytes, size_t length)
{
  const uint8_t *source = bytes;
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc ^= source[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
  }
  return ~crc;
}
