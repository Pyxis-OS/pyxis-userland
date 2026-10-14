#include "../common/bootstrap_environment.h"
#include <clock.h>
#include <completion.h>
#include <directory.h>
#include <endpoint.h>
#include <file.h>
#include <forwarder.h>
#include <handle.h>
#include <http_transaction.h>
#include <launcher.h>
#include <environment.h>
#include <provider.h>
#include <startup.h>
#include <stream.h>
#include <udp.h>
#include <wait.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool close_owned(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool ok = handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return ok;
}

static bool close_pair(struct forwarder_reply *pair)
{
  bool holder = close_owned(&pair->holder);
  bool controller = close_owned(&pair->controller);
  return holder && controller;
}

static bool http_lineage_deadline(handle_t clock, uint64_t *deadline)
{
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(30000000000)) {
    return false;
  }
  *deadline = now + UINT64_C(30000000000);
  return true;
}

/* The selected URI is the owner's CI-jobs fixture. Each exchange gets one
 * deadline covering preparation, staging, submission and response consumption. */
static bool http_lineage_exchange(handle_t provider, handle_t clock, const char *uri,
    uint64_t method, struct http_prepared *prepared, const char *label)
{
  static const char body[] = "{\"repo\":\"pyxis-os\",\"ref\":\"main\",\"steps\":[\"build\",\"test\"]}";
  uint64_t deadline;
  if (!http_lineage_deadline(clock, &deadline)) {
    fprintf(stderr, "HTTP lineage %s: clock deadline unavailable\n", label);
    return false;
  }
  const char *phase = "PREPARE";
  enum call_status status = http_transaction_prepare(provider, uri, method,
      HTTP_PREPARE_JSON, NULL, 0, deadline, prepared);
  if (status != CALL_OK) {
    goto failed;
  }
  /* Force adoption of COMMIT's newly returned observation, rather than keeping
   * the separately derived PREPARE observation. */
  phase = "release initial observation";
  if (!close_owned(&prepared->observation)) {
    status = CALL_BAD_HANDLE;
    goto failed;
  }
  if (method == HTTP_PREPARE_POST) {
    phase = "stage POST body";
    size_t offset = 0;
    while (offset < sizeof(body) - 1) {
      size_t written;
      status = file_write(prepared->body, offset, body + offset,
          sizeof(body) - 1 - offset, &written);
      if (status != CALL_OK) {
        goto failed;
      }
      if (!written || written > sizeof(body) - 1 - offset) {
        status = CALL_OUTCOME_UNKNOWN;
        goto failed;
      }
      offset += written;
    }
  }
  phase = "COMMIT handoff";
  struct http_commit_reply committed;
  status = http_transaction_commit(prepared, deadline, &committed);
  if (status != CALL_OK) {
    goto failed;
  }
  if (committed.flags != HTTP_COMMIT_RESPONSE || !prepared->response ||
      !prepared->observation || committed.http_status !=
      (method == HTTP_PREPARE_POST ? 201u : 200u)) {
    status = CALL_BAD_REQUEST;
    goto failed;
  }
  if (method == HTTP_PREPARE_POST) {
    phase = "COMMIT freezes body mutation";
    size_t written;
    status = file_write(prepared->body, 0, body, 1, &written);
    if (status != CALL_DENIED || written != 0) {
      status = CALL_BAD_REQUEST;
      goto failed;
    }
  }
  phase = "response through EOF";
  unsigned char bytes[256];
  uint64_t consumed = 0;
  for (;;) {
    size_t read;
    status = stream_read(prepared->response, bytes, sizeof(bytes), deadline, &read);
    if (status != CALL_OK) {
      goto failed;
    }
    if (!read) {
      break;
    }
    if (read > sizeof(bytes) || consumed > UINT64_MAX - read) {
      status = CALL_BAD_REQUEST;
      goto failed;
    }
    consumed += read;
  }
  phase = "final STATUS";
  struct http_transaction_status final;
  status = http_transaction_status(prepared->observation, deadline, &final);
  if (status != CALL_OK) {
    goto failed;
  }
  if (!consumed || final.phase != HTTP_TRANSACTION_COMPLETE ||
      final.framing != HTTP_FRAMING_COMPLETE || final.status != CALL_OK ||
      final.source_status != CALL_OK || final.cleanup_status != CALL_OK ||
      final.http_status != committed.http_status || final.consumed != consumed ||
      (method == HTTP_PREPARE_POST && final.uploaded != sizeof(body) - 1)) {
    status = CALL_BAD_REQUEST;
    goto failed;
  }
  phase = "METADATA";
  struct http_response_metadata metadata;
  status = http_transaction_metadata(prepared->observation, deadline, &metadata);
  if (status != CALL_OK) {
    goto failed;
  }
  const char *endpoint = !strncmp(uri, "json+", 5) ? uri + 5 : uri;
  if (strcmp(metadata.uri, endpoint)) {
    status = CALL_BAD_REQUEST;
    goto failed;
  }
  printf("HTTP lineage %s: response EOF, final STATUS and METADATA complete (%ju bytes)\n",
      label, (uintmax_t)consumed);
  return true;

failed:
  fprintf(stderr, "HTTP lineage %s: %s failed (status %u)\n", label, phase, status);
  if (prepared->control) {
    enum call_status aborted = http_transaction_abort(prepared->control, deadline);
    if (aborted != CALL_OK) {
      fprintf(stderr, "HTTP lineage %s: ABORT cleanup failed (status %u)\n", label, aborted);
    }
  }
  return false;
}

