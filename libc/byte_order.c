#include <arpa/inet.h>

/* Pyxis x86-64 has little-endian host order; network order is big-endian. */
uint32_t htonl(uint32_t value)
{
  return __builtin_bswap32(value);
}

uint16_t htons(uint16_t value)
{
  return __builtin_bswap16(value);
}

uint32_t ntohl(uint32_t value)
{
  return htonl(value);
}

uint16_t ntohs(uint16_t value)
{
  return htons(value);
}
