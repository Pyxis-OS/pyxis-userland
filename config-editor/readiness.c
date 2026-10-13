#include "readiness.h"
#include "catalog.h"
#include "../installer/digest.h"

#include <string.h>

static bool bytes_match(const struct config_bytes *bytes,
    const struct config_file_digest *expected)
{
  if (!bytes || !bytes->bytes || !bytes->size || bytes->size > CONFIG_FILE_MAX_BYTES ||
      !expected->present || !config_digest_valid(expected) || bytes->size != expected->size) {
    return false;
  }
  struct install_digest digest = {0};
  char hash[65];
  bool ok = install_digest_begin(&digest) &&
      install_digest_update(&digest, bytes->bytes, bytes->size) &&
      install_digest_finish(&digest, hash);
  install_digest_abort(&digest);
  return ok && !strcmp(hash, expected->sha256);
}

static bool compatible(const struct config_record *record,
    const struct config_bytes *archive_catalog, const char *kernel_revision,
    const char *program_revision)
{
  if (!record->committed || !record->boot.present ||
      !config_revision_valid(kernel_revision) || !config_revision_valid(program_revision) ||
      strcmp(kernel_revision, record->boot.kernel_revision) ||
      strcmp(program_revision, record->boot.program_revision) ||
      !bytes_match(archive_catalog, &record->boot.archive_catalog)) {
    return false;
  }
  struct config_catalog *catalog = NULL;
  bool ok = config_catalog_read("trusted archive catalog", archive_catalog->bytes,
      archive_catalog->size, &catalog) && !strcmp(catalog->revision, record->revision);
  config_catalog_free(catalog);
  return ok;
}

bool config_readiness_choose(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes *archive_catalog, const char *kernel_revision,
    const char *program_revision, const struct config_bytes *completion,
    const struct config_bytes esp[CONFIG_LUA_COUNT], const struct config_bytes *esp_catalog,
    bool provenance_valid, bool rescue, struct config_selection *selection,
    const char **reason)
{
  if (!archive || !selection || !reason) {
    return false;
  }
  struct config_record record = {0};
  bool ready = !rescue && provenance_valid && completion &&
      config_record_parse(completion->bytes, completion->size, &record) &&
      compatible(&record, archive_catalog, kernel_revision, program_revision);
  if (ready && record.boot.source == CONFIG_ACTIVE_ARCHIVE) {
    bool ok = config_selection_choose(archive, NULL, false, false, selection, reason);
    *reason = "committed boot-bound archive selection";
    return ok;
  }
  bool complete = ready && record.boot.source == CONFIG_ACTIVE_ESP && esp &&
      bytes_match(esp_catalog, &record.selected[CONFIG_CATALOG]) &&
      record.selected[CONFIG_CATALOG].size == record.boot.archive_catalog.size &&
      !strcmp(record.selected[CONFIG_CATALOG].sha256, record.boot.archive_catalog.sha256);
  for (size_t i = 0; complete && i < CONFIG_LUA_COUNT; ++i) {
    complete = bytes_match(&esp[i], &record.selected[i]);
  }
  bool ok = config_selection_choose(archive, esp, complete, rescue, selection, reason);
  if (!rescue && !complete) {
    *reason = "no compatible committed boot-bound ESP set; using archive defaults";
  }
  return ok;
}
