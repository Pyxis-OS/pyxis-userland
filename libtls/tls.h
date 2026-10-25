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

enum tls_step_result {
  TLS_STEP_PROGRESS,
  TLS_STEP_EOF,
  TLS_STEP_WANT_READ,
  TLS_STEP_WANT_WRITE,
  TLS_STEP_FAILED,
};

struct tls_result {
  enum tls_error error;
  enum call_status native_status;
  int library_error;
  uint32_t verify_flags;
  bool peer_alert_present;
  unsigned peer_alert;
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
 * fetch deadline. Prepare allocates/configures TLS and copies the reference name
 * without network I/O; the connection remains unverified. Open drives preparation
 * and the handshake synchronously, returning only after required verification.
 * Free the connection before its authority, stream or runtime becomes invalid. */
bool tls_connection_prepare(struct tls_runtime *runtime,
    const struct tls_authority *authority, handle_t stream, const char *name,
    uint64_t deadline_ns, struct tls_connection **connection, struct tls_result *result);
bool tls_connection_open(struct tls_runtime *runtime,
    const struct tls_authority *authority, handle_t stream, const char *name,
    uint64_t deadline_ns, struct tls_connection **connection, struct tls_result *result);

/* One library handshake state step using native TRY I/O, without a network
 * readiness wait. PROGRESS with complete false requires another scheduled step;
 * WANT_READ/WRITE requires the indicated stream readiness. Only PROGRESS with
 * complete true authorizes plaintext transfers, after chain/name/date and ALPN
 * verification. FAILED leaves complete false and permits only connection_free.
 * Do not call again after completion. Clock queries, allocation and certificate/
 * crypto work remain synchronous. The entropy callback may wait for native RNG
 * under the earlier of the original deadline and its five-second bound; this
 * API does not make that phase responsive. A state step may make several TRY
 * calls. Free may skip close_notify without wire I/O; retain skipped shutdown as
 * not attempted rather than a successful shutdown diagnostic. */
enum tls_step_result tls_handshake_step(struct tls_connection *connection,
    bool *complete, struct tls_result *result);

/* Successful nonempty transfers report positive short counts. EOF is true only
 * for authenticated close_notify; raw TCP EOF fails as truncation. Plaintext is
 * unavailable until verification succeeds. After a failed transfer, only free
 * the connection. A failed read leaves length zero; disregard its buffer. */
bool tls_read(struct tls_connection *connection, void *data, size_t capacity,
    size_t *length, bool *eof, struct tls_result *result);
bool tls_write(struct tls_connection *connection, const void *data, size_t size,
    size_t *accepted, struct tls_result *result);
/* At most one library read/write call using native TRY I/O, without a network
 * readiness wait. PROGRESS reports a positive short count; EOF is authenticated close_notify
 * on read only. WANT reports zero count and TLS_OK: wait for the indicated stream
 * readiness under the original deadline, then retry. FAILED reports zero count;
 * disregard read storage and only free the connection. A step can make several
 * native TRY calls while processing buffered TLS records. No TCP authority beyond
 * the borrowed stream's READ/WRITE is required.
 *
 * After write WANT, keep the exact plaintext pointer and size alive and unchanged
 * and resume with identical arguments until PROGRESS, tls_stop_write or
 * connection_free. The encrypted record remains owned by Mbed TLS; do not replay
 * the plaintext. Reads
 * may interleave, but any read-side BIO send while a write is pending fails with
 * TLS_UNSUPPORTED before native I/O (including a TLS 1.2 renegotiation alert).
 * A read WANT_WRITE owns buffered protocol output: resume that read before any
 * new application write. The retry first flushes that existing protocol record
 * through the public library alert API, then reads if the flush completes.
 * close_notify refuses pending application or read-side output without sending.
 * These rules also apply when mixing blocking and step
 * operations. Ownership remains single-task. */
enum tls_step_result tls_read_step(struct tls_connection *connection, void *data,
    size_t capacity, size_t *length, struct tls_result *result);
enum tls_step_result tls_write_step(struct tls_connection *connection, const void *data,
    size_t size, size_t *accepted, struct tls_result *result);
/* Irreversibly stop application writes without wire I/O or a clock check. This
 * idempotent local operation releases any saved plaintext pointer and size, so
 * the caller may release that storage. Encrypted application output stays owned
 * by the library until connection_free: reads may continue, but a read-side send
 * still fails with TLS_UNSUPPORTED if application output remains pending. Future
 * write and close_notify calls fail without sending. Only use on a live verified
 * connection; failed transfers still permit only connection_free. */
bool tls_stop_write(struct tls_connection *connection, struct tls_result *result);
/* At most 100 ms of the remaining original deadline; does not wait for peer
 * shutdown. Diagnostic failure after completed HTTP framing cannot invalidate
 * the retained response. No time remaining means a successful skipped attempt. */
bool tls_close_notify(struct tls_connection *connection, struct tls_result *result);
const char *tls_version(const struct tls_connection *connection);
const char *tls_ciphersuite(const struct tls_connection *connection);
void tls_connection_free(struct tls_connection *connection);
const char *tls_error_name(enum tls_error error);
/* Bounded local diagnostic; never includes certificates, peer text or credentials. */
void tls_result_detail(const struct tls_result *result, char *text, size_t capacity);

#endif
