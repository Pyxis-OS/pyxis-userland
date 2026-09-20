#include <io.h>
#include <abi/startup.h>

int main(const struct startup_info *startup)
{
  if (!startup || startup->version != STARTUP_VERSION ||
      startup->size < sizeof(*startup)) {
    return 1;
  }

  return print("Hello from C!\n");
}
