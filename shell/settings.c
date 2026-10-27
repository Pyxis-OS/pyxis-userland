#include "shell.h"
#include <environment.h>
#include <file.h>
#include <literal.h>
#include <shell_settings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The settings file is strict literal data that never reaches the evaluator.
 * Alias arguments remain literal strings when a command uses them. */
#define SETTINGS_FILE_MAX (64 * 1024)
/* One SET batch applies the whole file, so this stays within the kernel's
 * current batch admission (ENVIRONMENT_MUTATIONS_LIMIT, a tunable policy rather
 * than ABI); a smaller kernel bound refuses the batch and applies nothing. */
#define SETTINGS_MAX 16
#define SETTINGS_TEXT_MAX 4096

struct setting {
  char name[ENVIRONMENT_NAME_MAX + 1];
  const struct literal_value *value;
  bool exported;
};

const struct shell_alias *shell_alias_find(const struct shell *shell, const char *name)
{
  for (size_t i = 0; i < shell->alias_count; ++i) {
    if (!strcmp(shell->aliases[i].name, name)) {
      return &shell->aliases[i];
    }
  }
  return NULL;
}

static void aliases_free(struct shell_alias *aliases, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    free(aliases[i].name);
    for (size_t j = 0; aliases[i].arguments && j < aliases[i].count; ++j) {
      free(aliases[i].arguments[j]);
    }
    free(aliases[i].arguments);
  }
  free(aliases);
}

void shell_aliases_close(struct shell *shell)
{
  aliases_free(shell->aliases, shell->alias_count);
  shell->aliases = NULL;
  shell->alias_count = 0;
}

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
      return "more than 16 settings";
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

static bool alias_name(const struct literal_value *key)
{
  if (key->kind != LITERAL_STRING || !key->as.string.size) {
    return false;
  }
  for (size_t i = 0; i < key->as.string.size; ++i) {
    char byte = key->as.string.bytes[i];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == '+' || byte == '-')) {
      return false;
    }
  }
  return true;
}

static const char *collect_aliases(const struct literal_value *table,
    struct shell_alias **aliases, size_t *count)
{
  if (table->kind != LITERAL_TABLE) {
    return "aliases must be a table";
  }
  if (!table->as.table.count) {
    return NULL;
  }
  *aliases = calloc(table->as.table.count, sizeof(**aliases));
  if (!*aliases) {
    return "cannot allocate aliases";
  }
  for (const struct literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    if (entry->implicit || !alias_name(entry->key)) {
      return "an alias name must be a bare command name";
    }
    for (size_t i = 0; i < *count; ++i) {
      if (!strcmp((*aliases)[i].name, entry->key->as.string.bytes)) {
        return "an alias name appears more than once";
      }
    }
    const struct literal_value *value = entry->value;
    if (value->kind != LITERAL_TABLE || !value->as.table.count) {
      return "an alias must be a nonempty string array";
    }
    struct shell_alias *alias = &(*aliases)[(*count)++];
    alias->name = strdup(entry->key->as.string.bytes);
    alias->count = value->as.table.count;
    alias->arguments = calloc(alias->count, sizeof(*alias->arguments));
    if (!alias->name || !alias->arguments) {
      return "cannot allocate alias arguments";
    }
    for (const struct literal_entry *argument = value->as.table.entries; argument;
        argument = argument->next) {
      const struct literal_value *text = argument->value;
      if (argument->key->kind != LITERAL_INTEGER || argument->key->as.integer < 1 ||
          (uint64_t)argument->key->as.integer > alias->count ||
          text->kind != LITERAL_STRING) {
        return "an alias must be a contiguous string array";
      }
      if (strlen(text->as.string.bytes) != text->as.string.size ||
          text->as.string.size > SETTINGS_TEXT_MAX) {
        return "an alias argument contains NUL or exceeds 4096 bytes";
      }
      size_t index = (size_t)argument->key->as.integer - 1;
      if (!index && !text->as.string.size) {
        return "an alias command must not be empty";
      }
      alias->arguments[index] = strdup(text->as.string.bytes);
      if (!alias->arguments[index]) {
        return "cannot allocate alias arguments";
      }
    }
  }
  return NULL;
}

static const char *validate(const struct literal *document, struct setting *settings,
    size_t *count, struct shell_alias **aliases, size_t *alias_count)
{
  const struct literal_value *root = document->root;
  if (root->kind != LITERAL_TABLE) {
    return "the file must return a table";
  }
  for (const struct literal_entry *entry = root->as.table.entries; entry; entry = entry->next) {
    const struct literal_value *key = entry->key;
    if (entry->implicit || key->kind != LITERAL_STRING ||
        strlen(key->as.string.bytes) != key->as.string.size ||
        (strcmp(key->as.string.bytes, "variables") &&
         strcmp(key->as.string.bytes, "exported") &&
         strcmp(key->as.string.bytes, "aliases"))) {
      return "only variables, exported and aliases tables are allowed";
    }
  }
  const struct literal_value *variables = literal_field(root, "variables");
  const struct literal_value *exported = literal_field(root, "exported");
  const struct literal_value *alias_table = literal_field(root, "aliases");
  const char *reason = variables ? collect(variables, false, settings, count) : NULL;
  if (!reason && exported) {
    reason = collect(exported, true, settings, count);
  }
  if (!reason && alias_table) {
    reason = collect_aliases(alias_table, aliases, alias_count);
  }
  return reason;
}

/* Every check precedes one SET batch, so a refused file applies nothing. */
static bool apply(struct shell *shell, const struct setting *settings, size_t count)
{
  struct environment_capture current = {0};
  enum call_status status = environment_capture_init(shell->environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &current);
  if (status != CALL_OK) {
    fprintf(stderr, "shell: %s: cannot read the environment (status %u); none applied\n",
        SHELL_SETTINGS_PATH, status);
    return false;
  }
  struct environment_set_entry entries[SETTINGS_MAX];
  for (size_t i = 0; i < count; ++i) {
    const struct environment_value *held = environment_capture_find(&current, settings[i].name);
    if (shell_supplied(settings[i].name) || (held && held->kind == ENVIRONMENT_CAP)) {
      fprintf(stderr, "shell: %s: %s %s; none applied\n", SHELL_SETTINGS_PATH,
          settings[i].name, held && held->kind == ENVIRONMENT_CAP ?
          "holds a capability" : "is supplied by the shell");
      environment_capture_close(&current);
      return false;
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
  return status == CALL_OK;
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
  struct shell_alias *aliases = NULL;
  size_t alias_count = 0;
  const char *reason = validate(document, settings, &count, &aliases, &alias_count);
  if (reason) {
    fprintf(stderr, "shell: %s: %s; none applied\n", SHELL_SETTINGS_PATH, reason);
  } else if (apply(shell, settings, count)) {
    shell_aliases_close(shell);
    shell->aliases = aliases;
    shell->alias_count = alias_count;
    aliases = NULL;
    alias_count = 0;
  }
  aliases_free(aliases, alias_count);
  literal_free(document);
}
