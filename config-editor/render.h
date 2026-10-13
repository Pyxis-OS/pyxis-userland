#ifndef CONFIG_EDITOR_RENDER_H
#define CONFIG_EDITOR_RENDER_H

#include "catalog.h"

/* Emit canonical literal Lua value text (without return or separator). All
 * strings use byte-safe quotes/escapes. Caller owns output on success; NULL on
 * failure. The 1 MiB output bound also applies to one rendered entry. */
bool config_render_value(const struct config_literal_value *value,
    char **output, size_t *size);
/* Render one entry with its Lua/Limine markers and trailing Lua separator.
 * volume/setting entries use NAME as a string key; spaces are array values.
 * Limine defaults are exact string bytes between marker lines. */
bool config_render_entry(const struct config_catalog_entry *entry,
    const struct config_literal_value *value, char **output, size_t *size);

#endif
