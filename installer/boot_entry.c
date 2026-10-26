#include "boot_entry.h"
#include "installer.h"
#include "menu_ownership.h"
#include "digest.h"
#include "progress.h"
#include "esp_payload.h"
#include "esp_transfer.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <pyxis_fs/npfs.h>
#include <stdlib.h>
#include <string.h>

void install_guid_text(const uint8_t guid[16], char text[37])
{
  snprintf(text, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      npfs_get_u32(guid), (unsigned)npfs_get_u16(guid + 4),
      (unsigned)npfs_get_u16(guid + 6), guid[8], guid[9],
      guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

static char *entry(const char *name, bool rescue, const char *kernel, const char *archive,
    const uint8_t disk_guid[16], uint64_t pool_partition, const uint8_t esp_guid[16], size_t *size)
{
  char disk[37], location[43] = "boot()";
  install_guid_text(disk_guid, disk);
  if (esp_guid) {
    char esp[37];
    install_guid_text(esp_guid, esp);
    snprintf(location, sizeof(location), "guid(%s)", esp);
  }
  char *text = malloc(1024);
  if (!text) {
    return NULL;
  }
  int count = snprintf(text, 1024,
      "/%s\n  protocol: limine\n  path: %s:/%s\n  module_path: %s:/%s\n"
      "  paging_mode: 4level\n  kaslr: no\n  cmdline: " INSTALL_BOOT_INIT_OPTION
      " mount.disk=%s mount.pool=%llu%s\n", name, location, kernel, location, archive,
      disk, (unsigned long long)pool_partition, rescue ? " " INSTALL_RESCUE_OPTION : "");
  if (count <= 0 || count >= 1024) {
    free(text);
    return NULL;
  }
  *size = count;
  return text;
}

enum boot_entry_role { ENTRY_NORMAL, ENTRY_RESCUE, ENTRY_ROLES };
static const char *const role_ids[ENTRY_ROLES] = {"boot.normal", "boot.rescue"};
static const char *const role_names[ENTRY_ROLES] = {
  "Pyxis OS (Caelum)", "Pyxis OS (rescue)"
};

/* The owned, marked form of one entry, optionally preceded by a blank line. */
static char *owned_block(enum boot_entry_role role, const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, bool separated, size_t *size)
{
  size_t entry_size = 0;
  char *text = entry(role_names[role], role == ENTRY_RESCUE, paths->kernel, paths->archive,
      disk_guid, pool_partition, NULL, &entry_size);
  char *block = text ? malloc(entry_size + 128) : NULL;
  int count = block ? snprintf(block, entry_size + 128, "%s# pyxis:begin %s\n%s# pyxis:end %s\n",
      separated ? "\n" : "", role_ids[role], text, role_ids[role]) : -1;
  free(text);
  if (count <= 0 || (size_t)count >= entry_size + 128) {
    free(block);
    return NULL;
  }
  *size = (size_t)count;
  return block;
}

struct menu_edit {
  size_t start, end;
  char *text;
  size_t size;
};

bool install_boot_entry_plan(const struct config_blob *current,
    const struct install_boot_menu *inherited, const uint8_t disk_guid[16],
    uint64_t pool_partition, const struct install_boot_menu *next,
    struct config_blob *selected, bool *pending)
{
  *pending = true;
  if (!current->digest.present) {
    config_blob_destroy(selected);
    return true;
  }
  if (!config_blob_set(selected, current->bytes, current->size)) {
    return false;
  }
  if (memchr(current->bytes, 0, current->size)) {
    return true;
  }
  struct config_owned_set *owned = NULL;
  if (!config_owned_scan_limine("installed boot entries", (char *)current->bytes, current->size, &owned)) {
    return true;
  }
  struct menu_edit edits[ENTRY_ROLES + 1] = {0};
  struct literal_span spans[ENTRY_ROLES] = {0};
  unsigned count = 0;
  bool recognized = true, ok = true;
  for (unsigned i = 0; i < ENTRY_ROLES && recognized && ok; ++i) {
    enum boot_entry_role role = (enum boot_entry_role)i;
    const struct config_owned_span *marked = config_owned_find(owned, role_ids[role]);
    const struct install_boot_entry_paths *wanted = next->normal;
    const struct install_boot_entry_paths *old = inherited ? inherited->normal : NULL;
    if (!marked || (!old && !(inherited && inherited->entire_proven))) {
      recognized = false;
      break;
    }
    if (!inherited->entire_proven) {
      size_t expected_size;
      char *expected = entry(role_names[role], role == ENTRY_RESCUE, old->kernel, old->archive,
          disk_guid, pool_partition, NULL, &expected_size);
      if (!expected) {
        ok = false;
        break;
      }
      recognized = marked->content.end - marked->content.start == expected_size &&
          !memcmp(current->bytes + marked->content.start, expected, expected_size);
      free(expected);
    }
    if (!recognized) {
      break;
    }
    spans[role] = marked->span;
    edits[count] = (struct menu_edit){.start = spans[role].start, .end = spans[role].end};
    if (wanted) {
      edits[count].text = owned_block(role, wanted, disk_guid, pool_partition, false, &edits[count].size);
      ok = edits[count].text != NULL;
    }
    ++count;
  }
  const struct config_owned_span *legacy = config_owned_find(owned, "boot.previous");
  if (recognized && ok && legacy) {
    const struct install_boot_entry_paths *old = inherited ? inherited->legacy_previous : NULL;
    recognized = inherited && (inherited->entire_proven || old);
    if (recognized && !inherited->entire_proven) {
      size_t expected_size;
      char *expected = entry("Pyxis OS (previous)", false, old->kernel, old->archive,
          disk_guid, pool_partition, NULL, &expected_size);
      ok = expected != NULL;
      if (ok) {
        recognized = legacy->content.end - legacy->content.start == expected_size &&
            !memcmp(current->bytes + legacy->content.start, expected, expected_size);
      }
      free(expected);
    }
    if (recognized && ok && !next->legacy_previous) {
      edits[count++] = (struct menu_edit){.start = legacy->span.start, .end = legacy->span.end};
    }
  }
  if (recognized && ok && spans[ENTRY_NORMAL].end <= spans[ENTRY_RESCUE].start) {
    for (unsigned i = 1; i < count; ++i) {
      /* An insertion sorts before a replacement starting at the same offset. */
      for (unsigned j = i; j > 0 && (edits[j].start < edits[j - 1].start ||
          (edits[j].start == edits[j - 1].start && edits[j].end < edits[j - 1].end)); --j) {
        struct menu_edit swap = edits[j];
        edits[j] = edits[j - 1];
        edits[j - 1] = swap;
      }
    }
    size_t removed = 0, added = 0;
    for (unsigned i = 0; i < count; ++i) {
      ok = ok && (!i || edits[i - 1].end <= edits[i].start);
      removed += edits[i].end - edits[i].start;
      added += edits[i].size;
    }
    if (!ok || added > CONFIG_FILE_MAX_BYTES ||
        current->size - removed > CONFIG_FILE_MAX_BYTES - added) {
      ok = false;
    } else {
      size_t size = current->size - removed + added, used = 0, from = 0;
      unsigned char *bytes = malloc(size + 1);
      ok = bytes != NULL;
      for (unsigned i = 0; ok && i < count; ++i) {
        memcpy(bytes + used, current->bytes + from, edits[i].start - from);
        used += edits[i].start - from;
        if (edits[i].size) {
          memcpy(bytes + used, edits[i].text, edits[i].size);
          used += edits[i].size;
        }
        from = edits[i].end;
      }
      if (ok) {
        memcpy(bytes + used, current->bytes + from, current->size - from);
        ok = config_blob_set(selected, bytes, size);
        *pending = !ok;
      }
      free(bytes);
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    free(edits[i].text);
  }
  config_owned_free(owned);
  return ok;
}

bool install_boot_entry_current(const struct config_blob *current,
    const struct install_boot_menu *committed, const uint8_t disk_guid[16],
    uint64_t pool_partition, bool *is_current)
{
  *is_current = false;
  struct config_blob menu = {0};
  bool pending = true;
  if (!install_boot_entry_plan(current, committed, disk_guid, pool_partition, committed,
      &menu, &pending)) {
    config_blob_destroy(&menu);
    return false;
  }
  *is_current = !pending && config_blob_equal(&menu, current);
  config_blob_destroy(&menu);
  return true;
}

bool install_boot_entry_render_owned(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, struct config_blob *menu)
{
  const char *const ids[] = {"boot.normal", "boot.rescue"};
  const char *const names[] = {"Pyxis OS (Caelum)", "Pyxis OS (rescue)"};
  char *entries[2] = {0};
  size_t sizes[2] = {0};
  bool ok = true;
  for (unsigned i = 0; i < 2; ++i) {
    entries[i] = entry(names[i], i == 1, paths->kernel, paths->archive, disk_guid, pool_partition,
        NULL, &sizes[i]);
    ok &= entries[i] != NULL;
  }
  if (ok) {
    char bytes[4096];
    int size = snprintf(bytes, sizeof(bytes), "timeout: 3\n\n"
        "# pyxis:begin %s\n%s# pyxis:end %s\n\n"
        "# pyxis:begin %s\n%s# pyxis:end %s\n",
        ids[0], entries[0], ids[0], ids[1], entries[1], ids[1]);
    ok = size > 0 && (size_t)size < sizeof(bytes) && config_blob_set(menu, bytes, (size_t)size);
  }
  free(entries[0]);
  free(entries[1]);
  return ok;
}

bool install_boot_entry_render(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, const uint8_t esp_guid[16],
    struct config_blob *pair)
{
  size_t normal_size = 0, rescue_size = 0;
  char *normal = entry("Pyxis OS (Caelum)", false, paths->kernel, paths->archive,
      disk_guid, pool_partition, esp_guid, &normal_size);
  char *rescue = entry("Pyxis OS (rescue)", true, paths->kernel, paths->archive,
      disk_guid, pool_partition, esp_guid, &rescue_size);
  bool ok = normal && rescue;
  if (ok) {
    size_t size = normal_size + rescue_size + 2;
    char *bytes = malloc(size);
    ok = bytes != NULL;
    if (ok) {
      memcpy(bytes, normal, normal_size);
      bytes[normal_size] = '\n';
      memcpy(bytes + normal_size + 1, rescue, rescue_size);
      bytes[size - 1] = '\n';
      ok = config_blob_set(pair, bytes, size);
    }
    free(bytes);
  }
  free(normal);
  free(rescue);
  return ok;
}

void install_boot_entry_print(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, FILE *output)
{
  const char *names[] = {"Pyxis OS (Caelum)", "Pyxis OS (rescue)"};
  fputs("Manual boot entry proposal (existing menu untouched):\n", output);
  for (unsigned i = 0; i < 2; ++i) {
    size_t size = 0;
    char *text = entry(names[i], i == 1, paths->kernel, paths->archive, disk_guid, pool_partition,
        NULL, &size);
    if (text) {
      fwrite(text, 1, size, output);
      fputc('\n', output);
      free(text);
    }
  }
}

bool install_boot_payload_digest(const struct install_source *source, struct config_file_digest *output)
{
  if (!output) {
    return false;
  }
  *output = (struct config_file_digest){0};
  if (!source) {
    return false;
  }
  struct install_digest digest = {0};
  char sha256[65];
  unsigned char *buffer = malloc(FILE_READ_MAX_BYTES);
  bool ok = buffer && source->bytes && install_digest_begin(&digest);
  install_progress_begin("Freezing boot payload evidence", INSTALL_PROGRESS_SOURCE_READ, NULL);
  install_progress_bytes_total(source->bytes);
  for (uint64_t offset = 0; ok && offset < source->bytes;) {
    size_t count = source->bytes - offset < FILE_READ_MAX_BYTES ?
        (size_t)(source->bytes - offset) : FILE_READ_MAX_BYTES;
    ok = install_source_read(source, offset, buffer, count) && install_digest_update(&digest, buffer, count);
    if (ok) {
      offset += count;
    }
  }
  ok = ok && install_digest_finish(&digest, sha256);
  if (ok) {
    output->present = true;
    output->size = source->bytes;
    strcpy(output->sha256, sha256);
  }
  install_digest_abort(&digest);
  free(buffer);
  return ok;
}

