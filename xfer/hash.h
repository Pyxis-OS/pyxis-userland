#ifndef XFER_HASH_H
#define XFER_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <abi/handle.h>

bool hash_init(handle_t clock, handle_t random);
void hash_close(void);
/* One incremental SHA-256 at a time; a failed update or finish ends it. */
bool digest_begin(void);
bool digest_update(const void *bytes, size_t size);
bool digest_finish(char digest[65]);
void digest_abort(void);
bool digest_valid(const char *digest);
bool digest_equal(const char *expected, const char *actual);

#endif
