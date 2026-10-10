#ifndef LIBC_PYXIS_DESCRIPTOR_H
#define LIBC_PYXIS_DESCRIPTOR_H

#include <abi/handle.h>
#include <stddef.h>
#include <sys/types.h>

enum pyxis_descriptor_access {
  PYXIS_DESCRIPTOR_READ = 1u << 0,
  PYXIS_DESCRIPTOR_WRITE = 1u << 1,
};

struct pyxis_descriptor_binding {
  handle_t handle;
  struct handle_info info;
  unsigned int access;
  size_t buffered_read;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow for native queries, waits or delegation, without changing descriptor
 * input or position. Never close or perform data I/O through the borrowed
 * handle: libc owns its lifetime, cursor and read-ahead. The handle expires on
 * descriptor close; buffered_read is a snapshot, not a reservation. Failure
 * clears binding. No descriptor entry pointer escapes libc. */
int pyxis_descriptor_borrow(int descriptor, struct pyxis_descriptor_binding *binding);

/* Consume one caller-owned handle only after validation and slot reservation.
 * Success returns its descriptor and sets *handle to HANDLE_INVALID; failure
 * sets errno and preserves the caller's handle. Access must be a nonempty
 * READ/WRITE subset supported by the grant. FILE (native or callable exported),
 * native PIPE and native CONSOLE are supported. The file cursor starts at zero.
 * A handle already owned by a descriptor cannot be adopted again. */
int pyxis_descriptor_adopt(handle_t *handle, unsigned int access);

/* Immediate native stream transfers: pending libc read-ahead is returned first.
 * No fill/retry loop, stdio indicators or descriptor mode changes. FILE descriptors
 * fail with ENOTSUP before I/O; use read/write or pread/pwrite for files. Return
 * the transferred count, zero for EOF/zero length, or -1 with errno; live stream
 * unavailability is EAGAIN. Access and SSIZE_MAX are checked even at zero length. */
ssize_t pyxis_descriptor_try_read(int descriptor, void *buffer, size_t count);
ssize_t pyxis_descriptor_try_write(int descriptor, const void *buffer, size_t count);

#ifdef __cplusplus
}
#endif

#endif
