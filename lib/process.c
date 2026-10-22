#include <process.h>
#include <syscall.h>

enum call_status process_wait(handle_t process, struct process_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct process_result){0};
  struct message_header message = {PROTOCOL_PROCESS, PROCESS_WAIT};
  struct process_result reply;
  struct syscall_result call = syscall_call(process, &message, sizeof(message),
      &reply, sizeof(reply));
  if (call.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (call.reply_size != (call.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (call.status != CALL_OK) {
    return call.status;
  }
  if (reply.kind == PROCESS_START_FAILED) {
    if (!process_start_failure_valid(reply.exit_status)) {
      return CALL_BAD_REQUEST;
    }
  } else if ((reply.kind != PROCESS_EXITED && reply.kind != PROCESS_FAULTED &&
      reply.kind != PROCESS_TERMINATED) ||
      (reply.kind != PROCESS_EXITED && reply.exit_status) ||
      reply.exit_status < INT32_MIN || reply.exit_status > INT32_MAX) {
    return CALL_BAD_REQUEST;
  }
  *result = reply;
  return CALL_OK;
}

enum call_status process_terminate(handle_t process)
{
  struct message_header message = {PROTOCOL_PROCESS, PROCESS_TERMINATE};
  struct syscall_result call = syscall_call(process, &message, sizeof(message), NULL, 0);
  if (call.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (call.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return call.status;
}

const char *process_start_step_name(uint32_t reason)
{
  switch (reason >> PROCESS_START_STEP_SHIFT) {
  case PROCESS_START_RECORD: return "startup record";
  case PROCESS_START_MEMORY: return "memory resource";
  case PROCESS_START_TLS: return "TLS allocation";
  case PROCESS_START_FS: return "TLS binding";
  case PROCESS_START_RESOURCES: return "runtime resource retention";
  case PROCESS_START_STDIO_SNAPSHOT: return "standard-stream snapshot";
  case PROCESS_START_STDIN: return "standard input";
  case PROCESS_START_STDOUT: return "standard output";
  case PROCESS_START_STDERR: return "standard error";
  case PROCESS_START_STDIO_CLOSE: return "standard-stream snapshot close";
  default: return "unknown startup step";
  }
}

const char *process_start_status_name(uint32_t reason)
{
  switch (reason & PROCESS_START_STATUS_MASK) {
  case PROCESS_START_UNKNOWN_CALL: return "unknown system call";
  case CALL_BAD_HANDLE: return "invalid handle";
  case CALL_DENIED: return "permission denied";
  case CALL_BAD_OPERATION: return "operation not supported";
  case CALL_BAD_REQUEST: return "invalid request or reply";
  case CALL_BAD_BUFFER: return "invalid buffer";
  case CALL_UNAVAILABLE: return "resource unavailable";
  case CALL_QUEUE_FULL: return "queue full";
  case CALL_ENDPOINT_CLOSED: return "endpoint closed";
  case CALL_BUSY: return "resource busy";
  case CALL_NO_MEMORY: return "out of memory";
  case CALL_LIMIT: return "resource limit exceeded";
  case CALL_NOT_FOUND: return "resource not found";
  case CALL_WRONG_TYPE: return "unexpected object type";
  case CALL_ALREADY_EXISTS: return "already exists";
  case CALL_READ_ONLY: return "read-only resource";
  case CALL_INPUT_LOST: return "input lost";
  case CALL_TIMED_OUT: return "timed out";
  case CALL_NOT_EMPTY: return "not empty";
  case CALL_IO: return "I/O error";
  case CALL_NO_ROUTE: return "no network route";
  case CALL_BUFFER_TOO_SMALL: return "buffer too small";
  case CALL_CONNECTION_REFUSED: return "connection refused";
  case CALL_CONNECTION_RESET: return "connection reset";
  case CALL_NO_SPACE: return "no space left";
  case CALL_QUOTA: return "quota exceeded";
  case CALL_FILE_TOO_LARGE: return "file too large";
  case CALL_OUTCOME_UNKNOWN: return "operation outcome unknown";
  case CALL_ABANDONED: return "operation abandoned";
  case CALL_WOULD_BLOCK: return "operation would block";
  case CALL_LINK_NOT_FOLLOWED: return "symbolic link not followed";
  case CALL_NAME_TOO_LONG: return "name too long";
  case CALL_PIPE_VALUE: return "pipe value pending";
  default: return "native call failed";
  }
}
