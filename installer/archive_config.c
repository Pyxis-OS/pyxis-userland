#include "archive_config.h"
#include "../config-editor/render.h"
#include "../libconfig/boot.h"

#include <stdlib.h>
#include <string.h>

static struct config_literal_value *field(struct config_literal_value *table, const char *name)
{
  if (!table || table->kind != CONFIG_LITERAL_TABLE) {
    return NULL;
  }
  size_t size = strlen(name);
  for (struct config_literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    if (entry->key->kind == CONFIG_LITERAL_STRING && entry->key->as.string.size == size &&
        !memcmp(entry->key->as.string.bytes, name, size)) {
      return entry->value;
    }
  }
  return NULL;
}

static bool string_is(const struct config_literal_value *value, const char *text)
{
  return value && value->kind == CONFIG_LITERAL_STRING &&
      value->as.string.size == strlen(text) &&
      !memcmp(value->as.string.bytes, text, value->as.string.size);
}

static struct config_literal_value *native_partition(struct config_literal_value *value,
    const char *volume)
{
  if (!string_is(field(value, "kind"), "npfs") ||
      !string_is(field(value, "volume"), volume)) {
    return NULL;
  }
  struct config_literal_value *partition = field(value, "partition");
  return partition && partition->kind == CONFIG_LITERAL_INTEGER ? partition : NULL;
}

static bool valid_config(const char *bytes, size_t size)
{
  struct boot_config config;
  enum boot_config_result result = boot_config_read_archive_literal_bytes(
      "config/installed.lua", bytes, size, &config);
  boot_config_free(&config);
  return result == BOOT_CONFIG_OK;
}

bool install_archive_config_rebind(const char *bytes, size_t size, uint64_t pool_partition,
    char **output, size_t *output_size)
{
  *output = NULL;
  *output_size = 0;
  if (!pool_partition || pool_partition > UINT32_MAX || pool_partition > (uint64_t)INT64_MAX ||
      !valid_config(bytes, size)) {
    return false;
  }
  struct config_literal *document = NULL;
  struct config_literal_diagnostic diagnostic;
  if (!config_literal_parse(bytes, size, &document, &diagnostic)) {
    return false;
  }
  struct config_literal_value *volumes = field(document->root, "volumes");
  struct config_literal_value *system = native_partition(field(volumes, "system"), "system");
  struct config_literal_value *home = native_partition(field(volumes, "home"), "home");
  bool ok = system != NULL;
  bool changed = false;
  if (ok) {
    int64_t previous = system->as.integer;
    system->as.integer = (int64_t)pool_partition;
    changed = previous != system->as.integer;
    if (home && home->as.integer == previous) {
      home->as.integer = (int64_t)pool_partition;
    }
  }
  char *value = NULL, *rendered = NULL;
  size_t value_size = 0, rendered_size = size;
  if (ok && changed) {
    static const char prefix[] = "return ";
    size_t framing = sizeof(prefix) - 1 + 1;
    ok = config_render_value(document->root, &value, &value_size) &&
        value_size <= INSTALL_ARCHIVE_CONFIG_MAX_BYTES - framing;
    if (ok) {
      rendered_size = value_size + framing;
      rendered = malloc(rendered_size + 1);
      ok = rendered != NULL;
    }
    if (ok) {
      memcpy(rendered, prefix, sizeof(prefix) - 1);
      memcpy(rendered + sizeof(prefix) - 1, value, value_size);
      rendered[rendered_size - 1] = '\n';
      rendered[rendered_size] = '\0';
      ok = valid_config(rendered, rendered_size);
    }
  }
  free(value);
  config_literal_free(document);
  if (!ok) {
    free(rendered);
    return false;
  }
  *output = rendered;
  *output_size = rendered_size;
  return true;
}
