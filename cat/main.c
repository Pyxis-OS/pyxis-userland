#include <abi/message.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pyxis/resource.h>
#include <pyxis/stdio.h>

/* Bound transfer storage independently of file size. */
static unsigned char buffer[4096];

static int report_failure(const char *path, int error,
    const struct resource_description *description)
{
  if (fprintf(stderr, "cat: %s: %s", path, strerror(error)) < 0) {
    return -1;
  }
  if (description && description->detail_size) {
    if (fputs(": ", stderr) == EOF) {
      return -1;
    }
    for (size_t i = 0; i < description->detail_size; ++i) {
      unsigned char byte = description->detail[i];
      if (fputc(byte >= ' ' && byte <= '~' ? byte : '?', stderr) == EOF) {
        return -1;
      }
    }
  }
  return fputc('\n', stderr);
}

static int report_error(const char *path, int error)
{
  return report_failure(path, error, NULL);
}

/* A closed reader is a broken pipe; other final handoff failures are I/O. */
static int handoff_error(enum call_status status)
{
  return status == CALL_ENDPOINT_CLOSED ? EPIPE : EIO;
}

/* A capture holds a failed value for its consumer to report. A pipe reader
 * only sees the outcome, so the source is named here too, as it is when the
 * failed value could not be delivered. */
static bool stdout_is_pipe(void)
{
  struct pyxis_stdio_binding binding;
  return pyxis_stdio_stream(stdout, &binding) == 0 && binding.protocol == PROTOCOL_PIPE;
}

int main(int argc, char **argv)
{
  int result = EXIT_SUCCESS;
  int operands = argc > 1 ? argc - 1 : 1;
  for (int i = 0; i < operands; ++i) {
    const char *path = argc > 1 ? argv[i + 1] : "-";
    bool input = !strcmp(path, "-");
    const char *name = input ? "stdin" : path;
    struct pyxis_open_result opened = {0};
    FILE *file = input ? stdin : pyxis_fopen(path, "rb", &opened);
    if (!file) {
      int open_error = errno;
      struct resource_description description = opened.resource.info.description;
      if (operands == 1 && opened.available && opened.resource.resource &&
          opened.resource.info.description.outcome != RESOURCE_OK) {
        struct output_handoff_request owned = {opened.resource.resource, HANDLE_INVALID};
        enum call_status status = pyxis_stdio_handoff_owned(stdout, &owned);
        opened.resource.resource = owned.value;
        if (status == CALL_OK && !stdout_is_pipe()) {
          pyxis_open_result_close(&opened);
          return EXIT_FAILURE;
        }
        if (status != CALL_BAD_OPERATION) {
          report_failure(name, open_error, &description);
          pyxis_open_result_close(&opened);
          if (status != CALL_OK) {
            report_error("stdout", handoff_error(status));
          }
          return EXIT_FAILURE;
        }
      }
      int reported = report_failure(name, open_error, &description);
      pyxis_open_result_close(&opened);
      if (reported < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
      continue;
    }

    if (operands == 1) {
      enum call_status status = pyxis_stdio_handoff(stdout, file);
      if (status != CALL_BAD_OPERATION) {
        if (!input && fclose(file) != 0) {
          report_error(name, errno);
          return EXIT_FAILURE;
        }
        if (status != CALL_OK) {
          report_error("stdout", handoff_error(status));
          return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
      }
    }

    for (;;) {
      size_t count = fread_some(buffer, sizeof(buffer), file);
      /* A read can return bytes and an error together. Preserve the error
       * across output, but still copy the bytes that were read. */
      int read_error = ferror(file) ? errno : 0;
      if (count != 0 && fwrite(buffer, 1, count, stdout) != count) {
        report_error("stdout", errno);
        if (!input) {
          fclose(file);
        }
        return EXIT_FAILURE;
      }
      if (read_error != 0) {
        struct describe_reply description = {0};
        pyxis_describe(file, &description);
        if (report_failure(name, read_error, &description.description) < 0) {
          if (!input) {
            fclose(file);
          }
          return EXIT_FAILURE;
        }
        result = EXIT_FAILURE;
        break;
      }
      if (count == 0) {
        break;
      }
    }

    if (!input && fclose(file) != 0) {
      if (report_error(name, errno) < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
    }
  }
  return result;
}
