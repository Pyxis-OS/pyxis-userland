#include "capabilities.h"
#include <clock.h>
#include <file.h>
#include <handle.h>
#include <provider_http_private.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define REQUEST_DEFAULT_SECONDS UINT64_C(30)

struct shell_capability_table {
  struct shell_capability entries[SHELL_CAPABILITY_LIMIT];
};

static bool identifier(const char *name)
{
  if (!name || !*name) {
    return false;
  }
  size_t length = 0;
  for (const unsigned char *byte = (const void *)name; *byte; ++byte) {
    bool letter = (*byte >= 'a' && *byte <= 'z') || (*byte >= 'A' && *byte <= 'Z');
    if (!letter && *byte != '_' && (!length || *byte < '0' || *byte > '9')) {
      return false;
    }
    if (++length > SHELL_CAPABILITY_NAME_MAX) {
      return false;
    }
  }
  return true;
}

enum call_status shell_capability_lookup(struct shell *shell, const char *name,
    struct shell_capability **entry)
{
  *entry = NULL;
  if (name && *name == '@') {
    ++name;
  }
  if (!identifier(name)) {
    return CALL_BAD_REQUEST;
  }
  if (shell->capabilities) {
    for (size_t i = 0; i < SHELL_CAPABILITY_LIMIT; ++i) {
      struct shell_capability *candidate = &shell->capabilities->entries[i];
      if (!strcmp(candidate->name, name)) {
        *entry = candidate;
        return CALL_OK;
      }
    }
  }
  return CALL_NOT_FOUND;
}

enum call_status shell_capability_reserve(struct shell *shell, const char *name,
    struct shell_capability **entry)
{
  *entry = NULL;
  if (!identifier(name)) {
    return CALL_BAD_REQUEST;
  }
  if (getenv(name)) {
    return CALL_ALREADY_EXISTS;
  }
  struct shell_capability *existing;
  if (shell_capability_lookup(shell, name, &existing) == CALL_OK) {
    return CALL_ALREADY_EXISTS;
  }
  if (!shell->capabilities) {
    shell->capabilities = calloc(1, sizeof(*shell->capabilities));
    if (!shell->capabilities) {
      return CALL_NO_MEMORY;
    }
  }
  for (size_t i = 0; i < SHELL_CAPABILITY_LIMIT; ++i) {
    struct shell_capability *candidate = &shell->capabilities->entries[i];
    if (!candidate->name[0]) {
      struct shell_capability_state *state = calloc(1, sizeof(*state));
      if (!state) {
        return CALL_NO_MEMORY;
      }
      state->references = 1;
      candidate->state = state;
      memcpy(candidate->name, name, strlen(name) + 1);
      candidate->type = SHELL_CAPABILITY_REQUEST;
      *entry = candidate;
      return CALL_OK;
    }
  }
  return CALL_LIMIT;
}

