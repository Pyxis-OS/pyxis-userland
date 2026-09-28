#include "tls.h"
#include <clock.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <random.h>
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
  bool verified, failed, eof, closed;
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
  if (code == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
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
  /* The upstream bundle parser permits text but can ignore unmatched trailing
   * delimiters. Validate pairing so a partially malformed bundle cannot pass. */
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
  allocation_result = result;
  int code = mbedtls_x509_crt_parse(&runtime->trust, pem, length);
  allocation_result = NULL;
  if (!library_result(result, code, TLS_TRUST_ERROR)) {
    return false;
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

static int send_bytes(void *context, const unsigned char *data, size_t size)
{
  struct tls_connection *connection = context;
  if (!check_deadline()) {
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  if (size > TCP_WRITE_MAX_BYTES) {
    size = TCP_WRITE_MAX_BYTES;
  }
  struct tcp_write_reply reply;
  enum call_status status = tcp_write(connection->stream, data, size, operation.deadline, &reply);
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
  enum call_status status = tcp_read(connection->stream, data, size, operation.deadline, &reply);
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

bool tls_connection_open(struct tls_runtime *runtime,
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
  do {
    if (!check_deadline()) {
      success = false;
      goto done;
    }
    code = mbedtls_ssl_handshake(&created->ssl);
  } while (retryable(code) && result->error == TLS_OK);
  result->verify_flags = mbedtls_ssl_get_verify_result(&created->ssl);
  success = library_result(result, code, TLS_PROTOCOL_ERROR) && check_deadline() && check_utc();
  if (success && result->verify_flags) {
    success = fail(result, TLS_CERTIFICATE_ERROR);
  }
  const char *protocol = mbedtls_ssl_get_alpn_protocol(&created->ssl);
  if (success && protocol && strcmp(protocol, "http/1.1")) {
    success = fail(result, TLS_UNSUPPORTED);
  }
  created->verified = success;

done:
  end();
  if (!success) {
    tls_connection_free(created);
    return false;
  }
  *connection = created;
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

bool tls_read(struct tls_connection *connection, void *data, size_t capacity,
    size_t *length, bool *eof, struct tls_result *result)
{
  *length = 0;
  *eof = false;
  bool success = connection_begin(connection, result);
  if (!success) {
    end();
    return false;
  }
  if (connection->eof) {
    *eof = true;
    end();
    return true;
  }
  if (!data || !capacity) {
    connection->failed = true;
    end();
    return fail(result, TLS_BAD_STATE);
  }
  int code;
  do {
    code = mbedtls_ssl_read(&connection->ssl, data, capacity);
  } while (retryable(code) && check_deadline());
  if (code == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
    success = check_deadline();
    connection->eof = *eof = success;
  } else if (code > 0) {
    success = result->error == TLS_OK && check_deadline();
    if (success) {
      *length = code;
    }
  } else {
    success = library_result(result, code ? code : MBEDTLS_ERR_SSL_CONN_EOF, TLS_PROTOCOL_ERROR);
  }
  connection->failed = !success;
  end();
  return success;
}

bool tls_write(struct tls_connection *connection, const void *data, size_t size,
    size_t *accepted, struct tls_result *result)
{
  *accepted = 0;
  bool success = connection_begin(connection, result);
  if (!success) {
    end();
    return false;
  }
  if (!data || !size || connection->eof) {
    connection->failed = true;
    end();
    return fail(result, TLS_BAD_STATE);
  }
  int code;
  do {
    code = mbedtls_ssl_write(&connection->ssl, data, size);
  } while (retryable(code) && check_deadline());
  if (code > 0) {
    success = result->error == TLS_OK && check_deadline();
    if (success) {
      *accepted = code;
    }
  } else {
    success = library_result(result, code ? code : MBEDTLS_ERR_SSL_INTERNAL_ERROR, TLS_PROTOCOL_ERROR);
  }
  connection->failed = !success;
  end();
  return success;
}

bool tls_close_notify(struct tls_connection *connection, struct tls_result *result)
{
  *result = (struct tls_result){0};
  if (!connection || !owner || owner->connection != connection || !connection->verified ||
      connection->failed || operation.result) {
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
  case TLS_UNSUPPORTED: return "unsupported negotiated protocol";
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
