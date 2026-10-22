#ifndef LIBCONFIG_HANDOFF_H
#define LIBCONFIG_HANDOFF_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* The installed configuration files boot hands to its spaces, in order. */
enum config_lua_file { CONFIG_LUA_BOOT, CONFIG_LUA_SESSION, CONFIG_LUA_NETWORK, CONFIG_LUA_COUNT };

struct config_handoff {
  handle_t files[CONFIG_LUA_COUNT];
};

struct config_handoff_bytes {
  char *bytes;
  size_t size;
};

/* Initialize handles to HANDLE_INVALID (or {0}). Close releases every held
 * grant. */
bool config_handoff_close(struct config_handoff *handoff);
/* One handoff file. The caller supplies a private, unshared native RAM parent
 * and keeps it private for the entire call. NAME is created in it and unlinked
 * before any byte is written; success returns only its native READ grant in
 * FILE. No automatic retry or named-file repair. */
bool config_handoff_file(handle_t private_parent, const char *name, const char *bytes,
    size_t size, handle_t *file, enum call_status *status);

/* Borrow a trusted handoff FILE with exactly native READ authority. Read and
 * recompare its bytes and held identity/size without resolving a path. Stable
 * reads alone do not prove the producer removed every writer; the trusted
 * constructor's private namespace and writer retirement establish that.
 * Initialize snapshot with {0}; read replaces its owned, NUL-terminated bytes.
 */
bool config_handoff_read(handle_t file, struct config_handoff_bytes *snapshot,
    enum call_status *status);
void config_handoff_bytes_free(struct config_handoff_bytes *snapshot);

#endif
