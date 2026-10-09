#ifndef USERSPACE_HTTP_INTERNAL_H
#define USERSPACE_HTTP_INTERNAL_H

#include "http.h"
#include <stdbool.h>

enum http_error http_parse_uri(const char *text, struct http_uri *uri);
bool http_equal(const char *value, size_t length, const char *literal);
bool http_token(unsigned char byte);

#endif
