#ifndef USERSPACE_COMMON_UDP_H
#define USERSPACE_COMMON_UDP_H

#include <stdbool.h>
#include <udp.h>

/* Numeric dotted decimal and unsigned decimal only; no DNS or base prefixes. */
bool udp_parse_address(const char *text, uint32_t *address);
bool udp_parse_number(const char *text, unsigned maximum, unsigned *value);
enum call_status udp_deadline(handle_t clock, uint64_t interval, uint64_t *deadline);
const char *udp_error(enum call_status status);

#endif
