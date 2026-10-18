#include "boot_entry.h"
#include "installer.h"
#include "../config-editor/ownership.h"
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

static char *entry(const char *name, bool rescue, const char *kernel, const char *archive,
    const uint8_t disk_guid[16], const uint8_t esp_guid[16], size_t *size)
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
      " mount.disk=%s%s\n", name, location, kernel, location, archive,
      disk, rescue ? " " INSTALL_RESCUE_OPTION : "");
  if (count <= 0 || count >= 1024) {
    free(text);
    return NULL;
  }
  *size = count;
  return text;
}

static bool find_exact(const struct config_blob *current, const char *text, size_t size,
    struct literal_span *span)
{
  if (!current->digest.present || !size) {
    return false;
  }
  bool found = false;
  for (size_t offset = 0; offset + size <= current->size; ++offset) {
    if ((!offset || current->bytes[offset - 1] == '\n') &&
        !memcmp(current->bytes + offset, text, size)) {
      if (found) {
        return false;
      }
      size_t end = offset + size;
      while (end < current->size && current->bytes[end] == '\n') {
        ++end;
      }
      if (end < current->size && current->bytes[end] != '/') {
        return false;
      }
      *span = (struct literal_span){offset, offset + size};
      found = true;
    }
  }
  return found;
}

