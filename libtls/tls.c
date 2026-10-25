#include "tls.h"
#include <clock.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>

struct tls_runtime {
  mbedtls_x509_crt trust;
  mbedtls_ssl_config config;
  struct tls_connection *connection;
  size_t reserved, peak;
  bool ready, poisoned;
};

struct tls_connection {
  mbedtls_ssl_context ssl;
  struct tls_runtime *runtime;
  struct tls_authority authority;
  handle_t stream;
  uint64_t deadline;
  const void *pending_write;
  size_t pending_write_size;
  bool verified, failed, eof, closed;
  bool application_output_pending, read_output_pending, write_stopped;
};

/* Keep the payload aligned as malloc would; bookkeeping is charged too. */
union allocation_header {
  struct {
    struct tls_runtime *runtime;
    size_t charged;
  } account;
  max_align_t alignment;
};

static struct tls_runtime *owner;
static struct {
  const struct tls_authority *authority;
  uint64_t deadline;
  struct tls_result *result;
  bool nonblocking, reading;
} operation;
static struct tls_result *allocation_result;
static const char *const protocols[] = {"http/1.1", NULL};
static const char pem_begin[] = "-----BEGIN CERTIFICATE-----";
static const char pem_end[] = "-----END CERTIFICATE-----";

static bool fail(struct tls_result *result, enum tls_error error)
{
  if (result->error == TLS_OK) {
    result->error = error;
  }
  return false;
}

static bool native_failure(enum tls_error error, enum call_status status)
{
  if (operation.result && operation.result->error == TLS_OK) {
    operation.result->native_status = status;
    fail(operation.result, error);
  }
  return false;
}

static bool monotonic(uint64_t *now)
{
  if (!operation.authority) {
    return native_failure(TLS_CLOCK_ERROR, CALL_DENIED);
  }
  enum call_status status = clock_now(operation.authority->clock, now);
  return status == CALL_OK || native_failure(TLS_CLOCK_ERROR, status);
}

static bool check_deadline(void)
{
  if (operation.result && operation.result->error != TLS_OK) {
    return false;
  }
  uint64_t now;
  return monotonic(&now) &&
      (now < operation.deadline || native_failure(TLS_DEADLINE, CALL_TIMED_OUT));
}

static bool check_utc(void)
{
  struct clock_wall_reading reading;
  enum call_status status = clock_wall_now(operation.authority->clock, &reading);
  return status == CALL_OK || native_failure(TLS_CLOCK_ERROR, status);
}

static bool begin(const struct tls_authority *authority, uint64_t deadline,
    struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (operation.result || !authority) {
    return fail(result, TLS_BAD_STATE);
  }
  operation.authority = authority;
  operation.deadline = deadline;
  operation.result = result;
  allocation_result = result;
  if (authority->clock == HANDLE_INVALID) {
    return native_failure(TLS_CLOCK_ERROR, CALL_DENIED);
  }
  if (authority->random == HANDLE_INVALID) {
    return native_failure(TLS_ENTROPY_ERROR, CALL_DENIED);
  }
  return check_deadline();
}

static void end(void)
{
  operation.authority = NULL;
  operation.deadline = 0;
  operation.result = NULL;
  operation.nonblocking = false;
  operation.reading = false;
  allocation_result = NULL;
}

static void *counted_calloc(size_t count, size_t size)
{
  if (!owner) {
    return NULL;
  }
  if ((size && count > SIZE_MAX / size) ||
      count * size > SIZE_MAX - sizeof(union allocation_header)) {
    if (allocation_result) {
      fail(allocation_result, TLS_QUOTA);
    }
    return NULL;
  }
  size_t charged = count * size + sizeof(union allocation_header);
  if (charged > TLS_ALLOCATION_MAX - owner->reserved) {
    if (allocation_result) {
      fail(allocation_result, TLS_QUOTA);
    }
    return NULL;
  }
  union allocation_header *header = calloc(1, charged);
  if (!header) {
    if (allocation_result) {
      fail(allocation_result, TLS_NO_MEMORY);
    }
    return NULL;
  }
  header->account.runtime = owner;
  header->account.charged = charged;
  owner->reserved += charged;
  if (owner->reserved > owner->peak) {
    owner->peak = owner->reserved;
  }
  return header + 1;
}

void tls_deallocate(void *data)
{
  if (!data) {
    return;
  }
  union allocation_header *header = (union allocation_header *)data - 1;
  header->account.runtime->reserved -= header->account.charged;
  free(header);
}

