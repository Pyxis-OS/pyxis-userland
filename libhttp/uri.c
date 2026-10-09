#include "internal.h"
#include <string.h>

bool http_equal(const char *value, size_t length, const char *literal)
{
  if (length != strlen(literal)) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = value[i];
    if (byte >= 'A' && byte <= 'Z') {
      byte += 'a' - 'A';
    }
    if (byte != (unsigned char)literal[i]) {
      return false;
    }
  }
  return true;
}

bool http_token(unsigned char byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
      (byte >= '0' && byte <= '9') || (byte && strchr("!#$%&'*+-.^_`|~", byte));
}

enum http_error http_parse_uri(const char *text, struct http_uri *uri)
{
  switch (http_url_parse(text, uri)) {
  case CALL_OK: return HTTP_OK;
  case CALL_BAD_OPERATION: return HTTP_UNSUPPORTED;
  case CALL_FILE_TOO_LARGE: return HTTP_LIMIT;
  default: return HTTP_INVALID_URI;
  }
}
