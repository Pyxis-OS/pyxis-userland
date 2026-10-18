#ifndef USERSPACE_SCREENSHOT_SAVER_H
#define USERSPACE_SCREENSHOT_SAVER_H

#include <abi/screenshot_saver.h>
#include <abi/screen_capture.h>
#include <abi/syscall.h>

/* Private session-to-saver capability. NEXT never waits; use WAIT_READABLE
 * after WOULD_BLOCK. A successful CAPTURE owns reply->file. */
enum call_status screenshot_saver_next(handle_t saver,
    struct screenshot_saver_pending *pending);
enum call_status screenshot_saver_capture(handle_t saver, uint64_t request,
    handle_t capture, struct screen_capture_reply *reply);
/* Refuse a stale/expired request before publication; no filesystem mutation
 * is reserved by validation. */
enum call_status screenshot_saver_validate(handle_t saver, uint64_t request);
/* status is a call_status or the private NO_CLOCK completion reason. */
enum call_status screenshot_saver_complete(handle_t saver, uint64_t request,
    uint64_t status, const char *filename);

#endif
