#include "screenshot.h"
#include "../common/bootstrap_environment.h"
#include "../common/launch_environment.h"
#include <abi/clock.h>
#include <abi/memory.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <program.h>
#include <screenshot_saver.h>
#include <pyxis/resource.h>

static void startup_failure(const char *operation, enum call_status status)
{
  fprintf(stderr, "session: screenshot saver: %s: %s (status %u)\n", operation,
      strerror(pyxis_resource_errno(status)), (unsigned)status);
}

void session_start_screenshot_saver(void)
{
  handle_t saver = bootstrap_take_capability("screenshot_saver");
  if (saver == HANDLE_INVALID) {
    return;
  }
  handle_t image = HANDLE_INVALID;
  handle_t launcher = bootstrap_capability("launcher");
  handle_t home = bootstrap_capability("home");
  handle_t capture = bootstrap_capability("screen_capture");
  handle_t memory = bootstrap_capability("memory");
  handle_t clock = bootstrap_capability("clock");
  const struct {
    const char *name;
    handle_t handle;
  } required[] = {
    {"missing launcher", launcher}, {"missing home", home},
    {"missing screen_capture", capture}, {"missing memory", memory}, {"missing clock", clock},
  };
  for (size_t i = 0; i < sizeof(required) / sizeof(*required); ++i) {
    if (required[i].handle == HANDLE_INVALID) {
      startup_failure(required[i].name, CALL_UNAVAILABLE);
      goto done;
    }
  }
  enum call_status status = program_image("screenshot-saver", &image);
  if (status != CALL_OK) {
    startup_failure("open the screenshot-saver command", status);
    goto done;
  }
  const uint64_t home_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE |
      DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE;
  struct launch_grant grants[] = {
    {saver, SCREENSHOT_SAVER_RIGHT_SERVE, 0},
    {capture, SCREEN_CAPTURE_RIGHT_CAPTURE, 0},
    {home, home_rights, 0},
    {memory, MEMORY_RIGHT_MANAGE, 0},
    {clock, CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE, 0},
  };
  struct launch_environment_entry entries[] = {
    PROFILE_PRIVATE_CAP("screenshot_saver", 0), PROFILE_CAP("screen_capture", 1),
    PROFILE_CAP("home", 2), PROFILE_CAP("memory", 3), PROFILE_CAP("clock", 4),
  };
  const char *arguments[] = {"screenshot-saver"};
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants,
    .grant_count = sizeof(grants) / sizeof(*grants),
    .entries = (uintptr_t)entries, .entry_count = sizeof(entries) / sizeof(*entries),
    .argv = (uintptr_t)arguments, .argc = 1,
  };
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    startup_failure("launch screenshot-saver", status);
  } else if (handle_close(child) != 0) {
    startup_failure("close launch observer", CALL_BAD_HANDLE);
  }

done:
  if (image != HANDLE_INVALID && handle_close(image) != 0) {
    startup_failure("close image", CALL_BAD_HANDLE);
  }
  /* No accidental successor inheritance, including failure. Last-close lets
   * the system binding report unavailable without affecting ordinary startup. */
  if (handle_close(saver) != 0) {
    startup_failure("close registration", CALL_BAD_HANDLE);
  }
}
