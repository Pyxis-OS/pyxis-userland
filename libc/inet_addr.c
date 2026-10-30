#include <arpa/inet.h>
#include <stdbool.h>

/* One ISO C unsigned integer constant without sign, whitespace or suffix. */
static bool parse_part(const char **text, uint32_t *value)
{
  const char *cursor = *text;
  unsigned base = 10;
  if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
    base = 16;
    cursor += 2;
  } else if (cursor[0] == '0') {
    base = 8;
  }

  const char *digits = cursor;
  uint64_t result = 0;
  for (;; ++cursor) {
    unsigned digit;
    if (*cursor >= '0' && *cursor <= '9') {
      digit = (unsigned)(*cursor - '0');
    } else if (base == 16 && *cursor >= 'a' && *cursor <= 'f') {
      digit = (unsigned)(*cursor - 'a') + 10;
    } else if (base == 16 && *cursor >= 'A' && *cursor <= 'F') {
      digit = (unsigned)(*cursor - 'A') + 10;
    } else {
      break;
    }
    if (digit >= base) {
      return false;
    }
    result = result * base + digit;
    if (result > UINT32_MAX) {
      return false;
    }
  }
  if (cursor == digits) {
    return false;
  }
  *value = (uint32_t)result;
  *text = cursor;
  return true;
}

in_addr_t inet_addr(const char *text)
{
  const in_addr_t invalid = (in_addr_t)-1;
  uint32_t parts[4];
  unsigned count = 0;
  if (!text) {
    return invalid;
  }
  for (;;) {
    if (!parse_part(&text, &parts[count])) {
      return invalid;
    }
    ++count;
    if (*text == '\0') {
      break;
    }
    if (*text != '.' || count == 4) {
      return invalid;
    }
    ++text;
  }

  /* The last part fills the remaining low-order bytes. */
  uint32_t address = 0;
  for (unsigned i = 0; i + 1 < count; ++i) {
    if (parts[i] > 0xff) {
      return invalid;
    }
    address |= parts[i] << (24 - 8 * i);
  }
  uint32_t last = parts[count - 1];
  if (count > 1 && last > UINT32_MAX >> (8 * (count - 1))) {
    return invalid;
  }
  return htonl(address | last);
}
