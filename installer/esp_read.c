#include "installer.h"

#include <stdlib.h>
#include <string.h>

#define ESP_CLUSTER_BYTES 4096u
#define ESP_FAT_COUNT 2u
#define ESP_MIN_CLUSTERS 65525u
#define ESP_FAT_MASK UINT32_C(0x0fffffff)
#define ESP_FAT_EOC_MIN UINT32_C(0x0ffffff8)
#define ESP_MEDIA 0xf8u
#define ESP_DIRECTORY 0x10u
#define ESP_VOLUME_LABEL 0x08u
#define ESP_INVALID_ATTRIBUTES 0xc0u
#define ESP_LONG_NAME 0x0fu
#define ESP_LONG_LAST 0x40u
#define ESP_LONG_UNITS 13u
#define ESP_LONG_MAX_ENTRIES 20u
#define ESP_DIRECTORY_ENTRY_BYTES 32u
#define ESP_DELETED 0xe5u
#define ESP_BOOT_SIGNATURE 0xaa55u
#define ESP_FSINFO_LEAD UINT32_C(0x41615252)
#define ESP_FSINFO_STRUCTURE UINT32_C(0x61417272)
#define ESP_FSINFO_TRAIL UINT32_C(0xaa550000)
/* Recognition reads authored boot configuration, not arbitrary FAT files. */
#define ESP_CONFIG_MAX_BYTES 65536u
#define ESP_REVISION_MAX_BYTES 64u

struct esp_inspection {
  const struct install_disk *disk;
  const struct install_layout *layout;
  const char **reason;
  enum install_esp_state state;
  uint64_t fat_offset, fat_bytes, data_offset, fat_page;
  uint32_t clusters, root;
  uint8_t fat[ESP_CLUSTER_BYTES];
  uint8_t *seen;
  bool have_fat_page;
};

struct esp_path {
  const char *name, *alias;
  uint32_t first, size;
  bool directory, found, damaged, long_name_required;
};

