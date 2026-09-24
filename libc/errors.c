#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "stream.h"

int libc_call_errno(enum call_status status)
{
  switch (status) {
  case CALL_OK: return 0;
  case CALL_BAD_HANDLE: return EBADF;
  case CALL_DENIED: return EACCES;
  case CALL_BAD_OPERATION: return ENOTSUP;
  case CALL_BAD_REQUEST:
  case CALL_WRONG_TYPE: return EINVAL;
  case CALL_BAD_BUFFER: return EFAULT;
  case CALL_UNAVAILABLE: return ENODEV;
  case CALL_QUEUE_FULL: return EAGAIN;
  case CALL_ENDPOINT_CLOSED: return EPIPE;
  case CALL_BUSY: return EBUSY;
  case CALL_NO_MEMORY: return ENOMEM;
  case CALL_LIMIT: return EOVERFLOW;
  case CALL_NOT_FOUND: return ENOENT;
  case CALL_ALREADY_EXISTS: return EEXIST;
  case CALL_READ_ONLY: return EROFS;
  case CALL_INPUT_LOST:
  case CALL_IO: return EIO;
  case CALL_TIMED_OUT: return ETIMEDOUT;
  case CALL_NOT_EMPTY: return ENOTEMPTY;
  default: return EIO;
  }
}

char *strerror(int error)
{
  switch (error) {
  case 0: return "No error";
  case ENOMEM: return "Out of memory";
  case EINVAL: return "Invalid argument";
  case EOVERFLOW: return "Value too large";
  case ERANGE: return "Result out of range";
  case EBADF: return "Invalid stream or handle";
  case EACCES: return "Permission denied";
  case ENOTSUP: return "Operation not supported";
  case EFAULT: return "Invalid buffer";
  case ENODEV: return "Resource unavailable";
  case EAGAIN: return "Try again";
  case EPIPE: return "Endpoint closed";
  case EBUSY: return "Resource busy";
  case ENOENT: return "Not found";
  case EEXIST: return "Already exists";
  case EROFS: return "Read-only filesystem";
  case EIO: return "Input/output error";
  case ESPIPE: return "Stream is not seekable";
  case ETIMEDOUT: return "Operation timed out";
  case ENOTEMPTY: return "Directory not empty";
  default: return "Unknown error";
  }
}

void perror(const char *prefix)
{
  int saved = errno;
  if (prefix && *prefix) {
    if (fputs(prefix, stderr) == EOF || fputs(": ", stderr) == EOF) {
      goto done;
    }
  }
  if (fputs(strerror(saved), stderr) != EOF) {
    fputc('\n', stderr);
  }
done:
  errno = saved;
}
