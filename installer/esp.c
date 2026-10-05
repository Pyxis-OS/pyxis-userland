#include "installer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_CLUSTER_BYTES 4096u
#define ESP_RESERVED_SECTORS 32u
#define ESP_FAT_COUNT 2u
#define ESP_MIN_CLUSTERS 65525u
#define ESP_FAT_MASK UINT32_C(0x0fffffff)
#define ESP_FAT_LAST_CLUSTER UINT32_C(0x0fffffef)
#define ESP_FAT_EOC_MIN UINT32_C(0x0ffffff8)
#define ESP_FAT_EOC UINT32_C(0x0fffffff)
#define ESP_MEDIA 0xf8u
#define ESP_DIRECTORY 0x10u
#define ESP_ARCHIVE 0x20u
#define ESP_LONG_NAME 0x0fu
#define ESP_LONG_LAST 0x40u
#define ESP_BOOT_SIGNATURE 0xaa55u
#define ESP_FSINFO_LEAD UINT32_C(0x41615252)
#define ESP_FSINFO_STRUCTURE UINT32_C(0x61417272)
#define ESP_FSINFO_TRAIL UINT32_C(0xaa550000)
#define ESP_FSINFO_SECTOR 1u
#define ESP_BACKUP_SECTOR 6u
#define ESP_DIRECTORY_ENTRY_BYTES 32u
#define ESP_LONG_UNITS 13u
#define ESP_FAT_DATE 0x0021u /* 1980-01-01; no portable source creation time. */

enum esp_node_id {
  ESP_ROOT, ESP_EFI, ESP_EFI_BOOT, ESP_BOOT, ESP_LIMINE,
  ESP_EFI_FILE, ESP_KERNEL_FILE, ESP_ARCHIVE_FILE, ESP_CONFIG_FILE,
  ESP_REVISION_FILE, ESP_NODE_COUNT,
};

struct esp_node {
  const char *name, *alias;
  enum esp_node_id parent;
  const struct install_source *source;
  const char *contents;
  uint32_t size, first, clusters;
  bool directory;
};

struct install_esp {
  const struct install_disk *disk;
  const struct install_layout *layout;
  uint32_t sector_bytes, sectors, fat_sectors, cluster_count, next_cluster, serial;
  uint64_t fat_offset, fat_bytes, data_offset;
  uint8_t *fat;
  struct esp_node nodes[ESP_NODE_COUNT];
};

struct esp_reader {
  const struct install_esp *esp;
  uint8_t *fat, *seen;
  uint32_t root, clusters, used, free_count, next_free;
  uint64_t data_offset;
  uint32_t first[ESP_NODE_COUNT];
};

static uint16_t get16(const uint8_t *bytes)
{
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t get32(const uint8_t *bytes)
{
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
      ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void put16(uint8_t *bytes, uint16_t value)
{
  bytes[0] = value;
  bytes[1] = value >> 8;
}

static void put32(uint8_t *bytes, uint32_t value)
{
  bytes[0] = value;
  bytes[1] = value >> 8;
  bytes[2] = value >> 16;
  bytes[3] = value >> 24;
}

static bool all_zero(const uint8_t *bytes, size_t length)
{
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i]) {
      return false;
    }
  }
  return true;
}

static bool esp_read(const struct install_esp *esp, uint64_t offset,
    void *bytes, size_t length)
{
  return offset <= esp->layout->esp_bytes && length <= esp->layout->esp_bytes - offset &&
      install_read(esp->disk, esp->layout->esp_start + offset, bytes, length);
}

static bool esp_write(const struct install_esp *esp, uint64_t offset,
    const void *bytes, size_t length)
{
  return offset <= esp->layout->esp_bytes && length <= esp->layout->esp_bytes - offset &&
      install_write(esp->disk, esp->layout->esp_start + offset, bytes, length);
}

