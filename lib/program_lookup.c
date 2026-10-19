#include <program.h>
#include <file.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>

bool program_is_explicit(const char *command)
{
  return command && (strchr(command, '/') || strchr(command, ':'));
}

enum call_status program_search_path_validate(const char *path,
    const struct command_search_capture *search)
{
  if (!path) return CALL_OK;
  size_t count = search ? search->count : 0;
  if (count && !search->entries) return CALL_BAD_REQUEST;
  for (size_t i = 0; i < count; ++i) {
    size_t length = strlen(search->entries[i].label);
    if (strncmp(path, search->entries[i].label, length)) return CALL_BAD_OPERATION;
    path += length;
    if (i + 1 < count) {
      if (*path != ':') return CALL_BAD_OPERATION;
      ++path;
    }
  }
  return *path ? CALL_BAD_OPERATION : CALL_OK;
}

enum call_status program_open_search(const struct path_context *context,
    const struct command_search_capture *search, const char *command,
    struct path_workspace *workspace, struct command_selection **output)
{
  if (!output) return CALL_BAD_REQUEST;
  *output = NULL;
  if (!command || !*command || !workspace) return CALL_BAD_REQUEST;
  if (!program_is_explicit(command)) {
    return search && search->list ? command_search_resolve(context, search, command, output) :
        CALL_NOT_FOUND;
  }
  struct command_selection *selection = calloc(1, sizeof(*selection));
  if (!selection) return CALL_NO_MEMORY;
  selection->explicit_path = strdup(command);
  if (!selection->explicit_path) {
    command_selection_close(selection);
    return CALL_NO_MEMORY;
  }
  selection->ordinal = SIZE_MAX;
  strcpy(selection->label, "explicit");
  size_t end = strlen(command);
  while (end && command[end - 1] == '/') --end;
  enum call_status status;
  if (end >= 4 && !memcmp(command + end - 4, ".pxb", 4)) {
    status = bundle_open(context, command, &selection->bundle);
    if (status == CALL_OK) selection->image = bundle_program_image(selection->bundle);
  } else {
    status = path_resolve(context, command, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        workspace, &selection->image);
  }
  if (status == CALL_OK) *output = selection;
  else command_selection_close(selection);
  return status;
}

enum call_status program_open(const struct path_context *context, const char *command,
    struct path_workspace *workspace, struct command_selection **selection)
{
  if (!selection) return CALL_BAD_REQUEST;
  *selection = NULL;
  if (!command || !*command) return CALL_BAD_REQUEST;
  struct command_search_capture search = {0};
  enum call_status status = CALL_OK;
  if (!program_is_explicit(command)) {
    status = command_search_capture_init(startup_environment_handle(), &search);
    if (status == CALL_NOT_FOUND) status = CALL_OK;
  }
  if (status == CALL_OK) status = program_open_search(context, &search, command, workspace, selection);
  command_search_capture_close(&search);
  return status;
}
