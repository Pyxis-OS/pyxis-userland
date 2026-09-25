#include "../common/directory.h"
#include <abi/file.h>
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>

static enum call_status sync_path(const char *path)
{
  handle_t object = HANDLE_INVALID;
  enum call_status status = resolve_file(path, FILE_RIGHT_WRITE, &object);
  bool is_file = status == CALL_OK;
  if (status == CALL_WRONG_TYPE || status == CALL_DENIED) {
    /* A directory can deny WRITE_FILES before checking the leaf's type.
     * These fresh lookups may observe an external namespace change. */
    enum call_status file_status = status;
    status = resolve_directory(path, DIRECTORY_RIGHT_CREATE, &object);
    if (status == CALL_DENIED) {
      status = resolve_directory(path, DIRECTORY_RIGHT_REMOVE, &object);
    }
    /* A file can be denied WRITE while both directory probes reject its
     * type. Report the missing file authority in that case. */
    if (status == CALL_WRONG_TYPE && file_status == CALL_DENIED) {
      status = file_status;
    }
  }
  if (status != CALL_OK) {
    return status;
  }

  status = is_file ? file_sync(object) : directory_sync(object);
  if (handle_close(object) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  return status;
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    fputs("usage: sync path...\n", stderr);
    return EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    enum call_status status = sync_path(argv[i]);
    if (status != CALL_OK) {
      report_directory_error("sync", argv[i], status);
      result = EXIT_FAILURE;
    }
  }
  return result;
}
