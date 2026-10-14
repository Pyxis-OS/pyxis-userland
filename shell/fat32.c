#include "../common/bootstrap_environment.h"
#include "shell.h"
#include <pyxis/working_path.h>
#include <disk.h>
#include <mount.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAT_ROOT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)
/* Leaves room for the longest selected-source suffix in a bundle component. */
#define FAT_BINDING_NAME_MAX (ENVIRONMENT_NAME_MAX - (sizeof(BUNDLE_SOURCE_PREFIX) - 1) - \
    (sizeof("_mount") - 1))

static bool number(const char *text, uint64_t *value)
{
  *value = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || *value > (UINT32_MAX - (*text - '0')) / 10) {
      return false;
    }
    *value = *value * 10 + (*text - '0');
  }
  return *value != 0;
}

static size_t binding_length(const char *uri)
{
  size_t length = strlen(uri);
  if (length <= 3 || length - 3 > FAT_BINDING_NAME_MAX ||
      strcmp(uri + length - 3, "://") ||
      (length == 4 && uri[0] == '.') || (length == 5 && !memcmp(uri, "..", 2))) {
    return 0;
  }
  for (size_t i = 0; i < length - 3; ++i) {
    unsigned char byte = uri[i];
    if (byte <= ' ' || byte == 0x7f || byte == ':' || byte == '/' || byte == '\\') {
      return 0;
    }
  }
  return length - 3;
}

static char *copy_name(const char *name, size_t length, const char *suffix)
{
  size_t tail = strlen(suffix);
  if (length > SIZE_MAX - tail - 1) {
    return NULL;
  }
  char *copy = malloc(length + tail + 1);
  if (copy) {
    memcpy(copy, name, length);
    memcpy(copy + length, suffix, tail + 1);
  }
  return copy;
}

static struct shell_fat_mount *find_mount(struct shell *shell, const char *uri)
{
  size_t length = binding_length(uri);
  for (size_t i = 0; length && i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    struct shell_fat_mount *mount = &shell->fat_mounts[i];
    if (mount->name && strlen(mount->name) == length && !memcmp(mount->name, uri, length)) {
      return mount;
    }
  }
  return NULL;
}

static struct shell_fat_mount *empty_mount(struct shell *shell)
{
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    if (!shell->fat_mounts[i].name) {
      return &shell->fat_mounts[i];
    }
  }
  return NULL;
}

static void forget_mount(struct shell_fat_mount *mount)
{
  if (mount->write_root != HANDLE_INVALID) {
    handle_close(mount->write_root);
  }
  if (mount->control != HANDLE_INVALID) {
    handle_close(mount->control);
  }
  free(mount->name);
  free(mount->write_name);
  free(mount->control_name);
  *mount = (struct shell_fat_mount){0};
}

void shell_fat_close(struct shell *shell)
{
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    forget_mount(&shell->fat_mounts[i]);
  }
}

static bool configured_source_present(struct shell *shell, const char *name)
{
  char binding[ENVIRONMENT_NAME_MAX + 1];
  if (strlen(name) > ENVIRONMENT_NAME_MAX - (sizeof(BUNDLE_SOURCE_PREFIX) - 1)) {
    return false;
  }
  snprintf(binding, sizeof(binding), "%s%s", BUNDLE_SOURCE_PREFIX, name);
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(shell->environment, binding, &entry, NULL, 0);
  if (status == CALL_OK && entry.kind == ENVIRONMENT_CAP) {
    handle_close(entry.handle);
  }
  return status == CALL_OK || status == CALL_BUFFER_TOO_SMALL;
}

