#ifndef PYXIS_SHA256_H
#define PYXIS_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_BYTES 32
#define SHA256_BLOCK_BYTES 64

/* Initialize before use. The caller owns the context and all byte buffers;
 * one context is not concurrently mutable. Input/output buffers must not overlap
 * the context. No allocation or authority is used. */
struct sha256_context {
  uint64_t bytes;
  uint32_t state[8];
  unsigned char buffer[SHA256_BLOCK_BYTES];
  bool failed, finished;
};

void sha256_init(struct sha256_context *context);
/* A null input is allowed only for zero bytes. Refuse lengths beyond SHA-256's
 * 64-bit bit count; a failed update poisons the context until initialization. */
bool sha256_update(struct sha256_context *context, const void *bytes, size_t size);
/* Finish once; subsequent updates/finishes refuse until initialization. */
bool sha256_finish(struct sha256_context *context,
    unsigned char digest[SHA256_DIGEST_BYTES]);

/* Compression-only integration for existing SHA-224/SHA-256 providers. STATE
 * is eight initialized host-endian words; BLOCKS names COUNT complete 64-byte
 * blocks without overlap with STATE. No initialization or padding is applied.
 * A zero count permits a null BLOCKS pointer. */
void sha256_compress(uint32_t state[8], const unsigned char *blocks, size_t count);

#ifdef __cplusplus
}
#endif

#endif
