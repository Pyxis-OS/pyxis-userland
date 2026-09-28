#include "internal.h"
#include "../common/dns.h"
#include "../common/udp.h"
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

static bool hex_digit(unsigned char byte)
{
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
      (byte >= 'A' && byte <= 'F');
}

enum http_error http_parse_uri(const char *text, struct http_uri *uri)
{
  size_t length = strnlen(text, HTTP_URI_MAX + 1);
  if (length > HTTP_URI_MAX) {
    return HTTP_LIMIT;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = text[i];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || strchr("-._~:/?#[]@!$&'()*+,;=%", byte))) {
      return HTTP_INVALID_URI;
    }
    if (byte == '%' && (i + 2 >= length || !hex_digit(text[i + 1]) ||
        !hex_digit(text[i + 2]))) {
      return HTTP_INVALID_URI;
    }
  }
  const char *scheme_end = strchr(text, ':');
  if (!scheme_end || scheme_end == text ||
      !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z'))) {
    return HTTP_INVALID_URI;
  }
  for (const char *byte = text + 1; byte < scheme_end; ++byte) {
    if (!((*byte >= 'a' && *byte <= 'z') || (*byte >= 'A' && *byte <= 'Z') ||
        (*byte >= '0' && *byte <= '9') || strchr("+.-", *byte))) {
      return HTTP_INVALID_URI;
    }
  }
  if (!http_equal(text, scheme_end - text, "http")) {
    return HTTP_UNSUPPORTED;
  }
  if (length < 7 || scheme_end[1] != '/' || scheme_end[2] != '/') {
    return HTTP_INVALID_URI;
  }
  const char *start = text + 7;
  size_t authority_size = 0;
  while (start[authority_size] && !strchr("/?#", start[authority_size])) {
    ++authority_size;
  }
  if (!authority_size || authority_size >= sizeof(uri->authority)) {
    return HTTP_INVALID_URI;
  }
  memcpy(uri->authority, start, authority_size);
  uri->authority[authority_size] = 0;
  if (strchr(uri->authority, '@') || strchr(uri->authority, '[') ||
      strchr(uri->authority, ']')) {
    return HTTP_INVALID_URI;
  }
  const char *colon = strchr(uri->authority, ':');
  size_t host_size = colon ? (size_t)(colon - uri->authority) : authority_size;
  if (!host_size || host_size >= sizeof(uri->host)) {
    return HTTP_INVALID_URI;
  }
  memcpy(uri->host, start, host_size);
  uri->host[host_size] = 0;
  unsigned port = 80;
  if (colon && (!udp_parse_number(colon + 1, UINT16_MAX, &port) || !port)) {
    return HTTP_INVALID_URI;
  }
  uri->port = port;
  uint32_t address;
  struct dns_name name;
  if (!udp_parse_address(uri->host, &address) && !dns_name_from_text(uri->host, &name)) {
    return HTTP_INVALID_URI;
  }
  const char *path = start + authority_size;
  const char *fragment = strchr(path, '#');
  if (fragment && strchr(fragment + 1, '#')) {
    return HTTP_INVALID_URI;
  }
  for (const char *byte = path; *byte; ++byte) {
    if (*byte == '[' || *byte == ']') {
      return HTTP_INVALID_URI;
    }
  }
  size_t path_size = fragment ? (size_t)(fragment - path) : strlen(path);
  size_t prefix = !path_size || *path == '?';
  if (prefix) {
    uri->target[0] = '/';
  }
  memcpy(uri->target + prefix, path, path_size);
  uri->target[prefix + path_size] = 0;
  return HTTP_OK;
}
