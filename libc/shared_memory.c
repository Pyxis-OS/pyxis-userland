#include <errno.h>
#include <memory.h>
#include <pyxis/shared_memory.h>
#include <shared_memory.h>
#include "errors.h"

static int native_result(enum call_status status)
{
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}

int pyxis_shared_memory_create(handle_t memory, size_t size,
    struct shared_memory_created *created)
{
  return native_result(shared_memory_create(memory, size, created));
}

int pyxis_shared_memory_map(handle_t region, uint64_t rights,
    struct memory_reservation *view)
{
  return native_result(shared_memory_map(region, rights, view));
}

int pyxis_shared_memory_unmap(handle_t memory, uint64_t identity)
{
  return native_result(memory_release_reservation(memory, identity));
}
