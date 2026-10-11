#include <machine_settings.h>
#include <string.h>

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

bool machine_scroll_direction_parse(const char *name, size_t length, bool *natural)
{
  if (!name || !natural) {
    return false;
  }
  if (length == sizeof("natural") - 1 && !memcmp(name, "natural", length)) {
    *natural = true;
    return true;
  }
  if (length == sizeof("traditional") - 1 && !memcmp(name, "traditional", length)) {
    *natural = false;
    return true;
  }
  return false;
}
