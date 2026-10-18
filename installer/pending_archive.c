#include "pending_archive.h"
#include "../config-editor/readiness.h"
#include "../config-editor/catalog.h"

#include <stdlib.h>
#include <string.h>

#define NEWC_HEADER_BYTES 110u
#define NEWC_TYPE_MASK 0170000u
#define NEWC_REGULAR 0100000u
#define NEWC_DIRECTORY 0040000u

static bool digest_equal(const struct config_file_digest *left,
    const struct config_file_digest *right)
{
  return left->present == right->present && left->size == right->size &&
      !strcmp(left->sha256, right->sha256);
}

static bool newc_field(const unsigned char *header, unsigned index, uint32_t *value)
{
  *value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    unsigned char ch = header[6 + index * 8 + i];
    unsigned digit = ch >= '0' && ch <= '9' ? ch - '0' :
        ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 16;
    if (digit == 16) {
      return false;
    }
    *value = *value << 4 | digit;
  }
  return true;
}

/* Match the native newc bounds/type rules while retaining only four bounded
 * config files. Duplicate normalized names refuse instead of selecting one. */
bool install_pending_archive_configs(const struct install_source *source,
    struct config_blob files[CONFIG_SET_FILES])
{
  static const char *const names[] = {
    "config/installed.lua", "config/session.lua", "config/network.lua", "config/catalog.lua"
  };
  uint64_t offset = 0;
  while (offset < source->bytes) {
    unsigned char header[NEWC_HEADER_BYTES];
    uint32_t size, name_size, mode, links, checksum;
    if (source->bytes - offset < sizeof(header) ||
        !install_source_read(source, offset, header, sizeof(header)) || memcmp(header, "070701", 6) ||
        !newc_field(header, 1, &mode) || !newc_field(header, 4, &links) ||
        !newc_field(header, 6, &size) || !newc_field(header, 11, &name_size) ||
        !newc_field(header, 12, &checksum) || checksum || name_size < 2 ||
        name_size > CONFIG_FILE_MAX_BYTES) {
      return false;
    }
    offset += sizeof(header);
    if (name_size > source->bytes - offset) {
      return false;
    }
    char *name = malloc(name_size);
    if (!name) {
      return false;
    }
    bool ok = install_source_read(source, offset, name, name_size);
    size_t length = ok ? strnlen(name, name_size) : 0;
    ok = ok && length && length < name_size;
    for (size_t i = length; ok && i < name_size; ++i) {
      ok = !name[i];
    }
    uint64_t data = (offset + name_size + 3) & ~UINT64_C(3);
    uint64_t next = (data + size + 3) & ~UINT64_C(3);
    ok = ok && data <= source->bytes && size <= source->bytes - data && next <= source->bytes;
    bool trailer = ok && !strcmp(name, "TRAILER!!!");
    if (trailer) {
      ok = !size;
    } else {
      ok = ok && links && (((mode & NEWC_TYPE_MASK) == NEWC_DIRECTORY && !size) ||
          ((mode & NEWC_TYPE_MASK) == NEWC_REGULAR && links == 1));
    }
    const char *normalized = name;
    while (ok && !strncmp(normalized, "./", 2)) {
      normalized += 2;
    }
    for (unsigned i = 0; ok && !trailer && i < CONFIG_SET_FILES; ++i) {
      if (strcmp(normalized, names[i])) {
        continue;
      }
      ok = !files[i].digest.present && size && size <= CONFIG_FILE_MAX_BYTES &&
          (mode & NEWC_TYPE_MASK) == NEWC_REGULAR;
      unsigned char *bytes = ok ? malloc(size) : NULL;
      ok = ok && bytes && install_source_read(source, data, bytes, size) &&
          config_blob_set(&files[i], bytes, size);
      free(bytes);
    }
    free(name);
    if (!ok) {
      return false;
    }
    offset = next;
    if (trailer) {
      unsigned char padding[4096];
      while (offset < source->bytes) {
        size_t count = source->bytes - offset > sizeof(padding) ?
            sizeof(padding) : (size_t)(source->bytes - offset);
        if (!install_source_read(source, offset, padding, count)) {
          return false;
        }
        for (size_t i = 0; i < count; ++i) {
          if (padding[i]) {
            return false;
          }
        }
        offset += count;
      }
      for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) {
        if (!files[i].digest.present) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

bool install_pending_selection(const struct install_pending_request *request,
    const struct install_pending_plan *plan, const struct install_esp_payloads *payloads)
{
  struct install_source source = {0};
  struct config_blob extracted[CONFIG_SET_FILES] = {0};
  bool ok = install_esp_payload_source(payloads, INSTALL_ESP_ARCHIVE, &source) &&
      install_pending_archive_configs(&source, extracted) &&
      digest_equal(&extracted[CONFIG_CATALOG].digest, &plan->record.boot.archive_catalog);
  struct config_catalog *declared = NULL;
  ok = ok && config_catalog_read("pending boot archive catalog",
      (char *)extracted[CONFIG_CATALOG].bytes, extracted[CONFIG_CATALOG].size, &declared) &&
      !strcmp(declared->revision, plan->record.revision);
  config_catalog_free(declared);
  struct config_bytes archive[CONFIG_LUA_COUNT], esp[CONFIG_LUA_COUNT];
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    archive[i] = (struct config_bytes){(char *)extracted[i].bytes, extracted[i].size};
    esp[i] = (struct config_bytes){(char *)request->current[i].bytes, request->current[i].size};
  }
  struct config_bytes catalog = {(char *)extracted[CONFIG_CATALOG].bytes,
    extracted[CONFIG_CATALOG].size};
  struct config_bytes esp_catalog = {(char *)request->current[CONFIG_CATALOG].bytes,
    request->current[CONFIG_CATALOG].size};
  struct config_bytes record = {(char *)request->record->bytes, request->record->size};
  struct config_selection selected = {0};
  const char *reason = NULL;
  if (ok) {
    const struct config_boot_binding *binding = &plan->record.boot;
    ok = config_readiness_choose(archive, &catalog, binding->kernel_revision,
        binding->program_revision, &binding->kernel.digest, &binding->archive.digest,
        &record, esp, &esp_catalog, true, false, &selected, &reason) &&
        selected.from_esp == (binding->source == CONFIG_ACTIVE_ESP);
  }
  config_selection_free(&selected);
  for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) {
    config_blob_destroy(&extracted[i]);
  }
  return ok;
}

