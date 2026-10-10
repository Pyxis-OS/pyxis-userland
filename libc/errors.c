#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"

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
  case CALL_BUFFER_TOO_SMALL: return ERANGE;
  case CALL_UNAVAILABLE: return ENODEV;
  case CALL_QUEUE_FULL: return EAGAIN;
  case CALL_WOULD_BLOCK: return EAGAIN;
  case CALL_ENDPOINT_CLOSED: return EPIPE;
  case CALL_BUSY: return EBUSY;
  case CALL_NO_MEMORY: return ENOMEM;
  case CALL_NO_SPACE: return ENOSPC;
  case CALL_QUOTA: return EDQUOT;
  case CALL_FILE_TOO_LARGE: return EFBIG;
  case CALL_LIMIT: return EOVERFLOW;
  case CALL_NOT_FOUND: return ENOENT;
  case CALL_ALREADY_EXISTS: return EEXIST;
  case CALL_READ_ONLY: return EROFS;
  case CALL_INPUT_LOST:
  case CALL_IO:
  case CALL_OUTCOME_UNKNOWN: return EIO;
  case CALL_TIMED_OUT: return ETIMEDOUT;
  case CALL_NOT_EMPTY: return ENOTEMPTY;
  default: return EIO;
  }
}

/* Well-formed UTF-8, as mbtowc decodes it: shortest forms only, no surrogates,
 * nothing above U+10FFFF. */
static bool utf8_valid(const char *text)
{
  size_t length = strlen(text);
  while (length) {
    int count = mbtowc(NULL, text, length);
    if (count <= 0) {
      return false;
    }
    text += count;
    length -= (size_t)count;
  }
  return true;
}

int libc_path_errno(enum call_status status, const char *path, const char *other)
{
  if (status == CALL_BAD_REQUEST &&
      ((path && !utf8_valid(path)) || (other && !utf8_valid(other)))) {
    return EILSEQ;
  }
  return libc_call_errno(status);
}

char *strerror(int error)
{
  switch (error) {
  case 0: return "No error";
  case ENOMEM: return "Out of memory";
  case EMFILE: return "Too many open files";
  case ENOSPC: return "No space left on device";
  case EDQUOT: return "Storage quota exceeded";
  case EFBIG: return "File too large";
  case EINVAL: return "Invalid argument";
  case EOVERFLOW: return "Value too large";
  case ERANGE: return "Result out of range";
  case EILSEQ: return "Invalid character (not valid UTF-8)";
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
  case ENOTDIR: return "Not a directory";
  case ENOTTY: return "Not a terminal";
  case ENAMETOOLONG: return "Name too long";
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
