#include "screenshot.h"
#include "../common/bootstrap_environment.h"
#include "../common/launch_environment.h"
#include <abi/clock.h>
#include <abi/memory.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <screenshot_saver.h>

void session_start_screenshot_saver(void)
{
  handle_t saver = bootstrap_take_capability("screenshot_saver");
  if (saver == HANDLE_INVALID) {
    return;
  }
  handle_t image = HANDLE_INVALID;
  handle_t bin = bootstrap_capability("bin");
  handle_t launcher = bootstrap_capability("launcher");
  handle_t home = bootstrap_capability("home");
  handle_t capture = bootstrap_capability("screen_capture");
  handle_t memory = bootstrap_capability("memory");
  handle_t clock = bootstrap_capability("clock");
  if (bin == HANDLE_INVALID || launcher == HANDLE_INVALID || home == HANDLE_INVALID ||
      capture == HANDLE_INVALID || memory == HANDLE_INVALID ||
      clock == HANDLE_INVALID) {
    goto done;
  }
  enum call_status status = directory_lookup(bin, "screenshot-saver.pxe",
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }
  const uint64_t home_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE |
      DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE;
  struct launch_grant grants[] = {
    {saver, SCREENSHOT_SAVER_RIGHT_SERVE, 0},
    {capture, SCREEN_CAPTURE_RIGHT_CAPTURE, 0},
    {home, home_rights, 0},
    {memory, MEMORY_RIGHT_MANAGE, 0},
    {clock, CLOCK_RIGHT_READ, 0},
  };
  struct launch_environment_entry entries[] = {
    PROFILE_PRIVATE_CAP("screenshot_saver", 0), PROFILE_CAP("screen_capture", 1),
    PROFILE_CAP("home", 2), PROFILE_CAP("memory", 3), PROFILE_CAP("clock", 4),
  };
  const char *arguments[] = {"bin://screenshot-saver.pxe"};
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants,
    .grant_count = sizeof(grants) / sizeof(*grants),
    .entries = (uintptr_t)entries, .entry_count = sizeof(entries) / sizeof(*entries),
    .argv = (uintptr_t)arguments, .argc = 1,
  };
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status == CALL_OK) {
    handle_close(child);
  }

done:
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  /* No accidental successor inheritance, including failure. Last-close lets
   * the system binding report unavailable without affecting ordinary startup. */
  handle_close(saver);
}
