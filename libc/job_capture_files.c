#include <abi/file.h>
#include <clock.h>
#include <directory.h>
#include <errno.h>
#include <handle.h>
#include <inttypes.h>
#include <pyxis/descriptor.h>
#include <stdio.h>
#include "descriptor.h"
#include "job_capture_files.h"

static enum call_status adoption_status(int error)
{
  switch (error) {
  case ENOMEM: return CALL_NO_MEMORY;
  case EMFILE: return CALL_LIMIT;
  case EBADF: return CALL_BAD_HANDLE;
  case EACCES: return CALL_DENIED;
  case EBUSY: return CALL_BUSY;
  case ENOTSUP: return CALL_WRONG_TYPE;
  case EINVAL: return CALL_BAD_REQUEST;
  default: return CALL_IO;
  }
}

enum call_status libc_capture_file_create(handle_t private_root, handle_t clock,
    handle_t *file, int *writer)
{
  if (file) {
    *file = HANDLE_INVALID;
  }
  if (writer) {
    *writer = -1;
  }
  if (!file || !writer || private_root == HANDLE_INVALID || clock == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }

  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  char name[64];
  handle_t created = HANDLE_INVALID;
  handle_t writable = HANDLE_INVALID;
  status = CALL_ALREADY_EXISTS;
  for (unsigned int attempt = 0; attempt < TEMPORARY_CREATE_ATTEMPTS; ++attempt) {
    int length = snprintf(name, sizeof(name), "capture_%016" PRIx64 "_%u", now, attempt);
    if (length < 0 || (size_t)length >= sizeof(name)) {
      status = length < 0 ? CALL_IO : CALL_LIMIT;
      break;
    }
    status = directory_create(private_root, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ | FILE_RIGHT_WRITE, &created);
    if (status != CALL_ALREADY_EXISTS) {
      break;
    }
  }
  if (status == CALL_OK) {
    /* The held parent remains the same, and no writer can be delegated until
     * the name is removed. Unpredictable names are not the privacy boundary. */
    status = directory_remove(private_root, name, DIRECTORY_KIND_FILE);
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(created, FILE_RIGHT_WRITE, 0, &writable);
  }
  if (status == CALL_OK) {
    int descriptor = pyxis_descriptor_adopt(&writable, PYXIS_DESCRIPTOR_WRITE);
    if (descriptor >= 0) {
      *file = created;
      *writer = descriptor;
      return CALL_OK;
    }
    status = adoption_status(errno);
  }

  int saved_errno = errno;
  if (writable != HANDLE_INVALID) {
    descriptor_release_handle(writable);
  }
  if (created != HANDLE_INVALID) {
    descriptor_release_handle(created);
  }
  errno = saved_errno;
  return status;
}
