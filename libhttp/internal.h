#ifndef USERSPACE_HTTP_INTERNAL_H
#define USERSPACE_HTTP_INTERNAL_H

#include "http.h"
#include <stdbool.h>

enum http_error http_parse_uri(const char *text, struct http_uri *uri);
bool http_equal(const char *value, size_t length, const char *literal);
bool http_token(unsigned char byte);

/* Snapshot compatibility: bound header read-ahead independently of body
 * delivery, and account bytes discarded at a redirect handoff. */
struct http_stream *http_get_open_prefetch(const struct http_client *client,
    const char *uri, uint64_t deadline_ns, const struct http_response_limits *limits,
    size_t header_receive_max, struct http_response *response);
size_t http_stream_read_ahead(const struct http_stream *stream);
bool http_stream_check_deadline(struct http_stream *stream);

#endif