static bool http_lineage_method(handle_t provider, handle_t clock, const char *uri,
    uint64_t method)
{
  const char *label = method == HTTP_PREPARE_POST ? "POST" : "GET";
  struct forwarder_reply source = {0};
  struct http_prepared prepared = {0}, independent = {0};
  enum call_status created = forwarder_revocable(provider, 0, 0,
      FORWARDER_CREATE_SAME_RIGHTS, &source);
  bool ok = created == CALL_OK;
  if (!ok) {
    fprintf(stderr, "HTTP lineage %s: source copy failed (status %u)\n", label, created);
  }
  if (ok) {
    ok = http_lineage_exchange(source.holder, clock, uri, method, &prepared, label);
  }
  if (ok) {
    uint64_t deadline;
    struct http_transaction_status final;
    struct http_response_metadata metadata;
    unsigned char byte;
    size_t read;
    ok = http_lineage_deadline(clock, &deadline) &&
        forwarder_cut(source.controller) == CALL_OK &&
        stream_read(prepared.response, &byte, sizeof(byte), deadline, &read) == CALL_DENIED &&
        read == 0 &&
        http_transaction_status(prepared.observation, deadline, &final) == CALL_DENIED &&
        http_transaction_metadata(prepared.observation, deadline, &metadata) == CALL_DENIED;
    if (ok) {
      printf("HTTP lineage %s: source CUT denied response READ, STATUS and METADATA\n", label);
      ok = http_lineage_exchange(provider, clock, uri, HTTP_PREPARE_GET,
          &independent, "independent GET");
    } else {
      fprintf(stderr, "HTTP lineage %s: source CUT qualification failed\n", label);
    }
  }
  bool cleanup = http_prepared_close(&prepared) == CALL_OK;
  cleanup = (http_prepared_close(&independent) == CALL_OK) && cleanup;
  cleanup = close_pair(&source) && cleanup;
  if (!cleanup) {
    fprintf(stderr, "HTTP lineage %s: owned handle cleanup failed\n", label);
  }
  return ok && cleanup;
}

int counter_http_lineage_main(const char *scheme, const char *uri)
{
  handle_t clock = bootstrap_capability("clock");
  handle_t provider = HANDLE_INVALID;
  struct handle_info info;
  bool ok = clock != HANDLE_INVALID &&
      environment_capability(startup_environment_handle(), scheme, &provider) == CALL_OK &&
      handle_query(provider, &info) == CALL_OK && info.kind == HANDLE_KIND_EXPORTED &&
      info.protocol == PROTOCOL_PROVIDER && (info.rights & PROVIDER_RIGHT_OPEN_READ) &&
      (info.transport & HANDLE_TRANSPORT_CALL);
  if (!ok) {
    fputs("HTTP lineage: selected provider or clock authority unavailable\n", stderr);
  } else {
    ok = http_lineage_method(provider, clock, uri, HTTP_PREPARE_GET);
    if (ok && !(info.rights & PROVIDER_RIGHT_OPEN_WRITE)) {
      fputs("HTTP lineage POST: selected provider lacks OPEN_WRITE authority\n", stderr);
      ok = false;
    } else if (ok) {
      ok = http_lineage_method(provider, clock, uri, HTTP_PREPARE_POST);
    }
  }
  bool cleanup = close_owned(&provider);
  return ok && cleanup ? 0 : 1;
}