static enum call_status publish_mount(struct shell *shell, struct shell_fat_mount *mount,
    handle_t root)
{
  struct environment_set_entry entries[3] = {0};
  char write[ENVIRONMENT_NAME_MAX + 1], control[ENVIRONMENT_NAME_MAX + 1];
  snprintf(write, sizeof(write), "%s%s", BUNDLE_SOURCE_PREFIX, mount->write_name);
  snprintf(control, sizeof(control), "%s%s", BUNDLE_SOURCE_PREFIX, mount->control_name);
  const char *names[] = {mount->name, write, control};
  handle_t handles[] = {root, mount->write_root, mount->control};
  size_t count = 0;
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < 3 && status == CALL_OK; ++i) {
    if (handles[i] == HANDLE_INVALID) {
      continue;
    }
    struct handle_info info;
    status = handle_query(handles[i], &info);
    if (status != CALL_OK || !environment_name(entries[count].name, names[i])) {
      status = status == CALL_OK ? CALL_BAD_REQUEST : status;
      break;
    }
    entries[count].kind = ENVIRONMENT_CAP;
    entries[count].flags = i == 0 ? ENVIRONMENT_EXPORTED : 0;
    entries[count].set_flags = ENVIRONMENT_SET_REQUIRE_ABSENT;
    entries[count].handle = handles[i];
    entries[count].rights = info.rights;
    entries[count++].transport = info.transport;
  }
  if (status == CALL_OK) {
    status = environment_set_batch(shell->environment, entries, count);
    mount->bound = status == CALL_OK || status == CALL_OUTCOME_UNKNOWN;
  }
  if (handle_close(root) != 0 && status == CALL_OK) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  return status;
}

static enum call_status selected_control(struct shell *shell, const char *name, handle_t *control)
{
  *control = HANDLE_INVALID;
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    struct shell_fat_mount *mount = &shell->fat_mounts[i];
    if (mount->control_name && !strcmp(mount->control_name, name)) {
      return handle_copy(mount->control, control);
    }
  }
  if (strlen(name) > ENVIRONMENT_NAME_MAX - (sizeof(BUNDLE_SOURCE_PREFIX) - 1)) {
    return CALL_BAD_REQUEST;
  }
  char binding[ENVIRONMENT_NAME_MAX + 1];
  snprintf(binding, sizeof(binding), "%s%s", BUNDLE_SOURCE_PREFIX, name);
  enum call_status status = environment_capability(shell->environment, binding, control);
  if (status == CALL_OK) {
    struct handle_info info;
    status = handle_query(*control, &info);
    if (status == CALL_OK && info.protocol != PROTOCOL_MOUNT) {
      status = CALL_WRONG_TYPE;
    }
    if (status != CALL_OK) {
      handle_close(*control);
      *control = HANDLE_INVALID;
    }
  }
  return status;
}

static enum command_result list_disks(struct shell *shell, handle_t inventory)
{
  for (uint64_t i = 0;; ++i) {
    struct disk_info info;
    enum call_status status = disks_enumerate(inventory, i, &info);
    if (status == CALL_NOT_FOUND) {
      return COMMAND_OK;
    }
    if (status != CALL_OK) {
      return shell_directory_error(shell, "shell: mount", "disk inventory", status);
    }
    if (printf("disk %llu: %llu blocks of %llu bytes, preparation %llu, GPT %llu, %s\n",
        (unsigned long long)info.id, (unsigned long long)info.block_count,
        (unsigned long long)info.block_size, (unsigned long long)info.preparation,
        (unsigned long long)info.gpt_status,
        info.flags & DISK_FLAG_WRITABLE ? "writable" : "read-only") < 0) {
      return COMMAND_FATAL;
    }
  }
}

/* A returned pair is already published. On a later preparation failure, drain
 * our data and explicitly retire; keep a refused controller for release retry. */
static void abandon_open(struct shell *shell, struct shell_fat_mount *mount)
{
  if (mount->write_root != HANDLE_INVALID) {
    handle_close(mount->write_root);
    mount->write_root = HANDLE_INVALID;
  }
  enum call_status status = mount_retire(mount->control);
  if (status == CALL_OK) {
    forget_mount(mount);
  } else {
    shell_error(shell, "shell: mount cleanup for %s:// failed (status %u); "
        "controller retained for release\n", mount->name, status);
  }
}