static uint8_t alias_checksum(const char *alias)
{
  uint8_t sum = 0;
  for (unsigned i = 0; i < 11; ++i) {
    sum = ((sum & 1u) << 7) + (sum >> 1) + (uint8_t)alias[i];
  }
  return sum;
}

static bool same_name(const char *left, const char *right)
{
  /* The authored boot tree is ASCII; FAT resolves its names without case. */
  while (*left && *right) {
    unsigned a = (uint8_t)*left++, b = (uint8_t)*right++;
    if (a >= 'a' && a <= 'z') {
      a -= 'a' - 'A';
    }
    if (b >= 'a' && b <= 'z') {
      b -= 'a' - 'A';
    }
    if (a != b) {
      return false;
    }
  }
  return !*left && !*right;
}

static void set_short(uint8_t *entry, const char *alias, bool directory,
    uint32_t cluster, uint32_t size)
{
  memcpy(entry, alias, 11);
  entry[11] = directory ? ESP_DIRECTORY : ESP_ARCHIVE;
  put16(entry + 16, ESP_FAT_DATE);
  put16(entry + 18, ESP_FAT_DATE);
  put16(entry + 20, cluster >> 16);
  put16(entry + 24, ESP_FAT_DATE);
  put16(entry + 26, cluster);
  put32(entry + 28, size);
}

static void set_long(uint8_t *entry, const struct esp_node *node)
{
  static const uint8_t offsets[ESP_LONG_UNITS] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
  };
  size_t length = strlen(node->name);
  entry[0] = ESP_LONG_LAST | 1u;
  entry[11] = ESP_LONG_NAME;
  entry[13] = alias_checksum(node->alias);
  for (size_t i = 0; i < ESP_LONG_UNITS; ++i) {
    uint16_t unit = i < length ? (uint8_t)node->name[i] : i == length ? 0 : UINT16_MAX;
    put16(entry + offsets[i], unit);
  }
}

static void make_boot(const struct install_esp *esp, uint8_t *sector)
{
  memset(sector, 0, esp->sector_bytes);
  sector[0] = 0xeb;
  sector[1] = 0x58;
  sector[2] = 0x90;
  memcpy(sector + 3, "PYXIS   ", 8);
  put16(sector + 11, esp->sector_bytes);
  sector[13] = ESP_CLUSTER_BYTES / esp->sector_bytes;
  put16(sector + 14, ESP_RESERVED_SECTORS);
  sector[16] = ESP_FAT_COUNT;
  sector[21] = ESP_MEDIA;
  put16(sector + 24, 63);
  put16(sector + 26, 255);
  put32(sector + 28, esp->layout->esp_start / esp->sector_bytes);
  put32(sector + 32, esp->sectors);
  put32(sector + 36, esp->fat_sectors);
  put32(sector + 44, esp->nodes[ESP_ROOT].first);
  put16(sector + 48, ESP_FSINFO_SECTOR);
  put16(sector + 50, ESP_BACKUP_SECTOR);
  sector[64] = 0x80;
  sector[66] = 0x29;
  put32(sector + 67, esp->serial);
  memcpy(sector + 71, "NO NAME    ", 11);
  memcpy(sector + 82, "FAT32   ", 8);
  /* UEFI loads the EFI file; this is not an executable legacy boot loader. */
  put16(sector + 510, ESP_BOOT_SIGNATURE);
}

static void make_fsinfo(const struct install_esp *esp, uint8_t *sector)
{
  memset(sector, 0, esp->sector_bytes);
  put32(sector, ESP_FSINFO_LEAD);
  put32(sector + 484, ESP_FSINFO_STRUCTURE);
  put32(sector + 488, esp->cluster_count - (esp->next_cluster - 2));
  put32(sector + 492, esp->next_cluster < esp->cluster_count + 2 ?
      esp->next_cluster : UINT32_MAX);
  put32(sector + 508, ESP_FSINFO_TRAIL);
}

