#ifndef CONFIG_EDITOR_OWNERSHIP_H
#define CONFIG_EDITOR_OWNERSHIP_H

#include "catalog.h"

struct config_owned_span {
  char *id;
  struct literal_span span, content;
};
struct config_owned_set {
  struct config_owned_span *spans;
  size_t count;
};

/* Exact whole-line markers, balanced, unique and nonoverlapping. Lua scans
 * only actual line comments from the parsed document. A successful empty set
 * means no markers; find returns NULL for a missing ID. Invalid input returns
 * false and a NULL output, requiring printed changes. Marker spans do not prove
 * that their content is the catalog's complete target entry. Errors: stderr. */
bool config_owned_scan_lua(const char *name, const struct literal *document,
    struct config_owned_set **output);
bool config_owned_scan_limine(const char *name, const char *bytes, size_t size,
    struct config_owned_set **output);
const struct config_owned_span *config_owned_find(const struct config_owned_set *set,
    const char *id);
void config_owned_free(struct config_owned_set *set);

/* Structural certification of one declared menu entry: matching /NAME header,
 * one protocol/path, no additional/nested menu header or control/NUL bytes.
 * This does not certify firmware behavior or the referenced payload paths. */
bool config_limine_entry_valid(const char *entry_name, const char *bytes, size_t size);

#endif
