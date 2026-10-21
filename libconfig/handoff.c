#include "handoff.h"
#include "literal.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

static const char *const file_names[CONFIG_LUA_COUNT] = {
  "boot.lua", "session.lua", "network.lua"
};

static bool failed(enum call_status *status, enum call_status failure)
{
  *status = failure;
  return false;
}

static bool native_grant(handle_t handle, uint64_t protocol, uint64_t rights,
    bool exact, enum call_status *status)
{
  struct handle_info info;
  *status = handle_query(handle, &info);
  if (*status != CALL_OK) {
    return false;
  }
  return (info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol && !info.transport &&
      (exact ? info.rights == rights : (info.rights & rights) == rights)) ||
      failed(status, CALL_DENIED);
}

bool config_handoff_close(struct config_handoff *handoff)
{
  if (!handoff) {
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (handoff->files[i] != HANDLE_INVALID) {
      ok = handle_close(handoff->files[i]) == 0 && ok;
      handoff->files[i] = HANDLE_INVALID;
    }
  }
  return ok;
}

void config_handoff_bytes_free(struct config_handoff_bytes *snapshot)
{
  if (snapshot) {
    free(snapshot->bytes);
    *snapshot = (struct config_handoff_bytes){0};
  }
}

static bool source_valid(const struct config_selection *selection, size_t index)
{
  if (!selection->bytes[index] || !selection->sizes[index] ||
      selection->sizes[index] > LITERAL_SOURCE_LIMIT) {
    return false;
  }
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  bool ok = literal_parse(selection->bytes[index], selection->sizes[index],
      &document, &diagnostic);
  literal_free(document);
  return ok;
}

bool config_handoff_file(handle_t private_parent, const char *name, const char *bytes,
    size_t size, handle_t *file, enum call_status *status)
{
  handle_t writer = HANDLE_INVALID, reader = HANDLE_INVALID;
  *status = directory_create(private_parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ | FILE_RIGHT_WRITE, &writer);
  bool ok = *status == CALL_OK;
  if (ok) {
    *status = directory_remove(private_parent, name, DIRECTORY_KIND_FILE);
    ok = *status == CALL_OK &&
        native_grant(writer, PROTOCOL_FILE, FILE_RIGHT_READ | FILE_RIGHT_WRITE, true, status);
  }
  for (size_t offset = 0; ok && offset < size;) {
    size_t written = 0;
    size_t wanted = size - offset;
    if (wanted > FILE_WRITE_MAX_BYTES) {
      wanted = FILE_WRITE_MAX_BYTES;
    }
    *status = file_write(writer, offset, bytes + offset, wanted, &written);
    ok = *status == CALL_OK;
    if (ok && (!written || written > wanted)) {
      ok = failed(status, CALL_OUTCOME_UNKNOWN);
    }
    if (ok) {
      offset += written;
    }
  }
  if (ok) {
    *status = file_sync(writer);
    ok = *status == CALL_OK;
  }
  if (ok) {
    *status = handle_copy_restricted(writer, FILE_RIGHT_READ, 0, &reader);
    ok = *status == CALL_OK && native_grant(reader, PROTOCOL_FILE, FILE_RIGHT_READ, true, status);
  }
  if (writer != HANDLE_INVALID && handle_close(writer) != 0) {
    ok = failed(status, CALL_OUTCOME_UNKNOWN);
  }
  if (ok) {
    *file = reader;
  } else if (reader != HANDLE_INVALID && handle_close(reader) != 0) {
    failed(status, CALL_OUTCOME_UNKNOWN);
  }
  return ok;
}

bool config_handoff_create(handle_t private_parent, const struct config_selection *selection,
    struct config_handoff *handoff, enum call_status *status)
{
  if (!status) {
    return false;
  }
  *status = CALL_OK;
  if (!selection || !handoff) {
    return failed(status, CALL_BAD_REQUEST);
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (handoff->files[i] != HANDLE_INVALID || !source_valid(selection, i)) {
      return failed(status, CALL_BAD_REQUEST);
    }
  }
  uint64_t rights = DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
  struct directory_filesystem_info filesystem;
  if (!native_grant(private_parent, PROTOCOL_DIRECTORY, rights, false, status)) {
    return false;
  }
  *status = directory_filesystem_info(private_parent, &filesystem);
  if (*status != CALL_OK || filesystem.type != FILESYSTEM_TYPE_RAM) {
    return *status != CALL_OK ? false : failed(status, CALL_DENIED);
  }
  bool ok = true;
  for (size_t i = 0; ok && i < CONFIG_LUA_COUNT; ++i) {
    ok = config_handoff_file(private_parent, file_names[i], selection->bytes[i],
        selection->sizes[i], &handoff->files[i], status);
  }
  if (!ok && !config_handoff_close(handoff)) {
    failed(status, CALL_OUTCOME_UNKNOWN);
  }
  return ok;
}

static bool file_state(handle_t file, struct file_info_reply *info, enum call_status *status)
{
  *status = file_info(file, info);
  if (*status != CALL_OK) {
    return false;
  }
  uint64_t required = FILE_INFO_SIZE_VALID | FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID;
  return ((info->valid & required) == required && info->size &&
      info->size <= LITERAL_SOURCE_LIMIT) || failed(status, CALL_BAD_REQUEST);
}

bool config_handoff_read(handle_t file, struct config_handoff_bytes *snapshot,
    enum call_status *status)
{
  if (!status) {
    return false;
  }
  *status = CALL_OK;
  if (!snapshot) {
    return failed(status, CALL_BAD_REQUEST);
  }
  config_handoff_bytes_free(snapshot);
  struct file_info_reply initial, final;
  if (!native_grant(file, PROTOCOL_FILE, FILE_RIGHT_READ, true, status) ||
      !file_state(file, &initial, status)) {
    return false;
  }
  char *bytes = malloc((size_t)initial.size + 1);
  if (!bytes) {
    return failed(status, CALL_NO_MEMORY);
  }
  bool ok = true;
  for (size_t offset = 0; ok && offset < initial.size;) {
    size_t count = 0;
    *status = file_read(file, offset, bytes + offset, (size_t)initial.size - offset, &count);
    ok = *status == CALL_OK;
    if (ok && (!count || count > initial.size - offset)) {
      ok = failed(status, CALL_BAD_REQUEST);
    }
    if (ok) {
      offset += count;
    }
  }
  unsigned char compare[FILE_READ_MAX_BYTES];
  for (size_t offset = 0; ok && offset < initial.size;) {
    size_t count = 0, wanted = (size_t)initial.size - offset;
    if (wanted > sizeof(compare)) {
      wanted = sizeof(compare);
    }
    *status = file_read(file, offset, compare, wanted, &count);
    ok = *status == CALL_OK;
    if (ok && (!count || count > wanted || memcmp(bytes + offset, compare, count))) {
      ok = failed(status, CALL_BAD_REQUEST);
    }
    if (ok) {
      offset += count;
    }
  }
  if (ok) {
    ok = file_state(file, &final, status);
    if (ok && (initial.size != final.size || initial.domain != final.domain ||
        initial.object != final.object)) {
      ok = failed(status, CALL_BAD_REQUEST);
    }
  }
  if (ok) {
    bytes[initial.size] = '\0';
    snapshot->bytes = bytes;
    snapshot->size = (size_t)initial.size;
  } else {
    free(bytes);
  }
  return ok;
}