static bool source_read(const struct esp_node *node,
    uint64_t offset, void *bytes, size_t length)
{
  if (offset > node->size || length > node->size - offset) {
    return false;
  }
  if (node->source) {
    return install_source_read(node->source, offset, bytes, length);
  }
  memcpy(bytes, node->contents + offset, length);
  return true;
}

struct install_esp *install_esp_plan(const struct install_disk *disk,
    const struct install_layout *layout, const struct install_source *efi,
    const struct install_source *kernel, const struct install_source *archive,
    const char *configuration, size_t configuration_bytes,
    const char *revision, size_t revision_bytes, uint32_t serial)
{
  if (!disk || !layout || !efi || !kernel || !archive || !configuration ||
      !revision || !revision_bytes ||
      disk->info.block_size < 512 || disk->info.block_size > ESP_CLUSTER_BYTES ||
      (disk->info.block_size & (disk->info.block_size - 1)) ||
      layout->esp_bytes != INSTALL_ESP_BYTES ||
      layout->esp_start % disk->info.block_size || layout->esp_start > disk->bytes ||
      layout->esp_bytes > disk->bytes - layout->esp_start ||
      layout->esp_start / disk->info.block_size > UINT32_MAX ||
      efi->bytes > UINT32_MAX || kernel->bytes > UINT32_MAX ||
      archive->bytes > UINT32_MAX || configuration_bytes > UINT32_MAX ||
      revision_bytes > UINT32_MAX) {
    fputs("installer: invalid ESP extent, geometry or source size\n", stderr);
    return NULL;
  }
  struct install_esp *esp = calloc(1, sizeof(*esp));
  if (!esp) {
    return NULL;
  }
  esp->disk = disk;
  esp->layout = layout;
  esp->serial = serial;
  esp->sector_bytes = disk->info.block_size;
  esp->sectors = layout->esp_bytes / esp->sector_bytes;
  uint32_t cluster_sectors = ESP_CLUSTER_BYTES / esp->sector_bytes;
  uint64_t maximum_clusters = (esp->sectors - ESP_RESERVED_SECTORS) / cluster_sectors;
  /* Size for the upper bound before either FAT consumes sectors. Rounding up
   * cannot undersize the FAT and keeps both copies in whole I/O chunks. */
  esp->fat_bytes = ((maximum_clusters + 2) * 4 + ESP_CLUSTER_BYTES - 1) &
      ~(uint64_t)(ESP_CLUSTER_BYTES - 1);
  esp->fat_sectors = esp->fat_bytes / esp->sector_bytes;
  esp->fat_offset = (uint64_t)ESP_RESERVED_SECTORS * esp->sector_bytes;
  esp->data_offset = esp->fat_offset + ESP_FAT_COUNT * esp->fat_bytes;
  if (esp->data_offset >= layout->esp_bytes || esp->data_offset % ESP_CLUSTER_BYTES ||
      esp->fat_bytes > SIZE_MAX) {
    install_esp_destroy(esp);
    return NULL;
  }
  esp->cluster_count = (layout->esp_bytes - esp->data_offset) / ESP_CLUSTER_BYTES;
  if (esp->cluster_count < ESP_MIN_CLUSTERS ||
      esp->cluster_count > ESP_FAT_LAST_CLUSTER - 1 ||
      (uint64_t)(esp->cluster_count + 2) * 4 > esp->fat_bytes) {
    install_esp_destroy(esp);
    return NULL;
  }
  esp->nodes[ESP_ROOT] = (struct esp_node){
    .name = "", .alias = "", .parent = ESP_ROOT, .directory = true,
  };
  esp->nodes[ESP_EFI] = (struct esp_node){
    .name = "EFI", .alias = "EFI        ", .parent = ESP_ROOT, .directory = true,
  };
  esp->nodes[ESP_EFI_BOOT] = (struct esp_node){
    .name = "BOOT", .alias = "BOOT       ", .parent = ESP_EFI, .directory = true,
  };
  esp->nodes[ESP_BOOT] = (struct esp_node){
    .name = "boot", .alias = "BOOT       ", .parent = ESP_ROOT, .directory = true,
  };
  esp->nodes[ESP_LIMINE] = (struct esp_node){
    .name = "limine", .alias = "LIMINE     ", .parent = ESP_BOOT, .directory = true,
  };
  esp->nodes[ESP_EFI_FILE] = (struct esp_node){
    .name = "BOOTX64.EFI", .alias = "BOOTX64 EFI", .parent = ESP_EFI_BOOT,
    .source = efi, .size = efi->bytes,
  };
  esp->nodes[ESP_KERNEL_FILE] = (struct esp_node){
    .name = "caelum.elf", .alias = "CAELUM  ELF", .parent = ESP_BOOT,
    .source = kernel, .size = kernel->bytes,
  };
  esp->nodes[ESP_ARCHIVE_FILE] = (struct esp_node){
    .name = "initrd.cpio", .alias = "INITRD~1CPI", .parent = ESP_BOOT,
    .source = archive, .size = archive->bytes,
  };
  esp->nodes[ESP_CONFIG_FILE] = (struct esp_node){
    .name = "limine.conf", .alias = "LIMINE~1CON", .parent = ESP_LIMINE,
    .contents = configuration, .size = configuration_bytes,
  };
  esp->nodes[ESP_REVISION_FILE] = (struct esp_node){
    .name = "revision", .alias = "REVISION   ", .parent = ESP_BOOT,
    .contents = revision, .size = revision_bytes,
  };
  esp->next_cluster = 2;
  for (unsigned i = 0; i < ESP_NODE_COUNT; ++i) {
    struct esp_node *node = &esp->nodes[i];
    uint64_t clusters = node->directory ? 1 :
        ((uint64_t)node->size + ESP_CLUSTER_BYTES - 1) / ESP_CLUSTER_BYTES;
    if (clusters > esp->cluster_count - (esp->next_cluster - 2)) {
      fputs("installer: boot files do not fit in the ESP\n", stderr);
      install_esp_destroy(esp);
      return NULL;
    }
    node->clusters = clusters;
    node->first = clusters ? esp->next_cluster : 0;
    esp->next_cluster += clusters;
  }
  esp->fat = calloc(1, esp->fat_bytes);
  if (!esp->fat) {
    install_esp_destroy(esp);
    return NULL;
  }
  put32(esp->fat, ESP_FAT_MASK & (UINT32_C(0xffffff00) | ESP_MEDIA));
  put32(esp->fat + 4, ESP_FAT_EOC);
  for (unsigned i = 0; i < ESP_NODE_COUNT; ++i) {
    const struct esp_node *node = &esp->nodes[i];
    for (uint32_t j = 0; j < node->clusters; ++j) {
      put32(esp->fat + (size_t)(node->first + j) * 4,
          j + 1 == node->clusters ? ESP_FAT_EOC : node->first + j + 1);
    }
  }
  return esp;
}