static mbedtls_time_t utc_time(mbedtls_time_t *output)
{
  struct clock_wall_reading reading;
  mbedtls_time_t value = -1;
  if (!operation.authority) {
    native_failure(TLS_CLOCK_ERROR, CALL_DENIED);
  } else {
    enum call_status status = clock_wall_now(operation.authority->clock, &reading);
    if (status == CALL_OK) {
      value = reading.seconds;
    } else {
      native_failure(TLS_CLOCK_ERROR, status);
    }
  }
  if (output) {
    *output = value;
  }
  return value;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
  uint64_t now;
  return monotonic(&now) ? (mbedtls_ms_time_t)(now / UINT64_C(1000000)) : 0;
}

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
    size_t *estimate_bits, unsigned char *output, size_t output_size)
{
  *estimate_bits = 0;
  if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) {
    native_failure(TLS_ENTROPY_ERROR, CALL_BAD_OPERATION);
    return PSA_ERROR_NOT_SUPPORTED;
  }
  uint64_t now;
  if (!operation.authority || !monotonic(&now)) {
    native_failure(TLS_ENTROPY_ERROR, CALL_DENIED);
    return PSA_ERROR_INSUFFICIENT_ENTROPY;
  }
  uint64_t deadline = operation.deadline;
  if (now <= UINT64_MAX - TLS_ENTROPY_NS && now + TLS_ENTROPY_NS < deadline) {
    deadline = now + TLS_ENTROPY_NS;
  }
  size_t gathered = 0;
  while (gathered < output_size) {
    if (!check_deadline() || !monotonic(&now) || now >= deadline) {
      native_failure(TLS_DEADLINE, CALL_TIMED_OUT);
      goto failed;
    }
    size_t size = output_size - gathered;
    if (size > RANDOM_MAX_BYTES) {
      size = RANDOM_MAX_BYTES;
    }
    enum call_status status = random_read(operation.authority->random,
        output + gathered, size, deadline);
    if (status != CALL_OK) {
      native_failure(status == CALL_TIMED_OUT ? TLS_DEADLINE : TLS_ENTROPY_ERROR, status);
      goto failed;
    }
    gathered += size;
  }
  if (!check_deadline() || !monotonic(&now) || now >= deadline) {
    native_failure(TLS_DEADLINE, CALL_TIMED_OUT);
    goto failed;
  }
  if (output_size > SIZE_MAX / 8) {
    native_failure(TLS_ENTROPY_ERROR, CALL_LIMIT);
    goto failed;
  }
  *estimate_bits = output_size * 8;
  return 0;

failed:
  memset(output, 0, output_size);
  return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

static bool library_result(struct tls_result *result, int code, enum tls_error fallback)
{
  result->library_error = code;
  if (code == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE && owner && owner->connection) {
    int alert = mbedtls_ssl_get_fatal_alert(&owner->connection->ssl);
    if (alert >= 0) {
      result->peer_alert_present = true;
      result->peer_alert = (unsigned)alert;
    }
  }
  if (result->error != TLS_OK) {
    return false;
  }
  if (!code) {
    return true;
  }
  if (code == PSA_ERROR_INSUFFICIENT_MEMORY) {
    return fail(result, TLS_NO_MEMORY);
  }
  if (code == PSA_ERROR_BUFFER_TOO_SMALL) {
    return fail(result, TLS_LIMIT);
  }
  if (code == MBEDTLS_ERR_SSL_CONN_EOF) {
    return fail(result, TLS_TRUNCATED);
  }
  if (code == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || code == MBEDTLS_ERR_SSL_BAD_CERTIFICATE) {
    return fail(result, TLS_CERTIFICATE_ERROR);
  }
  return fail(result, fallback);
}

