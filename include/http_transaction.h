#ifndef USERSPACE_HTTP_TRANSACTION_H
#define USERSPACE_HTTP_TRANSACTION_H

#include <abi/http_transaction.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdbool.h>
#include <pyxis/response.h>

/* An explicitly owned native value. Copies/delegation use narrower grant copies,
 * not serialization or ambient inheritance. Close releases only these refs;
 * ABORT is an explicit operation. No close/flush commits a transaction. */
struct http_prepared {
  handle_t control, body, observation, response;
  uint64_t method, delivery, trust_flags;
  bool commit_attempted;
};

enum call_status http_transaction_prepare(handle_t provider, const char *uri,
    uint64_t method, uint64_t flags, const char *headers, size_t headers_size,
    uint64_t deadline_ns, struct http_prepared *prepared);
/* URI resolution borrows only declared bindings and namespace lookup. It never
 * provisions providers or acquires network/trust authority. json+ selects JSON
 * request headers; explicit preparation does not redirect or filter HTTP status. */
struct path_context;
enum call_status http_transaction_prepare_uri(const struct path_context *context,
    const char *uri, uint64_t method, uint64_t flags, const char *headers,
    size_t headers_size, uint64_t deadline_ns, struct http_prepared *prepared);
/* Anonymous ordinary GET: one deadline and aggregate framing budget, at most
 * ten redirects, successful 200/204 only. Owns returned prepared facets. */
enum call_status http_transaction_open_uri(const struct path_context *context,
    const char *uri, uint64_t deadline_ns, struct http_prepared *prepared,
    struct pyxis_response_info *info);
/* Explicit continuity for a redirect chain. NULL initializes origin/budget. */
enum call_status http_transaction_prepare_context(handle_t provider, const char *uri,
    uint64_t method, uint64_t flags, const char *headers, size_t headers_size,
    uint64_t deadline_ns, const struct http_prepare_context *context,
    struct http_prepared *prepared);
enum call_status http_transaction_metadata(handle_t observation, uint64_t deadline_ns,
    struct http_response_metadata *metadata);

enum call_status http_transaction_commit(struct http_prepared *prepared,
    uint64_t deadline_ns, struct http_commit_reply *reply);
enum call_status http_transaction_abort(handle_t control, uint64_t deadline_ns);
enum call_status http_transaction_status(handle_t observation, uint64_t deadline_ns,
    struct http_transaction_status *status);
enum call_status http_prepared_close(struct http_prepared *prepared);

#endif
