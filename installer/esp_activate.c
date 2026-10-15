#include "esp_activate.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <string.h>

#define ACTIVATE_DIRECTORY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define ACTIVATE_DIRECTORY_WRITE (ACTIVATE_DIRECTORY_READ | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)
#define ACTIVATE_CONTROL_RIGHTS (MOUNT_RIGHT_OPEN_ROOT | MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_WRITE)
#define ACTIVATE_IO_BYTES 3072u

static enum call_status native_grant(handle_t handle, uint64_t protocol, uint64_t rights)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return status;
  }
  return info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      (info.rights & rights) == rights ? CALL_OK : CALL_DENIED;
}

static enum call_status same_object(handle_t held, handle_t reopened, bool directory,
    uint64_t domain)
{
  struct file_info_reply before, after;
  enum call_status status = directory ? directory_info(held, &before) : file_info(held, &before);
  if (status == CALL_OK) {
    status = directory ? directory_info(reopened, &after) : file_info(reopened, &after);
  }
  if (status != CALL_OK) {
    return status;
  }
  return (before.valid & FILE_INFO_OBJECT_VALID) && (after.valid & FILE_INFO_OBJECT_VALID) &&
      before.domain == domain && after.domain == domain && before.object == after.object ?
      CALL_OK : CALL_DENIED;
}

static enum call_status current_target(const struct install_esp_activation_request *request)
{
  struct mount_state_reply current;
  enum call_status status = mount_observe(request->control, &current);
  if (status != CALL_OK) {
    return status;
  }
  const struct mount_state_reply *expected = &request->expected;
  if (!(expected->flags & MOUNT_STATE_IDENTITY_VALID) ||
      !(current.flags & MOUNT_STATE_IDENTITY_VALID) ||
      !(current.flags & MOUNT_STATE_READ_WRITE) ||
      (current.flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_FAILED |
        MOUNT_STATE_CLEANUP_PENDING | MOUNT_STATE_ALLOCATION_INVALID)) ||
      current.domain != expected->domain || current.device != expected->device ||
      current.partition != expected->partition || current.first_block != expected->first_block ||
      current.block_count != expected->block_count ||
      current.device_block_size != expected->device_block_size ||
      current.device_block_count != expected->device_block_count ||
      current.partition_attributes != expected->partition_attributes ||
      memcmp(current.disk_guid, expected->disk_guid, sizeof(current.disk_guid)) ||
      memcmp(current.partition_guid, expected->partition_guid, sizeof(current.partition_guid)) ||
      memcmp(current.partition_type, expected->partition_type, sizeof(current.partition_type))) {
    return CALL_DENIED;
  }
  handle_t root = HANDLE_INVALID;
  status = mount_reopen_fat32(request->control, ACTIVATE_DIRECTORY_READ, &root);
  if (status == CALL_OK) {
    status = same_object(request->root, root, true, current.domain);
  }
  if (root != HANDLE_INVALID) {
    handle_close(root);
  }
  return status;
}

static enum call_status write_text(handle_t parent, const char *name,
    const char *text, size_t bytes, handle_t *file)
{
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ | FILE_RIGHT_WRITE, file);
  if (status == CALL_NOT_FOUND) {
    status = directory_create(parent, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ | FILE_RIGHT_WRITE, file);
  }
  for (size_t offset = 0; status == CALL_OK && offset < bytes;) {
    size_t wanted = bytes - offset;
    if (wanted > ACTIVATE_IO_BYTES) {
      wanted = ACTIVATE_IO_BYTES;
    }
    size_t written = 0;
    status = file_write(*file, offset, text + offset, wanted, &written);
    if (status == CALL_OK && (!written || written > wanted)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (status == CALL_OK) {
      offset += written;
    }
  }
  if (status == CALL_OK) {
    status = file_resize(*file, bytes);
  }
  if (status == CALL_OK) {
    status = file_sync(*file);
  }
  return status;
}