static void make_directory(const struct install_esp *esp, unsigned index, uint8_t *bytes)
{
  memset(bytes, 0, ESP_CLUSTER_BYTES);
  const struct esp_node *directory = &esp->nodes[index];
  size_t offset = 0;
  if (index != ESP_ROOT) {
    set_short(bytes, ".          ", true, directory->first, 0);
    uint32_t parent = directory->parent == ESP_ROOT ? 0 :
        esp->nodes[directory->parent].first;
    set_short(bytes + ESP_DIRECTORY_ENTRY_BYTES, "..         ", true, parent, 0);
    offset = 2 * ESP_DIRECTORY_ENTRY_BYTES;
  }
  for (unsigned i = 1; i < ESP_NODE_COUNT; ++i) {
    const struct esp_node *node = &esp->nodes[i];
    if (node->parent != index) {
      continue;
    }
    set_long(bytes + offset, node);
    set_short(bytes + offset + ESP_DIRECTORY_ENTRY_BYTES, node->alias,
        node->directory, node->first, node->size);
    offset += 2 * ESP_DIRECTORY_ENTRY_BYTES;
  }
}

static bool esp_flush(const struct install_esp *esp)
{
  enum call_status status = disk_flush(esp->disk->handle);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: ESP sync failed (status %u); stopping\n", status);
    return false;
  }
  return true;
}

