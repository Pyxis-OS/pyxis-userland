#ifndef USERSPACE_SESSION_SCREENSHOT_H
#define USERSPACE_SESSION_SCREENSHOT_H

/* Consume the first session's private registration before any successor
 * capture. A missing grant means this is not the initial opted-in session. */
void session_start_screenshot_saver(void);

#endif
