#include "provider.h"
#include "trust.h"
#include "../common/dns.h"
#include "../common/provider_setup.h"
#include <clock.h>
#include <directory.h>
#include <endpoint.h>
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_STAGING_CAP (UINT64_C(16) * 1024 * 1024)

static bool parse_staging_cap(const char *text, uint64_t *cap)
{
  uint64_t value = 0;
  if (!text[0]) {
    return false;
  }
  for (size_t i = 0; text[i]; ++i) {
    unsigned digit = (unsigned char)text[i] - '0';
    if (digit > 9 || value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *cap = value;
  return value != 0;
}

static enum call_status close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return CALL_OK;
  }
  enum call_status status = handle_close(*handle) ? CALL_BAD_HANDLE : CALL_OK;
  *handle = HANDLE_INVALID;
  return status;
}

int main(int argc, char **argv)
{
  handle_t service = startup_resource("service");
  handle_t publication = startup_resource("publication");
  handle_t clock = startup_resource("clock");
  /* The launcher explicitly provisions an isolated RAM or NPFS directory. No
   * shared root/path or filename implicitly supplies staging privacy. */
  handle_t staging = startup_resource("staging");
  struct http_client client = {.scheme = HTTP_SCHEME_HTTP,
    .authority = {.tcp = startup_resource("tcp"), .udp = startup_resource("udp"),
      .random = startup_resource("random"), .clock = clock}};
  struct endpoint_create_reply endpoint = {0};
  handle_t published = HANDLE_INVALID, controller = HANDLE_INVALID;
  enum call_status status = CALL_OK, cleanup = CALL_OK;
  bool publication_attempted = false;
  bool cap_selected = false;
  uint64_t body_cap = DEFAULT_STAGING_CAP, staging_type = 0;
  const char *custom_bundle = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--https") && client.scheme != HTTP_SCHEME_HTTPS) {
      client.scheme = HTTP_SCHEME_HTTPS;
    } else if (!strcmp(argv[i], "--ca-bundle") && !custom_bundle &&
        i + 1 < argc && argv[i + 1][0]) {
      custom_bundle = argv[++i];
    } else if (!strcmp(argv[i], "--staging-cap") && !cap_selected &&
        i + 1 < argc && parse_staging_cap(argv[i + 1], &body_cap)) {
      cap_selected = true;
      ++i;
    } else {
      status = CALL_BAD_REQUEST;
      break;
    }
  }
  if (status != CALL_OK || (custom_bundle && client.scheme != HTTP_SCHEME_HTTPS)) {
    fputs("usage: httpfs [--https [--ca-bundle URI]] [--staging-cap BYTES]\n", stderr);
    status = CALL_BAD_REQUEST;
    goto done;
  }
  if (service == HANDLE_INVALID || publication == HANDLE_INVALID || clock == HANDLE_INVALID) {
    status = CALL_UNAVAILABLE;
    goto done;
  }
  if (!dns_select_server(NULL, &client.authority.dns_server)) {
    status = CALL_BAD_REQUEST;
    goto done;
  }
  if (staging != HANDLE_INVALID) {
    status = httpfs_staging_validate(staging, &staging_type);
    if (status != CALL_OK) {
      fprintf(stderr, "httpfs: staging refused (status %u); POST unavailable\n", status);
      status = close_handle(&staging);
      if (status != CALL_OK) {
        goto done;
      }
    }
  }
  if (staging_type != FILESYSTEM_TYPE_RAM) {
    body_cap = 0;
  }
  if (client.scheme == HTTP_SCHEME_HTTPS) {
    struct tls_authority authority = {client.authority.random, clock};
    struct tls_result result;
    if (!tls_runtime_create(&authority, &client.tls, &result)) {
      struct http_result failure = {.error = HTTP_TLS_ERROR, .tls_failure = result};
      status = http_result_status(&failure);
    } else {
      status = httpfs_trust_load(client.tls, HTTPFS_PUBLIC_BUNDLE, &result, &cleanup);
      if (status == CALL_OK && custom_bundle) {
        status = httpfs_trust_load(client.tls, custom_bundle, &result, &cleanup);
      }
      if (status == CALL_OK && !tls_runtime_ready(client.tls, &result)) {
        struct http_result failure = {.error = HTTP_TLS_ERROR, .tls_failure = result};
        status = http_result_status(&failure);
      }
    }
    if (status != CALL_OK) {
      fprintf(stderr, "httpfs: TLS setup failed (%s, native %u, library %d, verify 0x%x)\n",
          tls_error_name(result.error), result.native_status, result.library_error, result.verify_flags);
      goto done;
    }
  }
  status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    goto done;
  }
  status = close_handle(&endpoint.caller);
  if (status != CALL_OK) {
    goto done;
  }
  uint64_t rights = PROVIDER_RIGHT_OPEN_READ | (staging ? PROVIDER_RIGHT_OPEN_WRITE : 0);
  status = endpoint_export(service, endpoint.receiver, 1, PROTOCOL_PROVIDER,
      rights, HANDLE_TRANSPORT_CALL, &published, &controller);
  if (status != CALL_OK) {
    goto done;
  }
  struct endpoint_grant grant = {published, rights, HANDLE_TRANSPORT_CALL};
  status = provider_setup_report(publication, clock, CALL_OK, CALL_OK, &grant, &publication_attempted);
  cleanup = close_handle(&published);
  if (status == CALL_OK) {
    status = cleanup;
  }
  if (status == CALL_OK) {
    status = close_handle(&publication);
  }
  if (status == CALL_OK) {
    handle_t owned_controller = controller;
    controller = HANDLE_INVALID;
    status = httpfs_serve(service, endpoint.receiver, owned_controller, &client,
        custom_bundle != NULL, staging, body_cap);
  }

done:
  if (close_handle(&controller) != CALL_OK) {
    cleanup = CALL_BAD_HANDLE;
  }
  if (close_handle(&published) != CALL_OK || close_handle(&endpoint.caller) != CALL_OK ||
      close_handle(&endpoint.receiver) != CALL_OK) {
    cleanup = CALL_BAD_HANDLE;
  }
  tls_runtime_free(client.tls);
  if (status == CALL_OK) {
    status = cleanup;
  }
  if (!publication_attempted && publication != HANDLE_INVALID) {
    enum call_status reported = provider_setup_report(publication, clock, status, cleanup, NULL, NULL);
    if (reported != CALL_OK) {
      fprintf(stderr, "httpfs: setup report failed (status %u)\n", reported);
    }
  }
  if (close_handle(&publication) != CALL_OK && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "httpfs: provider failed (status %u)\n", status);
  }
  return status == CALL_OK ? 0 : 1;
}
