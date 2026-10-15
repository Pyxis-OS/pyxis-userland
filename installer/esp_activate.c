#include "esp_activate.h"
#include "digest.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <stdlib.h>
#include <string.h>

#define ACTIVATE_DIRECTORY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define ACTIVATE_DIRECTORY_WRITE (ACTIVATE_DIRECTORY_READ | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)
#define ACTIVATE_CONTROL_RIGHTS (MOUNT_RIGHT_OPEN_ROOT | MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_WRITE)
#define ACTIVATE_IO_BYTES 3072u

static const char *const classic_payload_names[INSTALL_ESP_CLASSIC_PAYLOAD_COUNT] = {
  "BOOTX64.EFI", "caelum.elf", "initrd.cpio",
};

struct install_esp_classic_files {
  handle_t root, control; /* Borrowed. */
  struct mount_state_reply expected;
  struct config_file_digest payloads[INSTALL_ESP_CLASSIC_PAYLOAD_COUNT];
  handle_t efi, efi_boot, boot, limine;
  handle_t files[INSTALL_ESP_CLASSIC_PAYLOAD_COUNT];
  bool synchronized;
};

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

static enum call_status observe_target(handle_t control,
    const struct mount_state_reply *expected)
{
  struct mount_state_reply current;
  enum call_status status = mount_observe(control, &current);
  if (status != CALL_OK) {
    return status;
  }
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
  return CALL_OK;
}

static bool close_reference(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  handle_t closed = *handle;
  *handle = HANDLE_INVALID;
  return handle_close(closed) == 0;
}

static enum call_status current_target(handle_t held_root, handle_t control,
    const struct mount_state_reply *expected)
{
  enum call_status status = observe_target(control, expected);
  handle_t root = HANDLE_INVALID;
  if (status == CALL_OK) {
    status = mount_reopen_fat32(control, ACTIVATE_DIRECTORY_READ, &root);
  }
  if (status == CALL_OK) {
    status = same_object(held_root, root, true, expected->domain);
  }
  if (!close_reference(&root)) {
    status = CALL_BAD_HANDLE;
  }
  return status;
}

static bool digest_matches(const struct config_file_digest *left,
    const struct config_file_digest *right)
{
  return left->present && right->present && left->size == right->size &&
      !strcmp(left->sha256, right->sha256);
}

static enum call_status source_size(const struct install_source *source, uint64_t *size)
{
  if (source->memory) {
    *size = source->bytes;
    return CALL_OK;
  }
  return file_size(source->handle, size);
}

static enum call_status stream_payload(const struct install_source *source, handle_t target,
    const struct config_file_digest *expected, size_t capacity)
{
  uint64_t size = 0;
  enum call_status status = source_size(source, &size);
  if (status != CALL_OK || size != expected->size) {
    return status != CALL_OK ? status : CALL_BAD_REQUEST;
  }
  struct install_digest hash = {0};
  if (!install_digest_begin(&hash)) {
    return CALL_NO_MEMORY;
  }
  if (target != HANDLE_INVALID && capacity > FILE_WRITE_MAX_BYTES) {
    capacity = FILE_WRITE_MAX_BYTES;
  }
  unsigned char bytes[FILE_READ_MAX_BYTES];
  install_progress_begin(target == HANDLE_INVALID ? "Verifying classic boot payload" :
      "Staging classic boot payload", INSTALL_PROGRESS_WORK, NULL);
  install_progress_bytes_total(size);
  for (uint64_t offset = 0; status == CALL_OK && offset < size;) {
    size_t wanted = size - offset > capacity ? capacity : (size_t)(size - offset);
    size_t count = 0;
    if (source->memory) {
      memcpy(bytes, source->memory + (size_t)offset, wanted);
      count = wanted;
    } else {
      status = file_read(source->handle, offset, bytes, wanted, &count);
    }
    if (status == CALL_OK && (!count || count > wanted)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (status == CALL_OK && !install_digest_update(&hash, bytes, count)) {
      status = CALL_NO_MEMORY;
    }
    for (size_t copied = 0; status == CALL_OK && target != HANDLE_INVALID && copied < count;) {
      size_t written = 0;
      status = file_write(target, offset + copied, bytes + copied, count - copied, &written);
      if (status == CALL_OK && (!written || written > count - copied)) {
        status = CALL_OUTCOME_UNKNOWN;
      }
      if (status == CALL_OK) {
        copied += written;
      }
    }
    if (status == CALL_OK) {
      offset += count;
      install_progress_advance(count, 0);
    }
  }
  if (status == CALL_OK) {
    status = source_size(source, &size);
    if (status == CALL_OK && size != expected->size) {
      status = CALL_BAD_REQUEST;
    }
  }
  char sha256[65];
  if (status == CALL_OK && !install_digest_finish(&hash, sha256)) {
    status = CALL_NO_MEMORY;
  }
  if (status == CALL_OK && strcmp(sha256, expected->sha256)) {
    status = CALL_BAD_REQUEST;
  }
  install_digest_abort(&hash);
  install_progress_finish();
  return status;
}

static enum call_status classic_directory(handle_t parent, const char *name, handle_t *child)
{
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
      ACTIVATE_DIRECTORY_WRITE, child);
  if (status == CALL_NOT_FOUND) {
    status = directory_create(parent, name, DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_WRITE, child);
  }
  return status;
}

