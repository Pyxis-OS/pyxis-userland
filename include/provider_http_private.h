#ifndef USERSPACE_PROVIDER_HTTP_PRIVATE_H
#define USERSPACE_PROVIDER_HTTP_PRIVATE_H

#include <stdbool.h>

/* Isolated spelling classification for interim consumers until B8. Generic
 * typed OPEN never falls back to a private HTTP operation. */
bool provider_http_uri(const char *path);

#endif