struct esp_long_name {
  char name[ESP_LONG_UNITS + 1];
  uint8_t remaining, checksum;
  bool active, readable;
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

static bool rebuild(struct esp_inspection *esp, const char *reason)
{
  if (esp->state == INSTALL_ESP_VALID) {
    esp->state = INSTALL_ESP_REBUILD;
    *esp->reason = reason;
  }
  return false;
}

static bool refuse(struct esp_inspection *esp, const char *reason)
{
  esp->state = INSTALL_ESP_REFUSED;
  *esp->reason = reason;
  return false;
}

static bool read_bytes(struct esp_inspection *esp, uint64_t offset,
    void *bytes, size_t length)
{
  if (offset > esp->layout->esp_bytes || length > esp->layout->esp_bytes - offset) {
    return rebuild(esp, "ESP metadata addresses data outside its partition");
  }
  if (!install_read(esp->disk, esp->layout->esp_start + offset, bytes, length)) {
    return refuse(esp, "ESP read failed");
  }
  return true;
}

static bool valid_cluster(const struct esp_inspection *esp, uint32_t cluster)
{
  return cluster >= 2 && cluster - 2 < esp->clusters;
}

static bool take_cluster(struct esp_inspection *esp, uint32_t cluster)
{
  if (!valid_cluster(esp, cluster) || (esp->seen[cluster / 8] & (1u << (cluster % 8)))) {
    return rebuild(esp, "ESP boot paths contain an invalid, shared or cyclic FAT chain");
  }
  esp->seen[cluster / 8] |= 1u << (cluster % 8);
  return true;
}

static bool fat_entry(struct esp_inspection *esp, uint32_t cluster, uint32_t *value)
{
  uint64_t offset = (uint64_t)cluster * 4;
  if (offset > esp->fat_bytes || 4 > esp->fat_bytes - offset) {
    return rebuild(esp, "ESP FAT entry lies outside its FAT");
  }
  uint64_t page = offset / ESP_CLUSTER_BYTES * ESP_CLUSTER_BYTES;
  if (!esp->have_fat_page || esp->fat_page != page) {
    uint8_t mirror[ESP_CLUSTER_BYTES];
    size_t length = esp->fat_bytes - page < ESP_CLUSTER_BYTES ?
        (size_t)(esp->fat_bytes - page) : ESP_CLUSTER_BYTES;
    if (!read_bytes(esp, esp->fat_offset + page, esp->fat, length) ||
        !read_bytes(esp, esp->fat_offset + esp->fat_bytes + page, mirror, length)) {
      return false;
    }
    if (memcmp(esp->fat, mirror, length)) {
      return rebuild(esp, "ESP FAT copies disagree");
    }
    esp->fat_page = page;
    esp->have_fat_page = true;
  }
  *value = get32(esp->fat + offset - page) & ESP_FAT_MASK;
  return true;
}

static bool read_cluster(struct esp_inspection *esp, uint32_t cluster, void *bytes)
{
  return read_bytes(esp, esp->data_offset + (uint64_t)(cluster - 2) * ESP_CLUSTER_BYTES,
      bytes, ESP_CLUSTER_BYTES);
}

static bool fsinfo_valid(const uint8_t *bytes, uint32_t clusters)
{
  uint32_t free_count = get32(bytes + 488), next = get32(bytes + 492);
  return get32(bytes) == ESP_FSINFO_LEAD && get32(bytes + 484) == ESP_FSINFO_STRUCTURE &&
      get32(bytes + 508) == ESP_FSINFO_TRAIL &&
      (free_count == UINT32_MAX || free_count <= clusters) &&
      (next == UINT32_MAX || (next >= 2 && next - 2 < clusters));
}

static bool read_metadata(struct esp_inspection *esp)
{
  uint8_t boot[ESP_CLUSTER_BYTES], backup[ESP_CLUSTER_BYTES];
  uint32_t sector_bytes = esp->disk->info.block_size;
  if (!read_bytes(esp, 0, boot, sector_bytes)) {
    return false;
  }
  uint32_t reserved = get16(boot + 14), fat_sectors = get32(boot + 36);
  uint32_t sectors = get32(boot + 32);
  uint32_t fsinfo = get16(boot + 48), backup_sector = get16(boot + 50);
  esp->fat_offset = (uint64_t)reserved * sector_bytes;
  esp->fat_bytes = (uint64_t)fat_sectors * sector_bytes;
  esp->data_offset = esp->fat_offset + ESP_FAT_COUNT * esp->fat_bytes;
  if (get16(boot + 510) != ESP_BOOT_SIGNATURE || get16(boot + 11) != sector_bytes ||
      (uint64_t)boot[13] * sector_bytes != ESP_CLUSTER_BYTES || !reserved ||
      boot[16] != ESP_FAT_COUNT || get16(boot + 17) || get16(boot + 19) ||
      boot[21] != ESP_MEDIA || get16(boot + 22) || get16(boot + 40) || get16(boot + 42) ||
      get32(boot + 28) != esp->layout->esp_start / sector_bytes ||
      (uint64_t)sectors * sector_bytes != esp->layout->esp_bytes || !fat_sectors ||
      esp->data_offset >= esp->layout->esp_bytes || !fsinfo || !backup_sector ||
      fsinfo == backup_sector || (uint64_t)fsinfo + backup_sector >= reserved) {
    return rebuild(esp, "ESP has unsupported or damaged FAT32 geometry");
  }
  esp->clusters = (esp->layout->esp_bytes - esp->data_offset) / ESP_CLUSTER_BYTES;
  esp->root = get32(boot + 44);
  if (esp->clusters < ESP_MIN_CLUSTERS ||
      (uint64_t)(esp->clusters + 2) * 4 > esp->fat_bytes || !valid_cluster(esp, esp->root)) {
    return rebuild(esp, "ESP has invalid FAT32 cluster bounds");
  }
  /* Valid primary bounds allow binding inspection despite damaged backup or
   * allocation metadata. Neither can hide a readable foreign configuration. */
  if (!read_bytes(esp, (uint64_t)backup_sector * sector_bytes, backup, sector_bytes)) {
    return false;
  }
  /* Boot code, OEM strings and serials do not establish installation identity. */
  if (get16(backup + 510) != ESP_BOOT_SIGNATURE || memcmp(boot + 11, backup + 11, 41)) {
    rebuild(esp, "ESP backup boot geometry disagrees");
  }
  if (!read_bytes(esp, (uint64_t)fsinfo * sector_bytes, boot, sector_bytes) ||
      !read_bytes(esp, (uint64_t)(backup_sector + fsinfo) * sector_bytes, backup, sector_bytes)) {
    return false;
  }
  if (!fsinfo_valid(boot, esp->clusters) || !fsinfo_valid(backup, esp->clusters)) {
    rebuild(esp, "ESP has damaged FAT32 allocation metadata");
  }
  uint32_t media, reserved_entry;
  if (!fat_entry(esp, 0, &media) || !fat_entry(esp, 1, &reserved_entry)) {
    return false;
  }
  if (media != (ESP_FAT_MASK & (UINT32_C(0xffffff00) | ESP_MEDIA)) ||
      reserved_entry < ESP_FAT_EOC_MIN) {
    rebuild(esp, "ESP has damaged reserved FAT entries");
  }
  esp->seen = calloc(1, ((size_t)esp->clusters + 2 + 7) / 8);
  return esp->seen != NULL || refuse(esp, "ESP inspection allocation failed");
}

static unsigned ascii_upper(unsigned byte)
{
  return byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
}

static bool same_name(const char *left, const char *right)
{
  while (*left && *right) {
    if (ascii_upper((uint8_t)*left++) != ascii_upper((uint8_t)*right++)) {
      return false;
    }
  }
  return !*left && !*right;
}

static bool same_alias(const uint8_t *entry, const char *alias)
{
  for (unsigned i = 0; i < 11; ++i) {
    if (ascii_upper(entry[i]) != ascii_upper((uint8_t)alias[i])) {
      return false;
    }
  }
  return true;
}

static uint8_t alias_checksum(const uint8_t *entry)
{
  uint8_t sum = 0;
  for (unsigned i = 0; i < 11; ++i) {
    sum = ((sum & 1u) << 7) + (sum >> 1) + entry[i];
  }
  return sum;
}

static bool read_long_name(struct esp_inspection *esp, const uint8_t *entry,
    struct esp_long_name *name)
{
  static const uint8_t offsets[ESP_LONG_UNITS] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
  };
  unsigned ordinal = entry[0] & ~ESP_LONG_LAST;
  if (entry[12] || get16(entry + 26) || !ordinal || ordinal > ESP_LONG_MAX_ENTRIES) {
    return rebuild(esp, "ESP boot directory has malformed long names");
  }
  if (entry[0] & ESP_LONG_LAST) {
    if (name->active) {
      return rebuild(esp, "ESP boot directory has overlapping long names");
    }
    *name = (struct esp_long_name){
      .remaining = ordinal, .checksum = entry[13], .active = true, .readable = ordinal == 1,
    };
  }
  if (!name->active || ordinal != name->remaining || entry[13] != name->checksum) {
    return rebuild(esp, "ESP boot directory has disordered long names");
  }
  if (name->readable) {
    bool ended = false;
    for (unsigned i = 0; i < ESP_LONG_UNITS; ++i) {
      uint16_t unit = get16(entry + offsets[i]);
      if (!ended && !unit) {
        ended = true;
      } else if (ended) {
        if (unit != UINT16_MAX) {
          return rebuild(esp, "ESP boot directory has malformed long names");
        }
      } else if (unit < 32 || unit > 126) {
        name->readable = false;
        break;
      } else {
        name->name[i] = unit;
      }
    }
  }
  --name->remaining;
  return true;
}