bool install_esp_classic_stage(const struct install_esp_activation_request *request,
    struct install_esp_classic_files **output, enum call_status *output_status)
{
  if (!output_status || !output) {
    return false;
  }
  *output_status = CALL_BAD_REQUEST;
  if (!request || *output) {
    return false;
  }
  struct install_esp_classic_files *result = calloc(1, sizeof(*result));
  if (!result) {
    *output_status = CALL_NO_MEMORY;
    return false;
  }
  *output = result;
  result->root = request->root;
  result->control = request->control;
  result->expected = request->expected;
  enum call_status status = native_grant(result->root, PROTOCOL_DIRECTORY, ACTIVATE_DIRECTORY_WRITE);
  if (status == CALL_OK) {
    status = native_grant(result->control, PROTOCOL_MOUNT, ACTIVATE_CONTROL_RIGHTS);
  }
  if (status == CALL_OK) {
    status = current_target(result->root, result->control, &result->expected);
  }
  for (unsigned i = 0; status == CALL_OK && i < INSTALL_ESP_CLASSIC_PAYLOAD_COUNT; ++i) {
    const struct install_source *source = &request->payloads[i];
    const struct config_file_digest *expected = &request->expected_payloads[i];
    if (!expected->present || !expected->size || expected->size > UINT32_MAX ||
        expected->size != source->bytes || strnlen(expected->sha256, sizeof(expected->sha256)) != 64) {
      status = CALL_BAD_REQUEST;
      break;
    }
    uint64_t size = 0;
    status = source_size(source, &size);
    if (status == CALL_OK && size != expected->size) {
      status = CALL_BAD_REQUEST;
    }
    struct config_file_digest actual;
    if (status == CALL_OK && !install_boot_payload_digest(source, &actual)) {
      status = CALL_UNAVAILABLE;
    }
    if (status == CALL_OK && !digest_matches(&actual, expected)) {
      status = CALL_BAD_REQUEST;
    }
    if (status == CALL_OK) {
      status = source_size(source, &size);
      if (status == CALL_OK && size != expected->size) {
        status = CALL_BAD_REQUEST;
      }
    }
    result->payloads[i] = *expected;
  }
  if (status == CALL_OK) {
    status = current_target(result->root, result->control, &result->expected);
  }
  if (status == CALL_OK) {
    status = classic_directory(result->root, "EFI", &result->efi);
  }
  if (status == CALL_OK) {
    status = classic_directory(result->efi, "BOOT", &result->efi_boot);
  }
  if (status == CALL_OK) {
    status = classic_directory(result->root, "boot", &result->boot);
  }
  if (status == CALL_OK) {
    status = classic_directory(result->boot, "limine", &result->limine);
  }
  for (unsigned i = 0; status == CALL_OK && i < INSTALL_ESP_CLASSIC_PAYLOAD_COUNT; ++i) {
    handle_t parent = i == INSTALL_ESP_CLASSIC_EFI ? result->efi_boot : result->boot;
    status = directory_lookup(parent, classic_payload_names[i], DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ | FILE_RIGHT_WRITE, &result->files[i]);
    if (status == CALL_NOT_FOUND) {
      status = directory_create(parent, classic_payload_names[i], DIRECTORY_KIND_FILE,
          FILE_RIGHT_READ | FILE_RIGHT_WRITE, &result->files[i]);
    }
    if (status == CALL_OK) {
      status = stream_payload(&request->payloads[i], result->files[i],
          &result->payloads[i], FILE_READ_MAX_BYTES);
    }
    if (status == CALL_OK) {
      status = file_resize(result->files[i], result->payloads[i].size);
    }
    if (status == CALL_OK) {
      status = file_sync(result->files[i]);
    }
  }
  handle_t parents[] = {result->efi_boot, result->efi, result->limine, result->boot, result->root};
  for (size_t i = 0; status == CALL_OK && i < sizeof(parents) / sizeof(parents[0]); ++i) {
    status = directory_sync(parents[i]);
  }
  if (status == CALL_OK) {
    status = mount_sync(result->control);
  }
  result->synchronized = status == CALL_OK;
  install_progress_finish();
  *output_status = status;
  return status == CALL_OK;
}