static bool revoked(handle_t file)
{
  uint64_t size;
  return file_size(file, &size) == CALL_DENIED;
}

static bool size_matches(handle_t file, uint64_t expected)
{
  uint64_t size;
  return file_size(file, &size) == CALL_OK && size == expected;
}

/* Explicit manual qualification on two existing native files. It creates no
 * fixture and changes neither file; callers select the root and components. */
int counter_forwarders_main(const char *scheme, const char *name, const char *replacement)
{
  handle_t root = bootstrap_capability(scheme);
  handle_t first = HANDLE_INVALID, second = HANDLE_INVALID;
  handle_t child = HANDLE_INVALID, child_copy = HANDLE_INVALID;
  handle_t holder_copy = HANDLE_INVALID, supervisor = HANDLE_INVALID;
  handle_t weak = HANDLE_INVALID;
  struct forwarder_reply directory = {0}, nested = {0}, automatic = {0}, slot = {0};
  uint64_t first_size = 0, second_size = 0;
  const char *phase = "selected native files";
  bool ok = root != HANDLE_INVALID &&
      directory_lookup(root, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &first) == CALL_OK &&
      directory_lookup(root, replacement, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &second) == CALL_OK &&
      file_size(first, &first_size) == CALL_OK && file_size(second, &second_size) == CALL_OK;
  if (ok) {
    phase = "directory child and nested lineage";
    ok = forwarder_revocable(root, DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES,
        0, 0, &directory) == CALL_OK &&
        forwarder_revocable(directory.holder, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &nested) == CALL_OK &&
        directory_lookup(nested.holder, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &child) == CALL_OK && handle_copy(child, &child_copy) == CALL_OK &&
        size_matches(child_copy, first_size) && forwarder_cut(directory.controller) == CALL_OK &&
        revoked(child) && revoked(child_copy) && size_matches(first, first_size);
  }
  if (ok) {
    handle_t denied = HANDLE_INVALID;
    ok = directory_lookup(nested.holder, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &denied) == CALL_DENIED && denied == HANDLE_INVALID;
  }
  if (ok) {
    phase = "last logical controller release";
    ok = forwarder_revocable(first, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &automatic) == CALL_OK && handle_copy(automatic.controller, &supervisor) == CALL_OK &&
        handle_copy(automatic.holder, &holder_copy) == CALL_OK &&
        close_owned(&automatic.controller) && size_matches(holder_copy, first_size) &&
        close_owned(&supervisor) && revoked(automatic.holder) && revoked(holder_copy);
  }
  if (ok) {
    phase = "slot retarget and supervisor takeover";
    ok = forwarder_slot(first, FILE_RIGHT_READ, 0, 0, &slot) == CALL_OK &&
        handle_copy(slot.controller, &supervisor) == CALL_OK &&
        close_owned(&slot.controller) && size_matches(slot.holder, first_size) &&
        forwarder_retarget(supervisor, second) == CALL_OK &&
        size_matches(slot.holder, second_size) &&
        forwarder_retarget(supervisor, root) == CALL_WRONG_TYPE &&
        handle_copy_restricted(first, 0, 0, &weak) == CALL_OK &&
        forwarder_retarget(supervisor, weak) == CALL_DENIED &&
        size_matches(slot.holder, second_size) &&
        forwarder_retarget(supervisor, HANDLE_INVALID) == CALL_OK;
  }
  if (ok) {
    phase = "empty slot and unmanaged target retention";
    uint64_t size;
    ok = file_size(slot.holder, &size) == CALL_NOT_FOUND &&
        forwarder_retarget(supervisor, first) == CALL_OK &&
        size_matches(slot.holder, first_size) && close_owned(&supervisor) &&
        size_matches(slot.holder, first_size);
  }
  if (ok) {
    phase = "terminal slot cut";
    ok = close_pair(&slot) && forwarder_slot(first, FILE_RIGHT_READ, 0, 0, &slot) == CALL_OK &&
        forwarder_cut(slot.controller) == CALL_OK && revoked(slot.holder) &&
        forwarder_retarget(slot.controller, second) == CALL_DENIED;
  }
  bool cleanup = close_pair(&directory);
  cleanup = close_pair(&nested) && cleanup;
  cleanup = close_pair(&automatic) && cleanup;
  cleanup = close_pair(&slot) && cleanup;
  cleanup = close_owned(&child) && cleanup;
  cleanup = close_owned(&child_copy) && cleanup;
  cleanup = close_owned(&holder_copy) && cleanup;
  cleanup = close_owned(&supervisor) && cleanup;
  cleanup = close_owned(&weak) && cleanup;
  cleanup = close_owned(&first) && cleanup;
  cleanup = close_owned(&second) && cleanup;
  if (!cleanup && ok) {
    phase = "owned handle cleanup";
  }
  ok = ok && cleanup;
  if (ok) {
    fputs("forwarders: lineage, cut, controller release and slot takeover complete\n", stdout);
  } else {
    fprintf(stderr, "forwarders: qualification failed at %s\n", phase);
  }
  return ok ? 0 : 1;
}

