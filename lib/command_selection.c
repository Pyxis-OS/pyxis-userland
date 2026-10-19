#include <command_selection.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <pxe/p1f.h>
#include <stdlib.h>
#include <string.h>

#define SEARCH_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)

struct captured_volume {
  struct package_command_info info;
  handle_t view;
};

static bool valid_name(const char *name)
{
  size_t length = name ? strnlen(name, PACKAGE_COMMAND_NAME_MAX + 1) : 0;
  if (!length || length > PACKAGE_COMMAND_NAME_MAX || !strcmp(name, ".") ||
      !strcmp(name, "..")) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = name[i];
    if (byte <= 0x20 || byte == 0x7f || byte == '/' || byte == ':' || byte == '\\') {
      return false;
    }
  }
  return true;
}

void command_selection_close(struct command_selection *selection)
{
  if (!selection) {
    return;
  }
  if (selection->bundle) {
    bundle_program_close(selection->bundle);
  } else if (selection->image) {
    handle_close(selection->image);
  }
  if (selection->package_view) {
    handle_close(selection->package_view);
  }
  free(selection);
}

static enum call_status validate_image(handle_t image)
{
  struct handle_info info;
  enum call_status status = handle_query(image, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != PROTOCOL_FILE || info.transport)) {
    status = CALL_WRONG_TYPE;
  }
  uint64_t magic = 0;
  size_t position = 0;
  while (status == CALL_OK && position < sizeof(magic)) {
    size_t count = 0;
    status = file_read(image, position, (char *)&magic + position,
        sizeof(magic) - position, &count);
    if (status == CALL_OK && (!count || count > sizeof(magic) - position)) {
      status = CALL_BAD_REQUEST;
    }
    if (status == CALL_OK) {
      position += count;
    }
  }
  return status == CALL_OK && magic != P1F_MAGIC ? CALL_BAD_REQUEST : status;
}

static enum call_status conventional(const struct path_context *context,
    handle_t directory, const char *name, struct command_selection *selection)
{
  char plain_name[PACKAGE_COMMAND_NAME_MAX + 5];
  char bundle_name[PACKAGE_COMMAND_NAME_MAX + 5];
  size_t length = strlen(name);
  memcpy(plain_name, name, length);
  memcpy(plain_name + length, ".pxe", 5);
  memcpy(bundle_name, name, length);
  memcpy(bundle_name + length, ".pxb", 5);
  struct directory_typed_lookup_reply plain = {0}, bundle = {0};
  enum call_status status = directory_lookup_typed(directory, plain_name,
      FILE_RIGHT_READ, SEARCH_DIRECTORY_RIGHTS, &plain);
  if (status == CALL_NAME_TOO_LONG) {
    status = CALL_NOT_FOUND;
  }
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    return status;
  }
  enum call_status other = directory_lookup_typed(directory, bundle_name,
      FILE_RIGHT_READ, SEARCH_DIRECTORY_RIGHTS, &bundle);
  if (other == CALL_NAME_TOO_LONG) {
    other = CALL_NOT_FOUND;
  }
  if (other != CALL_OK && other != CALL_NOT_FOUND) {
    status = other;
  } else if (plain.handle && bundle.handle) {
    status = CALL_BAD_REQUEST;
  } else if (plain.handle) {
    status = plain.protocol == PROTOCOL_FILE ? validate_image(plain.handle) : CALL_WRONG_TYPE;
    if (status == CALL_OK) {
      selection->image = plain.handle;
      plain.handle = HANDLE_INVALID;
      strcpy(selection->path, plain_name);
    }
  } else if (bundle.handle) {
    status = bundle.protocol == PROTOCOL_DIRECTORY ?
        bundle_open_directory(context, bundle.handle, NULL, &selection->bundle) : CALL_WRONG_TYPE;
    if (status == CALL_OK) {
      selection->image = bundle_program_image(selection->bundle);
      strcpy(selection->path, bundle_name);
    }
  } else {
    status = CALL_NOT_FOUND;
  }
  if (plain.handle) {
    handle_close(plain.handle);
  }
  if (bundle.handle) {
    handle_close(bundle.handle);
  }
  return status;
}

