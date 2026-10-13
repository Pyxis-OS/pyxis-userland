#include "render.h"
#include "ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buffer { char *bytes; size_t size, capacity, work; };

static bool append(struct buffer *buffer, const char *bytes, size_t size)
{
  if (size > CONFIG_LITERAL_SOURCE_LIMIT - buffer->size) return false;
  if (buffer->size + size + 1 > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < buffer->size + size + 1) {
      capacity *= 2;
      if (capacity > CONFIG_LITERAL_SOURCE_LIMIT + 1) capacity = CONFIG_LITERAL_SOURCE_LIMIT + 1;
    }
    void *grown = realloc(buffer->bytes, capacity);
    if (!grown) return false;
    buffer->bytes = grown;
    buffer->capacity = capacity;
  }
  if (size) memcpy(buffer->bytes + buffer->size, bytes, size);
  buffer->size += size;
  buffer->bytes[buffer->size] = '\0';
  return true;
}

static bool text(struct buffer *buffer, const char *bytes)
{
  return append(buffer, bytes, strlen(bytes));
}

static bool quote(struct buffer *buffer, const char *bytes, size_t size)
{
  static const char digits[] = "0123456789abcdef";
  if (!text(buffer, "\"")) return false;
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = (unsigned char)bytes[i];
    if (byte == '\\' || byte == '"') {
      char escape[] = {'\\', (char)byte};
      if (!append(buffer, escape, sizeof(escape))) return false;
    } else if (byte < 0x20 || byte == 0x7f) {
      char escape[] = {'\\', 'x', digits[byte >> 4], digits[byte & 15]};
      if (!append(buffer, escape, sizeof(escape))) return false;
    } else if (!append(buffer, bytes + i, 1)) {
      return false;
    }
  }
  return text(buffer, "\"");
}

static bool render(struct buffer *buffer, const struct config_literal_value *value, size_t depth)
{
  if (!value || depth > 32 || ++buffer->work > 8192) return false;
  char number[64];
  switch (value->kind) {
    case CONFIG_LITERAL_STRING:
      return quote(buffer, value->as.string.bytes, value->as.string.size);
    case CONFIG_LITERAL_BOOLEAN:
      return text(buffer, value->as.boolean ? "true" : "false");
    case CONFIG_LITERAL_INTEGER:
      snprintf(number, sizeof(number), "%lld", (long long)value->as.integer);
      return text(buffer, number);
    case CONFIG_LITERAL_NUMBER:
      if (!__builtin_isfinite(value->as.number)) return false;
      snprintf(number, sizeof(number), "%.17g", (double)value->as.number);
      if (!text(buffer, number)) return false;
      return strpbrk(number, ".eE") || text(buffer, ".0");
    case CONFIG_LITERAL_TABLE:
      if (!text(buffer, "{")) return false;
      for (const struct config_literal_entry *entry = value->as.table.entries; entry; entry = entry->next) {
        if (!text(buffer, " [") || !render(buffer, entry->key, depth + 1) || !text(buffer, "] = ") ||
            !render(buffer, entry->value, depth + 1) || !text(buffer, ",")) return false;
      }
      return text(buffer, " }");
  }
  return false;
}

static bool finish(struct buffer *buffer, bool valid, char **output, size_t *size)
{
  *output = NULL;
  *size = 0;
  if (!valid) {
    free(buffer->bytes);
    return false;
  }
  *output = buffer->bytes;
  *size = buffer->size;
  return true;
}

bool config_render_value(const struct config_literal_value *value,
    char **output, size_t *size)
{
  struct buffer buffer = {0};
  return finish(&buffer, render(&buffer, value, 0), output, size);
}

bool config_render_entry(const struct config_catalog_entry *entry,
    const struct config_literal_value *value, char **output, size_t *size)
{
  struct buffer buffer = {0};
  bool limine = entry->file == CONFIG_FILE_LIMINE;
  const char *prefix = limine ? "# pyxis:" : "-- pyxis:";
  bool valid = config_entry_id_valid(entry->id, strlen(entry->id)) &&
      text(&buffer, prefix) && text(&buffer, "begin ") && text(&buffer, entry->id) && text(&buffer, "\n");
  if (valid && limine) {
    valid = value && value->kind == CONFIG_LITERAL_STRING &&
        config_limine_entry_valid(entry->name, value->as.string.bytes, value->as.string.size) &&
        append(&buffer, value->as.string.bytes, value->as.string.size);
    if (valid && buffer.size && buffer.bytes[buffer.size - 1] != '\r' && buffer.bytes[buffer.size - 1] != '\n') {
      valid = text(&buffer, "\n");
    }
  } else if (valid) {
    if (entry->kind == CONFIG_ENTRY_VOLUME || entry->kind == CONFIG_ENTRY_SETTING) {
      valid = text(&buffer, "[") && quote(&buffer, entry->name, strlen(entry->name)) && text(&buffer, "] = ");
    }
    if (valid) valid = render(&buffer, value, 0) && text(&buffer, ",\n");
  }
  valid = valid && text(&buffer, prefix) && text(&buffer, "end ") &&
      text(&buffer, entry->id) && text(&buffer, "\n");
  return finish(&buffer, valid, output, size);
}

bool config_render_defaults(const struct config_catalog *catalog, enum config_file file,
    bool replacements, char **output, size_t *size)
{
  struct buffer buffer = {0};
  bool ok = file != CONFIG_FILE_LIMINE && text(&buffer, "return {\n");
  unsigned groups = file == CONFIG_FILE_BOOT ? 2 : 1;
  for (unsigned group = 0; group < groups && ok; ++group) {
    if (file == CONFIG_FILE_BOOT) {
      ok = text(&buffer, group ? "spaces = {\n" : "volumes = {\n");
    }
    for (size_t i = 0; i < catalog->count && ok; ++i) {
      const struct config_catalog_entry *entry = &catalog->entries[i];
      if (entry->file != file || (file == CONFIG_FILE_BOOT &&
          entry->kind != (group ? CONFIG_ENTRY_SPACE : CONFIG_ENTRY_VOLUME))) {
        continue;
      }
      const struct config_literal_value *value = replacements &&
          entry->migration.kind == CONFIG_MIGRATION_REPLACE ? entry->migration.value : entry->default_value;
      char *bytes;
      size_t count;
      ok = config_render_entry(entry, value, &bytes, &count);
      if (ok) {
        ok = append(&buffer, bytes, count);
        free(bytes);
      }
    }
    if (file == CONFIG_FILE_BOOT && ok) {
      ok = text(&buffer, "},\n");
    }
  }
  return finish(&buffer, ok && text(&buffer, "}\n"), output, size);
}
