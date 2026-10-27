#include <errno.h>
#include <limits.h>
#include <startup.h>
#include <unistd.h>

long sysconf(int name)
{
  if (name != _SC_NPROCESSORS_ONLN && name != _SC_NPROCESSORS_CONF) {
    errno = EINVAL;
    return -1;
  }
  size_t count = startup_usable_cpu_count();
  if (count > LONG_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  return (long)count;
}
