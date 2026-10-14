#include <errno.h>
#include <startup.h>
#include <environment.h>
#include <handle.h>
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
  handle_t system;
  enum call_status status = environment_capability(startup_environment_handle(), "system_info", &system);
  if (status == CALL_OK) {
    status = system_info_get_hostname(system, &hostname);
    if (handle_close(system) && status == CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  }
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