bool tls_runtime_create(const struct tls_authority *authority,
    struct tls_runtime **runtime, struct tls_result *result)
{
  *result = (struct tls_result){0};
  *runtime = NULL;
  if (owner || operation.result || !authority) {
    return fail(result, TLS_BAD_STATE);
  }
  uint64_t now;
  enum call_status status = clock_now(authority->clock, &now);
  if (status != CALL_OK) {
    result->native_status = status;
    return fail(result, TLS_CLOCK_ERROR);
  }
  if (now > UINT64_MAX - TLS_ENTROPY_NS) {
    return fail(result, TLS_LIMIT);
  }
  size_t charged = sizeof(union allocation_header) + sizeof(struct tls_runtime);
  union allocation_header *header = calloc(1, charged);
  if (!header) {
    return fail(result, TLS_NO_MEMORY);
  }
  owner = (struct tls_runtime *)(header + 1);
  header->account.runtime = owner;
  header->account.charged = charged;
  owner->reserved = owner->peak = charged;
  mbedtls_x509_crt_init(&owner->trust);
  mbedtls_ssl_config_init(&owner->config);
  mbedtls_platform_set_calloc_free(counted_calloc, tls_deallocate);
  mbedtls_platform_set_time(utc_time);
  bool success = begin(authority, now + TLS_ENTROPY_NS, result);
  if (success) {
    int code = psa_crypto_init();
    success = library_result(result, code, TLS_ENTROPY_ERROR) && check_deadline();
  }
  end();
  if (!success) {
    tls_runtime_free(owner);
    return false;
  }
  *runtime = owner;
  return true;
}

void *tls_allocate(struct tls_runtime *runtime, size_t size, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!runtime || runtime != owner || operation.result) {
    fail(result, TLS_BAD_STATE);
    return NULL;
  }
  allocation_result = result;
  void *data = counted_calloc(1, size);
  allocation_result = NULL;
  return data;
}

static size_t certificate_count(const mbedtls_x509_crt *chain)
{
  size_t count = 0;
  for (const mbedtls_x509_crt *crt = chain; crt; crt = crt->next) {
    if (crt->version) {
      ++count;
    }
  }
  return count;
}

bool tls_trust_import(struct tls_runtime *runtime, const unsigned char *pem,
    size_t length, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!runtime || runtime != owner || runtime->ready || runtime->poisoned || operation.result) {
    return fail(result, TLS_BAD_STATE);
  }
  runtime->poisoned = true;
  if (!pem || length < 2 || pem[length - 1] || memchr(pem, 0, length - 1)) {
    return fail(result, TLS_TRUST_ERROR);
  }
  /* The upstream bundle parser can skip malformed blocks and report success.
   * Validate delimiters and require every block to add a parsed certificate. */
  const char *cursor = (const char *)pem;
  size_t count = 0;
  for (;;) {
    const char *start = strstr(cursor, pem_begin);
    const char *stop = strstr(cursor, pem_end);
    if (!start) {
      if (stop || !count) {
        return fail(result, TLS_TRUST_ERROR);
      }
      break;
    }
    if (!stop || stop < start) {
      return fail(result, TLS_TRUST_ERROR);
    }
    const char *nested = strstr(start + sizeof(pem_begin) - 1, pem_begin);
    if (nested && nested < stop) {
      return fail(result, TLS_TRUST_ERROR);
    }
    cursor = stop + sizeof(pem_end) - 1;
    ++count;
  }
  size_t previous = certificate_count(&runtime->trust);
  allocation_result = result;
  int code = mbedtls_x509_crt_parse(&runtime->trust, pem, length);
  allocation_result = NULL;
  if (!library_result(result, code, TLS_TRUST_ERROR)) {
    return false;
  }
  if (certificate_count(&runtime->trust) != previous + count) {
    return fail(result, TLS_TRUST_ERROR);
  }
  runtime->poisoned = false;
  return true;
}

bool tls_runtime_ready(struct tls_runtime *runtime, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!runtime || runtime != owner || runtime->ready || runtime->poisoned ||
      !runtime->trust.version || operation.result) {
    return fail(result, TLS_BAD_STATE);
  }
  allocation_result = result;
  int code = mbedtls_ssl_config_defaults(&runtime->config, MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (!code) {
    mbedtls_ssl_conf_authmode(&runtime->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&runtime->config, &runtime->trust, NULL);
    mbedtls_ssl_conf_min_tls_version(&runtime->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&runtime->config, MBEDTLS_SSL_VERSION_TLS1_3);
    code = mbedtls_ssl_conf_alpn_protocols(&runtime->config, protocols);
  }
  allocation_result = NULL;
  if (!library_result(result, code, TLS_PROTOCOL_ERROR)) {
    runtime->poisoned = true;
    return false;
  }
  runtime->ready = true;
  return true;
}

size_t tls_reserved(const struct tls_runtime *runtime)
{
  return runtime->reserved;
}

size_t tls_peak(const struct tls_runtime *runtime)
{
  return runtime->peak;
}