static bool open_provider_file(handle_t provider, const char *uri, handle_t *file)
{
  struct provider_result result;
  enum call_status status = provider_open(provider, uri, FILE_RIGHT_READ, 0,
      &result, file);
  if (status != CALL_OK) {
    fprintf(stderr, "provider forwarders: OPEN %s failed (call status %u)\n", uri, status);
    return false;
  }
  if (result.status != CALL_OK || *file == HANDLE_INVALID) {
    fprintf(stderr, "provider forwarders: OPEN %s failed (status %u, provider %ju)\n",
        uri, result.status, (uintmax_t)result.provider_status);
    return false;
  }
  return true;
}

/* The selected files must remain unchanged for this manual run. Compare a
 * bounded read and size with an independently held reference, without fixtures. */
static bool file_matches(handle_t file, handle_t reference)
{
  uint64_t actual_size, expected_size;
  unsigned char actual[64], expected[64];
  size_t actual_read, expected_read;
  return file_size(file, &actual_size) == CALL_OK &&
      file_size(reference, &expected_size) == CALL_OK && actual_size == expected_size &&
      file_read(file, 0, actual, sizeof(actual), &actual_read) == CALL_OK &&
      file_read(reference, 0, expected, sizeof(expected), &expected_read) == CALL_OK &&
      actual_read == expected_read && memcmp(actual, expected, actual_read) == 0;
}

static bool provider_file_revoked(handle_t file)
{
  unsigned char byte;
  size_t read;
  return revoked(file) && file_read(file, 0, &byte, sizeof(byte), &read) == CALL_DENIED &&
      read == 0;
}

/* Only private wrappers are controlled here. Namespace bindings and the
 * published provider's controller remain outside this manual qualification. */
