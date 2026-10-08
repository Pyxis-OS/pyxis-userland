#ifndef USERSPACE_SCREEN_CAPTURE_H
#define USERSPACE_SCREEN_CAPTURE_H

#include <abi/screen_capture.h>
#include <abi/syscall.h>

/* Borrow CAPTURE authority, independently of per-space DRAW. Output is cleared
 * on failure. Success returns a new owned READ-only FILE: close it when done;
 * copied handles retain the same bytes until their last reference closes.
 * The bytes freeze one presenter composition, which may contain tearing from
 * concurrent drawing. See abi/screen_capture.h for pixel layout and semantics. */
enum call_status screen_capture_frame(handle_t capture, struct screen_capture_reply *reply);

#endif