void tls_runtime_free(struct tls_runtime *runtime)
{
  if (!runtime || runtime != owner) {
    return;
  }
  tls_connection_free(runtime->connection);
  mbedtls_ssl_config_free(&runtime->config);
  mbedtls_x509_crt_free(&runtime->trust);
  mbedtls_psa_crypto_free();
  tls_deallocate(runtime);
  owner = NULL;
}

static bool transport_deadline(uint64_t maximum_wait, uint64_t *deadline)
{
  uint64_t now;
  if ((operation.result && operation.result->error != TLS_OK) || !monotonic(&now)) {
    return false;
  }
  if (now >= operation.deadline) {
    return native_failure(TLS_DEADLINE, CALL_TIMED_OUT);
  }
  *deadline = operation.deadline;
  if (maximum_wait < operation.deadline - now) {
    *deadline = now + maximum_wait;
  }
  return true;
}

static int send_bytes(void *context, const unsigned char *data, size_t size)
{
  struct tls_connection *connection = context;
  /* A read-side alert must not flush an unfinished application record: the
   * subsequent application write retry would encrypt the plaintext again. */
  if (operation.reading && connection->application_output_pending) {
    native_failure(TLS_UNSUPPORTED, CALL_BAD_OPERATION);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  if (!check_deadline()) {
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  if (size > TCP_WRITE_MAX_BYTES) {
    size = TCP_WRITE_MAX_BYTES;
  }
  struct tcp_write_reply reply;
  enum call_status status;
  if (operation.nonblocking) {
    status = tcp_try_write(connection->stream, data, size, &reply);
  } else {
    uint64_t deadline;
    if (!transport_deadline(TCP_WRITE_MAX_WAIT_NS, &deadline)) {
      return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    status = tcp_write(connection->stream, data, size, deadline, &reply);
    if (status == CALL_TIMED_OUT) {
      if (!check_deadline()) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
      }
      status = tcp_try_write(connection->stream, data, size, &reply);
    }
  }
  if (status == CALL_WOULD_BLOCK) {
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  }
  if (status != CALL_OK || !reply.length) {
    native_failure(status == CALL_TIMED_OUT ? TLS_DEADLINE : TLS_TRANSPORT_ERROR,
        status == CALL_OK ? CALL_IO : status);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return reply.length;
}

static int receive_bytes(void *context, unsigned char *data, size_t size)
{
  struct tls_connection *connection = context;
  if (!check_deadline()) {
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  if (size > TCP_READ_MAX_BYTES) {
    size = TCP_READ_MAX_BYTES;
  }
  struct tcp_read_reply reply;
  enum call_status status;
  if (operation.nonblocking) {
    status = tcp_try_read(connection->stream, data, size, &reply);
  } else {
    uint64_t deadline;
    if (!transport_deadline(TCP_READ_MAX_WAIT_NS, &deadline)) {
      return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    status = tcp_read(connection->stream, data, size, deadline, &reply);
    if (status == CALL_TIMED_OUT) {
      if (!check_deadline()) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
      }
      status = tcp_try_read(connection->stream, data, size, &reply);
    }
  }
  if (status == CALL_WOULD_BLOCK) {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  if (status != CALL_OK) {
    native_failure(status == CALL_TIMED_OUT ? TLS_DEADLINE : TLS_TRANSPORT_ERROR, status);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return reply.length;
}

static bool retryable(int code)
{
  return code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE ||
      code == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS;
}

bool tls_connection_prepare(struct tls_runtime *runtime,
    const struct tls_authority *authority, handle_t stream, const char *name,
    uint64_t deadline_ns, struct tls_connection **connection, struct tls_result *result)
{
  *result = (struct tls_result){0};
  *connection = NULL;
  if (!runtime || runtime != owner || !runtime->ready || runtime->connection ||
      !name || !*name || stream == HANDLE_INVALID || operation.result) {
    return fail(result, TLS_BAD_STATE);
  }
  bool success = begin(authority, deadline_ns, result) && check_utc();
  struct tls_connection *created = NULL;
  int code = 0;
  if (!success) {
    goto done;
  }
  created = counted_calloc(1, sizeof(*created));
  if (!created) {
    success = false;
    goto done;
  }
  created->runtime = runtime;
  created->authority = *authority;
  created->stream = stream;
  created->deadline = deadline_ns;
  runtime->connection = created;
  mbedtls_ssl_init(&created->ssl);
  code = mbedtls_ssl_setup(&created->ssl, &runtime->config);
  if (!code) {
    code = mbedtls_ssl_set_hostname(&created->ssl, name);
  }
  if (code) {
    success = library_result(result, code, TLS_PROTOCOL_ERROR);
    goto done;
  }
  mbedtls_ssl_set_bio(&created->ssl, created, send_bytes, receive_bytes, NULL);
  success = check_deadline();

done:
  end();
  if (!success) {
    tls_connection_free(created);
    return false;
  }
  *connection = created;
  return true;
}

static enum tls_step_result handshake_transfer(struct tls_connection *connection,
    bool nonblocking, bool *complete, struct tls_result *result)
{
  *result = (struct tls_result){0};
  *complete = false;
  if (!connection || !owner || owner->connection != connection || connection->verified ||
      connection->failed || connection->closed || operation.result) {
    fail(result, TLS_BAD_STATE);
    return TLS_STEP_FAILED;
  }
  enum tls_step_result step = TLS_STEP_FAILED;
  if (!begin(&connection->authority, connection->deadline, result)) {
    goto done;
  }
  operation.nonblocking = nonblocking;
  int code = mbedtls_ssl_handshake_step(&connection->ssl);
  if (result->error != TLS_OK || !check_deadline()) {
    goto done;
  }
  if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE) {
    step = code == MBEDTLS_ERR_SSL_WANT_READ ? TLS_STEP_WANT_READ : TLS_STEP_WANT_WRITE;
    goto done;
  }
  if (code == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) {
    step = TLS_STEP_PROGRESS;
    goto done;
  }
  if (!library_result(result, code, TLS_PROTOCOL_ERROR)) {
    result->verify_flags = mbedtls_ssl_get_verify_result(&connection->ssl);
    goto done;
  }
  if (!mbedtls_ssl_is_handshake_over(&connection->ssl)) {
    step = TLS_STEP_PROGRESS;
    goto done;
  }
  result->verify_flags = mbedtls_ssl_get_verify_result(&connection->ssl);
  if (!check_utc() || !check_deadline()) {
    goto done;
  }
  if (result->verify_flags) {
    fail(result, TLS_CERTIFICATE_ERROR);
    goto done;
  }
  const char *protocol = mbedtls_ssl_get_alpn_protocol(&connection->ssl);
  if (protocol && strcmp(protocol, "http/1.1")) {
    fail(result, TLS_UNSUPPORTED);
    goto done;
  }
  connection->verified = true;
  *complete = true;
  step = TLS_STEP_PROGRESS;

done:
  connection->failed = step == TLS_STEP_FAILED;
  end();
  return step;
}

enum tls_step_result tls_handshake_step(struct tls_connection *connection,
    bool *complete, struct tls_result *result)
{
  return handshake_transfer(connection, true, complete, result);
}

bool tls_connection_open(struct tls_runtime *runtime,
    const struct tls_authority *authority, handle_t stream, const char *name,
    uint64_t deadline_ns, struct tls_connection **connection, struct tls_result *result)
{
  if (!tls_connection_prepare(runtime, authority, stream, name, deadline_ns,
      connection, result)) {
    return false;
  }
  bool complete = false;
  while (!complete) {
    if (handshake_transfer(*connection, false, &complete, result) == TLS_STEP_FAILED) {
      tls_connection_free(*connection);
      *connection = NULL;
      return false;
    }
  }
  return true;
}

static bool connection_begin(struct tls_connection *connection, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!connection || !owner || owner->connection != connection || !connection->verified ||
      connection->failed || connection->closed || operation.result) {
    return fail(result, TLS_BAD_STATE);
  }
  bool success = begin(&connection->authority, connection->deadline, result);
  connection->failed = !success;
  return success;
}

static enum tls_step_result transfer_result(struct tls_connection *connection,
    int code, size_t *count, bool reading, struct tls_result *result)
{
  enum tls_step_result step = TLS_STEP_FAILED;
  if (result->error == TLS_OK) {
    if (code > 0) {
      if (check_deadline()) {
        *count = code;
        step = TLS_STEP_PROGRESS;
      }
    } else if (reading && code == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      if (check_deadline()) {
        connection->eof = true;
        step = TLS_STEP_EOF;
      }
    } else if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE) {
      if (check_deadline()) {
        step = code == MBEDTLS_ERR_SSL_WANT_READ ? TLS_STEP_WANT_READ : TLS_STEP_WANT_WRITE;
      }
    } else {
      if (!code) {
        code = reading ? MBEDTLS_ERR_SSL_CONN_EOF : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
      }
      library_result(result, code, TLS_PROTOCOL_ERROR);
    }
  }
  connection->failed = step == TLS_STEP_FAILED;
  return step;
}

static enum tls_step_result read_transfer(struct tls_connection *connection,
    void *data, size_t capacity, size_t *length, bool nonblocking,
    struct tls_result *result)
{
  *length = 0;
  if (!connection_begin(connection, result)) {
    end();
    return TLS_STEP_FAILED;
  }
  if (connection->eof) {
    end();
    return TLS_STEP_EOF;
  }
  if (!data || !capacity) {
    connection->failed = true;
    fail(result, TLS_BAD_STATE);
    end();
    return TLS_STEP_FAILED;
  }
  operation.nonblocking = nonblocking;
  operation.reading = true;
  int code;
  do {
    code = 0;
    if (connection->read_output_pending) {
      /* Only a verified read with no pending application output can set this
       * flag, on BIO WANT_WRITE. Its record remains buffered: this public call
       * only flushes that record, and its alert arguments are unused. A stream
       * ssl_read retry does not itself flush a previous read-side alert. */
      code = mbedtls_ssl_send_alert_message(&connection->ssl,
          MBEDTLS_SSL_ALERT_LEVEL_WARNING, MBEDTLS_SSL_ALERT_MSG_NO_RENEGOTIATION);
      if (!code) {
        connection->read_output_pending = false;
      }
    }
    if (!code) {
      code = mbedtls_ssl_read(&connection->ssl, data, capacity);
    }
    if (code == MBEDTLS_ERR_SSL_WANT_WRITE && result->error == TLS_OK) {
      connection->read_output_pending = true;
    }
  } while (!nonblocking && retryable(code) && check_deadline());
  enum tls_step_result step = transfer_result(connection, code, length, true, result);
  end();
  return step;
}

static enum tls_step_result write_transfer(struct tls_connection *connection,
    const void *data, size_t size, size_t *accepted, bool nonblocking,
    struct tls_result *result)
{
  *accepted = 0;
  if (!connection_begin(connection, result)) {
    end();
    return TLS_STEP_FAILED;
  }
  if (!data || !size || connection->eof || connection->write_stopped ||
      connection->read_output_pending ||
      (connection->pending_write &&
       (connection->pending_write != data || connection->pending_write_size != size))) {
    connection->failed = true;
    fail(result, TLS_BAD_STATE);
    end();
    return TLS_STEP_FAILED;
  }
  connection->pending_write = data;
  connection->pending_write_size = size;
  connection->application_output_pending = true;
  operation.nonblocking = nonblocking;
  int code;
  do {
    code = mbedtls_ssl_write(&connection->ssl, data, size);
  } while (!nonblocking && retryable(code) && check_deadline());
  enum tls_step_result step = transfer_result(connection, code, accepted, false, result);
  if (step == TLS_STEP_PROGRESS) {
    connection->application_output_pending = false;
    connection->pending_write = NULL;
    connection->pending_write_size = 0;
  }
  end();
  return step;
}

enum tls_step_result tls_read_step(struct tls_connection *connection, void *data,
    size_t capacity, size_t *length, struct tls_result *result)
{
  return read_transfer(connection, data, capacity, length, true, result);
}

enum tls_step_result tls_write_step(struct tls_connection *connection, const void *data,
    size_t size, size_t *accepted, struct tls_result *result)
{
  return write_transfer(connection, data, size, accepted, true, result);
}

bool tls_read(struct tls_connection *connection, void *data, size_t capacity,
    size_t *length, bool *eof, struct tls_result *result)
{
  enum tls_step_result step = read_transfer(connection, data, capacity, length, false, result);
  *eof = step == TLS_STEP_EOF;
  return step == TLS_STEP_PROGRESS || step == TLS_STEP_EOF;
}

bool tls_write(struct tls_connection *connection, const void *data, size_t size,
    size_t *accepted, struct tls_result *result)
{
  return write_transfer(connection, data, size, accepted, false, result) == TLS_STEP_PROGRESS;
}

bool tls_stop_write(struct tls_connection *connection, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!connection || !owner || owner->connection != connection || !connection->verified ||
      connection->failed || operation.result) {
    return fail(result, TLS_BAD_STATE);
  }
  connection->write_stopped = true;
  connection->pending_write = NULL;
  connection->pending_write_size = 0;
  return true;
}

bool tls_close_notify(struct tls_connection *connection, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!connection || !owner || owner->connection != connection || !connection->verified ||
      connection->failed || operation.result) {
    return fail(result, TLS_BAD_STATE);
  }
  if (connection->write_stopped || connection->application_output_pending ||
      connection->read_output_pending) {
    connection->failed = true;
    return fail(result, TLS_BAD_STATE);
  }
  if (connection->closed) {
    return true;
  }
  uint64_t now;
  enum call_status status = clock_now(connection->authority.clock, &now);
  if (status != CALL_OK) {
    result->native_status = status;
    return fail(result, TLS_CLOCK_ERROR);
  }
  connection->closed = true;
  if (now >= connection->deadline) {
    return true;
  }
  uint64_t deadline = connection->deadline;
  if (now <= UINT64_MAX - TLS_CLOSE_NS && now + TLS_CLOSE_NS < deadline) {
    deadline = now + TLS_CLOSE_NS;
  }
  bool success = begin(&connection->authority, deadline, result);
  if (success) {
    int code;
    do {
      code = mbedtls_ssl_close_notify(&connection->ssl);
    } while (retryable(code) && check_deadline());
    success = library_result(result, code, TLS_PROTOCOL_ERROR) && check_deadline();
  }
  end();
  return success;
}

const char *tls_version(const struct tls_connection *connection)
{
  return connection->verified ? mbedtls_ssl_get_version(&connection->ssl) : NULL;
}

const char *tls_ciphersuite(const struct tls_connection *connection)
{
  return connection->verified ? mbedtls_ssl_get_ciphersuite(&connection->ssl) : NULL;
}

void tls_connection_free(struct tls_connection *connection)
{
  if (!connection) {
    return;
  }
  mbedtls_ssl_free(&connection->ssl);
  connection->runtime->connection = NULL;
  tls_deallocate(connection);
}

const char *tls_error_name(enum tls_error error)
{
  switch (error) {
  case TLS_OK: return "ok";
  case TLS_BAD_STATE: return "invalid TLS lifetime or operation";
  case TLS_TRUST_ERROR: return "malformed or empty trust bundle";
  case TLS_CERTIFICATE_ERROR: return "certificate verification failed";
  case TLS_PROTOCOL_ERROR: return "TLS protocol failure";
  case TLS_TRUNCATED: return "TLS stream truncated";
  case TLS_UNSUPPORTED: return "unsupported TLS operation or protocol";
  case TLS_LIMIT: return "TLS encoded size limit";
  case TLS_QUOTA: return "TLS allocation quota";
  case TLS_NO_MEMORY: return "TLS allocation failed";
  case TLS_CLOCK_ERROR: return "native clock failure";
  case TLS_ENTROPY_ERROR: return "native entropy failure";
  case TLS_TRANSPORT_ERROR: return "native transport failure";
  case TLS_DEADLINE: return "TLS deadline expired";
  default: return "invalid TLS result";
  }
}

static const char *alert_name(unsigned alert)
{
  switch (alert) {
  case MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE: return "unexpected_message";
  case MBEDTLS_SSL_ALERT_MSG_BAD_RECORD_MAC: return "bad_record_mac";
  case MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE: return "handshake_failure";
  case MBEDTLS_SSL_ALERT_MSG_BAD_CERT: return "bad_certificate";
  case MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT: return "unsupported_certificate";
  case MBEDTLS_SSL_ALERT_MSG_CERT_REVOKED: return "certificate_revoked";
  case MBEDTLS_SSL_ALERT_MSG_CERT_EXPIRED: return "certificate_expired";
  case MBEDTLS_SSL_ALERT_MSG_CERT_UNKNOWN: return "certificate_unknown";
  case MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER: return "illegal_parameter";
  case MBEDTLS_SSL_ALERT_MSG_UNKNOWN_CA: return "unknown_ca";
  case MBEDTLS_SSL_ALERT_MSG_ACCESS_DENIED: return "access_denied";
  case MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR: return "decode_error";
  case MBEDTLS_SSL_ALERT_MSG_DECRYPT_ERROR: return "decrypt_error";
  case MBEDTLS_SSL_ALERT_MSG_PROTOCOL_VERSION: return "protocol_version";
  case MBEDTLS_SSL_ALERT_MSG_INSUFFICIENT_SECURITY: return "insufficient_security";
  case MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR: return "internal_error";
  case MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT: return "unsupported_extension";
  case MBEDTLS_SSL_ALERT_MSG_UNRECOGNIZED_NAME: return "unrecognized_name";
  case MBEDTLS_SSL_ALERT_MSG_NO_APPLICATION_PROTOCOL: return "no_application_protocol";
  default: return NULL;
  }
}

void tls_result_detail(const struct tls_result *result, char *text, size_t capacity)
{
  if (!capacity) {
    return;
  }
  text[0] = 0;
  if (result->error == TLS_DEADLINE || result->native_status == CALL_TIMED_OUT) {
    snprintf(text, capacity, "Connection timed out");
    return;
  }
  if (result->error == TLS_TRANSPORT_ERROR) {
    if (result->native_status == CALL_CONNECTION_RESET) {
      snprintf(text, capacity, "Connection reset");
    } else if (result->native_status == CALL_ENDPOINT_CLOSED) {
      snprintf(text, capacity, "Connection closed");
    } else {
      snprintf(text, capacity, "Transport failed (status %u)", result->native_status);
    }
    return;
  }
  if (result->error == TLS_CERTIFICATE_ERROR && result->verify_flags &&
      result->verify_flags != UINT32_MAX) {
    static const struct { uint32_t flag; const char *name; } reasons[] = {
      {MBEDTLS_X509_BADCERT_EXPIRED, "expired"},
      {MBEDTLS_X509_BADCERT_FUTURE, "not yet valid"},
      {MBEDTLS_X509_BADCERT_CN_MISMATCH, "name mismatch"},
      {MBEDTLS_X509_BADCERT_REVOKED, "revoked"},
      {MBEDTLS_X509_BADCERT_NOT_TRUSTED, "untrusted issuer or bad signature"},
      {MBEDTLS_X509_BADCERT_BAD_MD, "unacceptable signature hash"},
      {MBEDTLS_X509_BADCERT_BAD_PK, "unacceptable signature algorithm"},
      {MBEDTLS_X509_BADCERT_BAD_KEY, "unacceptable key"},
      {MBEDTLS_X509_BADCERT_KEY_USAGE, "invalid key usage"},
      {MBEDTLS_X509_BADCERT_EXT_KEY_USAGE, "invalid extended key usage"},
    };
    snprintf(text, capacity, "Certificate verification failed: ");
    uint32_t remaining = result->verify_flags;
    bool first = true;
    for (size_t i = 0; i < sizeof(reasons) / sizeof(*reasons); ++i) {
      if (remaining & reasons[i].flag) {
        size_t used = strlen(text);
        snprintf(text + used, capacity - used, "%s%s", first ? "" : "; ", reasons[i].name);
        remaining &= ~reasons[i].flag;
        first = false;
      }
    }
    if (remaining) {
      size_t used = strlen(text);
      snprintf(text + used, capacity - used, "%sverify flags 0x%x", first ? "" : "; ", remaining);
    }
    return;
  }
  if (result->peer_alert_present) {
    const char *name = alert_name(result->peer_alert);
    if (name) {
      snprintf(text, capacity, "TLS peer alert: %s (%u)", name, result->peer_alert);
    } else {
      snprintf(text, capacity, "TLS peer alert %u", result->peer_alert);
    }
    return;
  }
  const char *reason;
  switch (result->library_error) {
  case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION: reason = "TLS protocol version not supported"; break;
  case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE: reason = "TLS handshake failed (no compatible negotiation)"; break;
  case MBEDTLS_ERR_SSL_NO_APPLICATION_PROTOCOL: reason = "TLS application protocol not supported"; break;
  case MBEDTLS_ERR_SSL_TIMEOUT: reason = "Connection timed out"; break;
  default: reason = NULL; break;
  }
  if (reason) {
    snprintf(text, capacity, "%s", reason);
  } else if (result->library_error) {
    unsigned code = (unsigned)result->library_error;
    snprintf(text, capacity, "%s (TLS error %s0x%04x)", tls_error_name(result->error),
        result->library_error < 0 ? "-" : "", result->library_error < 0 ? 0u - code : code);
  } else {
    snprintf(text, capacity, "%s", tls_error_name(result->error));
  }
}
