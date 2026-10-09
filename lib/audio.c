#include <audio.h>
#include <syscall.h>

static enum call_status audio_result(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static enum call_status audio_call(handle_t audio, uint64_t operation,
    void *reply, size_t size)
{
  struct message_header request = {PROTOCOL_AUDIO, operation};
  return audio_result(syscall_call(audio, &request, sizeof(request), reply, size), size);
}

enum call_status audio_acquire(handle_t audio, struct audio_acquire_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct audio_acquire_reply){0};
  struct audio_acquire_reply response;
  enum call_status status = audio_call(audio, AUDIO_ACQUIRE, &response, sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status audio_write(handle_t audio, const void *bytes, size_t size)
{
  if (size > AUDIO_WRITE_MAX) {
    return CALL_LIMIT;
  }
  if ((!bytes && size) || size % AUDIO_FRAME_BYTES) {
    return CALL_BAD_REQUEST;
  }
  struct audio_write_request request = {
    .header = {PROTOCOL_AUDIO, AUDIO_WRITE},
    .buffer = (uintptr_t)bytes,
    .length = size,
  };
  uint64_t accepted;
  enum call_status status = audio_result(syscall_call(audio, &request, sizeof(request),
      &accepted, sizeof(accepted)), sizeof(accepted));
  if (status == CALL_OK && accepted != size) {
    return CALL_BAD_REQUEST;
  }
  return status;
}

enum call_status audio_status(handle_t audio, struct audio_status_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct audio_status_reply){0};
  struct audio_status_reply response;
  enum call_status status = audio_call(audio, AUDIO_STATUS, &response, sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status audio_release(handle_t audio)
{
  return audio_call(audio, AUDIO_RELEASE, NULL, 0);
}
