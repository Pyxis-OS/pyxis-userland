#include "esp_transfer.h"
#include "digest.h"
#include "progress.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <string.h>

static enum call_status fail(enum call_status status)
{
  install_progress_finish();
  return status;
}

static enum call_status grant(handle_t handle, uint64_t protocol, uint64_t rights)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return fail(status);
  }
  return info.kind == HANDLE_KIND_NATIVE && !info.transport && info.protocol == protocol &&
      (info.rights & rights) == rights ? CALL_OK : fail(CALL_DENIED);
}

/* Hash/copy exactly the recorded extent. A short successful native transfer
 * advances only its known suffix; an error ends the operation without replay. */
enum call_status install_esp_transfer_stream(const struct install_source *source, handle_t target,
    uint64_t expected_size, const char *expected_sha256, uint64_t max_bytes)
{
  uint64_t size = 0;
  enum call_status status = CALL_OK;
  if (source->memory) {
    size = source->bytes;
  } else {
    status = file_size(source->handle, &size);
  }
  if (status != CALL_OK) {
    return fail(status);
  }
  if (size != expected_size || size > max_bytes) {
    return fail(CALL_BAD_REQUEST);
  }
  install_progress_begin(target == HANDLE_INVALID ? "Verifying boot payload bytes" :
      "Staging owned boot payload", INSTALL_PROGRESS_WORK, NULL);
  install_progress_bytes_total(size);
  struct install_digest hash = {0};
  bool ok = install_digest_begin(&hash);
  unsigned char bytes[FILE_READ_MAX_BYTES];
  size_t capacity = sizeof(bytes);
  if (target != HANDLE_INVALID && capacity > FILE_WRITE_MAX_BYTES) {
    capacity = FILE_WRITE_MAX_BYTES;
  }
  char sha256[65];
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t count = 0;
    size_t wanted = size - offset > capacity ? capacity : (size_t)(size - offset);
    if (source->memory) {
      memcpy(bytes, source->memory + (size_t)offset, wanted);
      count = wanted;
      status = CALL_OK;
    } else {
      status = file_read(source->handle, offset, bytes, wanted, &count);
    }
    ok = status == CALL_OK && count && count <= wanted;
    if (status == CALL_OK && !ok) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (ok) {
      ok = install_digest_update(&hash, bytes, count);
      if (!ok) {
        status = CALL_NO_MEMORY;
      }
    }
    for (size_t written = 0; ok && target != HANDLE_INVALID && written < count;) {
      size_t amount = 0;
      status = file_write(target, offset + written, bytes + written, count - written, &amount);
      ok = status == CALL_OK && amount && amount <= count - written;
      if (status == CALL_OK && !ok) {
        status = CALL_OUTCOME_UNKNOWN;
      }
      written += amount;
    }
    if (ok) {
      offset += count;
      install_progress_advance(count, 0);
    }
  }
  if (ok) {
    if (!source->memory) {
      status = file_size(source->handle, &size);
    }
    ok = status == CALL_OK && size == expected_size;
    if (status == CALL_OK && !ok) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (ok) {
    ok = install_digest_finish(&hash, sha256) && !strcmp(sha256, expected_sha256);
    if (!ok) {
      status = CALL_BAD_REQUEST;
    }
  }
  install_digest_abort(&hash);
  install_progress_finish();
  return ok ? CALL_OK : fail(status == CALL_OK ? CALL_NO_MEMORY : status);
}

/* RAM workspace authority is supplied explicitly by the trusted caller. Its
 * name is removed before bytes or a writable reference can escape. */
enum call_status install_esp_transfer_workspace(handle_t parent, handle_t *file)
{
  uint64_t rights = DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
  enum call_status status = grant(parent, PROTOCOL_DIRECTORY, rights);
  if (status != CALL_OK) {
    return status;
  }
  struct directory_filesystem_info filesystem;
  status = directory_filesystem_info(parent, &filesystem);
  if (status != CALL_OK) {
    return fail(status);
  }
  if (filesystem.type != FILESYSTEM_TYPE_RAM) {
    return fail(CALL_DENIED);
  }
  status = directory_create(parent, "esp-transfer", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ | FILE_RIGHT_WRITE, file);
  if (status == CALL_OK) {
    status = directory_remove(parent, "esp-transfer", DIRECTORY_KIND_FILE);
  }
  return status == CALL_OK ? CALL_OK : fail(status);
}

