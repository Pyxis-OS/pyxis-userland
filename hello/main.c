#include <console.h>
#include <abi/startup.h>

int main(const struct startup_info *startup)
{
  if (!startup || startup->version != STARTUP_VERSION ||
      startup->size < sizeof(*startup)) {
    return 1;
  }

  return console_print(startup->output, "Hello from C!\n");
}
