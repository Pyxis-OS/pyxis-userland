#ifndef LIBC_JOB_CAPTURE_FILES_H
#define LIBC_JOB_CAPTURE_FILES_H

#include <abi/handle.h>
#include <abi/syscall.h>

/* Borrow a retained, validated native private RAM root and READ clock. Private
 * RAM provisioning belongs to the caller; handle metadata cannot prove it.
 * Success owns an unlinked READ|WRITE FILE and a separate WRITE-only descriptor.
 * Failure clears both outputs and preserves the caller's root and clock. */
enum call_status libc_capture_file_create(handle_t private_root, handle_t clock,
    handle_t *file, int *writer);

#endif
