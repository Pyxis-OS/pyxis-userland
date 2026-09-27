#ifndef USERSPACE_NAMESPACE_H
#define USERSPACE_NAMESPACE_H

#include <abi/namespace.h>
#include <abi/syscall.h>
#include <stdbool.h>

/* Names are exact ASCII identifiers, one to NAMESPACE_NAME_MAX bytes. CREATE
 * returns an owned LOOKUP|MANAGE namespace handle. Bindings retain a copy of
 * the exported client grant at its supplied rights and transport masks. */
enum call_status namespace_create(handle_t service, handle_t *namespace_handle);
enum call_status namespace_publish(handle_t namespace_handle, const char *name,
    handle_t client, uint64_t rights, uint64_t transport);
enum call_status namespace_replace(handle_t namespace_handle, const char *name,
    handle_t client, uint64_t rights, uint64_t transport);
enum call_status namespace_remove(handle_t namespace_handle, const char *name);
/* LOOKUP returns an owned copy with the binding's fixed masks. */
enum call_status namespace_lookup(handle_t namespace_handle, const char *name,
    handle_t *client);

#endif
