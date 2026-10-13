#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const file_names[CONFIG_ALL_FILES] = {
  "boot.lua", "session.lua", "network.lua", "catalog.lua", "limine.conf"
};

const char *config_storage_file_name(enum config_storage_file file)
{
  return (unsigned)file < CONFIG_ALL_FILES ? file_names[file] : NULL;
}

static bool hex_valid(const char *text, size_t count)
{
  if (!text || strnlen(text, count + 1) != count) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') ||
        (text[i] >= 'a' && text[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool config_transaction_valid(const char *transaction)
{
  return hex_valid(transaction, 32);
}

bool config_digest_valid(const struct config_file_digest *digest)
{
  return digest && (digest->present ? digest->size <= CONFIG_FILE_MAX_BYTES &&
      hex_valid(digest->sha256, 64) : !digest->size && !digest->sha256[0]);
}

bool config_record_artifact_name(const char *transaction, enum config_storage_file file,
    const char *suffix, char name[CONFIG_ARTIFACT_NAME_BYTES])
{
  if (!name || !config_transaction_valid(transaction) || !config_storage_file_name(file) ||
      !suffix || (strcmp(suffix, "new") && strcmp(suffix, "previous") &&
      strcmp(suffix, "prepared") && strcmp(suffix, "committed"))) {
    return false;
  }
  int count = snprintf(name, CONFIG_ARTIFACT_NAME_BYTES, ".pyxis-config-%s-%s.%s",
      transaction, config_storage_file_name(file), suffix);
  return count > 0 && count < (int)CONFIG_ARTIFACT_NAME_BYTES;
}

static bool recovery_valid(const struct config_recovery_file *file,
    enum config_storage_file slot, const char *transaction)
{
  if (!config_digest_valid(&file->digest)) {
    return false;
  }
  if (!file->digest.present) {
    return !file->name[0];
  }
  const char prefix[] = ".pyxis-config-";
  if (strnlen(file->name, sizeof(file->name)) == sizeof(file->name) ||
      strncmp(file->name, prefix, sizeof(prefix) - 1)) {
    return false;
  }
  char id[33], expected[CONFIG_ARTIFACT_NAME_BYTES];
  if (strlen(file->name) < sizeof(prefix) - 1 + 32) {
    return false;
  }
  memcpy(id, file->name + sizeof(prefix) - 1, 32);
  id[32] = '\0';
  return (!transaction || !strcmp(id, transaction)) &&
      config_record_artifact_name(id, slot, "previous", expected) &&
      !strcmp(file->name, expected);
}

static bool revision_valid(const char *revision)
{
  size_t count = strnlen(revision, 65);
  if (!count || count > 63 || !strcmp(revision, ".") || !strcmp(revision, "..")) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    unsigned char ch = (unsigned char)revision[i];
    if (ch < 33 || ch > 126 || ch == '/') {
      return false;
    }
  }
  return true;
}

static bool record_valid(const struct config_record *record)
{
  if (!record || !config_transaction_valid(record->transaction) ||
      !revision_valid(record->revision)) {
    return false;
  }
  for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) {
    if (!record->selected[i].present || !config_digest_valid(&record->selected[i])) {
      return false;
    }
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!recovery_valid(&record->previous[i], i,
        i == CONFIG_LIMINE ? NULL : record->transaction) ||
        !recovery_valid(&record->obsolete[i], i, NULL) ||
        (record->previous[i].digest.present && record->obsolete[i].digest.present &&
        !strcmp(record->previous[i].name, record->obsolete[i].name))) {
      return false;
    }
  }
  return true;
}

bool config_record_render(const struct config_record *record, char **bytes, size_t *size)
{
  if (!bytes || !size) {
    return false;
  }
  *bytes = NULL;
  *size = 0;
  if (!record_valid(record)) {
    return false;
  }
  char *text = malloc(CONFIG_RECORD_MAX_BYTES + 1);
  if (!text) {
    return false;
  }
  int count = snprintf(text, CONFIG_RECORD_MAX_BYTES + 1,
      "PYXIS-CONFIG 1\nstate %s\ntransaction %s\nrevision %s\n",
      record->committed ? "committed" : "prepared", record->transaction, record->revision);
  size_t used = count > 0 ? (size_t)count : CONFIG_RECORD_MAX_BYTES + 1;
  for (unsigned i = 0; i < CONFIG_SET_FILES && used <= CONFIG_RECORD_MAX_BYTES; ++i) {
    count = snprintf(text + used, CONFIG_RECORD_MAX_BYTES + 1 - used,
        "selected %s %llu %s\n", file_names[i],
        (unsigned long long)record->selected[i].size, record->selected[i].sha256);
    used += count > 0 ? (size_t)count : CONFIG_RECORD_MAX_BYTES + 1;
  }
  for (unsigned group = 0; group < 2; ++group) {
    const struct config_recovery_file *files = group ? record->obsolete : record->previous;
    for (unsigned i = 0; i < CONFIG_ALL_FILES && used <= CONFIG_RECORD_MAX_BYTES; ++i) {
      bool present = files[i].digest.present;
      count = snprintf(text + used, CONFIG_RECORD_MAX_BYTES + 1 - used,
          "%s %s %s %llu %s\n", group ? "obsolete" : "previous", file_names[i],
          present ? files[i].name : "-", (unsigned long long)files[i].digest.size,
          present ? files[i].digest.sha256 : "-");
      used += count > 0 ? (size_t)count : CONFIG_RECORD_MAX_BYTES + 1;
    }
  }
  if (used > CONFIG_RECORD_MAX_BYTES) {
    free(text);
    return false;
  }
  *bytes = text;
  *size = used;
  return true;
}

