#ifndef USERSPACE_AUDIO_H
#define USERSPACE_AUDIO_H

#include <abi/audio.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* PLAYBACK authority. The exclusive session belongs to this process in the
 * grant's space; hiding that space does not stop playback. Replies clear on
 * failure. Closing or delegating a handle does not release or transfer it. */
enum call_status audio_acquire(handle_t audio, struct audio_acquire_reply *reply);
/* Copies complete S16LE stereo frames atomically, at most AUDIO_WRITE_MAX bytes.
 * Success validates the complete accepted byte count; QUEUE_FULL copies nothing.
 * WAIT_WRITABLE observes maximum-write room without
 * reserving it. Zero bytes still validate authority and session ownership. */
enum call_status audio_write(handle_t audio, const void *bytes, size_t size);
enum call_status audio_status(handle_t audio, struct audio_status_reply *reply);
/* Discards queued input; this is not an audible drain. Already mixed frames
 * may outlive release. Process exit also releases the session. */
enum call_status audio_release(handle_t audio);

#endif