bool install_esp_write(const struct install_esp *esp)
{
  if (!esp) {
    return false;
  }
  uint8_t bytes[ESP_CLUSTER_BYTES];
  /* Persist invalid boot geometry before any old FAT, directory or file bytes
   * can be replaced. Stale configuration/revision must not validate a mixed tree. */
  if (!install_zero(esp->disk, esp->layout->esp_start, esp->fat_offset) || !esp_flush(esp)) {
    return false;
  }
  make_fsinfo(esp, bytes);
  if (!esp_write(esp, (uint64_t)ESP_FSINFO_SECTOR * esp->sector_bytes, bytes, esp->sector_bytes) ||
      !esp_write(esp, (uint64_t)(ESP_BACKUP_SECTOR + ESP_FSINFO_SECTOR) * esp->sector_bytes,
          bytes, esp->sector_bytes)) {
    return false;
  }
  memset(bytes, 0, esp->sector_bytes);
  put16(bytes + 510, ESP_BOOT_SIGNATURE);
  if (!esp_write(esp, 2u * esp->sector_bytes, bytes, esp->sector_bytes) ||
      !esp_write(esp, (ESP_BACKUP_SECTOR + 2u) * esp->sector_bytes, bytes, esp->sector_bytes)) {
    return false;
  }
  for (unsigned i = 0; i < ESP_FAT_COUNT; ++i) {
    for (uint64_t offset = 0; offset < esp->fat_bytes; offset += ESP_CLUSTER_BYTES) {
      if (!esp_write(esp, esp->fat_offset + i * esp->fat_bytes + offset,
          esp->fat + offset, ESP_CLUSTER_BYTES)) {
        return false;
      }
    }
  }
  for (unsigned i = 0; i < ESP_NODE_COUNT; ++i) {
    const struct esp_node *node = &esp->nodes[i];
    if (node->directory) {
      make_directory(esp, i, bytes);
      uint64_t offset = esp->data_offset + (uint64_t)(node->first - 2) * ESP_CLUSTER_BYTES;
      if (!esp_write(esp, offset, bytes, ESP_CLUSTER_BYTES)) {
        return false;
      }
      continue;
    }
    uint64_t consumed = 0;
    for (uint32_t j = 0; j < node->clusters; ++j) {
      size_t length = node->size - consumed;
      if (length > ESP_CLUSTER_BYTES) {
        length = ESP_CLUSTER_BYTES;
      }
      memset(bytes, 0, sizeof(bytes));
      if (!source_read(node, consumed, bytes, length)) {
        return false;
      }
      uint64_t offset = esp->data_offset + (uint64_t)(node->first + j - 2) * ESP_CLUSTER_BYTES;
      if (!esp_write(esp, offset, bytes, sizeof(bytes))) {
        return false;
      }
      consumed += length;
    }
  }
  /* Publish boot geometry only after the complete replacement tree is durable.
   * The caller's final sync also covers both boot sectors. */
  if (!esp_flush(esp)) {
    return false;
  }
  make_boot(esp, bytes);
  return esp_write(esp, 0, bytes, esp->sector_bytes) &&
      esp_write(esp, (uint64_t)ESP_BACKUP_SECTOR * esp->sector_bytes, bytes, esp->sector_bytes);
}

