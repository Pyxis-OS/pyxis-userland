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
enum call_status screenshot_saver_complete(handle_t saver, uint64_t request,
    enum call_status status, const char *filename);

#endif