int counter_provider_forwarders_main(const char *provider_name,
    const char *scheme, const char *name)
{
  handle_t root = bootstrap_capability(scheme);
  handle_t provider = HANDLE_INVALID, native = HANDLE_INVALID;
  handle_t guide = HANDLE_INVALID, welcome = HANDLE_INVALID;
  handle_t child = HANDLE_INVALID, child_copy = HANDLE_INVALID;
  handle_t independent = HANDLE_INVALID, powerless = HANDLE_INVALID;
  handle_t old_child = HANDLE_INVALID, new_child = HANDLE_INVALID;
  handle_t denied_open = HANDLE_INVALID;
  struct forwarder_reply exported_slot = {0}, native_slot = {0};
  struct forwarder_reply provider_copy = {0}, automatic = {0};
  struct forwarder_reply provider_target = {0}, provider_slot = {0};
  uint64_t welcome_file_size = 0;
  char guide_uri[NAMESPACE_NAME_MAX + sizeof("://guide")];
  char welcome_uri[NAMESPACE_NAME_MAX + sizeof("://welcome")];
  const char *phase = "selected namespace and native file";
  int guide_size = snprintf(guide_uri, sizeof(guide_uri), "%s://guide", provider_name);
  int welcome_size = snprintf(welcome_uri, sizeof(welcome_uri), "%s://welcome", provider_name);
  bool ok = root != HANDLE_INVALID &&
      guide_size > 0 && (size_t)guide_size < sizeof(guide_uri) &&
      welcome_size > 0 && (size_t)welcome_size < sizeof(welcome_uri);
  if (ok) {
    enum call_status status = environment_capability(startup_environment_handle(), provider_name, &provider);
    ok = status == CALL_OK;
    if (!ok) {
      fprintf(stderr, "provider forwarders: lookup %s failed (status %u)\n",
          provider_name, status);
    }
  }
  if (ok) {
    struct handle_info info;
    ok = directory_lookup(root, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &native) == CALL_OK && handle_query(native, &info) == CALL_OK &&
        info.kind == HANDLE_KIND_NATIVE && info.protocol == PROTOCOL_FILE &&
        open_provider_file(provider, guide_uri, &guide) &&
        open_provider_file(provider, welcome_uri, &welcome) &&
        file_size(welcome, &welcome_file_size) == CALL_OK;
  }
  if (ok) {
    phase = "exported FILE slot retarget";
    ok = forwarder_slot(guide, FILE_RIGHT_READ, HANDLE_TRANSPORT_CALL, 0,
        &exported_slot) == CALL_OK && file_matches(exported_slot.holder, guide) &&
        forwarder_retarget(exported_slot.controller, welcome) == CALL_OK &&
        file_matches(exported_slot.holder, welcome) &&
        forwarder_retarget(exported_slot.controller, guide) == CALL_OK &&
        file_matches(exported_slot.holder, guide);
  }
  if (ok) {
    phase = "exported to native retarget rejection";
    ok = forwarder_retarget(exported_slot.controller, native) == CALL_WRONG_TYPE &&
        file_matches(exported_slot.holder, guide);
  }
  if (ok) {
    phase = "native to exported retarget rejection";
    ok = forwarder_slot(native, FILE_RIGHT_READ, 0, 0, &native_slot) == CALL_OK &&
        file_matches(native_slot.holder, native) &&
        forwarder_retarget(native_slot.controller, welcome) == CALL_WRONG_TYPE &&
        file_matches(native_slot.holder, native);
  }
  if (ok) {
    phase = "provider OPEN reply and COPY lineage";
    ok = forwarder_revocable(provider, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &provider_copy) == CALL_OK &&
        open_provider_file(provider_copy.holder, guide_uri, &child) &&
        handle_copy(child, &child_copy) == CALL_OK && file_matches(child, guide) &&
        file_matches(child_copy, guide) &&
        forwarder_cut(provider_copy.controller) == CALL_OK &&
        provider_file_revoked(child) && provider_file_revoked(child_copy);
  }
  if (ok) {
    phase = "independent provider OPEN after private CUT";
    ok = open_provider_file(provider, guide_uri, &independent) &&
        file_matches(independent, guide);
  }
  if (ok) {
    phase = "provider slot initial target and reply lineage";
    ok = forwarder_revocable(provider, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &provider_target) == CALL_OK &&
        forwarder_slot(provider_target.holder, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &provider_slot) == CALL_OK &&
        open_provider_file(provider_slot.holder, guide_uri, &old_child) &&
        file_matches(old_child, guide);
  }
  if (ok) {
    phase = "provider slot current target last controller release";
    struct provider_result result;
    ok = close_owned(&provider_target.controller) && provider_file_revoked(old_child) &&
        provider_open(provider_slot.holder, guide_uri, FILE_RIGHT_READ, 0,
        &result, &denied_open) == CALL_DENIED && denied_open == HANDLE_INVALID;
  }
  if (ok) {
    phase = "provider slot supervisor takeover after target CUT";
    ok = forwarder_retarget(provider_slot.controller, provider) == CALL_OK &&
        open_provider_file(provider_slot.holder, welcome_uri, &new_child) &&
        file_matches(new_child, welcome) && provider_file_revoked(old_child);
  }
  if (ok) {
    phase = "provider slot terminal CUT";
    ok = forwarder_cut(provider_slot.controller) == CALL_OK &&
        provider_file_revoked(new_child) &&
        forwarder_retarget(provider_slot.controller, provider) == CALL_DENIED &&
        file_matches(independent, guide);
  }
  if (ok) {
    phase = "powerless controller copy and last controller release";
    ok = forwarder_revocable(welcome, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &automatic) == CALL_OK &&
        handle_copy_restricted(automatic.controller, 0, 0, &powerless) == CALL_OK &&
        forwarder_cut(powerless) == CALL_DENIED && file_matches(automatic.holder, welcome) &&
        close_owned(&automatic.controller) && provider_file_revoked(automatic.holder) &&
        size_matches(welcome, welcome_file_size);
  }
  bool cleanup = close_pair(&exported_slot);
  cleanup = close_pair(&native_slot) && cleanup;
  cleanup = close_pair(&provider_copy) && cleanup;
  cleanup = close_pair(&automatic) && cleanup;
  cleanup = close_pair(&provider_target) && cleanup;
  cleanup = close_pair(&provider_slot) && cleanup;
  cleanup = close_owned(&powerless) && cleanup;
  cleanup = close_owned(&child) && cleanup;
  cleanup = close_owned(&child_copy) && cleanup;
  cleanup = close_owned(&independent) && cleanup;
  cleanup = close_owned(&old_child) && cleanup;
  cleanup = close_owned(&new_child) && cleanup;
  cleanup = close_owned(&denied_open) && cleanup;
  cleanup = close_owned(&guide) && cleanup;
  cleanup = close_owned(&welcome) && cleanup;
  cleanup = close_owned(&native) && cleanup;
  cleanup = close_owned(&provider) && cleanup;
  if (!cleanup && ok) {
    phase = "owned handle cleanup";
  }
  ok = ok && cleanup;
  if (ok) {
    fputs("provider forwarders: exported retarget, representation rejection, reply lineage, "
        "provider slot takeover and powerless controller release complete\n", stdout);
  } else {
    fprintf(stderr, "provider forwarders: qualification failed at %s\n", phase);
  }
  return ok ? 0 : 1;
}

