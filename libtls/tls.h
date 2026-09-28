#ifndef USERSPACE_TLS_H
#define USERSPACE_TLS_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLS_ALLOCATION_MAX (2 * 1024 * 1024)
#define TLS_ENTROPY_NS UINT64_C(5000000000)
#define TLS_CLOSE_NS UINT64_C(100000000)

struct tls_runtime;
struct tls_connection;

struct tls_authority {
  handle_t random, clock;
};

enum tls_error {
  TLS_OK,
  TLS_BAD_STATE,
  TLS_TRUST_ERROR,
  TLS_CERTIFICATE_ERROR,
  TLS_PROTOCOL_ERROR,
  TLS_TRUNCATED,
  TLS_UNSUPPORTED,
  TLS_LIMIT,
  TLS_QUOTA,
  TLS_NO_MEMORY,
  TLS_CLOCK_ERROR,
  TLS_ENTROPY_ERROR,
  TLS_TRANSPORT_ERROR,
  TLS_DEADLINE,
};

struct tls_result {
  enum tls_error error;
  enum call_status native_status;
  int library_error;
  uint32_t verify_flags;
};

/* One runtime and one connection per process, with single-task ownership.
 * No startup-resource lookup. Each operation initializes its result and clears
 * active borrowed authority on return. Retain the first failed result when
 * cleanup uses another result. Setup seeds PSA under one five-second deadline.
 * The runtime owns parsed trust and crypto state until runtime_free. */
bool tls_runtime_create(const struct tls_authority *authority,
    struct tls_runtime **runtime, struct tls_result *result);

/* Charge input buffers and bookkeeping to the same cap as library allocations.
 * Deallocate all caller-owned buffers before freeing the runtime. Import PEM
 * from a buffer obtained with tls_allocate (including its terminating NUL)
 * with exactly one terminating NUL; every certificate must parse successfully.
 * A failed import poisons setup; release that runtime rather than using a
 * partially accepted bundle. Import public roots, then optional augmentation,
 * then freeze trust with runtime_ready. No subsequent imports are allowed. */
void *tls_allocate(struct tls_runtime *runtime, size_t size, struct tls_result *result);
void tls_deallocate(void *data);
bool tls_trust_import(struct tls_runtime *runtime, const unsigned char *pem,
    size_t length, struct tls_result *result);
bool tls_runtime_ready(struct tls_runtime *runtime, struct tls_result *result);
size_t tls_reserved(const struct tls_runtime *runtime);
size_t tls_peak(const struct tls_runtime *runtime);
void tls_runtime_free(struct tls_runtime *runtime);

/* Borrow a connected stream and explicit fetch authority until connection_free;
 * never abort, shut down or close that stream here. The caller supplies a DNS
 * reference name without a terminal dot or port, and the original absolute
 * fetch deadline. Open returns only after required chain/name/date verification.
 * Free the connection before its authority, stream or runtime becomes invalid. */
bool tls_connection_open(struct tls_runtime *runtime,
    const struct tls_authority *authority, handle_t stream, const char *name,
    uint64_t deadline_ns, struct tls_connection **connection, struct tls_result *result);

/* Successful nonempty transfers report positive short counts. EOF is true only
 * for authenticated close_notify; raw TCP EOF fails as truncation. Plaintext is
 * unavailable until verification succeeds. After a failed transfer, only free
 * the connection. A failed read leaves length zero; disregard its buffer. */
bool tls_read(struct tls_connection *connection, void *data, size_t capacity,
    size_t *length, bool *eof, struct tls_result *result);
bool tls_write(struct tls_connection *connection, const void *data, size_t size,
    size_t *accepted, struct tls_result *result);
/* At most 100 ms of the remaining original deadline; does not wait for peer
 * shutdown. Diagnostic failure after completed HTTP framing cannot invalidate
 * the retained response. No time remaining means a successful skipped attempt. */
bool tls_close_notify(struct tls_connection *connection, struct tls_result *result);
const char *tls_version(const struct tls_connection *connection);
const char *tls_ciphersuite(const struct tls_connection *connection);
void tls_connection_free(struct tls_connection *connection);
const char *tls_error_name(enum tls_error error);

#endif
