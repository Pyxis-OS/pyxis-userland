#include "menu_ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scanner {
  const char *name, *bytes;
  size_t size;
  struct config_owned_set *set;
  struct config_owned_span *active;
};

static bool horizontal(char byte)
{
  return byte == ' ' || byte == '\t';
}

static size_t line_end(const char *bytes, size_t size, size_t start)
{
  while (start < size && bytes[start] != '\r' && bytes[start] != '\n') ++start;
  return start;
}

static size_t next_line(const char *bytes, size_t size, size_t end)
{
  if (end == size) return end;
  char first = bytes[end++];
  if (end < size && ((first == '\r' && bytes[end] == '\n') ||
      (first == '\n' && bytes[end] == '\r'))) ++end;
  return end;
}

static bool fail(struct scanner *scanner, size_t offset, const char *reason)
{
  fprintf(stderr, "ownership: %s: byte %zu: %s; markers not recognized\n",
      scanner->name, offset, reason);
  return false;
}

bool config_entry_id_valid(const char *bytes, size_t size)
{
  if (!size || size > CONFIG_ENTRY_ID_LIMIT) return false;
  for (size_t i = 0; i < size; ++i) {
    char byte = bytes[i];
    bool alphanumeric = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9');
    if (!alphanumeric && (i == 0 || (byte != '_' && byte != '-' && byte != '.' && byte != ':'))) {
      return false;
    }
  }
  return true;
}

const struct config_owned_span *config_owned_find(const struct config_owned_set *set,
    const char *id)
{
  if (!set) return NULL;
  for (size_t i = 0; i < set->count; ++i) {
    if (!strcmp(set->spans[i].id, id)) return &set->spans[i];
  }
  return NULL;
}

void config_owned_free(struct config_owned_set *set)
{
  if (!set) return;
  for (size_t i = 0; i < set->count; ++i) free(set->spans[i].id);
  free(set->spans);
  free(set);
}

static bool marker(struct scanner *scanner, size_t comment_start, size_t content_start,
    size_t end)
{
  const char *bytes = scanner->bytes;
  size_t candidate = content_start;
  while (candidate < end && horizontal(bytes[candidate])) ++candidate;
  if (end - candidate < 6 || memcmp(bytes + candidate, "pyxis:", 6)) return true;
  bool begin;
  size_t id_start;
  if (end - content_start >= 13 && !memcmp(bytes + content_start, " pyxis:begin ", 13)) {
    begin = true;
    id_start = content_start + 13;
  } else if (end - content_start >= 11 && !memcmp(bytes + content_start, " pyxis:end ", 11)) {
    begin = false;
    id_start = content_start + 11;
  } else {
    return fail(scanner, comment_start, "malformed ownership marker");
  }
  if (!config_entry_id_valid(bytes + id_start, end - id_start)) {
    return fail(scanner, comment_start, "invalid ownership marker ID");
  }
  size_t line_start = comment_start;
  while (line_start && bytes[line_start - 1] != '\r' && bytes[line_start - 1] != '\n') --line_start;
  for (size_t i = line_start; i < comment_start; ++i) {
    if (!horizontal(bytes[i])) return fail(scanner, comment_start, "ownership marker must occupy a whole line");
  }
  char id[CONFIG_ENTRY_ID_LIMIT + 1];
  memcpy(id, bytes + id_start, end - id_start);
  id[end - id_start] = '\0';
  if (begin) {
    if (scanner->active) return fail(scanner, comment_start, "overlapping ownership markers");
    if (config_owned_find(scanner->set, id)) return fail(scanner, comment_start, "duplicate ownership ID");
    if (scanner->set->count == CONFIG_OWNED_SPAN_LIMIT) return fail(scanner, comment_start, "too many ownership spans");
    struct config_owned_span *span = &scanner->set->spans[scanner->set->count++];
    span->id = strdup(id);
    if (!span->id) return fail(scanner, comment_start, "cannot allocate ownership ID");
    span->span.start = line_start;
    span->content.start = next_line(bytes, scanner->size, end);
    scanner->active = span;
  } else {
    if (!scanner->active || strcmp(scanner->active->id, id)) {
      return fail(scanner, comment_start, "unmatched ownership end marker");
    }
    scanner->active->content.end = line_start;
    scanner->active->span.end = next_line(bytes, scanner->size, end);
    scanner->active = NULL;
  }
  return true;
}

static bool start(struct scanner *scanner)
{
  scanner->set = calloc(1, sizeof(*scanner->set));
  if (!scanner->set) return fail(scanner, 0, "cannot allocate ownership set");
  scanner->set->spans = calloc(CONFIG_OWNED_SPAN_LIMIT, sizeof(*scanner->set->spans));
  if (!scanner->set->spans) {
    config_owned_free(scanner->set);
    scanner->set = NULL;
    return fail(scanner, 0, "cannot allocate ownership spans");
  }
  return true;
}

static bool finish(struct scanner *scanner, bool valid, struct config_owned_set **output)
{
  if (valid && scanner->active) valid = fail(scanner, scanner->active->span.start, "missing ownership end marker");
  if (!valid) {
    config_owned_free(scanner->set);
    return false;
  }
  *output = scanner->set;
  return true;
}

bool config_owned_scan_limine(const char *name, const char *bytes, size_t size,
    struct config_owned_set **output)
{
  *output = NULL;
  struct scanner scanner = {.name = name, .bytes = bytes, .size = size};
  if (size > LITERAL_SOURCE_LIMIT || (!bytes && size) || (size && memchr(bytes, '\0', size))) {
    return fail(&scanner, 0, "invalid or oversized Limine source");
  }
  if (!start(&scanner)) return false;
  bool valid = true;
  for (size_t position = 0; position < size;) {
    size_t end = line_end(bytes, size, position);
    size_t comment = position;
    while (comment < end && horizontal(bytes[comment])) ++comment;
    if (comment < end && bytes[comment] == '#' && !marker(&scanner, comment, comment + 1, end)) {
      valid = false;
      break;
    }
    position = next_line(bytes, size, end);
  }
  return finish(&scanner, valid, output);
}