static enum call_status operation_deadline(struct shell *shell, uint64_t seconds,
    uint64_t *deadline)
{
  uint64_t now;
  enum call_status status = clock_now(shell->clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (!seconds || seconds > (UINT64_MAX - now) / NANOSECONDS_PER_SECOND) {
    return CALL_LIMIT;
  }
  *deadline = now + seconds * NANOSECONDS_PER_SECOND;
  return CALL_OK;
}

static bool terminal(uint64_t phase)
{
  return phase == HTTP_TRANSACTION_COMPLETE || phase == HTTP_TRANSACTION_FAILED ||
      phase == HTTP_TRANSACTION_ABORTED;
}

static enum call_status observe(struct shell *shell,
    const struct shell_capability *entry, struct http_transaction_status *result)
{
  if (!entry->value.observation) {
    return CALL_UNAVAILABLE;
  }
  /* Observation survives the original work deadline; this bounds the local
   * STATUS wait without renewing staging, transport or framing. */
  uint64_t deadline;
  enum call_status status = operation_deadline(shell, REQUEST_DEFAULT_SECONDS, &deadline);
  return status == CALL_OK ?
      http_transaction_status(entry->value.observation, deadline, result) : status;
}

static void latch(struct shell_capability *entry, enum call_status status)
{
  if (entry->state->failure == CALL_OK) {
    entry->state->failure = status == CALL_OK ? CALL_ABANDONED : status;
  }
  if (status == CALL_OUTCOME_UNKNOWN) {
    entry->state->uncertain = true;
  }
}

static enum call_status abort_unfinished(struct shell *shell,
    struct shell_capability *entry)
{
  if (!entry->value.control) {
    return CALL_OK;
  }
  struct http_transaction_status result;
  enum call_status status = observe(shell, entry, &result);
  if (status == CALL_OK && terminal(result.phase)) {
    return CALL_OK;
  }
  uint64_t deadline;
  status = operation_deadline(shell, REQUEST_DEFAULT_SECONDS, &deadline);
  return status == CALL_OK ?
      http_transaction_abort(entry->value.control, deadline) : status;
}

void shell_capability_fail(struct shell *shell, struct shell_capability *entry,
    enum call_status status)
{
  if (!entry || !entry->state) {
    return;
  }
  latch(entry, status);
  enum call_status cleanup = abort_unfinished(shell, entry);
  if (cleanup != CALL_OK) {
    entry->state->uncertain = true;
    shell_error(shell, "request: %s: abort cleanup failed (status %u)\n",
        entry->name, cleanup);
  }
}

enum call_status shell_capability_prepare(struct shell *shell,
    struct shell_capability *entry, const char *uri, uint64_t method,
    const char *headers, size_t headers_size, uint64_t timeout_seconds)
{
  if (!entry || !entry->state || entry->type != SHELL_CAPABILITY_REQUEST ||
      entry->state->deadline_ns || entry->state->failure != CALL_OK ||
      entry->value.control || method > HTTP_PREPARE_POST || !uri ||
      (headers_size && !headers)) {
    return CALL_BAD_REQUEST;
  }
  uint64_t deadline;
  enum call_status status = operation_deadline(shell, timeout_seconds ? timeout_seconds :
      REQUEST_DEFAULT_SECONDS, &deadline);
  if (status == CALL_OK) {
    entry->state->deadline_ns = deadline;
    status = http_transaction_prepare_uri(shell->directory, uri, method, 0,
        headers, headers_size, deadline, &entry->value);
  }
  if (status != CALL_OK) {
    latch(entry, status);
  }
  return status;
}

handle_t shell_capability_body(const struct shell_capability *entry)
{
  return entry && entry->state && entry->type == SHELL_CAPABILITY_REQUEST &&
      entry->state->failure == CALL_OK ? entry->value.body : HANDLE_INVALID;
}

enum command_result shell_capability_commit(struct shell *shell,
    struct shell_capability *entry)
{
  if (!entry || !entry->state || !entry->value.control) {
    return shell_error(shell, "request: commit requires request control\n");
  }
  if (entry->state->failure != CALL_OK || entry->state->uncertain) {
    return shell_error(shell, "request: %s: locally failed%s; commit refused (status %u)\n",
        entry->name, entry->state->uncertain ? "; outcome uncertain" : "",
        entry->state->failure);
  }
  struct http_commit_reply reply;
  enum call_status status = http_transaction_commit(&entry->value,
      entry->state->deadline_ns, &reply);
  if (status != CALL_OK) {
    if (entry->value.delivery == ENDPOINT_DELIVERED &&
        (status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ||
        status == CALL_OUTCOME_UNKNOWN)) {
      entry->state->uncertain = true;
    }
    shell_capability_fail(shell, entry, status);
    return shell_error(shell, "request: %s: commit failed (status %u)%s\n",
        entry->name, status, entry->state->uncertain ? "; outcome uncertain" : "");
  }
  if (reply.http_status < 200 || reply.http_status >= 300) {
    return shell_error(shell, "request: %s: HTTP %llu; response retained\n",
        entry->name, (unsigned long long)reply.http_status);
  }
  return COMMAND_OK;
}

static enum call_status drop(struct shell_capability *entry)
{
  enum call_status status = http_prepared_close(&entry->value);
  struct shell_capability_state *state = entry->state;
  if (--state->references == 0) {
    free(state);
  }
  *entry = (struct shell_capability){0};
  return status;
}

static enum call_status copy_facet(handle_t source, uint64_t rights,
    handle_t *destination)
{
  return source ? handle_copy_restricted(source, rights, HANDLE_TRANSPORT_CALL, destination) :
      CALL_OK;
}

static enum call_status copy_entry(struct shell *shell, const char *name,
    struct shell_capability *source, enum shell_capability_type type)
{
  if ((type == SHELL_CAPABILITY_RESPONSE && !source->value.response) ||
      (type == SHELL_CAPABILITY_STATUS && !source->value.observation) ||
      (type == SHELL_CAPABILITY_REQUEST && source->type != SHELL_CAPABILITY_REQUEST)) {
    return CALL_WRONG_TYPE;
  }
  struct shell_capability *destination;
  enum call_status status = shell_capability_reserve(shell, name, &destination);
  if (status != CALL_OK) {
    return status;
  }
  destination->type = type;
  destination->value.method = source->value.method;
  destination->value.trust_flags = source->value.trust_flags;
  if (type == SHELL_CAPABILITY_REQUEST) {
    status = copy_facet(source->value.control, TRANSACTION_RIGHTS,
        &destination->value.control);
    if (status == CALL_OK) {
      status = copy_facet(source->value.body, FILE_RIGHT_WRITE, &destination->value.body);
    }
  }
  if (status == CALL_OK && type != SHELL_CAPABILITY_STATUS) {
    status = copy_facet(source->value.response, FILE_RIGHT_READ, &destination->value.response);
  }
  if (status == CALL_OK) {
    status = copy_facet(source->value.observation, HTTP_TRANSACTION_RIGHT_STATUS,
        &destination->value.observation);
  }
  if (status != CALL_OK) {
    drop(destination);
    return status;
  }
  free(destination->state);
  destination->state = source->state;
  ++destination->state->references;
  return CALL_OK;
}

static const char *type_name(enum shell_capability_type type)
{
  switch (type) {
  case SHELL_CAPABILITY_REQUEST: return "request";
  case SHELL_CAPABILITY_RESPONSE: return "response";
  case SHELL_CAPABILITY_STATUS: return "status";
  }
  return "unknown";
}

static const char *phase_name(uint64_t phase)
{
  switch (phase) {
  case HTTP_TRANSACTION_PREPARING: return "preparing";
  case HTTP_TRANSACTION_STAGING: return "staging";
  case HTTP_TRANSACTION_OPENING: return "opening";
  case HTTP_TRANSACTION_READING: return "reading";
  case HTTP_TRANSACTION_COMPLETE: return "complete";
  case HTTP_TRANSACTION_FAILED: return "failed";
  case HTTP_TRANSACTION_ABORTED: return "aborted";
  }
  return "unknown";
}

static enum command_result show_status(struct shell *shell,
    struct shell_capability *entry, bool summary)
{
  struct http_transaction_status result;
  enum call_status status = observe(shell, entry, &result);
  if (printf("%s: %s phase=%s local_status=%u uncertain=%s", entry->name,
      type_name(entry->type), status == CALL_OK ? phase_name(result.phase) : "unavailable",
      entry->state->failure, entry->state->uncertain ? "yes" : "no") < 0) {
    return COMMAND_FATAL;
  }
  if (!summary && status == CALL_OK && printf(
      " submission=%llu HTTP=%llu framing=%llu status=%llu source=%llu cleanup=%llu"
      " uploaded=%llu received=%llu consumed=%llu deadline_ns=%llu",
      (unsigned long long)result.submission, (unsigned long long)result.http_status,
      (unsigned long long)result.framing, (unsigned long long)result.status,
      (unsigned long long)result.source_status, (unsigned long long)result.cleanup_status,
      (unsigned long long)result.uploaded, (unsigned long long)result.received,
      (unsigned long long)result.consumed, (unsigned long long)result.deadline_ns) < 0) {
    return COMMAND_FATAL;
  }
  if (putchar('\n') == EOF) {
    return COMMAND_FATAL;
  }
  if (status != CALL_OK && entry->value.observation) {
    return shell_error(shell, "request: %s: status unavailable (status %u)\n",
        entry->name, status);
  }
  bool failed = entry->state->failure != CALL_OK || entry->state->uncertain;
  if (status == CALL_OK && (result.phase == HTTP_TRANSACTION_FAILED ||
      result.phase == HTTP_TRANSACTION_ABORTED || result.framing == HTTP_FRAMING_FAILED ||
      result.status != CALL_OK || result.source_status != CALL_OK ||
      result.cleanup_status != CALL_OK)) {
    failed = true;
  }
  return failed ? COMMAND_FAILED : COMMAND_OK;
}

static bool seconds_value(const char *text, uint64_t *seconds)
{
  *seconds = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || *seconds > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    *seconds = *seconds * 10 + (*text - '0');
  }
  return *seconds != 0;
}

