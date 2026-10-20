#include "shell.h"
#include <environment.h>
#include <file.h>
#include <literal.h>
#include <shell_settings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The settings file is strict literal data that never reaches the evaluator,
 * so a writer of it gains no shell authority: only TEXT values can change. */
#define SETTINGS_FILE_MAX (64 * 1024)
#define SETTINGS_MAX 64
#define SETTINGS_TEXT_MAX 4096

struct setting {
  char name[ENVIRONMENT_NAME_MAX + 1];
  const struct literal_value *value;
  bool exported;
};

/* Names the shell's own startup or launch path supplies. */
static const char *const supplied_names[] = {
  "home", "commands", "DNS_SERVER", "stdin.append", "stdout.append", "stderr.append",
};

static bool shell_supplied(const char *name)
{
  for (size_t i = 0; i < sizeof(supplied_names) / sizeof(*supplied_names); ++i) {
    if (!strcmp(name, supplied_names[i])) {
      return true;
    }
  }
  return !strncmp(name, "network.", strlen("network."));
}

static enum call_status read_settings(handle_t file, char **bytes, size_t *size)
{
  *bytes = NULL;
  *size = 0;
  uint64_t length;
  enum call_status status = file_size(file, &length);
  if (status != CALL_OK) {
    return status;
  }
  if (length > SETTINGS_FILE_MAX) {
    return CALL_FILE_TOO_LARGE;
  }
  char *buffer = malloc(length ? (size_t)length : 1);
  if (!buffer) {
    return CALL_NO_MEMORY;
  }
  size_t used = 0;
  while (status == CALL_OK && used < length) {
    size_t count = 0;
    status = file_read(file, used, buffer + used, (size_t)length - used, &count);
    if (status == CALL_OK && !count) {
      status = CALL_IO;
    }
    used += count;
  }
  if (status != CALL_OK) {
    free(buffer);
    return status;
  }
  *bytes = buffer;
  *size = used;
  return CALL_OK;
}

static const char *collect(const struct literal_value *table, bool exported,
    struct setting *settings, size_t *count)
{
  if (table->kind != LITERAL_TABLE) {
    return "variables and exported must be tables";
  }
  for (const struct literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    const struct literal_value *key = entry->key, *value = entry->value;
    if (entry->implicit || key->kind != LITERAL_STRING) {
      return "each setting needs a name key";
    }
    if (*count == SETTINGS_MAX) {
      return "more than 64 settings";
    }
    struct setting *setting = &settings[*count];
    if (strlen(key->as.string.bytes) != key->as.string.size ||
        !environment_name(setting->name, key->as.string.bytes)) {
      return "a setting name is not a valid variable name";
    }
    if (value->kind != LITERAL_STRING) {
      return "setting values must be strings";
    }
    if (strlen(value->as.string.bytes) != value->as.string.size ||
        value->as.string.size > SETTINGS_TEXT_MAX) {
      return "a setting value contains NUL or exceeds 4096 bytes";
    }
    for (size_t i = 0; i < *count; ++i) {
      if (!strcmp(settings[i].name, setting->name)) {
        return "a name appears in both variables and exported";
      }
    }
    setting->value = value;
    setting->exported = exported;
    ++*count;
  }
  return NULL;
}

static const char *validate(const struct literal *document, struct setting *settings,
    size_t *count)
{
  const struct literal_value *root = document->root;
  if (root->kind != LITERAL_TABLE) {
    return "the file must return a table";
  }
  for (const struct literal_entry *entry = root->as.table.entries; entry; entry = entry->next) {
    if (entry->implicit || entry->key->kind != LITERAL_STRING ||
        (strcmp(entry->key->as.string.bytes, "variables") &&
         strcmp(entry->key->as.string.bytes, "exported"))) {
      return "only variables and exported tables are allowed";
    }
  }
  const struct literal_value *variables = literal_field(root, "variables");
  const struct literal_value *exported = literal_field(root, "exported");
  const char *reason = variables ? collect(variables, false, settings, count) : NULL;
  if (!reason && exported) {
    reason = collect(exported, true, settings, count);
  }
  return reason;
}

/* Every check precedes one SET batch, so a refused file applies nothing. */
static void apply(struct shell *shell, const struct setting *settings, size_t count)
{
  struct environment_capture current = {0};
  enum call_status status = environment_capture_init(shell->environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &current);
  if (status != CALL_OK) {
    fprintf(stderr, "shell: %s: cannot read the environment (status %u); none applied\n",
        SHELL_SETTINGS_PATH, status);
    return;
  }
  struct environment_set_entry entries[SETTINGS_MAX];
  for (size_t i = 0; i < count; ++i) {
    const struct environment_value *held = environment_capture_find(&current, settings[i].name);
    if (shell_supplied(settings[i].name) || (held && held->kind == ENVIRONMENT_CAP)) {
      fprintf(stderr, "shell: %s: %s %s; none applied\n", SHELL_SETTINGS_PATH,
          settings[i].name, held && held->kind == ENVIRONMENT_CAP ?
          "holds a capability" : "is supplied by the shell");
      environment_capture_close(&current);
      return;
    }
    entries[i] = (struct environment_set_entry){
      .kind = ENVIRONMENT_TEXT,
      .flags = settings[i].exported ? ENVIRONMENT_EXPORTED : 0,
      .text_address = (uintptr_t)settings[i].value->as.string.bytes,
      .text_length = settings[i].value->as.string.size,
    };
    memcpy(entries[i].name, settings[i].name, sizeof(entries[i].name));
  }
  environment_capture_close(&current);
  status = count ? environment_set_batch(shell->environment, entries, count) : CALL_OK;
  if (status != CALL_OK) {
    fprintf(stderr, "shell: %s: cannot apply settings (status %u); none applied\n",
        SHELL_SETTINGS_PATH, status);
  }
}

void shell_settings_load(struct shell *shell, handle_t file)
{
  char *bytes;
  size_t size;
  enum call_status status = read_settings(file, &bytes, &size);
  handle_close(file);
  if (status != CALL_OK) {
    fprintf(stderr, "shell: %s: cannot read (status %u)%s; none applied\n", SHELL_SETTINGS_PATH,
        status, status == CALL_FILE_TOO_LARGE ? ", larger than 64 KiB" : "");
    return;
  }
  struct literal *document;
  struct literal_diagnostic diagnostic;
  if (!literal_parse(bytes, size, &document, &diagnostic)) {
    fprintf(stderr, "shell: %s:%zu:%zu: %s; none applied\n", SHELL_SETTINGS_PATH,
        diagnostic.line, diagnostic.column, diagnostic.reason);
    free(bytes);
    return;
  }
  free(bytes);
  struct setting settings[SETTINGS_MAX];
  size_t count = 0;
  const char *reason = validate(document, settings, &count);
  if (reason) {
    fprintf(stderr, "shell: %s: %s; none applied\n", SHELL_SETTINGS_PATH, reason);
  } else {
    apply(shell, settings, count);
  }
  literal_free(document);
}
