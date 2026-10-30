#ifndef LIBC_ARPA_INET_H
#define LIBC_ARPA_INET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t in_addr_t;

/* Byte-order conversions and IPv4 text parsing. No socket or address
 * formatting interface is provided by this header. */
uint32_t htonl(uint32_t value);
uint16_t htons(uint16_t value);
uint32_t ntohl(uint32_t value);
uint16_t ntohs(uint16_t value);

/* POSIX dotted notation a.b.c.d, a.b.c, a.b or a; each part decimal, octal
 * (leading 0) or hexadecimal (0x). Returns network order, or (in_addr_t)-1
 * for invalid text, which is also the value of 255.255.255.255. */
in_addr_t inet_addr(const char *text);

#ifdef __cplusplus
}
#endif

#endif