static void erase_headers(char *headers, size_t size)
{
  volatile char *bytes = headers;
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = 0;
  }
  free(headers);
}

static enum call_status read_headers(struct shell *shell, const char *path,
    char **headers, size_t *size)
{
  *headers = NULL;
  *size = 0;
  if (provider_http_uri(path) || !strncmp(path, "json+", 5)) {
    return CALL_WRONG_TYPE;
  }
  handle_t file = HANDLE_INVALID;
  enum call_status status = shell_open_redirect(shell, path, true, &file);
  struct handle_info info;
  uint64_t length = 0;
  if (status == CALL_OK) {
    status = handle_query(file, &info);
    if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE)) {
      status = CALL_WRONG_TYPE;
    }
  }
  if (status == CALL_OK) {
    status = file_size(file, &length);
    if (status == CALL_OK && length > ENDPOINT_DATA_MAX) {
      status = CALL_FILE_TOO_LARGE;
    }
  }
  char *bytes = NULL;
  if (status == CALL_OK && length) {
    bytes = malloc(length);
    if (!bytes) {
      status = CALL_NO_MEMORY;
    }
  }
  size_t total = 0;
  while (status == CALL_OK && total < length) {
    size_t read = 0;
    status = file_read(file, total, bytes + total, length - total, &read);
    if (status == CALL_OK && !read) {
      status = CALL_IO;
    }
    total += read;
  }
  if (file && handle_close(file) && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (status != CALL_OK) {
    erase_headers(bytes, bytes ? length : 0);
    return status;
  }
  *headers = bytes;
  *size = length;
  return CALL_OK;
}

