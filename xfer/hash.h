#ifndef XFER_HASH_H
#define XFER_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <abi/handle.h>

bool hash_init(handle_t clock, handle_t random);
void hash_close(void);
bool file_digest(const void *bytes, size_t size, char digest[65]);
bool digest_valid(const char *digest);
bool digest_equal(const char *expected, const char *actual);

#endif
