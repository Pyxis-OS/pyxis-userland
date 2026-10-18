#include "../common/bootstrap_environment.h"
#include "encode.h"
#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <inttypes.h>
#include <screenshot_saver.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wait.h>

#define SAVE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define TEMPORARY_ATTEMPTS 64
#define DESTINATION_ATTEMPTS 1000

static bool close_owned(handle_t *handle)
{
  handle_t owned = *handle;
  *handle = HANDLE_INVALID;
  return owned == HANDLE_INVALID || handle_close(owned) == 0;
}

static enum call_status screenshots_directory(handle_t home, handle_t *directory)
{
  enum call_status status = directory_lookup(home, "screenshots", DIRECTORY_KIND_DIRECTORY,
      SAVE_RIGHTS, directory);
  if (status != CALL_NOT_FOUND) {
    return status;
  }
  status = directory_create(home, "screenshots", DIRECTORY_KIND_DIRECTORY,
      SAVE_RIGHTS, directory);
  if (status == CALL_ALREADY_EXISTS) {
    status = directory_lookup(home, "screenshots", DIRECTORY_KIND_DIRECTORY,
        SAVE_RIGHTS, directory);
  }
  return status;
}

static uint64_t timestamp(handle_t clock, char *text, size_t capacity)
{
  struct clock_wall_reading wall;
  enum call_status status = clock_wall_now(clock, &wall);
  if (status != CALL_OK) {
    return status == CALL_UNAVAILABLE ? SCREENSHOT_SAVER_STATUS_NO_CLOCK : status;
  }
  time_t seconds = wall.seconds;
  struct tm calendar;
  if (!gmtime_r(&seconds, &calendar) || calendar.tm_year < -1900 ||
      calendar.tm_year > 8099 ||
      !strftime(text, capacity, "%Y-%m-%dT%H-%M-%S", &calendar)) {
    return CALL_LIMIT;
  }
  return CALL_OK;
}

static uint64_t save(handle_t saver, handle_t capture, handle_t home,
    handle_t clock, const struct screenshot_saver_pending *pending,
    char *filename, size_t capacity)
{
  struct screen_capture_reply snapshot = {0};
  handle_t directory = HANDLE_INVALID, output = HANDLE_INVALID;
  bool reserved = false;
  char temporary[64], time_text[32];
  uint64_t status = screenshot_saver_capture(saver, pending->request,
      capture, &snapshot);
  if (status != CALL_OK) {
    return status;
  }
  status = timestamp(clock, time_text, sizeof(time_text));
  if (status != CALL_OK) {
    goto done;
  }
  status = screenshots_directory(home, &directory);
  if (status != CALL_OK) {
    goto done;
  }
  for (unsigned attempt = 0; attempt < TEMPORARY_ATTEMPTS; ++attempt) {
    snprintf(temporary, sizeof(temporary), ".screenshot-%016" PRIx64 "-%u",
        pending->request, attempt);
    status = directory_create(directory, temporary, DIRECTORY_KIND_FILE,
        FILE_RIGHT_WRITE, &output);
    if (status != CALL_ALREADY_EXISTS) {
      break;
    }
  }
  if (status != CALL_OK) {
    goto done;
  }
  reserved = true;
  bool encoded = screenshot_encode(&snapshot, output, "Print Screen");
  bool snapshot_closed = close_owned(&snapshot.file);
  bool output_closed = close_owned(&output);
  if (!encoded || !snapshot_closed || !output_closed) {
    status = CALL_IO;
    goto done;
  }
  for (uint64_t suffix = 1; suffix <= DESTINATION_ATTEMPTS; ++suffix) {
    int length = suffix == 1 ?
        snprintf(filename, capacity, "%s-%s.png", pending->space, time_text) :
        snprintf(filename, capacity, "%s-%s-%" PRIu64 ".png", pending->space,
            time_text, suffix);
    if (length < 0 || (size_t)length >= capacity) {
      status = CALL_LIMIT;
      goto done;
    }
    status = screenshot_saver_validate(saver, pending->request);
    if (status != CALL_OK) {
      goto done;
    }
    status = directory_rename(directory, temporary, directory, filename,
        DIRECTORY_RENAME_NO_REPLACE, DIRECTORY_KIND_FILE);
    if (status != CALL_ALREADY_EXISTS) {
      /* Rename has no reply buffer: BAD_BUFFER is a definite refusal too. */
      if (status == CALL_OK || status == CALL_OUTCOME_UNKNOWN) {
        reserved = false;
      }
      goto done;
    }
  }
  status = CALL_LIMIT;

done:
  bool snapshot_closed_final = close_owned(&snapshot.file);
  bool output_closed_final = close_owned(&output);
  if (reserved) {
    enum call_status removed = directory_remove(directory, temporary, DIRECTORY_KIND_FILE);
    if (removed != CALL_OK && removed != CALL_NOT_FOUND) {
      status = CALL_IO;
    }
  }
  bool directory_closed = close_owned(&directory);
  if (!snapshot_closed_final || !output_closed_final || !directory_closed) {
    status = CALL_IO;
  }
  return status;
}

int main(void)
{
  handle_t saver = bootstrap_take_capability("screenshot_saver");
  handle_t capture = bootstrap_capability("screen_capture");
  handle_t home = bootstrap_capability("home");
  handle_t clock = bootstrap_capability("clock");
  if (saver == HANDLE_INVALID || capture == HANDLE_INVALID ||
      home == HANDLE_INVALID || clock == HANDLE_INVALID) {
    return EXIT_FAILURE;
  }
  const struct wait_interest interest = {.handle = saver, .events = WAIT_READABLE};
  for (;;) {
    struct screenshot_saver_pending pending;
    enum call_status status = screenshot_saver_next(saver, &pending);
    if (status == CALL_WOULD_BLOCK) {
      uint64_t now, events;
      if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
        break;
      }
      status = wait_many(&interest, 1, now + WAIT_MAX_WAIT_NS, &events);
      if (status == CALL_TIMED_OUT) {
        continue;
      }
      if (status != CALL_OK || (events & (WAIT_ERROR | WAIT_CLOSED))) {
        break;
      }
      continue;
    }
    if (status != CALL_OK || !pending.request ||
        !memchr(pending.space, '\0', sizeof(pending.space))) {
      break;
    }
    char filename[SCREENSHOT_SAVER_FILENAME_MAX + 1];
    uint64_t completion = save(saver, capture, home, clock, &pending, filename, sizeof(filename));
    status = screenshot_saver_complete(saver, pending.request, completion,
        completion == CALL_OK ? filename : NULL);
    /* A timed-out request can finish I/O late. NEXT re-establishes readiness;
     * never replay that save or let its stale completion replace feedback. */
    if (status != CALL_OK && status != CALL_DENIED) {
      break;
    }
  }
  close_owned(&saver);
  return EXIT_FAILURE;
}
