#include <directory.h>
#include <file.h>
#include <shell_settings.h>

/* A FILE READ lookup is bounded by the parent's READ_FILES right. */
#define SETTINGS_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)

enum call_status shell_settings_open(handle_t home, handle_t *file)
{
  *file = HANDLE_INVALID;
  if (home == HANDLE_INVALID) {
    return CALL_NOT_FOUND;
  }
  handle_t config = HANDLE_INVALID, shell = HANDLE_INVALID;
  enum call_status status = directory_lookup(home, ".config", DIRECTORY_KIND_DIRECTORY,
      SETTINGS_DIRECTORY_RIGHTS, &config);
  if (status == CALL_OK) {
    status = directory_lookup(config, "shell", DIRECTORY_KIND_DIRECTORY,
        SETTINGS_DIRECTORY_RIGHTS, &shell);
  }
  if (status == CALL_OK) {
    status = directory_lookup(shell, "startup.lua", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, file);
  }
  if (shell != HANDLE_INVALID) {
    handle_close(shell);
  }
  if (config != HANDLE_INVALID) {
    handle_close(config);
  }
  return status;
}
