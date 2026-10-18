#include "archive_config.h"
#include "../config-editor/render.h"
#include "../libconfig/boot.h"

#include <stdlib.h>
#include <string.h>

static struct literal_value *field(struct literal_value *table, const char *name)
{
  if (!table || table->kind != LITERAL_TABLE) {
    return NULL;
  }
  size_t size = strlen(name);
  for (struct literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    if (entry->key->kind == LITERAL_STRING && entry->key->as.string.size == size &&
        !memcmp(entry->key->as.string.bytes, name, size)) {
      return entry->value;
    }
  }
  return NULL;
}

static bool string_is(const struct literal_value *value, const char *text)
{
  return value && value->kind == LITERAL_STRING &&
      value->as.string.size == strlen(text) &&
      !memcmp(value->as.string.bytes, text, value->as.string.size);
}

static struct literal_value *native_partition(struct literal_value *value,
    const char *volume)
{
  if (!string_is(field(value, "kind"), "npfs") ||
      !string_is(field(value, "volume"), volume)) {
    return NULL;
  }
  struct literal_value *partition = field(value, "partition");
  return partition && partition->kind == LITERAL_INTEGER ? partition : NULL;
}

/* The partition of any npfs volume, whatever its volume name. */
static struct literal_value *npfs_partition(struct literal_value *value)
{
  if (!string_is(field(value, "kind"), "npfs")) {
    return NULL;
  }
  struct literal_value *partition = field(value, "partition");
  return partition && partition->kind == LITERAL_INTEGER ? partition : NULL;
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
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  if (!literal_parse(bytes, size, &document, &diagnostic)) {
    return false;
  }
  struct literal_value *volumes = field(document->root, "volumes");
  struct literal_value *system = native_partition(field(volumes, "system"), "system");
  bool ok = system != NULL;
  bool changed = false;
  if (ok) {
    /* Every npfs volume on the archive's pool partition (home, packages and any
     * later one) moves with system; volumes elsewhere are left as configured. */
    int64_t previous = system->as.integer;
    changed = previous != (int64_t)pool_partition;
    for (struct literal_entry *entry = volumes->as.table.entries; entry; entry = entry->next) {
      struct literal_value *partition = npfs_partition(entry->value);
      if (partition && partition->as.integer == previous) {
        partition->as.integer = (int64_t)pool_partition;
      }
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
  literal_free(document);
  if (!ok) {
    free(rendered);
    return false;
  }
  *output = rendered;
  *output_size = rendered_size;
  return true;
}
