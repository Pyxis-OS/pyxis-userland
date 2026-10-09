#include <errno.h>
#include <startup.h>
#include <string.h>
#include <system_info.h>
#include <unistd.h>
#include "errors.h"

int gethostname(char *name, size_t size)
{
  if (!name) {
    errno = EFAULT;
    return -1;
  }
  struct system_info_hostname hostname;
  enum call_status status = system_info_get_hostname(startup_resource("system_info"), &hostname);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  size_t required = strlen(hostname.name) + 1;
  if (size < required) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(name, hostname.name, required);
  return 0;
}
