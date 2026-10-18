#ifndef USERSPACE_LITERAL_H
#define USERSPACE_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LITERAL_SOURCE_LIMIT (1024 * 1024)

/* All spans are half-open offsets into the document's owned source copy.
 * Entry spans start at the key/value and include their trailing separator.
 * Comments between a value and its separator lie in both spans; callers must
 * check marker/entry boundaries before selecting an owned span. Implicit array
 * keys have a zero-length span at the value start. Comments never establish
 * ownership by themselves. */
struct literal_span { size_t start, end; };

enum literal_kind {
  LITERAL_TABLE, LITERAL_STRING, LITERAL_INTEGER,
  LITERAL_NUMBER, LITERAL_BOOLEAN,
};

struct literal_value;
struct literal_entry {
  struct literal_value *key, *value;
  struct literal_entry *next;
  struct literal_span span;
  bool implicit;
};

struct literal_value {
  enum literal_kind kind;
  struct literal_span span;
  union {
    struct { struct literal_entry *entries; size_t count; } table;
    struct { char *bytes; size_t size; } string;
    int64_t integer;
    double number;
    bool boolean;
  } as;
};

struct literal_comment {
  struct literal_span span, content;
  bool long_comment;
};

struct literal {
  char *source;
  size_t size;
  struct literal_value *root;
  struct literal_comment *comments;
  size_t comment_count;
};

struct literal_diagnostic {
  size_t offset, line, column;
  const char *reason;
};

/* Parse only literal return-table data, never execute source. Limits: 1 MiB
 * source, 32 nested tables, 8192 values/entries/comments, 16 Mi work units.
 * Output is NULL on failure. Unsupported Lua is an error, never a rewrite.
 * Strings retain decoded bytes (including embedded NUL) and source spans.
 * The caller frees a successful document with literal_free. */
bool literal_parse(const char *bytes, size_t size,
    struct literal **output, struct literal_diagnostic *diagnostic);
void literal_free(struct literal *document);

const struct literal_value *literal_field(
    const struct literal_value *table, const char *key);
/* Table equality compares keys/values independent of source order and spelling.
 * Exceeding the comparison bound conservatively returns false. */
bool literal_equal(const struct literal_value *a,
    const struct literal_value *b);

#endif