static bool take_cluster(struct esp_reader *reader, uint32_t cluster)
{
  if (cluster < 2 || cluster - 2 >= reader->clusters ||
      (reader->seen[cluster / 8] & (1u << (cluster % 8)))) {
    return false;
  }
  reader->seen[cluster / 8] |= 1u << (cluster % 8);
  ++reader->used;
  return true;
}

static uint32_t next_cluster(const struct esp_reader *reader, uint32_t cluster)
{
  return get32(reader->fat + (size_t)cluster * 4) & ESP_FAT_MASK;
}

static bool read_cluster(const struct esp_reader *reader, uint32_t cluster, void *bytes)
{
  return esp_read(reader->esp,
      reader->data_offset + (uint64_t)(cluster - 2) * ESP_CLUSTER_BYTES,
      bytes, ESP_CLUSTER_BYTES);
}

static bool read_long(const uint8_t *entry, char name[ESP_LONG_UNITS + 1])
{
  static const uint8_t offsets[ESP_LONG_UNITS] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
  };
  if (entry[0] != (ESP_LONG_LAST | 1u) || entry[12] || get16(entry + 26)) {
    return false;
  }
  bool ended = false;
  memset(name, 0, ESP_LONG_UNITS + 1);
  for (unsigned i = 0; i < ESP_LONG_UNITS; ++i) {
    uint16_t unit = get16(entry + offsets[i]);
    if (!ended && !unit) {
      ended = true;
    } else if (ended) {
      if (unit != UINT16_MAX) {
        return false;
      }
    } else if (unit > 127) {
      return false;
    } else {
      name[i] = unit;
    }
  }
  return name[0] != 0;
}

static uint32_t entry_cluster(const uint8_t *entry)
{
  return ((uint32_t)get16(entry + 20) << 16) | get16(entry + 26);
}

static bool verify_directory(struct esp_reader *reader, unsigned index)
{
  uint8_t bytes[ESP_CLUSTER_BYTES];
  bool found[ESP_NODE_COUNT] = {0}, dot = false, dotdot = false;
  bool ended = false, have_long = false;
  uint8_t checksum = 0;
  char name[ESP_LONG_UNITS + 1];
  uint32_t cluster = reader->first[index];
  for (;;) {
    if (!take_cluster(reader, cluster) || !read_cluster(reader, cluster, bytes)) {
      return false;
    }
    for (size_t offset = 0; offset < sizeof(bytes); offset += ESP_DIRECTORY_ENTRY_BYTES) {
      const uint8_t *entry = bytes + offset;
      if (ended || !entry[0]) {
        if (have_long || !all_zero(entry, ESP_DIRECTORY_ENTRY_BYTES)) {
          return false;
        }
        ended = true;
        continue;
      }
      if (entry[11] == ESP_LONG_NAME) {
        if (have_long || !read_long(entry, name)) {
          return false;
        }
        checksum = entry[13];
        have_long = true;
        continue;
      }
      uint32_t first = entry_cluster(entry), size = get32(entry + 28);
      if (!memcmp(entry, ".          ", 11) || !memcmp(entry, "..         ", 11)) {
        bool parent = entry[1] == '.';
        uint32_t wanted = parent ? reader->first[reader->esp->nodes[index].parent] :
            reader->first[index];
        if (parent && reader->esp->nodes[index].parent == ESP_ROOT) {
          wanted = 0;
        }
        if (index == ESP_ROOT || have_long || entry[11] != ESP_DIRECTORY || size ||
            first != wanted || (parent ? dotdot : dot)) {
          return false;
        }
        if (parent) {
          dotdot = true;
        } else {
          dot = true;
        }
        continue;
      }
      if (!have_long || checksum != alias_checksum((const char *)entry)) {
        return false;
      }
      bool matched = false;
      for (unsigned i = 1; i < ESP_NODE_COUNT; ++i) {
        const struct esp_node *node = &reader->esp->nodes[i];
        if (node->parent != index || !same_name(name, node->name)) {
          continue;
        }
        if (found[i] || memcmp(entry, node->alias, 11) || size != node->size ||
            entry[11] != (node->directory ? ESP_DIRECTORY : ESP_ARCHIVE)) {
          return false;
        }
        reader->first[i] = first;
        found[i] = true;
        matched = true;
        break;
      }
      if (!matched) {
        return false;
      }
      have_long = false;
    }
    uint32_t next = next_cluster(reader, cluster);
    if (next >= ESP_FAT_EOC_MIN) {
      break;
    }
    cluster = next;
  }
  if (!ended || have_long || (index != ESP_ROOT && (!dot || !dotdot))) {
    return false;
  }
  for (unsigned i = 1; i < ESP_NODE_COUNT; ++i) {
    if (reader->esp->nodes[i].parent == index && !found[i]) {
      return false;
    }
  }
  return true;
}

