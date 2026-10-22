#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return config_payload_digest_valid(digest) &&
      (!digest->present || digest->size <= CONFIG_FILE_MAX_BYTES);
}

bool config_payload_digest_valid(const struct config_file_digest *digest)
{
  return digest && (digest->present ? hex_valid(digest->sha256, 64) :
      !digest->size && !digest->sha256[0]);
}

bool config_record_artifact_name(const char *transaction, const char *file,
    const char *suffix, char name[CONFIG_ARTIFACT_NAME_BYTES])
{
  if (!name || !config_transaction_valid(transaction) || !file ||
      (strcmp(file, CONFIG_MENU_NAME) && strcmp(file, CONFIG_RECORD_NAME)) || !suffix || (strcmp(suffix, "new") && strcmp(suffix, "previous") &&
      strcmp(suffix, "prepared") && strcmp(suffix, "committed"))) {
    return false;
  }
  int count = snprintf(name, CONFIG_ARTIFACT_NAME_BYTES, ".pyxis-config-%s-%s.%s",
      transaction, file, suffix);
  return count > 0 && count < (int)CONFIG_ARTIFACT_NAME_BYTES;
}

/* A previous copy of the menu, named for the transaction that made it. */
static bool recovery_valid(const struct config_recovery_file *file)
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
  return config_record_artifact_name(id, CONFIG_MENU_NAME, "previous", expected) &&
      !strcmp(file->name, expected);
}

