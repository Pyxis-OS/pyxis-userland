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
#define FAT_BINDING_NAME_MAX (255 - sizeof("_mount") + 1)

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

static handle_t configured_source(const char *name)
{
  const struct startup_binding *resources = startup_resources();
  size_t prefix = sizeof(BUNDLE_SOURCE_PREFIX) - 1;
  for (size_t i = 0; i < startup_resource_count(); ++i) {
    const char *binding = (const char *)(uintptr_t)resources[i].name;
    if (!strncmp(binding, BUNDLE_SOURCE_PREFIX, prefix) && !strcmp(binding + prefix, name)) {
      struct handle_info info;
      if (handle_query(resources[i].handle, &info) != CALL_BAD_HANDLE) {
        return resources[i].handle;
      }
    }
  }
  return HANDLE_INVALID;
}

enum call_status shell_fat_authority(struct shell *shell,
    struct bundle_authority *authority, size_t capacity, size_t *count)
{
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    struct shell_fat_mount *mount = &shell->fat_mounts[i];
    const char *names[] = {mount->write_name, mount->control_name};
    handle_t handles[] = {mount->write_root, mount->control};
    uint64_t protocols[] = {PROTOCOL_DIRECTORY, PROTOCOL_MOUNT};
    for (size_t j = 0; j < 2; ++j) {
      if (!names[j] || handles[j] == HANDLE_INVALID) {
        continue;
      }
      for (size_t k = 0; k < *count; ++k) {
        if (!strcmp(authority[k].resource, names[j])) {
          return CALL_ALREADY_EXISTS;
        }
      }
      if (*count == capacity) {
        return CALL_LIMIT;
      }
      authority[(*count)++] = (struct bundle_authority){names[j], handles[j], protocols[j]};
    }
  }
  return CALL_OK;
}