static bool verify_file(struct esp_reader *reader, unsigned index)
{
  const struct esp_node *node = &reader->esp->nodes[index];
  uint32_t cluster = reader->first[index];
  if (!node->size) {
    return cluster == 0;
  }
  uint8_t actual[ESP_CLUSTER_BYTES], expected[ESP_CLUSTER_BYTES];
  uint64_t consumed = 0;
  while (consumed < node->size) {
    if (!take_cluster(reader, cluster) || !read_cluster(reader, cluster, actual)) {
      return false;
    }
    size_t length = node->size - consumed;
    if (length > ESP_CLUSTER_BYTES) {
      length = ESP_CLUSTER_BYTES;
    }
    if (!source_read(node, consumed, expected, length) ||
        memcmp(actual, expected, length) || !all_zero(actual + length, sizeof(actual) - length)) {
      return false;
    }
    consumed += length;
    uint32_t next = next_cluster(reader, cluster);
    if (consumed == node->size) {
      return next >= ESP_FAT_EOC_MIN;
    }
    cluster = next;
  }
  return false;
}

static bool read_metadata(struct esp_reader *reader)
{
  const struct install_esp *esp = reader->esp;
  uint8_t actual[ESP_CLUSTER_BYTES], expected[ESP_CLUSTER_BYTES];
  if (!esp_read(esp, 0, actual, esp->sector_bytes)) {
    return false;
  }
  make_boot(esp, expected);
  if (memcmp(actual, expected, esp->sector_bytes)) {
    return false;
  }
  uint32_t sector_bytes = get16(actual + 11), cluster_sectors = actual[13];
  uint32_t reserved = get16(actual + 14), fat_count = actual[16];
  uint32_t sectors = get32(actual + 32), fat_sectors = get32(actual + 36);
  uint64_t fat_bytes = (uint64_t)fat_sectors * sector_bytes;
  reader->data_offset = ((uint64_t)reserved + (uint64_t)fat_count * fat_sectors) * sector_bytes;
  if (!cluster_sectors || (uint64_t)cluster_sectors * sector_bytes != ESP_CLUSTER_BYTES ||
      reader->data_offset >= (uint64_t)sectors * sector_bytes ||
      (uint64_t)sectors * sector_bytes != esp->layout->esp_bytes || fat_bytes > SIZE_MAX) {
    return false;
  }
  reader->clusters = ((uint64_t)sectors * sector_bytes - reader->data_offset) / ESP_CLUSTER_BYTES;
  reader->root = get32(actual + 44);
  if (reader->clusters < ESP_MIN_CLUSTERS ||
      (uint64_t)(reader->clusters + 2) * 4 > fat_bytes) {
    return false;
  }
  reader->first[ESP_ROOT] = reader->root;
  if (!esp_read(esp, (uint64_t)get16(actual + 50) * sector_bytes, expected, sector_bytes) ||
      memcmp(actual, expected, sector_bytes)) {
    return false;
  }
  if (!esp_read(esp, (uint64_t)ESP_FSINFO_SECTOR * sector_bytes, actual, sector_bytes) ||
      !esp_read(esp, (uint64_t)(ESP_BACKUP_SECTOR + ESP_FSINFO_SECTOR) * sector_bytes,
          expected, sector_bytes) || memcmp(actual, expected, sector_bytes) ||
      get32(actual) != ESP_FSINFO_LEAD || get32(actual + 484) != ESP_FSINFO_STRUCTURE ||
      get32(actual + 508) != ESP_FSINFO_TRAIL || !all_zero(actual + 4, 480) ||
      !all_zero(actual + 496, 12) || !all_zero(actual + 512, sector_bytes - 512)) {
    return false;
  }
  reader->free_count = get32(actual + 488);
  reader->next_free = get32(actual + 492);
  for (uint32_t sector = 2; sector < reserved; ++sector) {
    if (sector == ESP_BACKUP_SECTOR || sector == ESP_BACKUP_SECTOR + ESP_FSINFO_SECTOR) {
      continue;
    }
    memset(expected, 0, sector_bytes);
    if (sector == 2 || sector == ESP_BACKUP_SECTOR + 2) {
      put16(expected + 510, ESP_BOOT_SIGNATURE);
    }
    if (!esp_read(esp, (uint64_t)sector * sector_bytes, actual, sector_bytes) ||
        memcmp(actual, expected, sector_bytes)) {
      return false;
    }
  }
  reader->fat = malloc(fat_bytes);
  reader->seen = calloc(1, ((size_t)reader->clusters + 2 + 7) / 8);
  if (!reader->fat || !reader->seen) {
    return false;
  }
  uint64_t fat_offset = (uint64_t)reserved * sector_bytes;
  for (uint64_t offset = 0; offset < fat_bytes; offset += sizeof(actual)) {
    if (!esp_read(esp, fat_offset + offset, reader->fat + offset, sizeof(actual)) ||
        !esp_read(esp, fat_offset + fat_bytes + offset, actual, sizeof(actual)) ||
        memcmp(reader->fat + offset, actual, sizeof(actual))) {
      return false;
    }
  }
  return get32(reader->fat) == (ESP_FAT_MASK & (UINT32_C(0xffffff00) | ESP_MEDIA)) &&
      get32(reader->fat + 4) == ESP_FAT_EOC;
}

