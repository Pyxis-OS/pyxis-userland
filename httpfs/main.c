#include "../common/bootstrap_environment.h"
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
#define DEFAULT_STORAGE_CAP (UINT64_C(64) * 1024 * 1024)

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

static enum call_status select_roots(enum http_scheme primary,
    char roles[PROVIDER_SETUP_ROOT_MAX * (PROVIDER_SETUP_ROLE_MAX + 1)],
    struct provider_setup_root roots[PROVIDER_SETUP_ROOT_MAX],
    enum httpfs_root indices[PROVIDER_SETUP_ROOT_MAX], size_t *root_count)
{
  struct environment_entry_reply entry = {0};
  enum call_status status = environment_lookup(startup_environment_handle(),
      PROVIDER_SETUP_ROOTS_ENTRY, &entry, roles,
      PROVIDER_SETUP_ROOT_MAX * (PROVIDER_SETUP_ROLE_MAX + 1));
  if (status != CALL_OK) {
    return status;
  }
  if (entry.kind != ENVIRONMENT_TEXT) {
    if (entry.handle != HANDLE_INVALID) {
      handle_close(entry.handle);
    }
    return CALL_WRONG_TYPE;
  }
  size_t count = 0;
  char *role = roles;
  for (;;) {
    char *next = strchr(role, ',');
    if (next) {
      *next++ = 0;
    }
    if (count == PROVIDER_SETUP_ROOT_MAX || !role[0] ||
        strlen(role) > PROVIDER_SETUP_ROLE_MAX) {
      return CALL_BAD_REQUEST;
    }
    enum httpfs_root index;
    if (!strcmp(role, PROVIDER_SETUP_DEFAULT_ROLE)) {
      index = primary == HTTP_SCHEME_HTTPS ? HTTPFS_ROOT_HTTPS : HTTPFS_ROOT_HTTP;
    } else if (!strcmp(role, "http")) {
      index = HTTPFS_ROOT_HTTP;
    } else if (!strcmp(role, "https")) {
      index = HTTPFS_ROOT_HTTPS;
    } else if (!strcmp(role, "json-http")) {
      index = HTTPFS_ROOT_JSON_HTTP;
    } else if (!strcmp(role, "json-https")) {
      index = HTTPFS_ROOT_JSON_HTTPS;
    } else {
      return CALL_BAD_REQUEST;
    }
    for (size_t i = 0; i < count; ++i) {
      if (indices[i] == index) {
        return CALL_BAD_REQUEST;
      }
    }
    roots[count].role = role;
    indices[count++] = index;
    if (!next) {
      break;
    }
    role = next;
  }
  *root_count = count;
  return CALL_OK;
}