#define MANUAL_COMPLETION_WAIT_NS UINT64_C(5000000000)
#define MANUAL_COMPLETION_DRAIN_NS UINT64_C(1000000000)
#define MANUAL_LOOPBACK_ADDRESS UINT32_C(0x7f000001)

static enum call_status manual_completion_deadline(handle_t clock,
    uint64_t *deadline, uint64_t *collection_deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (now > UINT64_MAX - MANUAL_COMPLETION_WAIT_NS - MANUAL_COMPLETION_DRAIN_NS) {
    return CALL_LIMIT;
  }
  *deadline = now + MANUAL_COMPLETION_WAIT_NS;
  /* This grace bounds the manual command's wait, not kernel cleanup lifetime. */
  *collection_deadline = *deadline + MANUAL_COMPLETION_DRAIN_NS;
  return CALL_OK;
}

static enum call_status collect_manual_completion(handle_t receiver,
    uint64_t deadline, struct endpoint_packet *packet)
{
  struct wait_interest interest = {.handle = receiver, .events = WAIT_READABLE};
  for (;;) {
    enum call_status status = endpoint_try_receive(receiver, packet);
    if (status != CALL_WOULD_BLOCK) {
      return status;
    }
    uint64_t events;
    status = wait_many(&interest, 1, deadline, &events);
    if (status != CALL_OK) {
      return status;
    }
    if (events & (WAIT_CLOSED | WAIT_ERROR)) {
      return CALL_ENDPOINT_CLOSED;
    }
  }
}

static bool acknowledge_manual_completion(struct endpoint_packet *packet)
{
  bool ok = close_owned(&packet->receipt);
  for (size_t i = 0; i < packet->grant_count; ++i) {
    ok = close_owned(&packet->grants[i].handle) && ok;
  }
  return ok;
}

/* The caller selects an unchanged native archive file. The socket is a new
 * empty loopback binding; no datagram or file mutation is submitted. */
