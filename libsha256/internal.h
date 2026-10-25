#ifndef PYXIS_SHA256_INTERNAL_H
#define PYXIS_SHA256_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const uint32_t sha256_round_constants[64];
void sha256_portable_blocks(uint32_t state[8], const unsigned char *blocks, size_t count);
#if defined(__x86_64__)
bool sha256_x86_available(void);
void sha256_x86_blocks(uint32_t state[8], const unsigned char *blocks, size_t count);
#endif

#endif