bool install_esp_classic_verify(struct install_esp_classic_files *result,
    enum call_status *output_status)
{
  if (!output_status) {
    return false;
  }
  *output_status = CALL_BAD_REQUEST;
  if (!result || !result->synchronized) {
    return false;
  }
  enum call_status status = current_target(result->root, result->control, &result->expected);
  if (status == CALL_OK) {
    status = mount_sync(result->control);
  }
  if (status == CALL_OK) {
    status = current_target(result->root, result->control, &result->expected);
  }
  handle_t efi = HANDLE_INVALID, efi_boot = HANDLE_INVALID;
  handle_t boot = HANDLE_INVALID, limine = HANDLE_INVALID;
  if (status == CALL_OK) {
    status = directory_lookup(result->root, "EFI", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_READ, &efi);
  }
  if (status == CALL_OK) {
    status = same_object(result->efi, efi, true, result->expected.domain);
  }
  if (status == CALL_OK) {
    status = directory_lookup(efi, "BOOT", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_READ, &efi_boot);
  }
  if (status == CALL_OK) {
    status = same_object(result->efi_boot, efi_boot, true, result->expected.domain);
  }
  if (status == CALL_OK) {
    status = directory_lookup(result->root, "boot", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_READ, &boot);
  }
  if (status == CALL_OK) {
    status = same_object(result->boot, boot, true, result->expected.domain);
  }
  if (status == CALL_OK) {
    status = directory_lookup(boot, "limine", DIRECTORY_KIND_DIRECTORY,
        ACTIVATE_DIRECTORY_READ, &limine);
  }
  if (status == CALL_OK) {
    status = same_object(result->limine, limine, true, result->expected.domain);
  }
  size_t capacity = FILE_READ_MAX_BYTES;
  if (result->expected.device_block_size && result->expected.device_block_size <= capacity) {
    capacity -= capacity % (size_t)result->expected.device_block_size;
  }
  for (unsigned i = 0; status == CALL_OK && i < INSTALL_ESP_CLASSIC_PAYLOAD_COUNT; ++i) {
    handle_t file = HANDLE_INVALID;
    handle_t parent = i == INSTALL_ESP_CLASSIC_EFI ? efi_boot : boot;
    status = directory_lookup(parent, classic_payload_names[i], DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &file);
    if (status == CALL_OK) {
      status = same_object(result->files[i], file, false, result->expected.domain);
    }
    if (status == CALL_OK) {
      struct install_source source = {.handle = file, .bytes = result->payloads[i].size};
      status = stream_payload(&source, HANDLE_INVALID, &result->payloads[i], capacity);
    }
    if (!close_reference(&file)) {
      status = CALL_BAD_HANDLE;
    }
  }
  if (status == CALL_OK) {
    status = current_target(result->root, result->control, &result->expected);
  }
  handle_t *parents[] = {&limine, &boot, &efi_boot, &efi};
  for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); ++i) {
    if (!close_reference(parents[i])) {
      status = CALL_BAD_HANDLE;
    }
  }
  *output_status = status;
  return status == CALL_OK;
}

bool install_esp_classic_close(struct install_esp_classic_files *result)
{
  if (!result) {
    return true;
  }
  bool closed = true;
  for (unsigned i = 0; i < INSTALL_ESP_CLASSIC_PAYLOAD_COUNT; ++i) {
    closed = close_reference(&result->files[i]) && closed;
  }
  handle_t *parents[] = {&result->limine, &result->boot, &result->efi_boot, &result->efi};
  for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); ++i) {
    closed = close_reference(parents[i]) && closed;
  }
  free(result);
  return closed;
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
  if (!close_reference(&file)) {
    status = CALL_BAD_HANDLE;
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
    status = current_target(request->root, request->control, &request->expected);
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
    status = current_target(request->root, request->control, &request->expected);
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
    status = current_target(request->root, request->control, &request->expected);
  }
  handle_t *handles[] = {&reopened_limine, &reopened_boot, &configuration, &revision, &limine, &boot};
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
    if (!close_reference(handles[i])) {
      status = CALL_BAD_HANDLE;
    }
  }
  *output_status = status;
  return status == CALL_OK;
}
