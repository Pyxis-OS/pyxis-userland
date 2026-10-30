#ifndef LIBC_PYXIS_SHARED_MEMORY_H
#define LIBC_PYXIS_SHARED_MEMORY_H

#include <abi/handle.h>
#include <abi/memory.h>
#include <abi/shared_memory.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow explicit native handles; return 0 on success or -1 with errno.
 * CREATE needs MEMORY CREATE_SHARED; MAP needs region READ or READ|WRITE.
 * Success returns an owned region handle or caller-owned full-region view;
 * create/map failure clears the output. No authority is selected implicitly.
 * Handle close and creator exit alone do not unmap a view. UNMAP needs MEMORY
 * MANAGE and its process-local identity, including after region handle close.
 * An uncertain native outcome becomes EIO; effects may already have committed.
 * No helper retries, closes or unmaps a possibly published result on failure. */
int pyxis_shared_memory_create(handle_t memory, size_t size,
    struct shared_memory_created *created);
int pyxis_shared_memory_map(handle_t region, uint64_t rights,
    struct memory_reservation *view);
int pyxis_shared_memory_unmap(handle_t memory, uint64_t identity);

#ifdef __cplusplus
}
#endif

#endif