static enum command_result prepare_command(struct shell *shell, char **arguments,
    size_t count)
{
  if (count < 5) {
    return shell_error(shell,
        "usage: request prepare NAME GET|POST URI [--timeout SECONDS] [--headers-file PATH]\n");
  }
  uint64_t method;
  if (!strcmp(arguments[3], "GET")) {
    method = HTTP_PREPARE_GET;
  } else if (!strcmp(arguments[3], "POST")) {
    method = HTTP_PREPARE_POST;
  } else {
    return shell_error(shell, "request: prepare method must be GET or POST\n");
  }
  uint64_t seconds = REQUEST_DEFAULT_SECONDS;
  bool timeout_set = false;
  const char *headers_path = NULL;
  for (size_t i = 5; i < count; i += 2) {
    if (i + 1 == count) {
      return shell_error(shell, "request: prepare option requires a value\n");
    }
    if (!strcmp(arguments[i], "--timeout") && !timeout_set &&
        seconds_value(arguments[i + 1], &seconds)) {
      timeout_set = true;
    } else if (!strcmp(arguments[i], "--headers-file") && !headers_path) {
      headers_path = arguments[i + 1];
    } else {
      return shell_error(shell, "request: invalid prepare option\n");
    }
  }
  struct shell_capability *entry;
  enum call_status status = shell_capability_reserve(shell, arguments[2], &entry);
  if (status != CALL_OK) {
    return shell_error(shell, "request: %s: reserve failed (status %u)\n", arguments[2], status);
  }
  char *headers = NULL;
  size_t headers_size = 0;
  if (headers_path) {
    status = read_headers(shell, headers_path, &headers, &headers_size);
  }
  if (status == CALL_OK) {
    status = shell_capability_prepare(shell, entry, arguments[4], method,
        headers, headers_size, seconds);
  } else {
    latch(entry, status);
  }
  erase_headers(headers, headers_size);
  return status == CALL_OK ? COMMAND_OK :
      shell_error(shell, "request: %s: prepare failed (status %u)\n", entry->name, status);
}

