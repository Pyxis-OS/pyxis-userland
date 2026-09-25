#ifndef LIBCONFIG_H
#define LIBCONFIG_H

#include <lua.h>
#include <stddef.h>

enum config_result { CONFIG_OK, CONFIG_MISSING, CONFIG_ERROR };

/* Read trusted text configuration in a fresh restricted Lua environment. The
 * chunk must return exactly one table. decode runs inside the protected call
 * with that table at index 1 and output as lightuserdata at index 2; it returns
 * no results. The consumer owns schema, defaults and cleanup of any partially
 * populated native output, including after CONFIG_ERROR. No state escapes.
 * Errors are reported to stderr; a missing file is silent and distinct. */
enum config_result config_read(const char *path, lua_CFunction decode, void *output);

/* Raw table access and allowed-key checks; each consumer supplies its keys. */
void config_field(lua_State *state, int index, const char *key);
void config_keys(lua_State *state, int index, const char *const *keys, size_t count);

#endif
