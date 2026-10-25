/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <sha256.h>
#include <string.h>

void sha256_init(struct sha256_context *context)
{
  *context = (struct sha256_context){.state = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  }};
}

bool sha256_update(struct sha256_context *context, const void *bytes, size_t size)
{
  if (context->failed || context->finished) {
    return false;
  }
  if ((!bytes && size) || size > UINT64_MAX / 8 - context->bytes) {
    context->failed = true;
    return false;
  }
  const unsigned char *input = bytes;
  size_t used = (size_t)(context->bytes % SHA256_BLOCK_BYTES);
  context->bytes += size;
  if (used && size) {
    size_t added = SHA256_BLOCK_BYTES - used;
    if (added > size) {
      added = size;
    }
    memcpy(context->buffer + used, input, added);
    used += added;
    input += added;
    size -= added;
    if (used < SHA256_BLOCK_BYTES) {
      return true;
    }
    sha256_compress(context->state, context->buffer, 1);
  }
  size_t blocks = size / SHA256_BLOCK_BYTES;
  if (blocks) {
    sha256_compress(context->state, input, blocks);
    size_t consumed = blocks * SHA256_BLOCK_BYTES;
    input += consumed;
    size -= consumed;
  }
  if (size) {
    memcpy(context->buffer, input, size);
  }
  return true;
}

bool sha256_finish(struct sha256_context *context,
    unsigned char digest[SHA256_DIGEST_BYTES])
{
  if (context->failed || context->finished) {
    return false;
  }
  if (!digest) {
    context->failed = true;
    return false;
  }
  uint64_t bits = context->bytes * 8;
  size_t used = (size_t)(context->bytes % SHA256_BLOCK_BYTES);
  context->buffer[used++] = 0x80;
  if (used > SHA256_BLOCK_BYTES - 8) {
    memset(context->buffer + used, 0, SHA256_BLOCK_BYTES - used);
    sha256_compress(context->state, context->buffer, 1);
    used = 0;
  }
  memset(context->buffer + used, 0, SHA256_BLOCK_BYTES - 8 - used);
  for (size_t i = 0; i < 8; ++i) {
    context->buffer[SHA256_BLOCK_BYTES - 1 - i] = (unsigned char)(bits >> (8 * i));
  }
  sha256_compress(context->state, context->buffer, 1);
  for (size_t i = 0; i < 8; ++i) {
    digest[4 * i] = (unsigned char)(context->state[i] >> 24);
    digest[4 * i + 1] = (unsigned char)(context->state[i] >> 16);
    digest[4 * i + 2] = (unsigned char)(context->state[i] >> 8);
    digest[4 * i + 3] = (unsigned char)context->state[i];
  }
  volatile unsigned char *clear = (volatile unsigned char *)context;
  for (size_t i = 0; i < sizeof(*context); ++i) {
    clear[i] = 0;
  }
  context->finished = true;
  return true;
}
