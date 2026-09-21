#include <console.h>
#include <handle.h>
#include <abi/startup.h>

int main(const struct startup_info *startup)
{
  if (!startup || startup->version != STARTUP_VERSION ||
      startup->size < sizeof(*startup)) {
    return 1;
  }

  int result = console_print(startup->output, "Hello from C!\n");
  if (handle_close(startup->output) != 0) {
    return 1;
  }
  return result;
}
