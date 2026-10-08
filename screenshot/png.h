#ifndef USERSPACE_SCREENSHOT_PNG_H
#define USERSPACE_SCREENSHOT_PNG_H

#include <screen_capture.h>
#include <stdbool.h>

/* Borrow both FILE handles. Success means the complete PNG was written;
 * closing, publication and durability belong to the caller. */
bool screenshot_encode(const struct screen_capture_reply *snapshot, handle_t output,
    const char *destination);

#endif