bool config_revision_valid(const char *revision)
{
  if (!revision) {
    return false;
  }
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

bool config_boot_payload_path(const char *revision, const char *attempt,
    const char *name, char path[CONFIG_PAYLOAD_PATH_BYTES])
{
  if (!path || !config_revision_valid(revision) || !config_transaction_valid(attempt) ||
      !name || (strcmp(name, "kernel.elf") && strcmp(name, "boot.arc") &&
      strcmp(name, "revision.txt") && strcmp(name, "ownership.record"))) {
    return false;
  }
  int count = snprintf(path, CONFIG_PAYLOAD_PATH_BYTES, "EFI/pyxis/%s/%s/%s",
      revision, attempt, name);
  return count > 0 && count < (int)CONFIG_PAYLOAD_PATH_BYTES;
}

static bool payload_valid(const struct config_boot_payload *payload,
    const struct config_boot_binding *binding, const char *name)
{
  char expected[CONFIG_PAYLOAD_PATH_BYTES];
  return payload->digest.present && payload->digest.size &&
      config_payload_digest_valid(&payload->digest) &&
      strnlen(payload->path, sizeof(payload->path)) < sizeof(payload->path) &&
      config_boot_payload_path(binding->kernel_revision, binding->attempt, name, expected) &&
      !strcmp(payload->path, expected);
}

bool config_boot_binding_valid(const struct config_boot_binding *binding)
{
  if (!binding) {
    return false;
  }
  if (!binding->present) {
    return !binding->kernel_revision[0] && !binding->program_revision[0] &&
        !binding->attempt[0] && !binding->kernel.path[0] && !binding->archive.path[0] &&
        !binding->revision.path[0] &&
        !binding->kernel.digest.present && config_payload_digest_valid(&binding->kernel.digest) &&
        !binding->archive.digest.present && config_payload_digest_valid(&binding->archive.digest) &&
        !binding->revision.digest.present && config_payload_digest_valid(&binding->revision.digest);
  }
  return config_revision_valid(binding->kernel_revision) &&
      config_revision_valid(binding->program_revision) &&
      config_transaction_valid(binding->attempt) &&
      payload_valid(&binding->kernel, binding, "kernel.elf") &&
      payload_valid(&binding->archive, binding, "boot.arc") &&
      payload_valid(&binding->revision, binding, "revision.txt");
}

bool config_record_valid(const struct config_record *record)
{
  return record && config_transaction_valid(record->transaction) &&
      record->boot.present && config_boot_binding_valid(&record->boot) &&
      config_boot_binding_valid(&record->previous_boot) &&
      !(record->previous_boot.present &&
          !strcmp(record->previous_boot.kernel.path, record->boot.kernel.path)) &&
      recovery_valid(&record->previous_menu) && recovery_valid(&record->obsolete_menu) &&
      !(record->previous_menu.digest.present && record->obsolete_menu.digest.present &&
          !strcmp(record->previous_menu.name, record->obsolete_menu.name));
}

static bool render_binding(const char *label, const struct config_boot_binding *binding,
    char *text, size_t *used)
{
  if (*used > CONFIG_RECORD_MAX_BYTES) {
    return false;
  }
  int count;
  if (!binding->present) {
    count = snprintf(text + *used, CONFIG_RECORD_MAX_BYTES + 1 - *used,
        "%s unbound\n", label);
  } else {
    count = snprintf(text + *used, CONFIG_RECORD_MAX_BYTES + 1 - *used,
        "%s bound %s %s %s\n"
        "payload kernel %s %llu %s\n"
        "payload archive %s %llu %s\n"
        "payload revision %s %llu %s\n",
        label, binding->kernel_revision, binding->program_revision, binding->attempt,
        binding->kernel.path, (unsigned long long)binding->kernel.digest.size,
        binding->kernel.digest.sha256,
        binding->archive.path, (unsigned long long)binding->archive.digest.size,
        binding->archive.digest.sha256,
        binding->revision.path, (unsigned long long)binding->revision.digest.size,
        binding->revision.digest.sha256);
  }
  if (count <= 0 || (size_t)count > CONFIG_RECORD_MAX_BYTES - *used) {
    return false;
  }
  *used += (size_t)count;
  return true;
}

bool config_record_render(const struct config_record *record, char **bytes, size_t *size)
{
  if (!bytes || !size) {
    return false;
  }
  *bytes = NULL;
  *size = 0;
  if (!config_record_valid(record)) {
    return false;
  }
  char *text = malloc(CONFIG_RECORD_MAX_BYTES + 1);
  if (!text) {
    return false;
  }
  int count = snprintf(text, CONFIG_RECORD_MAX_BYTES + 1,
      "PYXIS-CONFIG 1\nstate %s\ntransaction %s\n",
      record->committed ? "committed" : "prepared", record->transaction);
  size_t used = count > 0 ? (size_t)count : CONFIG_RECORD_MAX_BYTES + 1;
  if (!render_binding("boot", &record->boot, text, &used) ||
      !render_binding("previous-boot", &record->previous_boot, text, &used)) {
    free(text);
    return false;
  }
  for (unsigned group = 0; group < 2 && used <= CONFIG_RECORD_MAX_BYTES; ++group) {
    const struct config_recovery_file *file = group ? &record->obsolete_menu :
        &record->previous_menu;
    bool present = file->digest.present;
    count = snprintf(text + used, CONFIG_RECORD_MAX_BYTES + 1 - used,
        "%s " CONFIG_MENU_NAME " %s %llu %s\n", group ? "obsolete" : "previous",
        present ? file->name : "-", (unsigned long long)file->digest.size,
        present ? file->digest.sha256 : "-");
    used += count > 0 ? (size_t)count : CONFIG_RECORD_MAX_BYTES + 1;
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

static bool parse_digest(char **cursor, struct config_file_digest *digest)
{
  char *length = token(cursor, ' ');
  char *hash = length ? token(cursor, '\n') : NULL;
  if (!decimal(length, &digest->size) || !hash) {
    return false;
  }
  if (!strcmp(hash, "-")) {
    return !digest->size;
  }
  if (!hex_valid(hash, 64)) {
    return false;
  }
  digest->present = true;
  strcpy(digest->sha256, hash);
  return true;
}

static bool parse_payload(char **cursor, const char *kind, struct config_boot_payload *payload)
{
  if (!word(cursor, "payload", ' ') || !word(cursor, kind, ' ')) {
    return false;
  }
  char *path = token(cursor, ' ');
  if (!path || strlen(path) >= sizeof(payload->path)) {
    return false;
  }
  strcpy(payload->path, path);
  return parse_digest(cursor, &payload->digest);
}

static bool parse_binding(char **cursor, const char *label, struct config_boot_binding *binding)
{
  if (!word(cursor, label, ' ')) {
    return false;
  }
  if (!strncmp(*cursor, "unbound\n", 8)) {
    *cursor += 8;
    return true;
  }
  if (!word(cursor, "bound", ' ')) {
    return false;
  }
  char *kernel = token(cursor, ' ');
  char *program = kernel ? token(cursor, ' ') : NULL;
  char *attempt = program ? token(cursor, '\n') : NULL;
  if (!config_revision_valid(kernel) || !config_revision_valid(program) ||
      !config_transaction_valid(attempt)) {
    return false;
  }
  binding->present = true;
  strcpy(binding->kernel_revision, kernel);
  strcpy(binding->program_revision, program);
  strcpy(binding->attempt, attempt);
  return parse_payload(cursor, "kernel", &binding->kernel) &&
      parse_payload(cursor, "archive", &binding->archive) &&
      parse_payload(cursor, "revision", &binding->revision);
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
  ok = ok && parse_binding(&cursor, "boot", &parsed.boot) &&
      parse_binding(&cursor, "previous-boot", &parsed.previous_boot);
  for (unsigned group = 0; ok && group < 2; ++group) {
    struct config_recovery_file *file = group ? &parsed.obsolete_menu : &parsed.previous_menu;
    ok = word(&cursor, group ? "obsolete" : "previous", ' ') &&
        word(&cursor, CONFIG_MENU_NAME, ' ');
    char *name = ok ? token(&cursor, ' ') : NULL;
    char *length = name ? token(&cursor, ' ') : NULL;
    char *hash = length ? token(&cursor, '\n') : NULL;
    ok = name && strlen(name) < sizeof(file->name) &&
        decimal(length, &file->digest.size) && hash;
    if (ok && strcmp(name, "-")) {
      ok = hex_valid(hash, 64);
      if (ok) {
        strcpy(file->name, name);
        file->digest.present = true;
        strcpy(file->digest.sha256, hash);
      }
    } else if (ok) {
      ok = !strcmp(hash, "-") && !file->digest.size;
    }
  }
  ok = ok && !*cursor && config_record_valid(&parsed);
  free(text);
  if (ok) {
    *record = parsed;
  }
  return ok;
}