static enum call_status selected_control(struct shell *shell, const char *name, handle_t *control)
{
  *control = HANDLE_INVALID;
  struct bundle_authority sources[BUNDLE_SOURCE_LIMIT];
  size_t count;
  enum call_status status = bundle_sources_from_startup(sources, BUNDLE_SOURCE_LIMIT, &count);
  if (status == CALL_OK) {
    status = shell_fat_authority(shell, sources, BUNDLE_SOURCE_LIMIT, &count);
  }
  if (status != CALL_OK) {
    return status;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(sources[i].resource, name)) {
      if (sources[i].protocol != PROTOCOL_MOUNT) {
        return CALL_WRONG_TYPE;
      }
      *control = sources[i].source;
      return CALL_OK;
    }
  }
  return CALL_UNAVAILABLE;
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
  handle_t inventory = configured_source("fat_mounts");
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
  } else if (configured_source(mount->write_name) != HANDLE_INVALID ||
      configured_source(mount->control_name) != HANDLE_INVALID) {
    status = CALL_ALREADY_EXISTS;
  }
  if (status == CALL_OK) {
    struct bundle_authority sources[BUNDLE_SOURCE_LIMIT];
    size_t source_count;
    status = bundle_sources_from_startup(sources, BUNDLE_SOURCE_LIMIT, &source_count);
    if (status == CALL_OK) {
      status = shell_fat_authority(shell, sources, BUNDLE_SOURCE_LIMIT, &source_count);
    }
    size_t added = access == MOUNT_ACCESS_READ_WRITE ? 2 : 1;
    if (status == CALL_OK && added > BUNDLE_SOURCE_LIMIT - source_count) {
      status = CALL_LIMIT;
    }
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
  status = shell_publish_root(shell, binding, root);
  if (status != CALL_OK) {
    abandon_open(shell, mount);
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

static bool contains(const handle_t *handles, size_t count, handle_t handle)
{
  for (size_t i = 0; i < count; ++i) {
    if (handles[i] == handle) {
      return true;
    }
  }
  return false;
}

static void append_handle(handle_t *handles, size_t *count, handle_t handle)
{
  if (handle != HANDLE_INVALID && !contains(handles, *count, handle)) {
    handles[(*count)++] = handle;
  }
}

/* Identify every selected data source while a root still pins the domain.
 * Closing startup grants leaves stale snapshot names; the common grant collector
 * ignores BAD_HANDLE rather than resurrecting those capabilities. */
static enum call_status collect_data(struct shell *shell, uint64_t domain,
    handle_t root, handle_t *handles, size_t *count)
{
  *count = 0;
  append_handle(handles, count, root);
  const struct startup_binding *resources = startup_resources();
  size_t prefix = sizeof(BUNDLE_SOURCE_PREFIX) - 1;
  for (size_t i = 0; i < startup_resource_count(); ++i) {
    const char *name = (const char *)(uintptr_t)resources[i].name;
    if (strncmp(name, BUNDLE_SOURCE_PREFIX, prefix)) {
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(resources[i].handle, &info);
    if (status == CALL_BAD_HANDLE) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (info.protocol != PROTOCOL_DIRECTORY) {
      continue;
    }
    uint64_t selected;
    bool valid;
    status = directory_domain(resources[i].handle, &selected, &valid);
    if (status != CALL_OK) {
      return status;
    }
    if (valid && selected == domain) {
      append_handle(handles, count, resources[i].handle);
    }
  }
  /* libc owns copies of this initial chain. Moving its cwd does not release
   * the original startup grants, so include them in explicit FAT drainage. */
  const handle_t *startup_cwd = startup_working_directories();
  for (size_t i = 0; i < startup_working_directory_count(); ++i) {
    uint64_t selected;
    bool valid;
    enum call_status status = directory_domain(startup_cwd[i], &selected, &valid);
    if (status == CALL_BAD_HANDLE) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (valid && selected == domain) {
      append_handle(handles, count, startup_cwd[i]);
    }
  }
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    if (shell->fat_mounts[i].domain == domain) {
      append_handle(handles, count, shell->fat_mounts[i].write_root);
    }
  }
  return CALL_OK;
}

static enum call_status unbind_data(struct shell *shell, const handle_t *handles, size_t count)
{
  struct path_root previous[PROFILE_DIRECTORY_LIMIT];
  bool owned[PROFILE_DIRECTORY_LIMIT];
  size_t old_count = shell->directory->root_count, retained = 0;
  memcpy(previous, shell->roots, old_count * sizeof(*previous));
  memcpy(owned, shell->owns_root, old_count * sizeof(*owned));
  for (size_t i = 0; i < old_count; ++i) {
    if (!contains(handles, count, previous[i].handle)) {
      shell->roots[retained] = previous[i];
      shell->owns_root[retained++] = owned[i];
    }
  }
  enum call_status status = pyxis_working_bindings(shell->roots, retained, shell->namespace);
  if (status != CALL_OK) {
    memcpy(shell->roots, previous, old_count * sizeof(*previous));
    memcpy(shell->owns_root, owned, old_count * sizeof(*owned));
    return status;
  }
  for (size_t i = 0; i < old_count; ++i) {
    if (owned[i] && contains(handles, count, previous[i].handle)) {
      free((void *)previous[i].name);
    }
  }
  for (size_t i = retained; i < old_count; ++i) {
    shell->roots[i] = (struct path_root){0};
    shell->owns_root[i] = false;
  }
  for (size_t i = 0; i < SHELL_FAT_MOUNT_LIMIT; ++i) {
    struct shell_fat_mount *mount = &shell->fat_mounts[i];
    if (contains(handles, count, mount->write_root)) {
      mount->write_root = HANDLE_INVALID;
    }
    if (mount->bound) {
      mount->bound = false;
      for (size_t j = 0; j < retained; ++j) {
        mount->bound |= !strcmp(mount->name, shell->roots[j].name);
      }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (handle_close(handles[i]) != 0) {
      status = CALL_OUTCOME_UNKNOWN;
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
  handle_t control = mount ? mount->control : HANDLE_INVALID;
  enum call_status status = configured ? selected_control(shell, arguments[2], &control) :
      control == HANDLE_INVALID ? CALL_UNAVAILABLE : CALL_OK;
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
    return shell_directory_error(shell, "shell: release", uri, status);
  }
  if (mount) {
    struct mount_state_reply retained;
    status = mount_observe(mount->control, &retained);
    if (status == CALL_OK && retained.domain != state.domain) {
      status = CALL_DENIED;
    }
    if (status != CALL_OK) {
      return shell_directory_error(shell, "shell: release", uri, status);
    }
    mount->domain = retained.domain;
  }
  handle_t root = HANDLE_INVALID;
  size_t length = binding_length(uri);
  for (size_t i = 0; i < shell->directory->root_count; ++i) {
    if (strlen(shell->roots[i].name) == length && !memcmp(shell->roots[i].name, uri, length)) {
      root = shell->roots[i].handle;
    }
  }
  if (root == HANDLE_INVALID && (!mount || mount->bound)) {
    return shell_directory_error(shell, "shell: release", uri, CALL_NOT_FOUND);
  }
  if (root != HANDLE_INVALID) {
    uint64_t domain;
    bool valid;
    status = directory_domain(root, &domain, &valid);
    if (status == CALL_OK && (!valid || domain != state.domain)) {
      status = CALL_DENIED;
    }
    if (status != CALL_OK) {
      return shell_directory_error(shell, "shell: release", uri, status);
    }
  }
  if (!mount) {
    mount = empty_mount(shell);
    if (!mount) {
      return shell_directory_error(shell, "shell: release", uri, CALL_LIMIT);
    }
    mount->name = copy_name(uri, length, "");
    if (!mount->name) {
      return shell_directory_error(shell, "shell: release", uri, CALL_NO_MEMORY);
    }
    status = handle_copy(control, &mount->control);
    if (status != CALL_OK) {
      forget_mount(mount);
      return shell_directory_error(shell, "shell: release", uri, status);
    }
    mount->domain = state.domain;
    mount->bound = true;
  }
  size_t capacity = startup_resource_count();
  size_t startup_depth = startup_working_directory_count();
  if (startup_depth > SIZE_MAX - capacity) {
    return shell_directory_error(shell, "shell: release", uri, CALL_LIMIT);
  }
  capacity += startup_depth;
  if (capacity > SIZE_MAX - SHELL_FAT_MOUNT_LIMIT - 1 ||
      capacity + SHELL_FAT_MOUNT_LIMIT + 1 > SIZE_MAX / sizeof(handle_t)) {
    return shell_directory_error(shell, "shell: release", uri, CALL_LIMIT);
  }
  capacity += SHELL_FAT_MOUNT_LIMIT + 1;
  handle_t *handles = malloc(capacity * sizeof(*handles));
  if (!handles) {
    return shell_directory_error(shell, "shell: release", uri, CALL_NO_MEMORY);
  }
  size_t data_count;
  status = collect_data(shell, state.domain, root, handles, &data_count);
  if (status == CALL_OK) {
    status = leave_mount(shell, state.domain);
  }
  if (status == CALL_OK) {
    status = unbind_data(shell, handles, data_count);
  }
  free(handles);
  if (status == CALL_OK) {
    status = mount_retire(mount->control);
  }
  if (status == CALL_OK) {
    forget_mount(mount);
    return COMMAND_OK;
  }
  return shell_error(shell, "shell: release %s failed (status %u); controller retained%s\n",
      uri, status, mount->bound ? "" : ", binding removed; retry release after references drain");
}