static bool same_volume(const struct package_command_info *left,
    const struct package_command_info *right)
{
  return !memcmp(left->pool_id, right->pool_id, 16) &&
      !memcmp(left->volume_id, right->volume_id, 16);
}

static enum call_status package_target(const struct path_context *context,
    handle_t view, const char *name, struct command_selection *selection)
{
  struct package_command_lookup_reply reply;
  enum call_status status = package_commands_lookup(view, name, FILE_RIGHT_READ,
      SEARCH_DIRECTORY_RIGHTS, &reply);
  if (status != CALL_OK) {
    return status;
  }
  if (reply.protocol == PROTOCOL_DIRECTORY) {
    status = bundle_open_package_directory(context, reply.handle,
        reply.target.command, view, &selection->bundle);
    handle_close(reply.handle);
    if (status == CALL_OK) {
      selection->image = bundle_program_image(selection->bundle);
    }
  } else {
    status = validate_image(reply.handle);
    if (status == CALL_OK) {
      selection->image = reply.handle;
    } else {
      handle_close(reply.handle);
    }
  }
  if (status == CALL_OK) {
    status = handle_copy(view, &selection->package_view);
  }
  if (status == CALL_OK) {
    selection->package = true;
    selection->origin = reply.info;
    selection->target = reply.target;
    strcpy(selection->path, reply.target.path);
  }
  return status;
}

enum call_status command_search_resolve(const struct path_context *context,
    const struct command_search_capture *search, const char *name,
    struct command_selection **output)
{
  if (!output) {
    return CALL_BAD_REQUEST;
  }
  *output = NULL;
  if (!search || !search->list || (search->count && !search->entries) || !valid_name(name)) {
    return CALL_BAD_REQUEST;
  }
  if (search->count > SIZE_MAX / sizeof(struct captured_volume)) {
    return CALL_LIMIT;
  }
  struct captured_volume *volumes = search->count ? calloc(search->count, sizeof(*volumes)) : NULL;
  struct command_selection *selection = calloc(1, sizeof(*selection));
  if (!selection || (search->count && !volumes)) {
    free(selection);
    free(volumes);
    return CALL_NO_MEMORY;
  }
  enum call_status status = CALL_NOT_FOUND;
  size_t volume_count = 0;
  for (size_t i = 0; i < search->count; ++i) {
    struct package_command_info info;
    status = package_commands_info(search->entries[i].handle, &info);
    if (status == CALL_BAD_OPERATION) {
      status = conventional(context, search->entries[i].handle, name, selection);
    } else if (status == CALL_OK) {
      handle_t previous = HANDLE_INVALID;
      for (size_t j = 0; j < volume_count; ++j) {
        if (same_volume(&info, &volumes[j].info)) {
          previous = volumes[j].view;
          break;
        }
      }
      handle_t fixed = HANDLE_INVALID;
      status = package_commands_capture(search->entries[i].handle, previous, &fixed);
      if (status == CALL_OK) {
        status = package_target(context, fixed, name, selection);
        if (!previous) {
          volumes[volume_count++] = (struct captured_volume){.info = info, .view = fixed};
        } else {
          handle_close(fixed);
        }
      }
    }
    if (status == CALL_NOT_FOUND) {
      continue;
    }
    if (status == CALL_OK) {
      selection->ordinal = i;
      memcpy(selection->label, search->entries[i].label, sizeof(selection->label));
      strcpy(selection->name, name);
    }
    break;
  }
  for (size_t i = 0; i < volume_count; ++i) {
    handle_close(volumes[i].view);
  }
  free(volumes);
  if (status == CALL_OK) {
    *output = selection;
  } else {
    command_selection_close(selection);
  }
  return status;
}
