#include <program_interpreters.h>
#include <directory.h>
#include <environment.h>
#include <file.h>
#include <handle.h>
#include <string.h>

static const char *const record_names[] = {
  "interpreter.shell", "interpreter.lua", "interpreter.lua51",
};

static enum call_status image_identity(handle_t image, struct file_info_reply *identity)
{
  struct handle_info info;
  enum call_status status = handle_query(image, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE || info.transport) {
    return CALL_WRONG_TYPE;
  }
  if (!(info.rights & FILE_RIGHT_READ)) {
    return CALL_DENIED;
  }
  status = file_info(image, identity);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t required = FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID;
  return (identity->valid & required) == required ? CALL_OK : CALL_UNAVAILABLE;
}

static enum call_status retain_image(handle_t source, struct program_interpreter_record *record)
{
  struct file_info_reply info;
  enum call_status status = image_identity(source, &info);
  if (status != CALL_OK) {
    return status;
  }
  handle_t held;
  status = handle_copy_restricted(source, FILE_RIGHT_READ, 0, &held);
  if (status == CALL_OK) {
    if (record->image != HANDLE_INVALID) {
      handle_close(record->image);
    }
    *record = (struct program_interpreter_record){held, info.domain, info.object};
  }
  return status;
}

/* The paths below are fixed official publication layouts, not environment URI
 * schemes. Borrow the root and own only one traversal component at a time. */
static enum call_status select_image(handle_t root, const char *const *parts,
    size_t count, struct program_interpreter_record *record)
{
  if (root == HANDLE_INVALID) {
    return CALL_NOT_FOUND;
  }
  struct handle_info info;
  enum call_status status = handle_query(root, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_DIRECTORY || info.transport) {
    return CALL_WRONG_TYPE;
  }
  handle_t current = root;
  for (size_t i = 0; i < count; ++i) {
    bool final = i + 1 == count;
    handle_t next;
    status = directory_lookup(current, parts[i],
        final ? DIRECTORY_KIND_FILE : DIRECTORY_KIND_DIRECTORY,
        final ? FILE_RIGHT_READ : DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &next);
    if (current != root) {
      handle_close(current);
    }
    if (status != CALL_OK) {
      return status;
    }
    current = next;
  }
  status = retain_image(current, record);
  handle_close(current);
  return status;
}

void program_interpreters_close(struct program_interpreters *interpreters)
{
  if (!interpreters) {
    return;
  }
  for (size_t i = 0; i < 3; ++i) {
    if (interpreters->records[i].image != HANDLE_INVALID) {
      handle_close(interpreters->records[i].image);
    }
  }
  *interpreters = (struct program_interpreters){0};
}

enum call_status program_interpreters_capture(handle_t environment, handle_t archive_root,
    handle_t package_root, enum program_interpreter_kind self_kind, handle_t self_image,
    struct program_interpreters *interpreters)
{
  if (!interpreters) {
    return CALL_BAD_REQUEST;
  }
  *interpreters = (struct program_interpreters){0};
  if ((unsigned)self_kind > PROGRAM_INTERPRETER_LUA51 ||
      ((self_kind == PROGRAM_INTERPRETER_NONE) != (self_image == HANDLE_INVALID))) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = CALL_OK;
  for (size_t i = 0; environment != HANDLE_INVALID && i < 3; ++i) {
    struct environment_entry_reply entry;
    status = environment_lookup(environment, record_names[i], &entry, NULL, 0);
    if (status == CALL_NOT_FOUND) {
      continue;
    }
    if (status == CALL_BUFFER_TOO_SMALL) {
      status = CALL_WRONG_TYPE;
    }
    if (status != CALL_OK) {
      goto fail;
    }
    if (entry.kind != ENVIRONMENT_CAP || (entry.flags & ENVIRONMENT_EXPORTED)) {
      if (entry.kind == ENVIRONMENT_CAP) {
        handle_close(entry.handle);
      }
      status = CALL_WRONG_TYPE;
      goto fail;
    }
    status = retain_image(entry.handle, &interpreters->records[i]);
    handle_close(entry.handle);
    if (status != CALL_OK) {
      goto fail;
    }
  }
  if (self_image != HANDLE_INVALID) {
    status = retain_image(self_image, &interpreters->records[self_kind - 1]);
    if (status != CALL_OK) {
      goto fail;
    }
  }
  static const char *const shell_archive[] = {"init-shell.pxe"};
  static const char *const lua_package[] = {"lua", "lua.pxb", "app", "bin", "lua.pxe"};
  static const char *const lua51_package[] = {"lua5.1", "lua5.1.pxb", "app", "bin", "lua5.1.pxe"};
  const char *const *const candidates[] = {NULL, lua_package, lua51_package};
  for (size_t i = 0; i < 3; ++i) {
    if (interpreters->records[i].image != HANDLE_INVALID) {
      continue;
    }
    if (!i) {
      status = select_image(archive_root, shell_archive, 1, &interpreters->records[i]);
    } else {
      status = select_image(package_root, candidates[i], 5, &interpreters->records[i]);
      if (status == CALL_NOT_FOUND) {
        status = select_image(archive_root, candidates[i] + 1, 4, &interpreters->records[i]);
      }
    }
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      goto fail;
    }
  }
  return CALL_OK;
fail:
  program_interpreters_close(interpreters);
  return status;
}

enum program_interpreter_kind program_interpreters_match(
    const struct program_interpreters *interpreters, handle_t candidate)
{
  if (!interpreters || candidate == HANDLE_INVALID) {
    return PROGRAM_INTERPRETER_NONE;
  }
  struct file_info_reply info;
  if (image_identity(candidate, &info) != CALL_OK) {
    return PROGRAM_INTERPRETER_NONE;
  }
  for (size_t i = 0; i < 3; ++i) {
    const struct program_interpreter_record *record = &interpreters->records[i];
    if (record->image != HANDLE_INVALID && info.domain == record->domain && info.object == record->object) {
      return (enum program_interpreter_kind)(i + 1);
    }
  }
  return PROGRAM_INTERPRETER_NONE;
}

enum call_status program_interpreters_stage(const struct program_interpreters *interpreters,
    struct launch_request *request, struct launch_grant *grants, size_t grant_capacity,
    struct launch_environment_entry *entries, size_t entry_capacity)
{
  if (!interpreters || !request || (grant_capacity && !grants) || (entry_capacity && !entries) ||
      request->grant_count > grant_capacity || request->entry_count > entry_capacity ||
      request->grants != (uintptr_t)grants || request->entries != (uintptr_t)entries) {
    return CALL_BAD_REQUEST;
  }
  size_t grant_count = request->grant_count, entry_count = request->entry_count;
  for (size_t i = 0; i < 3; ++i) {
    if (interpreters->records[i].image == HANDLE_INVALID) {
      continue;
    }
    for (size_t j = 0; j < entry_count; ++j) {
      if (!strcmp(record_names[i], (const char *)(uintptr_t)entries[j].name)) {
        return CALL_ALREADY_EXISTS;
      }
    }
    if (grant_count == grant_capacity || entry_count == entry_capacity) {
      return CALL_LIMIT;
    }
    entries[entry_count++] = (struct launch_environment_entry){
      .name = (uintptr_t)record_names[i], .kind = ENVIRONMENT_CAP, .value = grant_count,
    };
    grants[grant_count++] = (struct launch_grant){interpreters->records[i].image, FILE_RIGHT_READ, 0};
  }
  request->grant_count = grant_count;
  request->entry_count = entry_count;
  return CALL_OK;
}
