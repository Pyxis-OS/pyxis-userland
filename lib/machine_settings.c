#include <machine_settings.h>

bool machine_hostname_valid(const char *name, size_t length)
{
  if (!name || !length || length > MACHINE_HOSTNAME_MAX ||
      name[0] == '-' || name[length - 1] == '-') {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = name[i];
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '-') {
      return false;
    }
  }
  return true;
}
