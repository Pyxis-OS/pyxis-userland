#ifndef USERSPACE_HTTP_INTERNAL_H
#define USERSPACE_HTTP_INTERNAL_H

#include "http.h"
#include <stdbool.h>

struct http_uri {
  char host[256];
  char authority[262];
  char target[HTTP_URI_MAX + 2];
  uint16_t port;
};

enum http_error http_parse_uri(const char *text, struct http_uri *uri);
bool http_equal(const char *value, size_t length, const char *literal);
bool http_token(unsigned char byte);

#endif