bool install_esp_verify(const struct install_esp *esp)
{
  if (!esp) {
    return false;
  }
  struct esp_reader reader = {.esp = esp};
  bool success = read_metadata(&reader);
  for (unsigned i = 0; success && i < ESP_NODE_COUNT; ++i) {
    success = esp->nodes[i].directory ? verify_directory(&reader, i) : verify_file(&reader, i);
  }
  if (success) {
    for (uint32_t cluster = 2; cluster < reader.clusters + 2; ++cluster) {
      bool used = (reader.seen[cluster / 8] & (1u << (cluster % 8))) != 0;
      uint32_t entry = get32(reader.fat + (size_t)cluster * 4);
      if ((entry & ~ESP_FAT_MASK) || (!used && entry)) {
        success = false;
        break;
      }
    }
    size_t end = (size_t)(reader.clusters + 2) * 4;
    if (!all_zero(reader.fat + end, esp->fat_bytes - end) ||
        reader.free_count != reader.clusters - reader.used) {
      success = false;
    }
    if (reader.next_free != UINT32_MAX &&
        (reader.next_free < 2 || reader.next_free - 2 >= reader.clusters ||
         (reader.seen[reader.next_free / 8] & (1u << (reader.next_free % 8))))) {
      success = false;
    }
  }
  free(reader.fat);
  free(reader.seen);
  if (!success) {
    fputs("installer: ESP metadata, boot paths or source-byte verification failed\n", stderr);
  }
  return success;
}

void install_esp_destroy(struct install_esp *esp)
{
  if (esp) {
    free(esp->fat);
    free(esp);
  }
}