static enum call_status verify_text(handle_t parent, const char *name, handle_t held,
    const char *text, size_t bytes, uint64_t domain)
{
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  uint64_t size = 0;
  if (status == CALL_OK) {
    status = same_object(held, file, false, domain);
  }
  if (status == CALL_OK) {
    status = file_size(file, &size);
    if (status == CALL_OK && size != bytes) {
      status = CALL_BAD_REQUEST;
    }
  }
  char actual[ACTIVATE_IO_BYTES];
  for (size_t offset = 0; status == CALL_OK && offset < bytes;) {
    size_t wanted = bytes - offset > sizeof(actual) ? sizeof(actual) : bytes - offset;
    size_t read = 0;
    status = file_read(file, offset, actual, wanted, &read);
    if (status == CALL_OK && (!read || read > wanted || memcmp(actual, text + offset, read))) {
      status = CALL_BAD_REQUEST;
    }
    if (status == CALL_OK) {
      offset += read;
    }
  }
  if (status == CALL_OK) {
    status = file_size(file, &size);
    if (status == CALL_OK && size != bytes) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (file != HANDLE_INVALID) {
    handle_close(file);
  }
  return status;
}

bool install_esp_activate_classic(const struct install_esp_activation_request *request,
    enum call_status *output_status)
{
  if (!output_status) {
    return false;
  }
  *output_status = CALL_BAD_REQUEST;
  if (!request || !request->configuration || !request->configuration_bytes ||
      request->configuration_bytes > UINT32_MAX || !request->revision ||
      !request->revision_bytes || request->revision_bytes > UINT32_MAX) {
    return false;
  }
  handle_t boot = HANDLE_INVALID, limine = HANDLE_INVALID;
  handle_t revision = HANDLE_INVALID, configuration = HANDLE_INVALID;
  handle_t reopened_boot = HANDLE_INVALID, reopened_limine = HANDLE_INVALID;
  enum call_status status = native_grant(request->root, PROTOCOL_DIRECTORY, ACTIVATE_DIRECTORY_WRITE);
  if (status == CALL_OK) {
    status = native_grant(request->control, PROTOCOL_MOUNT, ACTIVATE_CONTROL_RIGHTS);
  }
  if (status == CALL_OK) {
    status = current_target(request);
  }
  if (status == CALL_OK) {
    status = directory_lookup(request->root, "boot", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_WRITE, &boot);
  }
  if (status == CALL_OK) {
    status = directory_lookup(boot, "limine", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_WRITE, &limine);
  }
  if (status == CALL_OK) {
    status = write_text(boot, "revision", request->revision, request->revision_bytes, &revision);
  }
  if (status == CALL_OK) {
    status = write_text(limine, "limine.conf", request->configuration,
        request->configuration_bytes, &configuration);
  }
  if (status == CALL_OK) {
    status = directory_sync(limine);
  }
  if (status == CALL_OK) {
    status = directory_sync(boot);
  }
  if (status == CALL_OK) {
    status = mount_sync(request->control);
  }
  if (status == CALL_OK) {
    status = current_target(request);
  }
  if (status == CALL_OK) {
    status = directory_lookup(request->root, "boot", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_READ, &reopened_boot);
  }
  if (status == CALL_OK) {
    status = same_object(boot, reopened_boot, true, request->expected.domain);
  }
  if (status == CALL_OK) {
    status = directory_lookup(reopened_boot, "limine", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_READ, &reopened_limine);
  }
  if (status == CALL_OK) {
    status = same_object(limine, reopened_limine, true, request->expected.domain);
  }
  if (status == CALL_OK) {
    status = verify_text(reopened_limine, "limine.conf", configuration,
        request->configuration, request->configuration_bytes, request->expected.domain);
  }
  if (status == CALL_OK) {
    status = verify_text(reopened_boot, "revision", revision,
        request->revision, request->revision_bytes, request->expected.domain);
  }
  if (status == CALL_OK) {
    status = current_target(request);
  }
  handle_t handles[] = {reopened_limine, reopened_boot, configuration, revision, limine, boot};
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
    if (handles[i] != HANDLE_INVALID) {
      handle_close(handles[i]);
    }
  }
  *output_status = status;
  return status == CALL_OK;
}