bool install_boot_entry_plan(const struct config_blob *current,
    const struct install_boot_entry_paths *previous, const uint8_t disk_guid[16],
    const struct install_boot_entry_paths *next, bool legacy_pair_verified,
    struct config_blob *selected, bool *adoption, bool *pending)
{
  *adoption = false;
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
  const char *ids[] = {"boot.normal", "boot.rescue"};
  const char *names[] = {"Pyxis OS (Caelum)", "Pyxis OS (rescue)"};
  struct literal_span spans[2];
  char *replacement[2] = {0};
  size_t replacement_size[2] = {0};
  bool recognized = true, ok = true;
  for (unsigned i = 0; i < 2 && recognized && ok; ++i) {
    const struct config_owned_span *marked = config_owned_find(owned, ids[i]);
    const char *old_kernel = previous != NULL ? previous->kernel : "boot/caelum.elf";
    const char *old_archive = previous != NULL ? previous->archive : "boot/initrd.cpio";
    size_t expected_size;
    char *expected = entry(names[i], i == 1, old_kernel, old_archive, disk_guid, NULL, &expected_size);
    if (!expected) {
      ok = false;
      break;
    }
    if (marked && previous != NULL) {
      recognized = marked->content.end - marked->content.start == expected_size &&
          !memcmp(current->bytes + marked->content.start, expected, expected_size);
      spans[i] = marked->span;
    } else if (!owned->count && !previous && legacy_pair_verified) {
      recognized = find_exact(current, expected, expected_size, &spans[i]);
      *adoption = recognized;
    } else {
      recognized = false;
    }
    free(expected);
    if (!recognized) {
      break;
    }
    size_t new_size = 0;
    char *new_entry = entry(names[i], i == 1, next->kernel, next->archive, disk_guid, NULL, &new_size);
    replacement[i] = new_entry ? malloc(new_size + 128) : NULL;
    if (!new_entry || !replacement[i]) {
      free(new_entry);
      ok = false;
      break;
    }
    int count = snprintf(replacement[i], new_size + 128,
        "# pyxis:begin %s\n%s# pyxis:end %s\n", ids[i], new_entry, ids[i]);
    free(new_entry);
    ok = count > 0 && (size_t)count < new_size + 128;
    replacement_size[i] = count > 0 ? (size_t)count : 0;
  }
  if (recognized && ok && spans[0].end <= spans[1].start) {
    size_t removed = spans[0].end - spans[0].start + spans[1].end - spans[1].start;
    size_t added = replacement_size[0] + replacement_size[1];
    if (added > CONFIG_FILE_MAX_BYTES || current->size - removed > CONFIG_FILE_MAX_BYTES - added) {
      ok = false;
    } else {
      size_t size = current->size - removed + added, used = 0, from = 0;
      unsigned char *bytes = malloc(size + 1);
      ok = bytes != NULL;
      for (unsigned i = 0; ok && i < 2; ++i) {
        size_t prefix = spans[i].start - from;
        memcpy(bytes + used, current->bytes + from, prefix);
        used += prefix;
        memcpy(bytes + used, replacement[i], replacement_size[i]);
        used += replacement_size[i];
        from = spans[i].end;
      }
      if (ok) {
        memcpy(bytes + used, current->bytes + from, current->size - from);
        ok = config_blob_set(selected, bytes, size);
        *pending = !ok;
      }
      free(bytes);
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    free(replacement[i]);
  }
  config_owned_free(owned);
  return ok;
}

bool install_boot_entry_render_owned(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], struct config_blob *menu)
{
  const char *const ids[] = {"boot.normal", "boot.rescue"};
  const char *const names[] = {"Pyxis OS (Caelum)", "Pyxis OS (rescue)"};
  char *entries[2] = {0};
  size_t sizes[2] = {0};
  bool ok = true;
  for (unsigned i = 0; i < 2; ++i) {
    entries[i] = entry(names[i], i == 1, paths->kernel, paths->archive, disk_guid, NULL, &sizes[i]);
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
    const uint8_t disk_guid[16], const uint8_t esp_guid[16], struct config_blob *pair)
{
  size_t normal_size = 0, rescue_size = 0;
  char *normal = entry("Pyxis OS (Caelum)", false, paths->kernel, paths->archive,
      disk_guid, esp_guid, &normal_size);
  char *rescue = entry("Pyxis OS (rescue)", true, paths->kernel, paths->archive,
      disk_guid, esp_guid, &rescue_size);
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
    const uint8_t disk_guid[16], FILE *output)
{
  const char *names[] = {"Pyxis OS (Caelum)", "Pyxis OS (rescue)"};
  fputs("Manual boot entry proposal (existing menu untouched):\n", output);
  for (unsigned i = 0; i < 2; ++i) {
    size_t size = 0;
    char *text = entry(names[i], i == 1, paths->kernel, paths->archive, disk_guid, NULL, &size);
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

static bool legacy_kernel_valid(handle_t file, uint64_t bytes)
{
  struct install_source source = {.handle = file, .bytes = bytes};
  uint8_t header[64], program[56];
  if (bytes < sizeof(header) || !install_source_read(&source, 0, header, sizeof(header)) ||
      memcmp(header, "\177ELF\2\1\1", 7) || npfs_get_u16(header + 16) != 2 ||
      npfs_get_u16(header + 18) != 62 || npfs_get_u32(header + 20) != 1 ||
      npfs_get_u64(header + 24) < UINT64_C(0xffffffff80000000) ||
      npfs_get_u16(header + 54) != sizeof(program)) {
    return false;
  }
  uint64_t first = npfs_get_u64(header + 32);
  uint16_t count = npfs_get_u16(header + 56);
  if (!count || count > 128 || first > bytes || (uint64_t)count * sizeof(program) > bytes - first) {
    return false;
  }
  bool executable = false;
  for (unsigned i = 0; i < count; ++i) {
    if (!install_source_read(&source, first + i * sizeof(program), program, sizeof(program))) {
      return false;
    }
    if (npfs_get_u32(program) == 1) {
      uint64_t offset = npfs_get_u64(program + 8), size = npfs_get_u64(program + 32);
      if (offset > bytes || size > bytes - offset || size > npfs_get_u64(program + 40)) {
        return false;
      }
      executable |= (npfs_get_u32(program + 4) & 1) != 0;
    }
  }
  return executable;
}

static bool legacy_revision_valid(const struct config_blob *blob)
{
  if (!blob->digest.present || blob->size < 2 || blob->size > 64 || blob->bytes[blob->size - 1] != '\n' ||
      memchr(blob->bytes, 0, blob->size)) {
    return false;
  }
  char revision[65];
  memcpy(revision, blob->bytes, blob->size - 1);
  revision[blob->size - 1] = 0;
  return config_revision_valid(revision) && strcmp(revision, INSTALL_UNKNOWN_REVISION);
}

static bool legacy_source_current(handle_t file, const struct file_info_reply *expected,
    uint64_t size)
{
  struct file_info_reply current;
  uint64_t bytes;
  return file_info(file, &current) == CALL_OK &&
      (current.valid & FILE_INFO_OBJECT_VALID) && (current.valid & FILE_INFO_DOMAIN_VALID) &&
      current.domain == expected->domain && current.object == expected->object &&
      file_size(file, &bytes) == CALL_OK && bytes == size;
}

/* Initial certification has no expected digest yet. Copy only confirmed native
 * extents to private RAM, then run the existing complete hash and semantic
 * checks there. Publication separately rechecks that reviewed digest on FAT. */
static bool legacy_materialize(handle_t file, handle_t ram,
    const struct file_info_reply *identity, uint64_t size)
{
  bool ok = size && size <= INSTALL_ESP_PAYLOAD_MAX_BYTES &&
      legacy_source_current(file, identity, size) && file_resize(ram, 0) == CALL_OK;
  install_progress_begin("Freezing legacy boot payload", INSTALL_PROGRESS_SOURCE_READ, NULL);
  install_progress_bytes_total(size);
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t wanted = size - offset > FILE_TRANSFER_MAX_BYTES ?
        FILE_TRANSFER_MAX_BYTES : (size_t)(size - offset);
    size_t copied = 0;
    enum call_status status = file_transfer(ram, offset, file, offset, wanted, &copied);
    ok = status == CALL_OK && copied && copied <= wanted;
    if (ok) {
      offset += copied;
      install_progress_advance(copied, 0);
    }
  }
  uint64_t bytes;
  ok = ok && legacy_source_current(file, identity, size) &&
      file_size(ram, &bytes) == CALL_OK && bytes == size;
  install_progress_finish();
  return ok;
}

bool install_boot_legacy_certify(handle_t root, handle_t workspace,
    struct config_file_digest payloads[INSTALL_BOOT_LEGACY_PAYLOADS])
{
  if (!payloads) {
    return false;
  }
  memset(payloads, 0, sizeof(*payloads) * INSTALL_BOOT_LEGACY_PAYLOADS);
  const char *names[INSTALL_BOOT_LEGACY_PAYLOADS] = {"caelum.elf", "initrd.cpio", "revision"};
  const uint64_t rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES;
  handle_t parent = HANDLE_INVALID, ram = HANDLE_INVALID;
  handle_t files[INSTALL_BOOT_LEGACY_PAYLOADS] = {HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID};
  struct config_file_digest digests[INSTALL_BOOT_LEGACY_PAYLOADS] = {0};
  struct config_blob revision = {0};
  uint64_t sizes[INSTALL_BOOT_LEGACY_PAYLOADS] = {0};
  struct file_info_reply identities[INSTALL_BOOT_LEGACY_PAYLOADS] = {0}, root_info;
  bool eligible = directory_info(root, &root_info) == CALL_OK &&
      (root_info.valid & FILE_INFO_DOMAIN_VALID) && root_info.domain &&
      directory_lookup(root, "boot", DIRECTORY_KIND_DIRECTORY, rights, &parent) == CALL_OK;
  for (unsigned i = 0; eligible && i < INSTALL_BOOT_LEGACY_PAYLOADS; ++i) {
    struct handle_info grant;
    eligible = directory_lookup(parent, names[i], DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &files[i]) == CALL_OK &&
        handle_query(files[i], &grant) == CALL_OK && grant.kind == HANDLE_KIND_NATIVE && !grant.transport &&
        grant.protocol == PROTOCOL_FILE && (grant.rights & FILE_RIGHT_READ) &&
        file_info(files[i], &identities[i]) == CALL_OK &&
        (identities[i].valid & FILE_INFO_OBJECT_VALID) && (identities[i].valid & FILE_INFO_DOMAIN_VALID) &&
        identities[i].domain == root_info.domain &&
        file_size(files[i], &sizes[i]) == CALL_OK && sizes[i] && sizes[i] <= INSTALL_ESP_PAYLOAD_MAX_BYTES;
  }
  if (eligible) {
    eligible = sizes[2] <= 64 && install_esp_transfer_workspace(workspace, &ram) == CALL_OK;
  }
  if (eligible) {
    unsigned char bytes[64];
    uint64_t final_size;
    struct install_source source = {.handle = ram, .bytes = sizes[2]};
    eligible = legacy_materialize(files[2], ram, &identities[2], sizes[2]) &&
        install_source_read(&source, 0, bytes, (size_t)sizes[2]) &&
        file_size(ram, &final_size) == CALL_OK && final_size == sizes[2] &&
        config_blob_set(&revision, bytes, (size_t)sizes[2]) && legacy_revision_valid(&revision);
  }
  for (unsigned i = 0; eligible && i < 2; ++i) {
    struct install_source source = {.handle = ram, .bytes = sizes[i]};
    eligible = legacy_materialize(files[i], ram, &identities[i], sizes[i]) &&
        (i == 0 ? legacy_kernel_valid(ram, sizes[i]) : install_archive_rescue_identity(&source)) &&
        install_boot_payload_digest(&source, &digests[i]);
  }
  for (unsigned i = 0; eligible && i < INSTALL_BOOT_LEGACY_PAYLOADS; ++i) {
    eligible = legacy_source_current(files[i], &identities[i], sizes[i]);
  }
  if (ram != HANDLE_INVALID && handle_close(ram) != CALL_OK) {
    eligible = false;
  }
  if (parent != HANDLE_INVALID && handle_close(parent) != CALL_OK) {
    eligible = false;
  }
  for (unsigned i = 0; i < INSTALL_BOOT_LEGACY_PAYLOADS; ++i) {
    if (files[i] != HANDLE_INVALID && handle_close(files[i]) != CALL_OK) {
      eligible = false;
    }
  }
  if (eligible) {
    digests[2] = revision.digest;
    memcpy(payloads, digests, sizeof(digests));
  }
  config_blob_destroy(&revision);
  return eligible;
}
