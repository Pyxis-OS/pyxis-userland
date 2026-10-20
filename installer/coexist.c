#include "coexist.h"

#include <string.h>

#define DEFAULT_POOL_BYTES (UINT64_C(64) * 1024 * 1024 * 1024)

void install_coexist_layout(const struct disk_partition_selection *esp,
    const struct disk_partition_selection *pool, struct install_layout *layout)
{
  *layout = (struct install_layout){.esp_partition = esp->partition,
      .pool_partition = pool->partition,
      .esp_start = esp->first_block * esp->device_block_size,
      .esp_bytes = esp->block_count * esp->device_block_size,
      .pool_start = pool->first_block * pool->device_block_size,
      .pool_bytes = pool->block_count * pool->device_block_size,
      .esp_attributes = esp->attributes, .pool_attributes = pool->attributes};
  memcpy(layout->disk_guid, pool->disk_guid, 16);
  memcpy(layout->esp_guid, esp->partition_guid, 16);
  memcpy(layout->pool_guid, pool->partition_guid, 16);
}

static const uint8_t esp_type[16] = {
  0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
  0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
static const uint8_t pool_type[16] = {
  0xa3, 0x94, 0x81, 0x1a, 0x07, 0x8a, 0xff, 0x4d,
  0x83, 0x0e, 0x4c, 0xb4, 0xed, 0x7a, 0xac, 0x00,
};

static void partition_selection(const struct disk_partition_selection *device,
    const struct install_gpt_partition *partition, struct disk_partition_selection *selection)
{
  *selection = *device;
  selection->partition = partition->entry;
  selection->first_block = partition->first_block;
  selection->block_count = partition->block_count;
  selection->attributes = partition->attributes;
  memcpy(selection->partition_guid, partition->guid, sizeof(selection->partition_guid));
  memcpy(selection->partition_type, partition->type_guid, sizeof(selection->partition_type));
}

static void consider_gap(uint64_t first, uint64_t end, uint64_t alignment,
    struct disk_partition_selection *gap)
{
  uint64_t skip = first % alignment ? alignment - first % alignment : 0;
  if (skip > end - first) {
    return;
  }
  first += skip;
  end -= end % alignment;
  /* Ascending inspection keeps the lower-LBA gap when aligned sizes tie. */
  if (end > first && end - first > gap->block_count) {
    gap->first_block = first;
    gap->block_count = end - first;
  }
}

static const char *candidate_reason(const struct install_coexist *facts)
{
  if (!facts->esp_count) {
    return "no EFI System Partition is present";
  }
  if (!facts->gap.partition) {
    return "GPT has no unused partition slot";
  }
  if (!facts->gap.block_count) {
    return "no unallocated MiB-aligned usable gap";
  }
  return NULL;
}

bool install_coexist_inspect(const struct install_disk *disk,
    struct install_coexist *facts, const char **reason)
{
  if (reason) {
    *reason = "missing disk or coexistence observation output";
  }
  if (!facts) {
    return false;
  }
  memset(facts, 0, sizeof(*facts));
  if (!disk || !disk->info.id) {
    return false;
  }
  if (disk->info.block_size != INSTALL_COEXIST_BLOCK_SIZE) {
    if (reason) {
      *reason = "coexistence requires 512-byte logical sectors";
    }
    return false;
  }
  if (!install_gpt_inspect(disk, &facts->gpt, reason)) {
    return false;
  }
  facts->gap = (struct disk_partition_selection){.id = disk->info.id,
      .device_block_size = disk->info.block_size, .device_block_count = disk->info.block_count};
  memcpy(facts->gap.disk_guid, facts->gpt.disk_guid, sizeof(facts->gap.disk_guid));
  memcpy(facts->gap.partition_type, pool_type, sizeof(facts->gap.partition_type));
  bool used[INSTALL_GPT_MAX_PARTITIONS] = {false};
  uint32_t order[INSTALL_GPT_MAX_PARTITIONS];
  for (uint32_t i = 0; i < facts->gpt.partition_count; ++i) {
    const struct install_gpt_partition *partition = &facts->gpt.partitions[i];
    used[partition->entry - 1] = true;
    uint32_t position = i;
    while (position && facts->gpt.partitions[order[position - 1]].first_block >
        partition->first_block) {
      order[position] = order[position - 1];
      --position;
    }
    order[position] = i;
    if (!memcmp(partition->type_guid, esp_type, sizeof(esp_type))) {
      partition_selection(&facts->gap, partition, &facts->esps[facts->esp_count++]);
    } else if (!memcmp(partition->type_guid, pool_type, sizeof(pool_type))) {
      partition_selection(&facts->gap, partition, &facts->pools[facts->pool_count++]);
    }
  }
  for (uint32_t i = 0; i < facts->gpt.slot_count; ++i) {
    if (!used[i]) {
      facts->gap.partition = (uint64_t)i + 1;
      break;
    }
  }
  uint64_t cursor = facts->gpt.first_usable;
  uint64_t alignment = INSTALL_MIB / disk->info.block_size;
  for (uint32_t i = 0; i < facts->gpt.partition_count; ++i) {
    const struct install_gpt_partition *partition = &facts->gpt.partitions[order[i]];
    consider_gap(cursor, partition->first_block, alignment, &facts->gap);
    cursor = partition->first_block + partition->block_count;
  }
  consider_gap(cursor, facts->gpt.last_usable + 1, alignment, &facts->gap);
  uint64_t gap_bytes = facts->gap.block_count * disk->info.block_size;
  facts->default_bytes = gap_bytes < DEFAULT_POOL_BYTES ? gap_bytes : DEFAULT_POOL_BYTES;
  if (reason) {
    *reason = candidate_reason(facts);
  }
  return true;
}

bool install_coexist_choose(const struct install_coexist *facts,
    const uint8_t pool_guid[16], uint64_t size_bytes,
    struct disk_partition_selection *selection, const char **reason)
{
  if (reason) {
    *reason = "missing coexistence facts, pool GUID or selection output";
  }
  if (!selection) {
    return false;
  }
  *selection = (struct disk_partition_selection){0};
  if (!facts || !pool_guid) {
    return false;
  }
  const char *failure = candidate_reason(facts);
  if (failure) {
    goto failed;
  }
  uint8_t nonzero = 0;
  for (size_t i = 0; i < 16; ++i) {
    nonzero |= pool_guid[i];
  }
  failure = "new pool GUID must be nonzero and distinct from existing partitions";
  if (!nonzero) {
    goto failed;
  }
  for (uint32_t i = 0; i < facts->gpt.partition_count; ++i) {
    if (!memcmp(pool_guid, facts->gpt.partitions[i].guid, 16)) {
      goto failed;
    }
  }
  if (!size_bytes) {
    size_bytes = facts->default_bytes;
  }
  failure = "pool size must be a positive whole MiB within the selected gap";
  if (!size_bytes || size_bytes % INSTALL_MIB ||
      !facts->gap.device_block_size ||
      size_bytes / facts->gap.device_block_size > facts->gap.block_count) {
    goto failed;
  }
  *selection = facts->gap;
  selection->block_count = size_bytes / selection->device_block_size;
  memcpy(selection->partition_guid, pool_guid, sizeof(selection->partition_guid));
  if (reason) {
    *reason = NULL;
  }
  return true;
failed:
  if (reason) {
    *reason = failure;
  }
  return false;
}