static char *token(char **cursor, char delimiter)
{
  char *start = *cursor;
  char *end = strchr(start, delimiter);
  if (!end || end == start) {
    return NULL;
  }
  *end = '\0';
  *cursor = end + 1;
  return start;
}

static bool word(char **cursor, const char *expected, char delimiter)
{
  char *found = token(cursor, delimiter);
  return found && !strcmp(found, expected);
}

static bool decimal(const char *text, uint64_t *value)
{
  if (!text || !*text || (text[0] == '0' && text[1])) {
    return false;
  }
  uint64_t result = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || result >
        (UINT64_MAX - (unsigned)(*text - '0')) / 10) {
      return false;
    }
    result = result * 10 + (unsigned)(*text - '0');
  }
  *value = result;
  return true;
}

bool config_record_parse(const void *bytes, size_t size, struct config_record *record)
{
  if (!record) {
    return false;
  }
  *record = (struct config_record){0};
  if (!bytes || !size || size > CONFIG_RECORD_MAX_BYTES || memchr(bytes, '\0', size)) {
    return false;
  }
  char *text = malloc(size + 1);
  if (!text) {
    return false;
  }
  memcpy(text, bytes, size);
  text[size] = '\0';
  char *cursor = text;
  struct config_record parsed = {0};
  bool ok = word(&cursor, "PYXIS-CONFIG 1", '\n') && word(&cursor, "state", ' ');
  char *state = ok ? token(&cursor, '\n') : NULL;
  ok = state && (!strcmp(state, "prepared") || !strcmp(state, "committed"));
  if (ok) {
    parsed.committed = !strcmp(state, "committed");
  }
  ok = ok && word(&cursor, "transaction", ' ');
  char *id = ok ? token(&cursor, '\n') : NULL;
  ok = id && config_transaction_valid(id);
  if (ok) {
    strcpy(parsed.transaction, id);
  }
  ok = ok && word(&cursor, "revision", ' ');
  char *revision = ok ? token(&cursor, '\n') : NULL;
  ok = revision && revision_valid(revision);
  if (ok) {
    strcpy(parsed.revision, revision);
  }
  for (unsigned i = 0; ok && i < CONFIG_SET_FILES; ++i) {
    ok = word(&cursor, "selected", ' ') && word(&cursor, file_names[i], ' ');
    char *length = ok ? token(&cursor, ' ') : NULL;
    char *hash = length ? token(&cursor, '\n') : NULL;
    ok = decimal(length, &parsed.selected[i].size) && hash && hex_valid(hash, 64);
    if (ok) {
      parsed.selected[i].present = true;
      strcpy(parsed.selected[i].sha256, hash);
    }
  }
  for (unsigned group = 0; ok && group < 2; ++group) {
    struct config_recovery_file *files = group ? parsed.obsolete : parsed.previous;
    for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
      ok = word(&cursor, group ? "obsolete" : "previous", ' ') &&
          word(&cursor, file_names[i], ' ');
      char *name = ok ? token(&cursor, ' ') : NULL;
      char *length = name ? token(&cursor, ' ') : NULL;
      char *hash = length ? token(&cursor, '\n') : NULL;
      ok = name && strlen(name) < sizeof(files[i].name) &&
          decimal(length, &files[i].digest.size) && hash;
      if (ok && strcmp(name, "-")) {
        ok = hex_valid(hash, 64);
        if (ok) {
          strcpy(files[i].name, name);
          files[i].digest.present = true;
          strcpy(files[i].digest.sha256, hash);
        }
      } else if (ok) {
        ok = !strcmp(hash, "-") && !files[i].digest.size;
      }
    }
  }
  ok = ok && !*cursor && record_valid(&parsed);
  free(text);
  if (ok) {
    *record = parsed;
  }
  return ok;
}