enum command_result shell_mount_fat32(struct shell *shell, char **arguments, size_t count)
{
  handle_t inventory = bootstrap_capability(BUNDLE_SOURCE_PREFIX "fat_mounts");
  if (count == 3 && !strcmp(arguments[2], "--list")) {
    return list_disks(shell, inventory);
  }
  uint64_t disk = 0, partition = 0, access = MOUNT_ACCESS_READ_ONLY;
  bool access_set = false;
  for (size_t i = 2; i + 1 < count; ++i) {
    if (!strcmp(arguments[i], "--disk") && !disk && i + 2 < count) {
      if (!number(arguments[++i], &disk)) {
        goto usage;
      }
    } else if (!strcmp(arguments[i], "--partition") && !partition && i + 2 < count) {
      if (!number(arguments[++i], &partition)) {
        goto usage;
      }
    } else if (!access_set && (!strcmp(arguments[i], "--read-only") ||
        !strcmp(arguments[i], "--read-write"))) {
      access = !strcmp(arguments[i], "--read-write") ? MOUNT_ACCESS_READ_WRITE :
          MOUNT_ACCESS_READ_ONLY;
      access_set = true;
    } else {
      goto usage;
    }
  }
  if (count < 3 || !disk || !partition || !access_set || !binding_length(arguments[count - 1])) {
    goto usage;
  }
  const char *uri = arguments[count - 1];
  if (find_mount(shell, uri)) {
    return shell_directory_error(shell, "shell: mount", uri, CALL_ALREADY_EXISTS);
  }
  struct shell_fat_mount *mount = empty_mount(shell);
  if (!mount) {
    return shell_directory_error(shell, "shell: mount", uri, CALL_LIMIT);
  }
  size_t length = binding_length(uri);
  char *binding = NULL;
  enum call_status status = shell_reserve_root(shell, uri, length, &binding);
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: mount", uri, status);
  }
  mount->name = copy_name(uri, length, "");
  mount->write_name = copy_name(uri, length, "_write");
  mount->control_name = copy_name(uri, length, "_mount");
  if (!mount->name || !mount->write_name || !mount->control_name) {
    status = CALL_NO_MEMORY;
  } else if (configured_source_present(shell, mount->write_name) ||
      configured_source_present(shell, mount->control_name)) {
    status = CALL_ALREADY_EXISTS;
  }
  if (status == CALL_OK) {
    struct environment_capture capture = {0};
    struct bundle_authority sources[BUNDLE_SOURCE_LIMIT];
    size_t source_count = 0;
    status = environment_capture_init(shell->environment,
        ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
    if (status == CALL_OK) {
      status = bundle_sources_from_capture(&capture, sources, BUNDLE_SOURCE_LIMIT, &source_count);
    }
    size_t added = access == MOUNT_ACCESS_READ_WRITE ? 2 : 1;
    if (status == CALL_OK && added > BUNDLE_SOURCE_LIMIT - source_count) {
      status = CALL_LIMIT;
    }
    environment_capture_close(&capture);
  }
  if (status != CALL_OK) {
    free(binding);
    forget_mount(mount);
    return shell_directory_error(shell, "shell: mount", uri, status);
  }
  struct mount_fat32_reply opened;
  status = disks_open_fat32(inventory, disk, partition, access,
      access == MOUNT_ACCESS_READ_WRITE ? DIRECTORY_CONTENT_RIGHTS : FAT_ROOT_RIGHTS,
      NULL, &opened);
  if (status != CALL_OK) {
    free(binding);
    forget_mount(mount);
    return shell_directory_error(shell, "shell: mount", uri, status);
  }
  mount->control = opened.control;
  handle_t root = opened.root;
  if (access == MOUNT_ACCESS_READ_WRITE) {
    mount->write_root = opened.root;
    status = mount_reopen_fat32(mount->control, FAT_ROOT_RIGHTS, &root);
  }
  struct mount_state_reply state;
  if (status == CALL_OK) {
    status = mount_observe(mount->control, &state);
    if (status == CALL_OK) {
      mount->domain = state.domain;
    }
  }
  if (status != CALL_OK) {
    if (root != HANDLE_INVALID && root != mount->write_root) {
      handle_close(root);
    }
    free(binding);
    abandon_open(shell, mount);
    return shell_directory_error(shell, "shell: mount", uri, status);
  }
  free(binding);
  status = publish_mount(shell, mount, root);
  if (status != CALL_OK) {
    if (!mount->bound) {
      abandon_open(shell, mount);
    }
    return shell_directory_error(shell, "shell: mount", uri, status);
  }
  mount->bound = true;
  return COMMAND_OK;

usage:
  return shell_error(shell, "usage: mount --fat32 --disk ID --partition N "
      "(--read-only | --read-write) NAME://\n"
      "       mount --fat32 --list\n");
}

