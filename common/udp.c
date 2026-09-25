#include "udp.h"
#include <clock.h>

bool udp_parse_address(const char *text, uint32_t *address)
{
  uint32_t result = 0;
  for (unsigned i = 0; i < 4; ++i) {
    unsigned value = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      if (++digits > 3) {
        return false;
      }
      value = value * 10 + (unsigned)(*text++ - '0');
    }
    if (!digits || value > 255 || (i != 3 && *text++ != '.')) {
      return false;
    }
    result = (result << 8) | value;
  }
  if (*text) {
    return false;
  }
  *address = result;
  return true;
}

bool udp_parse_number(const char *text, unsigned maximum, unsigned *value)
{
  unsigned result = 0;
  if (!*text) {
    return false;
  }
  while (*text) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    unsigned digit = (unsigned)(*text++ - '0');
    if (digit > maximum || result > (maximum - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

enum call_status udp_deadline(handle_t clock, uint64_t interval, uint64_t *deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (now > UINT64_MAX - interval) {
    return CALL_LIMIT;
  }
  *deadline = now + interval;
  return CALL_OK;
}

const char *udp_error(enum call_status status)
{
  switch (status) {
  case CALL_TIMED_OUT: return "Timed out";
  case CALL_NO_ROUTE: return "No route to destination";
  case CALL_QUEUE_FULL: return "Network queue full";
  case CALL_BUSY: return "Endpoint already has a call in this direction";
  case CALL_DENIED: return "Permission denied";
  case CALL_NO_MEMORY: return "Out of network memory";
  case CALL_UNAVAILABLE: return "Networking or bound address unavailable";
  case CALL_ENDPOINT_CLOSED: return "Endpoint shut down";
  case CALL_ALREADY_EXISTS: return "Address and port already bound";
  case CALL_LIMIT: return "Network resource or payload limit reached";
  case CALL_BUFFER_TOO_SMALL: return "Receive buffer too small";
  case CALL_BAD_REQUEST: return "Invalid network request";
  default: return "Network operation failed";
  }
}
