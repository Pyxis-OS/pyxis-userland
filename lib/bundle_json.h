#ifndef USERSPACE_BUNDLE_JSON_H
#define USERSPACE_BUNDLE_JSON_H

#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum bundle_json_type {
  BUNDLE_JSON_OBJECT,
  BUNDLE_JSON_ARRAY,
  BUNDLE_JSON_STRING,
  BUNDLE_JSON_INTEGER,
  BUNDLE_JSON_BOOLEAN,
  BUNDLE_JSON_NULL,
};

struct bundle_json_node {
  enum bundle_json_type type;
  size_t child;
  size_t next;
  char *string;
  uint64_t integer;
};

struct bundle_json {
  char *text;
  size_t size;
  struct bundle_json_node *nodes;
  size_t count;
  size_t capacity;
};

/* Takes ownership of text, including on failure. Decodes strings in place;
 * rejects duplicate object keys, invalid UTF-8 and embedded NUL strings. */
enum call_status bundle_json_parse(char *text, size_t size, struct bundle_json *json);
void bundle_json_close(struct bundle_json *json);

#endif
