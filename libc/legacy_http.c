#include "legacy_http.h"
#include "thread_private.h"
#include "thread_platform.h"
#include <clock.h>
#include <provider_http_private.h>
#include <pyxis/working_path.h>
#include <string.h>

bool libc_legacy_http_path(const char *path)
{
  return provider_http_uri(path) ||
      (path && !strncmp(path, "json+", 5) && provider_http_uri(path + 5));
}

void libc_legacy_http_close(struct libc_legacy_open *opened)
{
  /* The typed result describes a handle owned in prepared, not a second grant. */
  opened->result.resource = HANDLE_INVALID;
  resource_result_close(&opened->result);
  http_prepared_close(&opened->prepared);
}

enum call_status libc_legacy_http_open(const char *path,
    const struct descriptor_mode *mode, const struct pyxis_http_options *options,
    bool prepare_only, struct libc_legacy_open *opened)
{
  *opened = (struct libc_legacy_open){0};
  if (!path || mode->append || mode->exclusive ||
      (mode->writable && !mode->truncate)) {
    return CALL_BAD_REQUEST;
  }
  opened->deadline = options ? options->deadline_ns : 0;
  enum call_status status = CALL_OK;
  if (!opened->deadline) {
    uint64_t now;
    status = clock_now(libc_thread_clock(), &now);
    if (status == CALL_OK) {
      if (now > UINT64_MAX - UINT64_C(30000000000)) {
        status = CALL_LIMIT;
      } else {
        opened->deadline = now + UINT64_C(30000000000);
      }
    }
  }
  struct pyxis_working_snapshot working = {0};
  if (status == CALL_OK) {
    status = pyxis_working_snapshot_init(&working, NULL);
  }
  opened->submitted = !prepare_only && !mode->writable;
  uint64_t method = mode->writable ? HTTP_PREPARE_POST : HTTP_PREPARE_GET;
  if (status == CALL_OK) {
    status = opened->submitted ?
        http_transaction_open_uri(&working.context, path, opened->deadline,
            &opened->prepared, &opened->response) :
        http_transaction_prepare_uri(&working.context, path, method,
            options ? options->flags : 0, options ? options->headers : NULL,
            options ? options->headers_size : 0, opened->deadline, &opened->prepared);
  }
  pyxis_working_snapshot_close(&working);
  if (status == CALL_OK) {
    handle_t resource = opened->submitted ? opened->prepared.response :
        opened->prepared.control;
    status = resource_result_adopt(resource, &opened->result);
  }
  if (status != CALL_OK) {
    libc_legacy_http_close(opened);
  }
  return status;
}