static bool find_paths(struct esp_inspection *esp, uint32_t first,
    struct esp_path *paths, size_t count)
{
  uint8_t bytes[ESP_CLUSTER_BYTES];
  struct esp_long_name name = {0};
  bool ended = false;
  uint32_t cluster = first;
  for (;;) {
    if (!take_cluster(esp, cluster) || !read_cluster(esp, cluster, bytes)) {
      return false;
    }
    for (size_t offset = 0; !ended && offset < sizeof(bytes); offset += ESP_DIRECTORY_ENTRY_BYTES) {
      const uint8_t *entry = bytes + offset;
      if (!entry[0]) {
        if (name.active) {
          return rebuild(esp, "ESP boot directory has an unterminated long name");
        }
        ended = true;
        break;
      }
      if (entry[0] == ESP_DELETED) {
        name = (struct esp_long_name){0};
        continue;
      }
      if (entry[11] == ESP_LONG_NAME) {
        if (!read_long_name(esp, entry, &name)) {
          return false;
        }
        continue;
      }
      if (name.active && (name.remaining || alias_checksum(entry) != name.checksum)) {
        return rebuild(esp, "ESP boot directory has a mismatched long name");
      }
      for (size_t i = 0; i < count; ++i) {
        struct esp_path *path = &paths[i];
        if ((path->long_name_required || !same_alias(entry, path->alias)) &&
            !(name.active && name.readable && same_name(name.name, path->name))) {
          continue;
        }
        uint32_t start = ((uint32_t)get16(entry + 20) << 16) | get16(entry + 26);
        uint32_t size = get32(entry + 28);
        if (path->found || (entry[11] & (ESP_VOLUME_LABEL | ESP_INVALID_ATTRIBUTES)) ||
            !!(entry[11] & ESP_DIRECTORY) != path->directory ||
            (path->directory && size) ||
            ((path->directory || size) ? !valid_cluster(esp, start) : start != 0)) {
          path->found = true;
          path->damaged = true;
          rebuild(esp, "ESP boot path is ambiguous or damaged");
          continue;
        }
        path->first = start;
        path->size = size;
        path->found = true;
      }
      name = (struct esp_long_name){0};
    }
    uint32_t next;
    if (!fat_entry(esp, cluster, &next)) {
      return false;
    }
    if (next >= ESP_FAT_EOC_MIN) {
      break;
    }
    cluster = next;
  }
  return !name.active || rebuild(esp, "ESP boot directory has an unterminated long name");
}

