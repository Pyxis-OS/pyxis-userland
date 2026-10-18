#include "../common/bootstrap_environment.h"
#include <abi/pipe.h>
#include <abi/file.h>
#include <file.h>
#include <pipe.h>
#include <startup.h>
#include <stdio.h>

int main(void)
{
  struct pyxis_stdio_binding input = bootstrap_stdio(ENVIRONMENT_STDIN);
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  if (input.protocol != PROTOCOL_PIPE || output.protocol != PROTOCOL_FILE) {
    fputs("http-stage: requires declared pipe input and FILE writer\n", stderr);
    return 1;
  }
  unsigned char bytes[PIPE_READ_MAX_BYTES];
  uint64_t offset = 0;
  enum call_status status = CALL_OK;
  for (;;) {
    size_t count;
    status = pipe_read(input.handle, bytes, FILE_WRITE_MAX_BYTES, &count);
    if (status != CALL_OK || !count) {
      break;
    }
    for (size_t done = 0; done < count;) {
      size_t written;
      if (output.append) {
        status = file_size(output.handle, &offset);
        if (status != CALL_OK) {
          break;
        }
      }
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
  bool failed = fflush(stdout) != 0 || ferror(stdout);
  failed |= fclose(stdout) != 0;
  failed |= fclose(stdin) != 0;
  if (failed && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "http-stage: byte relay failed (status %u)\n", status);
    if (status == CALL_LIMIT) {
      fputs("http-stage: output limit reached; for RAM-staged uploads configure private NPFS backing\n",
          stderr);
    }
  }
  return status == CALL_OK ? 0 : 1;
}
