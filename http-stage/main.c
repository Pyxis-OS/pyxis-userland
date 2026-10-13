#include <abi/pipe.h>
#include <abi/file.h>
#include <file.h>
#include <pipe.h>
#include <startup.h>
#include <stdio.h>

int main(void)
{
  struct startup_stream input = startup_stream(STARTUP_STDIN);
  struct startup_stream output = startup_stream(STARTUP_STDOUT);
  if (input.protocol != PROTOCOL_PIPE || output.protocol != PROTOCOL_FILE) {
    fputs("http-stage: requires declared pipe input and staging writer\n", stderr);
    return 1;
  }
  unsigned char bytes[4096];
  uint64_t offset = 0;
  enum call_status status = CALL_OK;
  for (;;) {
    size_t count;
    status = pipe_read(input.handle, bytes, sizeof(bytes), &count);
    if (status != CALL_OK || !count) {
      break;
    }
    for (size_t done = 0; done < count;) {
      size_t written;
      status = file_write(output.handle, offset, bytes + done, count - done, &written);
      if (status != CALL_OK) {
        break;
      }
      offset += written;
      done += written;
    }
    if (status != CALL_OK) {
      break;
    }
  }
  if (fflush(stdout) || ferror(stdout) || fclose(stdout)) {
    if (status == CALL_OK) {
      status = CALL_IO;
    }
  }
  if (status != CALL_OK) {
    fprintf(stderr, "http-stage: staging failed (status %u)\n", status);
    if (status == CALL_LIMIT) {
      fputs("http-stage: staging limit reached; configure private NPFS backing for larger uploads\n",
          stderr);
    }
  }
  return status == CALL_OK ? 0 : 1;
}