enum command_result shell_sync_fat32(struct shell *shell, char **arguments, size_t count)
{
  if (count != 3) {
    return shell_error(shell, "usage: sync --mount NAME:// | sync --mount SOURCE\n");
  }
  struct shell_fat_mount *mount = find_mount(shell, arguments[2]);
  handle_t control = mount ? mount->control : HANDLE_INVALID;
  enum call_status status = mount ? CALL_OK : selected_control(shell, arguments[2], &control);
  if (status == CALL_OK) {
    status = mount_sync(control);
  }
  if (!mount && control != HANDLE_INVALID && handle_close(control) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  return status == CALL_OK ? COMMAND_OK :
      shell_directory_error(shell, "shell: sync", arguments[2], status);
}

static enum call_status directory_domain(handle_t directory, uint64_t *domain, bool *valid)
{
  struct file_info_reply info;
  enum call_status status = directory_info(directory, &info);
  *domain = info.domain;
  *valid = (info.valid & FILE_INFO_DOMAIN_VALID) != 0;
  return status;
}

static enum call_status leave_mount(struct shell *shell, uint64_t domain)
{
  if (!shell->directory->count) {
    return CALL_OK;
  }
  uint64_t current;
  bool valid;
  enum call_status status = directory_domain(
      shell->directory->directories[shell->directory->count - 1], &current, &valid);
  if (status != CALL_OK || !valid || current != domain) {
    return status;
  }
  handle_t home;
  status = shell_open_directory(shell, "home://", 0, &home);
  if (status != CALL_OK) {
    return status;
  }
  status = directory_domain(home, &current, &valid);
  handle_close(home);
  if (status != CALL_OK || (valid && current == domain)) {
    return status == CALL_OK ? CALL_BUSY : status;
  }
  status = shell_change_directory(shell, "home://");
  if (status == CALL_OK) {
    status = directory_domain(shell->directory->directories[shell->directory->count - 1],
        &current, &valid);
    if (status == CALL_OK && valid && current == domain) {
      status = CALL_BUSY;
    }
    if (status == CALL_OK && puts("shell: cwd moved to home:// before release") == EOF) {
      status = CALL_IO;
    }
  }
  return status;
}

/* Unpublish selected data entries while the capture still pins their domain.
 * Release every captured grant before retirement; controller entries survive. */
static enum call_status unpublish_data(struct shell *shell, uint64_t domain)
{
  struct environment_capture capture = {0};
  enum call_status status = environment_capture_init(shell->environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
  if (status != CALL_OK) {
    return status;
  }
  struct environment_unset_entry *entries = calloc(capture.count, sizeof(*entries));
  if (capture.count && !entries) {
    environment_capture_close(&capture);
    return CALL_NO_MEMORY;
  }
  size_t count = 0;
  for (size_t i = 0; i < capture.count && status == CALL_OK; ++i) {
    const struct environment_value *entry = &capture.values[i];
    if (entry->kind != ENVIRONMENT_CAP) {
      continue;
    }
    struct handle_info info;
    status = handle_query(entry->handle, &info);
    if (status != CALL_OK || info.protocol != PROTOCOL_DIRECTORY) {
      continue;
    }
    uint64_t selected;
    bool valid;
    status = directory_domain(entry->handle, &selected, &valid);
    if (status == CALL_OK && valid && selected == domain) {
      environment_name(entries[count].name, entry->name);
      entries[count++].identity = entry->identity;
    }
  }
  if (status == CALL_OK) {
    status = environment_unset_batch(shell->environment, entries, count);
  }
  free(entries);
  environment_capture_close(&capture);
  if (status != CALL_OK) {
    return status;
  }
  for (size_t i = 0; i < shell->initial_directory_count; ++i) {
    if (shell->initial_directories[i] == HANDLE_INVALID) {
      continue;
    }
    uint64_t selected;
    bool valid;
    enum call_status queried = directory_domain(shell->initial_directories[i], &selected, &valid);
    if (queried == CALL_BAD_HANDLE) {
      continue;
    }
    if (queried != CALL_OK) {
      return queried;
    }
    if (valid && selected == domain) {
      if (handle_close(shell->initial_directories[i]) != 0) {
        return CALL_OUTCOME_UNKNOWN;
      }
      shell->initial_directories[i] = HANDLE_INVALID;
    }
  }
  if (shell->history_home != HANDLE_INVALID) {
    uint64_t selected;
    bool valid;
    status = directory_domain(shell->history_home, &selected, &valid);
    if (status == CALL_OK && valid && selected == domain) {
      handle_close(shell->history_home);
      shell->history_home = HANDLE_INVALID;
      shell->history_saving = false;
    }
  }
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    struct shell_fat_mount *mount = &shell->fat_mounts[i];
    if (mount->domain == domain) {
      mount->bound = false;
      if (mount->write_root != HANDLE_INVALID) {
        if (handle_close(mount->write_root) != 0) {
          status = CALL_OUTCOME_UNKNOWN;
        }
        mount->write_root = HANDLE_INVALID;
      }
    }
  }
  return status;
}

enum command_result shell_release_fat32(struct shell *shell, char **arguments, size_t count)
{
  bool configured = count == 4 && !strcmp(arguments[1], "--control");
  if ((count != 2 && !configured) || !binding_length(arguments[count - 1])) {
    return shell_error(shell, "usage: release NAME:// | release --control SOURCE NAME://\n");
  }
  const char *uri = arguments[count - 1];
  struct shell_fat_mount *mount = find_mount(shell, uri);
  handle_t control = HANDLE_INVALID, root = HANDLE_INVALID;
  enum call_status status = configured ? selected_control(shell, arguments[2], &control) :
      mount ? handle_copy(mount->control, &control) : CALL_UNAVAILABLE;
  struct handle_info authority;
  if (status == CALL_OK) {
    status = handle_query(control, &authority);
  }
  if (status == CALL_OK && (authority.protocol != PROTOCOL_MOUNT ||
      authority.kind != HANDLE_KIND_NATIVE)) {
    status = CALL_WRONG_TYPE;
  }
  if (status == CALL_OK && (authority.rights & (MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_RETIRE)) !=
      (MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_RETIRE)) {
    status = CALL_DENIED;
  }
  struct mount_state_reply state;
  if (status == CALL_OK) {
    status = mount_observe(control, &state);
  }
  if (status != CALL_OK) {
    goto done;
  }
  if (mount && mount->domain != state.domain) {
    status = CALL_DENIED;
    goto done;
  }
  size_t length = binding_length(uri);
  char name[ENVIRONMENT_NAME_MAX + 1];
  if (length > ENVIRONMENT_NAME_MAX) {
    status = CALL_BAD_REQUEST;
    goto done;
  }
  memcpy(name, uri, length);
  name[length] = 0;
  status = environment_capability(shell->environment, name, &root);
  if (status == CALL_NOT_FOUND && mount && !mount->bound) {
    status = CALL_OK;
  }
  if (status != CALL_OK) {
    goto done;
  }
  if (root != HANDLE_INVALID) {
    uint64_t domain;
    bool valid;
    status = directory_domain(root, &domain, &valid);
    if (status == CALL_OK && (!valid || domain != state.domain)) {
      status = CALL_DENIED;
    }
    if (status != CALL_OK) {
      goto done;
    }
  }
  if (!mount) {
    mount = empty_mount(shell);
    if (!mount) {
      status = CALL_LIMIT;
      goto done;
    }
    mount->name = copy_name(uri, length, "");
    status = mount->name ? handle_copy(control, &mount->control) : CALL_NO_MEMORY;
    if (status != CALL_OK) {
      forget_mount(mount);
      mount = NULL;
      goto done;
    }
    mount->domain = state.domain;
    mount->bound = true;
  }
  status = leave_mount(shell, state.domain);
  if (status == CALL_OK) {
    status = unpublish_data(shell, state.domain);
  }
  if (root != HANDLE_INVALID) {
    if (handle_close(root) != 0 && status == CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    root = HANDLE_INVALID;
  }
  if (status == CALL_OK) {
    status = mount_retire(mount->control);
  }
  if (status == CALL_OK) {
    if (mount->control_name) {
      char control_name[ENVIRONMENT_NAME_MAX + 1];
      snprintf(control_name, sizeof(control_name), "%s%s", BUNDLE_SOURCE_PREFIX, mount->control_name);
      status = environment_unset(shell->environment, control_name);
    }
    if (status == CALL_OK) {
      forget_mount(mount);
    }
  }

done:
  if (root != HANDLE_INVALID) {
    handle_close(root);
  }
  if (control != HANDLE_INVALID) {
    handle_close(control);
  }
  if (status == CALL_OK) {
    return COMMAND_OK;
  }
  if (mount) {
    return shell_error(shell, "shell: release %s failed (status %u); controller retained%s\n",
        uri, status, mount->bound ? "" : ", binding removed; retry after references drain");
  }
  return shell_directory_error(shell, "shell: release", uri, status);
}
