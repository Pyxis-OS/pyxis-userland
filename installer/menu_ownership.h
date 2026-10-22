#ifndef INSTALLER_MENU_OWNERSHIP_H
#define INSTALLER_MENU_OWNERSHIP_H

#include <literal.h>
#include <stdbool.h>
#include <stddef.h>

/* Ownership marker IDs: an ASCII letter or digit, then letters, digits and
 * _-.: up to the limit. A menu holds at most CONFIG_OWNED_SPAN_LIMIT spans. */
#define CONFIG_ENTRY_ID_LIMIT 64
#define CONFIG_OWNED_SPAN_LIMIT 128

struct config_owned_span {
  char *id;
  struct literal_span span, content;
};
struct config_owned_set {
  struct config_owned_span *spans;
  size_t count;
};

/* Exact whole-line markers, balanced, unique and nonoverlapping. A successful
 * empty set means no markers; find returns NULL for a missing ID. Invalid input
 * returns false and a NULL output. Marker spans do not prove what their content
 * is. Errors: stderr. */
bool config_entry_id_valid(const char *bytes, size_t size);
bool config_owned_scan_limine(const char *name, const char *bytes, size_t size,
    struct config_owned_set **output);
const struct config_owned_span *config_owned_find(const struct config_owned_set *set,
    const char *id);
void config_owned_free(struct config_owned_set *set);

#endif
