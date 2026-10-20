#ifndef LIBCONFIG_H
#define LIBCONFIG_H

#include <lua.h>
#include <stddef.h>

enum config_result { CONFIG_OK, CONFIG_MISSING, CONFIG_ERROR };

/* A caller-owned reason for one failure, so the caller can report it in its own
 * single line. Readers given NULL report to stderr instead. */
#define CONFIG_FAILURE_MAX 256
struct config_failure {
  char text[CONFIG_FAILURE_MAX];
};

/* Read trusted text configuration in a fresh restricted Lua environment. The
 * chunk must return exactly one table. decode runs inside the protected call
 * with that table at index 1 and output as lightuserdata at index 2; it returns
 * no results. The consumer owns schema, defaults and cleanup of any partially
 * populated native output, including after CONFIG_ERROR. No state escapes.
 * Errors are reported to stderr; a missing file is silent and distinct. */
enum config_result config_read(const char *path, lua_CFunction decode, void *output);

/* As config_read, for SIZE caller bytes already read through a capability.
 * NAME labels errors only; the bytes are borrowed until return. */
enum config_result config_read_bytes(const char *name, const char *bytes, size_t size,
    lua_CFunction decode, void *output);

/* Untrusted declarative input: parse bounded literal return-table data without
 * evaluating Lua. The protected decoder and partial-output ownership contract
 * are the same as config_read_bytes. Unsupported handwritten Lua is an error. */
enum config_result config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, lua_CFunction decode, void *output);
/* As config_read_literal_bytes; a non-NULL FAILURE receives the reason, without
 * NAME, instead of stderr. */
enum config_result config_read_literal_bytes_reason(const char *name, const char *bytes,
    size_t size, lua_CFunction decode, void *output, struct config_failure *failure);

/* Raw table access and allowed-key checks; each consumer supplies its keys. */
void config_field(lua_State *state, int index, const char *key);
void config_keys(lua_State *state, int index, const char *const *keys, size_t count);

#endif
