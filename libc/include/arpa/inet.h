#ifndef LIBC_ARPA_INET_H
#define LIBC_ARPA_INET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only these host/network byte-order conversions are implemented. No socket,
 * address parsing or address formatting interface is provided by this header. */
uint32_t htonl(uint32_t value);
uint16_t htons(uint16_t value);
uint32_t ntohl(uint32_t value);
uint16_t ntohs(uint16_t value);

#ifdef __cplusplus
}
#endif

#endif