enum command_result shell_request_command(struct shell *shell,
    char **arguments, size_t count)
{
  if (count >= 2 && !strcmp(arguments[1], "prepare")) {
    return prepare_command(shell, arguments, count);
  }
  if (count == 2 && !strcmp(arguments[1], "list")) {
    enum command_result outcome = COMMAND_OK;
    if (shell->capabilities) {
      for (size_t i = 0; i < SHELL_CAPABILITY_LIMIT; ++i) {
        struct shell_capability *entry = &shell->capabilities->entries[i];
        if (entry->name[0]) {
          enum command_result result = show_status(shell, entry, true);
          if (result == COMMAND_FATAL) {
            return result;
          }
          if (result != COMMAND_OK) {
            outcome = result;
          }
        }
      }
    }
    return outcome;
  }
  if (count >= 2 && !strcmp(arguments[1], "copy")) {
    enum shell_capability_type type = SHELL_CAPABILITY_REQUEST;
    bool selected = false;
    size_t first = 2;
    if (count > 2 && !strcmp(arguments[2], "--response")) {
      type = SHELL_CAPABILITY_RESPONSE;
      selected = true;
      ++first;
    } else if (count > 2 && !strcmp(arguments[2], "--status")) {
      type = SHELL_CAPABILITY_STATUS;
      selected = true;
      ++first;
    }
    if (count != first + 2) {
      return shell_error(shell, "usage: request copy [--response|--status] NAME SOURCE\n");
    }
    struct shell_capability *source;
    enum call_status status = shell_capability_lookup(shell, arguments[first + 1], &source);
    if (status == CALL_OK) {
      status = copy_entry(shell, arguments[first], source, selected ? type : source->type);
    }
    return status == CALL_OK ? COMMAND_OK :
        shell_error(shell, "request: copy failed (status %u)\n", status);
  }
  if (count == 3) {
    struct shell_capability *entry;
    enum call_status status = shell_capability_lookup(shell, arguments[2], &entry);
    if (status != CALL_OK) {
      return shell_error(shell, "request: %s: lookup failed (status %u)\n", arguments[2], status);
    }
    if (!strcmp(arguments[1], "commit")) {
      return shell_capability_commit(shell, entry);
    }
    if (!strcmp(arguments[1], "status")) {
      return show_status(shell, entry, false);
    }
    if (!strcmp(arguments[1], "drop")) {
      status = drop(entry);
      return status == CALL_OK ? COMMAND_OK :
          shell_error(shell, "request: drop failed (status %u)\n", status);
    }
    if (!strcmp(arguments[1], "abort")) {
      if (!entry->value.control) {
        return shell_error(shell, "request: abort requires request control\n");
      }
      uint64_t deadline;
      status = operation_deadline(shell, REQUEST_DEFAULT_SECONDS, &deadline);
      if (status == CALL_OK) {
        status = http_transaction_abort(entry->value.control, deadline);
      }
      latch(entry, status == CALL_OK ? CALL_ABANDONED : status);
      if (status == CALL_OUTCOME_UNKNOWN || status == CALL_TIMED_OUT ||
          status == CALL_ENDPOINT_CLOSED) {
        entry->state->uncertain = true;
      }
      return status == CALL_OK ? COMMAND_OK :
          shell_error(shell, "request: %s: abort failed (status %u)\n", entry->name, status);
    }
  }
  return shell_error(shell,
      "usage: request prepare|commit|abort|status|drop|copy|list ...\n");
}

void shell_capabilities_close(struct shell *shell)
{
  if (!shell->capabilities) {
    return;
  }
  for (size_t i = 0; i < SHELL_CAPABILITY_LIMIT; ++i) {
    struct shell_capability *entry = &shell->capabilities->entries[i];
    if (!entry->name[0]) {
      continue;
    }
    enum call_status status = abort_unfinished(shell, entry);
    if (status != CALL_OK) {
      shell_error(shell, "request: %s: exit abort failed (status %u)\n", entry->name, status);
    }
    status = drop(entry);
    if (status != CALL_OK) {
      shell_error(shell, "request: exit close failed (status %u)\n", status);
    }
  }
  free(shell->capabilities);
  shell->capabilities = NULL;
}