static bool read_file(struct esp_inspection *esp, const struct esp_path *path,
    void *output, bool *complete)
{
  if (complete) {
    *complete = false;
  }
  uint8_t bytes[ESP_CLUSTER_BYTES];
  uint8_t *destination = output;
  uint32_t cluster = path->first, remaining = path->size;
  while (remaining) {
    if (!take_cluster(esp, cluster) || !read_cluster(esp, cluster, bytes)) {
      return false;
    }
    size_t length = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    memcpy(destination, bytes, length);
    destination += length;
    remaining -= length;
    if (complete && !remaining) {
      *complete = true;
    }
    uint32_t next;
    if (!fat_entry(esp, cluster, &next)) {
      return false;
    }
    if (remaining ? next >= ESP_FAT_EOC_MIN : next < ESP_FAT_EOC_MIN) {
      return rebuild(esp, "ESP boot file size disagrees with its FAT chain");
    }
    cluster = next;
  }
  return true;
}

static bool horizontal_space(char byte)
{
  return byte == ' ' || byte == '\t' || byte == '\r';
}

static bool same_token(const char *token, size_t length, const char *wanted)
{
  return length == strlen(wanted) && !memcmp(token, wanted, length);
}

static bool configuration_matches(struct esp_inspection *esp, const char *bytes, size_t size)
{
  char guid[37];
  install_guid_text(esp->layout->disk_guid, guid);
  bool normal = false;
  for (size_t offset = 0; offset < size;) {
    size_t end = offset;
    while (end < size && bytes[end] != '\n') {
      unsigned byte = (uint8_t)bytes[end++];
      if ((byte < 32 && byte != '\t' && byte != '\r') || byte > 126) {
        rebuild(esp, "ESP boot configuration is not readable ASCII text");
      }
    }
    size_t cursor = offset;
    offset = end < size ? end + 1 : end;
    while (cursor < end && horizontal_space(bytes[cursor])) {
      ++cursor;
    }
    if (end - cursor < 8 || memcmp(bytes + cursor, "cmdline:", 8)) {
      continue;
    }
    cursor += 8;
    unsigned installed = 0;
    bool bound = false;
    while (cursor < end) {
      while (cursor < end && horizontal_space(bytes[cursor])) {
        ++cursor;
      }
      if (cursor == end || bytes[cursor] == '#') {
        break;
      }
      size_t start = cursor;
      while (cursor < end && !horizontal_space(bytes[cursor])) {
        ++cursor;
      }
      size_t length = cursor - start;
      if (same_token(bytes + start, length, "space.pyxis=app://init-installed")) {
        ++installed;
      }
      if (length >= 11 && !memcmp(bytes + start, "mount.disk=", 11)) {
        if (bound) {
          return refuse(esp, "ESP boot configuration repeats mount.disk");
        }
        if (length != 11 + 36) {
          return refuse(esp, "ESP boot configuration has an invalid disk binding");
        }
        for (unsigned i = 0; i < 36; ++i) {
          if (ascii_upper((uint8_t)bytes[start + 11 + i]) != ascii_upper((uint8_t)guid[i])) {
            return refuse(esp, "ESP boot configuration names a different disk");
          }
        }
        bound = true;
      }
    }
    if (installed > 1) {
      rebuild(esp, "ESP boot configuration repeats the installed space");
    }
    normal |= installed == 1 && bound;
  }
  if (!normal) {
    return rebuild(esp, "ESP boot configuration lacks this disk's installed command line");
  }
  return esp->state == INSTALL_ESP_VALID;
}

