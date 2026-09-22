#include <startup.h>
#include <stdlib.h>

char *getenv(const char *name)
{
  return (char *)startup_environment(name);
}