int main(int argc, char **argv)
{
  handle_t service = bootstrap_capability("service");
  handle_t publication = bootstrap_take_capability("publication");
  handle_t clock = bootstrap_capability("clock");
  /* The launcher explicitly provisions an isolated RAM or NPFS directory. No
   * shared root/path or filename implicitly supplies staging privacy. */
  handle_t staging = bootstrap_capability("staging");
  struct http_client client = {.scheme = HTTP_SCHEME_HTTP,
    .authority = {.tcp = bootstrap_capability("tcp"), .udp = bootstrap_capability("udp"),
      .random = bootstrap_capability("random"), .clock = clock}};
  struct endpoint_create_reply endpoint = {0};
  handle_t published[HTTPFS_ROOT_COUNT] = {0};
  handle_t controllers[HTTPFS_ROOT_COUNT] = {0};
  struct provider_setup_root roots[PROVIDER_SETUP_ROOT_MAX] = {0};
  enum httpfs_root indices[PROVIDER_SETUP_ROOT_MAX];
  char roles[PROVIDER_SETUP_ROOT_MAX * (PROVIDER_SETUP_ROLE_MAX + 1)];
  size_t root_count = 0;
  enum call_status status = CALL_OK, cleanup = CALL_OK;
  bool publication_attempted = false;
  bool https_selected = false, cap_selected = false, storage_selected = false;
  uint64_t body_cap = DEFAULT_STAGING_CAP, staging_type = 0;
  uint64_t storage_cap = DEFAULT_STORAGE_CAP;
  const char *custom_bundle = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--https") && !https_selected) {
      https_selected = true;
      client.scheme = HTTP_SCHEME_HTTPS;
    } else if (!strcmp(argv[i], "--ca-bundle") && !custom_bundle &&
        i + 1 < argc && argv[i + 1][0]) {
      custom_bundle = argv[++i];
      client.scheme = HTTP_SCHEME_HTTPS;
    } else if (!strcmp(argv[i], "--staging-cap") && !cap_selected &&
        i + 1 < argc && parse_staging_cap(argv[i + 1], &body_cap)) {
      cap_selected = true;
      ++i;
    } else if (!strcmp(argv[i], "--storage-cap") && !storage_selected &&
        i + 1 < argc && parse_staging_cap(argv[i + 1], &storage_cap)) {
      storage_selected = true;
      ++i;
    } else {
      status = CALL_BAD_REQUEST;
      break;
    }
  }
  if (status != CALL_OK) {
    fputs("usage: httpfs [--https] [--ca-bundle URI] [--staging-cap BYTES] [--storage-cap BYTES]\n", stderr);
    status = CALL_BAD_REQUEST;
    goto done;
  }
  if (service == HANDLE_INVALID || publication == HANDLE_INVALID || clock == HANDLE_INVALID) {
    status = CALL_UNAVAILABLE;
    goto done;
  }
  status = select_roots(client.scheme, roles, roots, indices, &root_count);
  if (status != CALL_OK) {
    goto done;
  }
  if (!dns_select_server(NULL, &client.authority.dns_server)) {
    status = CALL_BAD_REQUEST;
    goto done;
  }
  if (staging != HANDLE_INVALID) {
    status = httpfs_staging_validate(staging, &staging_type);
    if (status != CALL_OK) {
      fprintf(stderr, "httpfs: staging refused (status %u); GET/POST unavailable\n", status);
      status = close_handle(&staging);
      if (status != CALL_OK) {
        goto done;
      }
    }
  }
  if (staging_type != FILESYSTEM_TYPE_RAM) {
    body_cap = 0;
    storage_cap = 0;
  }
  {
    /* Public TLS is optional for an HTTP-primary instance. */
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
      tls_runtime_free(client.tls);
      client.tls = NULL;
      if (client.scheme == HTTP_SCHEME_HTTPS || cleanup != CALL_OK) {
        goto done;
      }
      status = CALL_OK;
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
  size_t ready_count = 0;
  for (size_t i = 0; i < root_count; ++i) {
    enum httpfs_root index = indices[i];
    bool tls = index == HTTPFS_ROOT_HTTPS || index == HTTPFS_ROOT_JSON_HTTPS;
    bool json = index == HTTPFS_ROOT_JSON_HTTP || index == HTTPFS_ROOT_JSON_HTTPS;
    if (tls && !client.tls) {
      continue;
    }
    uint64_t rights = PROVIDER_RIGHT_OPEN_READ |
        (staging ? PROVIDER_RIGHT_OPEN_WRITE : 0) | (json ? 0 : PROVIDER_RIGHT_REMOVE);
    status = endpoint_export(service, endpoint.receiver, HTTPFS_ROOT_OBJECT_BASE + index,
        PROTOCOL_PROVIDER, rights, HANDLE_TRANSPORT_CALL, &published[index], &controllers[index]);
    if (status != CALL_OK) {
      goto done;
    }
    roots[ready_count++] = (struct provider_setup_root){.role = roots[i].role,
      .grant = {published[index], rights, HANDLE_TRANSPORT_CALL}};
  }
  status = provider_setup_report_roots(publication, clock, CALL_OK, CALL_OK,
      roots, ready_count, &publication_attempted);
  for (size_t i = 0; i < HTTPFS_ROOT_COUNT; ++i) {
    if (close_handle(&published[i]) != CALL_OK) {
      cleanup = CALL_BAD_HANDLE;
    }
  }
  if (status == CALL_OK) {
    status = cleanup;
  }
  if (status == CALL_OK) {
    status = close_handle(&publication);
  }
  if (status == CALL_OK) {
    handle_t owned_controllers[HTTPFS_ROOT_COUNT];
    memcpy(owned_controllers, controllers, sizeof(controllers));
    memset(controllers, 0, sizeof(controllers));
    status = httpfs_serve(service, endpoint.receiver, owned_controllers, &client,
        custom_bundle != NULL, staging, staging_type == FILESYSTEM_TYPE_RAM,
        body_cap, storage_cap);
  }

done:
  for (size_t i = 0; i < HTTPFS_ROOT_COUNT; ++i) {
    if (close_handle(&controllers[i]) != CALL_OK) {
      cleanup = CALL_BAD_HANDLE;
    }
    if (close_handle(&published[i]) != CALL_OK) {
      cleanup = CALL_BAD_HANDLE;
    }
  }
  if (close_handle(&endpoint.caller) != CALL_OK) {
    cleanup = CALL_BAD_HANDLE;
  }
  if (close_handle(&endpoint.receiver) != CALL_OK) {
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