static void revision_text(const uint8_t *bytes, size_t size, char revision[64])
{
  if (size && bytes[size - 1] == '\n') {
    --size;
  }
  if (!size || size >= 64) {
    return;
  }
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] < 33 || bytes[i] > 126) {
      return;
    }
  }
  memcpy(revision, bytes, size);
  revision[size] = '\0';
}

enum install_esp_state install_esp_inspect(const struct install_disk *disk,
    const struct install_layout *layout, char revision[64], const char **reason)
{
  strcpy(revision, "unknown");
  *reason = NULL;
  struct esp_inspection esp = {
    .disk = disk, .layout = layout, .reason = reason, .state = INSTALL_ESP_VALID,
  };
  if ((disk->info.block_size != 512 && disk->info.block_size != 4096) ||
      layout->esp_bytes != INSTALL_ESP_BYTES || layout->esp_start % disk->info.block_size ||
      layout->esp_start > disk->bytes || layout->esp_bytes > disk->bytes - layout->esp_start ||
      layout->esp_start / disk->info.block_size > UINT32_MAX) {
    refuse(&esp, "ESP extent or logical-sector geometry is unsupported");
    return esp.state;
  }
  bool success = read_metadata(&esp);
  struct esp_path boot = {.name = "boot", .alias = "BOOT       ", .directory = true};
  struct esp_path children[2] = {
    {.name = "limine", .alias = "LIMINE     ", .directory = true},
    {.name = "revision", .alias = "REVISION   "},
  };
  struct esp_path config = {.name = "limine.conf", .long_name_required = true};
  /* A path found before later directory damage can still expose a foreign
   * binding. Incomplete validation keeps its REBUILD diagnostic. */
  if (success) {
    bool complete = find_paths(&esp, esp.root, &boot, 1);
    success = esp.state != INSTALL_ESP_REFUSED && !boot.damaged &&
        (complete || (esp.state == INSTALL_ESP_REBUILD && boot.found));
  }
  if (success && !boot.found) {
    success = rebuild(&esp, "ESP lacks the installed boot directory");
  }
  if (success) {
    bool complete = find_paths(&esp, boot.first, children, 2);
    success = esp.state != INSTALL_ESP_REFUSED && !children[0].damaged &&
        (complete || (esp.state == INSTALL_ESP_REBUILD && children[0].found));
  }
  if (success && !children[0].found) {
    success = rebuild(&esp, "ESP lacks the installed Limine directory");
  }
  if (success) {
    bool complete = find_paths(&esp, children[0].first, &config, 1);
    success = esp.state != INSTALL_ESP_REFUSED && !config.damaged &&
        (complete || (esp.state == INSTALL_ESP_REBUILD && config.found));
  }
  if (success && (!config.found || !config.size || config.size > ESP_CONFIG_MAX_BYTES)) {
    success = rebuild(&esp, "ESP boot configuration is absent or exceeds the 64 KiB inspection limit");
  }
  char *configuration = NULL;
  if (success) {
    configuration = malloc(config.size);
    success = configuration != NULL || refuse(&esp, "ESP inspection allocation failed");
  }
  if (success) {
    bool complete;
    success = read_file(&esp, &config, configuration, &complete);
    /* Complete successful data reads can still expose a foreign binding when
     * the terminal FAT entry is damaged. Failed raw reads never qualify. */
    if (complete && esp.state != INSTALL_ESP_REFUSED) {
      success = configuration_matches(&esp, configuration, config.size);
    }
  }
  if (success && children[1].found && !children[1].damaged) {
    uint8_t bytes[ESP_REVISION_MAX_BYTES];
    if (children[1].size > sizeof(bytes)) {
      success = rebuild(&esp, "ESP revision record exceeds the 64-byte inspection limit");
    } else {
      success = read_file(&esp, &children[1], bytes, NULL);
      if (success) {
        revision_text(bytes, children[1].size, revision);
      }
    }
  }
  free(configuration);
  free(esp.seen);
  return esp.state;
}