int counter_completion_cut_main(const char *scheme, const char *name)
{
  handle_t service = bootstrap_capability("service");
  handle_t udp = bootstrap_capability("udp");
  handle_t clock = bootstrap_capability("clock");
  handle_t root = bootstrap_capability(scheme);
  handle_t file = HANDLE_INVALID;
  struct endpoint_create_reply endpoint = {0};
  struct udp_open_reply socket = {0};
  struct forwarder_reply udp_copy = {0}, file_copy = {0};
  struct endpoint_packet packet = {0};
  uint64_t pending = 0, deadline = 0, collection_deadline = 0;
  unsigned char expected = 0;
  size_t read = 0;
  const char *phase = "held service, udp, clock and selected root";
  enum call_status status = CALL_UNAVAILABLE;
  bool ok = false;
  if (service == HANDLE_INVALID || udp == HANDLE_INVALID || clock == HANDLE_INVALID ||
      root == HANDLE_INVALID) {
    goto cleanup;
  }
  phase = "selected native file read";
  status = directory_lookup(root, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = file_read(file, 0, &expected, sizeof(expected), &read);
  if (status != CALL_OK || read != 1) {
    goto cleanup;
  }
  phase = "owned completion receiver and loopback socket";
  status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = udp_open(udp, MANUAL_LOOPBACK_ADDRESS, 0, &socket);
  if (status != CALL_OK) {
    goto cleanup;
  }
  phase = "private UDP receive submission";
  status = forwarder_revocable(socket.handle, UDP_RIGHT_RECEIVE, 0, 0, &udp_copy);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = manual_completion_deadline(clock, &deadline, &collection_deadline);
  if (status != CALL_OK) {
    goto cleanup;
  }
  struct completion_udp_receive_request receive = {.capacity = 1};
  status = completion_submit(udp_copy.holder, endpoint.receiver, 1,
      PROTOCOL_UDP, UDP_RECEIVE, deadline, &receive, sizeof(receive));
  if (status != CALL_OK) {
    goto cleanup;
  }
  pending = 1;
  phase = "UDP CUT and completion collection";
  status = forwarder_cut(udp_copy.controller);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = collect_manual_completion(endpoint.receiver, collection_deadline, &packet);
  if (status != CALL_OK) {
    goto cleanup;
  }
  phase = "UDP denied-before-start completion";
  printf("completion cut: UDP result %ju, reason %ju, flags %ju\n",
      (uintmax_t)packet.result, (uintmax_t)packet.reason, (uintmax_t)packet.completion_flags);
  ok = packet.kind == ENDPOINT_MESSAGE_COMPLETION && packet.correlation_id == 1 &&
      packet.protocol == PROTOCOL_UDP && packet.operation == UDP_RECEIVE &&
      packet.result == CALL_DENIED && packet.reason == CALL_DENIED &&
      (packet.completion_flags & COMPLETION_FLAG_NOT_STARTED) &&
      (packet.completion_flags & COMPLETION_FLAG_CANCEL_REQUESTED) &&
      packet.size == 0 && packet.grant_count == 0 && packet.receipt != HANDLE_INVALID;
  if (acknowledge_manual_completion(&packet)) {
    pending = 0;
  } else {
    ok = false;
  }
  if (!ok) {
    goto cleanup;
  }
  ok = false;
  phase = "private FILE read submission";
  status = forwarder_revocable(file, FILE_RIGHT_READ, 0, 0, &file_copy);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = manual_completion_deadline(clock, &deadline, &collection_deadline);
  if (status != CALL_OK) {
    goto cleanup;
  }
  struct file_read_request request = {.offset = 0, .capacity = 1};
  status = completion_submit(file_copy.holder, endpoint.receiver, 2,
      PROTOCOL_FILE, FILE_READ, deadline, &request, sizeof(request));
  if (status != CALL_OK) {
    goto cleanup;
  }
  pending = 2;
  phase = "FILE CUT after submission and completion collection";
  status = forwarder_cut(file_copy.controller);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = collect_manual_completion(endpoint.receiver, collection_deadline, &packet);
  if (status != CALL_OK) {
    goto cleanup;
  }
  phase = "FILE started read retains real byte after CUT";
  printf("completion cut: FILE result %ju, reason %ju, flags %ju\n",
      (uintmax_t)packet.result, (uintmax_t)packet.reason, (uintmax_t)packet.completion_flags);
  struct file_read_reply reply = {0};
  if (packet.size == sizeof(reply) + 1) {
    memcpy(&reply, packet.data, sizeof(reply));
  }
  ok = packet.kind == ENDPOINT_MESSAGE_COMPLETION && packet.correlation_id == 2 &&
      packet.protocol == PROTOCOL_FILE && packet.operation == FILE_READ &&
      packet.result == CALL_OK && packet.reason != CALL_DENIED &&
      !(packet.completion_flags & COMPLETION_FLAG_NOT_STARTED) &&
      packet.grant_count == 0 && packet.receipt != HANDLE_INVALID &&
      packet.size == sizeof(reply) + 1 && reply.read == 1 &&
      packet.data[sizeof(reply)] == expected;
  if (acknowledge_manual_completion(&packet)) {
    pending = 0;
  } else {
    ok = false;
  }
cleanup:
  if (pending) {
    enum call_status cancelled = completion_cancel(endpoint.receiver, pending);
    if (cancelled != CALL_OK && cancelled != CALL_NOT_FOUND) {
      fprintf(stderr, "completion cut: cleanup cancellation failed (status %u)\n", cancelled);
    }
  }
  bool cleanup = close_owned(&endpoint.receiver);
  cleanup = close_owned(&endpoint.caller) && cleanup;
  cleanup = close_pair(&udp_copy) && cleanup;
  cleanup = close_pair(&file_copy) && cleanup;
  cleanup = close_owned(&socket.handle) && cleanup;
  cleanup = close_owned(&file) && cleanup;
  if (!cleanup && ok) {
    phase = "owned handle cleanup";
  }
  ok = ok && cleanup;
  if (ok) {
    fputs("completion cut: UDP denied before start; started FILE read retained after CUT\n", stdout);
  } else {
    fprintf(stderr, "completion cut: qualification failed at %s (status %u)\n", phase, status);
  }
  return ok ? 0 : 1;
}

/* Keep only observation and private wrapper control while the last native
 * group CONTROL grants close. Neither may prolong group supervision. */
int counter_authority_close_main(void)
{
  handle_t launcher = bootstrap_capability("launcher");
  handle_t clock = bootstrap_capability("clock");
  handle_t observer = HANDLE_INVALID;
  struct execution_group_create_reply group = {0};
  struct forwarder_reply wrapper = {0};
  const char *phase = "held launcher and clock";
  enum call_status status = CALL_UNAVAILABLE;
  bool ok = false;
  if (launcher == HANDLE_INVALID || clock == HANDLE_INVALID) {
    goto cleanup;
  }
  phase = "empty scoped group creation";
  status = launcher_create_group(launcher, &group);
  if (status != CALL_OK) {
    goto cleanup;
  }
  phase = "private group wrapper and WAIT-only observer";
  status = forwarder_revocable(group.supervision,
      EXECUTION_GROUP_RIGHT_CONTROL | EXECUTION_GROUP_RIGHT_WAIT, 0, 0, &wrapper);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = handle_copy_restricted(wrapper.holder, EXECUTION_GROUP_RIGHT_WAIT, 0, &observer);
  if (status != CALL_OK) {
    goto cleanup;
  }
  struct handle_info info;
  status = handle_query(observer, &info);
  if (status != CALL_OK || info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != PROTOCOL_EXECUTION_GROUP || info.rights != EXECUTION_GROUP_RIGHT_WAIT ||
      info.transport != 0) {
    goto cleanup;
  }
  phase = "last native group CONTROL release";
  if (!close_owned(&group.supervision) || !close_owned(&wrapper.holder)) {
    status = CALL_BAD_HANDLE;
    goto cleanup;
  }
  phase = "WAIT-only observer reports group cleanup";
  uint64_t now;
  status = clock_now(clock, &now);
  if (status != CALL_OK) {
    goto cleanup;
  }
  if (now > UINT64_MAX - MANUAL_COMPLETION_WAIT_NS) {
    status = CALL_LIMIT;
    goto cleanup;
  }
  struct wait_interest interest = {.handle = observer, .events = WAIT_COMPLETE};
  uint64_t events = 0;
  status = wait_many(&interest, 1, now + MANUAL_COMPLETION_WAIT_NS, &events);
  ok = status == CALL_OK && (events & WAIT_COMPLETE);
cleanup:
  bool cleanup = close_owned(&group.supervision);
  cleanup = close_owned(&group.launcher) && cleanup;
  cleanup = close_owned(&observer) && cleanup;
  cleanup = close_pair(&wrapper) && cleanup;
  if (!cleanup && ok) {
    phase = "owned handle cleanup";
  }
  ok = ok && cleanup;
  if (ok) {
    fputs("authority close: WAIT-only observer retained no group CONTROL\n", stdout);
  } else {
    fprintf(stderr, "authority close: qualification failed at %s (status %u)\n", phase, status);
  }
  return ok ? 0 : 1;
}
