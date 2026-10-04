#include "installer.h"

#include <pyxis_fs/npfs.h>
#include <stdio.h>
#include <string.h>

#define GPT_HEADER_BYTES 92u
#define GPT_ENTRY_BYTES 128u
#define GPT_ENTRY_COUNT 128u
#define GPT_ARRAY_BYTES (GPT_ENTRY_COUNT * GPT_ENTRY_BYTES)

static const uint8_t esp_type[16] = {
  0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
  0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
static const uint8_t pool_type[16] = {
  0xa3, 0x94, 0x81, 0x1a, 0x07, 0x8a, 0xff, 0x4d,
  0x83, 0x0e, 0x4c, 0xb4, 0xed, 0x7a, 0xac, 0x00,
};

void install_guid_text(const uint8_t guid[16], char text[37])
{
  snprintf(text, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      npfs_get_u32(guid), (unsigned)npfs_get_u16(guid + 4),
      (unsigned)npfs_get_u16(guid + 6), guid[8], guid[9],
      guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

bool install_layout_plan(const struct install_disk *disk, struct install_layout *layout)
{
  uint64_t block_size = disk->info.block_size;
  if (block_size < 512 || block_size > 4096 || (block_size & (block_size - 1)) ||
      disk->info.block_count > UINT64_MAX / block_size ||
      disk->bytes != disk->info.block_count * block_size) {
    return false;
  }
  uint64_t tail_bytes = GPT_ARRAY_BYTES + block_size;
  uint64_t pool_start = INSTALL_MIB + INSTALL_ESP_BYTES;
  if (disk->bytes <= tail_bytes || disk->bytes - tail_bytes <= pool_start) {
    return false;
  }
  uint64_t pool_end = disk->bytes - tail_bytes;
  pool_end -= pool_end % NPFS_BLOCK_SIZE;
  if (pool_end <= pool_start) {
    return false;
  }
  layout->esp_start = INSTALL_MIB;
  layout->esp_bytes = INSTALL_ESP_BYTES;
  layout->pool_start = pool_start;
  layout->pool_bytes = pool_end - pool_start;
  return true;
}

static void partition_entry(uint8_t *entry, const uint8_t type[16],
    const uint8_t guid[16], uint64_t first, uint64_t bytes,
    uint64_t block_size, const char *name)
{
  memcpy(entry, type, 16);
  memcpy(entry + 16, guid, 16);
  npfs_put_u64(entry + 32, first / block_size);
  npfs_put_u64(entry + 40, (first + bytes) / block_size - 1);
  for (size_t i = 0; name[i]; ++i) {
    npfs_put_u16(entry + 56 + 2 * i, (unsigned char)name[i]);
  }
}

static void header_encode(uint8_t *bytes, const struct install_disk *disk,
    const struct install_layout *layout, bool backup, uint32_t array_crc)
{
  uint64_t last = disk->info.block_count - 1;
  uint64_t array_blocks = GPT_ARRAY_BYTES / disk->info.block_size;
  memset(bytes, 0, disk->info.block_size);
  memcpy(bytes, "EFI PART", 8);
  npfs_put_u32(bytes + 8, UINT32_C(0x00010000));
  npfs_put_u32(bytes + 12, GPT_HEADER_BYTES);
  npfs_put_u64(bytes + 24, backup ? last : 1);
  npfs_put_u64(bytes + 32, backup ? 1 : last);
  npfs_put_u64(bytes + 40, 2 + array_blocks);
  npfs_put_u64(bytes + 48, last - array_blocks - 1);
  memcpy(bytes + 56, layout->disk_guid, 16);
  npfs_put_u64(bytes + 72, backup ? last - array_blocks : 2);
  npfs_put_u32(bytes + 80, GPT_ENTRY_COUNT);
  npfs_put_u32(bytes + 84, GPT_ENTRY_BYTES);
  npfs_put_u32(bytes + 88, array_crc);
  npfs_put_u32(bytes + 16, install_crc32(bytes, GPT_HEADER_BYTES));
}

bool install_gpt_write(const struct install_disk *disk, const struct install_layout *layout)
{
  uint8_t entries[GPT_ARRAY_BYTES] = {0};
  uint8_t block[DISK_IO_MAX_BYTES];
  uint64_t block_size = disk->info.block_size;
  partition_entry(entries, esp_type, layout->esp_guid,
      layout->esp_start, layout->esp_bytes, block_size, "Pyxis EFI");
  partition_entry(entries + GPT_ENTRY_BYTES, pool_type, layout->pool_guid,
      layout->pool_start, layout->pool_bytes, block_size, "Pyxis pool");
  uint32_t array_crc = install_crc32(entries, sizeof(entries));
  uint64_t backup_header = disk->bytes - block_size;
  if (!install_write(disk, backup_header - sizeof(entries), entries, sizeof(entries))) {
    return false;
  }
  header_encode(block, disk, layout, true, array_crc);
  if (!install_write(disk, backup_header, block, block_size) ||
      !install_write(disk, 2 * block_size, entries, sizeof(entries))) {
    return false;
  }
  header_encode(block, disk, layout, false, array_crc);
  if (!install_write(disk, block_size, block, block_size)) {
    return false;
  }
  memset(block, 0, block_size);
  uint8_t *protective = block + 446;
  protective[2] = 2;
  protective[4] = 0xee;
  protective[5] = protective[6] = protective[7] = 0xff;
  npfs_put_u32(protective + 8, 1);
  uint64_t covered = disk->info.block_count - 1;
  npfs_put_u32(protective + 12, covered > UINT32_MAX ? UINT32_MAX : (uint32_t)covered);
  npfs_put_u16(block + 510, UINT16_C(0xaa55));
  return install_write(disk, 0, block, block_size);
}