/* Staging hashes the source range confirmed by each transfer. Verification
 * first transfers FAT bytes into RAM and hashes only that confirmed RAM range.
 * No error is retried, and no unconfirmed tail participates in a digest. */
enum call_status install_esp_transfer_file(handle_t source, handle_t destination, bool verifying,
    uint64_t expected_size, const char *expected_sha256, uint64_t max_bytes)
{
  uint64_t size;
  enum call_status status = file_size(source, &size);
  if (status != CALL_OK) {
    return fail(status);
  }
  if (size != expected_size || size > max_bytes) {
    return fail(CALL_BAD_REQUEST);
  }
  install_progress_begin(verifying ? "Verifying boot payload bytes" : "Staging owned boot payload",
      INSTALL_PROGRESS_WORK, NULL);
  install_progress_bytes_total(size);
  struct install_digest hash = {0};
  bool ok = install_digest_begin(&hash);
  unsigned char bytes[FILE_READ_MAX_BYTES];
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t wanted = size - offset > FILE_TRANSFER_MAX_BYTES ?
        FILE_TRANSFER_MAX_BYTES : (size_t)(size - offset);
    size_t transferred = 0;
    status = file_transfer(destination, verifying ? 0 : offset, source, offset, wanted, &transferred);
    ok = status == CALL_OK && transferred && transferred <= wanted;
    if (status == CALL_OK && !ok) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    for (size_t consumed = 0; ok && consumed < transferred;) {
      size_t capacity = transferred - consumed;
      if (capacity > sizeof(bytes)) {
        capacity = sizeof(bytes);
      }
      size_t count = 0;
      status = file_read(verifying ? destination : source,
          (verifying ? 0 : offset) + consumed, bytes, capacity, &count);
      ok = status == CALL_OK && count && count <= capacity;
      if (status == CALL_OK && !ok) {
        status = CALL_BAD_REQUEST;
      }
      if (ok && !install_digest_update(&hash, bytes, count)) {
        ok = false;
        status = CALL_NO_MEMORY;
      }
      if (ok) {
        consumed += count;
      }
    }
    if (ok) {
      offset += transferred;
      install_progress_advance(transferred, 0);
    }
  }
  if (ok) {
    status = file_size(source, &size);
    ok = status == CALL_OK && size == expected_size;
    if (status == CALL_OK && !ok) {
      status = CALL_BAD_REQUEST;
    }
  }
  char sha256[65];
  if (ok) {
    ok = install_digest_finish(&hash, sha256) && !strcmp(sha256, expected_sha256);
    if (!ok) {
      status = CALL_BAD_REQUEST;
    }
  }
  install_digest_abort(&hash);
  install_progress_finish();
  return ok ? CALL_OK : fail(status == CALL_OK ? CALL_NO_MEMORY : status);
}

enum call_status install_esp_transfer_source(const struct install_source *source, handle_t target,
    handle_t workspace, uint64_t expected_size, const char *expected_sha256, uint64_t max_bytes)
{
  handle_t file = source->handle;
  enum call_status status;
  if (source->memory) {
    status = file_resize(workspace, 0);
    if (status != CALL_OK) {
      return fail(status);
    }
    status = install_esp_transfer_stream(source, workspace, expected_size, expected_sha256, max_bytes);
    if (status != CALL_OK) {
      return status;
    }
    file = workspace;
  }
  status = install_esp_transfer_file(file, target, false, expected_size, expected_sha256, max_bytes);
  if (status == CALL_OK && source->memory) {
    status = file_resize(workspace, 0);
    if (status != CALL_OK) {
      return fail(status);
    }
  }
  return status;
}
