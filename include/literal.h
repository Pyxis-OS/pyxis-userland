#ifndef USERSPACE_LITERAL_H
#define USERSPACE_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_LITERAL_SOURCE_LIMIT (1024 * 1024)

/* All spans are half-open offsets into the document's owned source copy.
 * Entry spans start at the key/value and include their trailing separator.
 * Comments between a value and its separator lie in both spans; callers must
 * check marker/entry boundaries before selecting an owned span. Implicit array
 * keys have a zero-length span at the value start. Comments never establish
 * ownership by themselves. */
struct config_literal_span { size_t start, end; };

enum config_literal_kind {
  CONFIG_LITERAL_TABLE, CONFIG_LITERAL_STRING, CONFIG_LITERAL_INTEGER,
  CONFIG_LITERAL_NUMBER, CONFIG_LITERAL_BOOLEAN,
};

struct config_literal_value;
struct config_literal_entry {
  struct config_literal_value *key, *value;
  struct config_literal_entry *next;
  struct config_literal_span span;
  bool implicit;
};

struct config_literal_value {
  enum config_literal_kind kind;
  struct config_literal_span span;
  union {
    struct { struct config_literal_entry *entries; size_t count; } table;
    struct { char *bytes; size_t size; } string;
    int64_t integer;
    double number;
    bool boolean;
  } as;
};

struct config_literal_comment {
  struct config_literal_span span, content;
  bool long_comment;
};

struct config_literal {
  char *source;
  size_t size;
  struct config_literal_value *root;
  struct config_literal_comment *comments;
  size_t comment_count;
};

struct config_literal_diagnostic {
  size_t offset, line, column;
  const char *reason;
};

/* Parse only literal return-table data, never execute source. Limits: 1 MiB
 * source, 32 nested tables, 8192 values/entries/comments, 16 Mi work units.
 * Output is NULL on failure. Unsupported Lua is an error, never a rewrite.
 * Strings retain decoded bytes (including embedded NUL) and source spans.
 * The caller frees a successful document with config_literal_free. */
bool config_literal_parse(const char *bytes, size_t size,
    struct config_literal **output, struct config_literal_diagnostic *diagnostic);
void config_literal_free(struct config_literal *document);

const struct config_literal_value *config_literal_field(
    const struct config_literal_value *table, const char *key);
/* Table equality compares keys/values independent of source order and spelling.
 * Exceeding the comparison bound conservatively returns false. */
bool config_literal_equal(const struct config_literal_value *a,
    const struct config_literal_value *b);

#endif
