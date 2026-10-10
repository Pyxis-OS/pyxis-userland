#include <pyxis/working_path.h>
#include "shell.h"
#include "../common/directory.h"
#include <abi/directory.h>
#include <abi/file.h>
#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_NAME ".history"
#define HISTORY_FILE_LINES 1000
#define HISTORY_FILE_BYTES 65536
/* Room for the whole file plus one new entry and its LF. */
#define HISTORY_BUFFER (HISTORY_FILE_BYTES + SHELL_LINE_CAPACITY)
#define HISTORY_SAVE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define HISTORY_TEMPORARY_ATTEMPTS 4

/* A saved entry is what the editor accepts: printable ASCII, below the line
 * capacity, without a leading space. */
static bool valid_entry(const char *line, size_t length)
{
  if (!length || length >= SHELL_LINE_CAPACITY || line[0] == ' ') {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (line[i] < ' ' || line[i] > '~') {
      return false;
    }
  }
  return true;
}

/* Reads the newest HISTORY_FILE_BYTES of the file into buffer and returns the
 * byte count, dropping a partial first line when older bytes were skipped. A
 * missing file is empty. */
static enum call_status read_history(handle_t home, char *buffer, size_t *size)
{
  *size = 0;
  handle_t file;
  enum call_status status = directory_lookup(home, HISTORY_NAME, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status == CALL_NOT_FOUND) {
    return CALL_OK;
  }
  if (status != CALL_OK) {
    return status;
  }
  uint64_t total;
  status = file_size(file, &total);
  uint64_t offset = total > HISTORY_FILE_BYTES ? total - HISTORY_FILE_BYTES : 0;
  size_t count = 0;
  while (status == CALL_OK && offset + count < total) {
    size_t read;
    status = file_read(file, offset + count, buffer + count, HISTORY_FILE_BYTES - count, &read);
    if (status == CALL_OK && !read) {
      break;
    }
    count += read;
  }
  if (handle_close(file) != 0 && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    return status;
  }
  size_t start = 0;
  if (offset) {
    char *newline = memchr(buffer, '\n', count);
    start = newline ? (size_t)(newline - buffer) + 1 : count;
  }
  memmove(buffer, buffer + start, count - start);
  *size = count - start;
  return CALL_OK;
}

/* Keeps only valid, LF-terminated entries, compacting them to the front of
 * buffer in order. Returns the new size and sets *lines. */
static size_t keep_entries(char *buffer, size_t size, size_t *lines)
{
  size_t kept = 0;
  *lines = 0;
  for (size_t start = 0; start < size;) {
    char *newline = memchr(buffer + start, '\n', size - start);
    if (!newline) {
      break;
    }
    size_t length = (size_t)(newline - (buffer + start));
    if (valid_entry(buffer + start, length)) {
      memmove(buffer + kept, buffer + start, length + 1);
      kept += length + 1;
      ++*lines;
    }
    start += length + 1;
  }
  return kept;
}

/* Drops the oldest whole entries until both file bounds hold. */
static size_t trim_entries(char *buffer, size_t size, size_t lines)
{
  size_t start = 0;
  while (lines > HISTORY_FILE_LINES || size - start > HISTORY_FILE_BYTES) {
    char *newline = memchr(buffer + start, '\n', size - start);
    start = (size_t)(newline - buffer) + 1;
    --lines;
  }
  memmove(buffer, buffer + start, size - start);
  return size - start;
}

static handle_t find_home(const struct shell *shell)
{
  for (size_t i = 0; i < shell->directory->root_count; ++i) {
    if (!strcmp(shell->roots[i].name, "home")) {
      return shell->roots[i].handle;
    }
  }
  return HANDLE_INVALID;
}

void shell_history_load(struct shell *shell)
{
  handle_t home = find_home(shell);
  if (home == HANDLE_INVALID) {
    return;
  }
  uint64_t rights;
  shell->history_home = home;
  shell->history_saving = handle_rights(home, &rights, NULL) == CALL_OK &&
      (rights & HISTORY_SAVE_RIGHTS) == HISTORY_SAVE_RIGHTS;
  char *buffer = malloc(HISTORY_BUFFER);
  size_t size, lines;
  if (!buffer || read_history(home, buffer, &size) != CALL_OK) {
    free(buffer);
    return;
  }
  size = keep_entries(buffer, size, &lines);
  size_t skip = lines > TERM_HISTORY_ENTRIES ? lines - TERM_HISTORY_ENTRIES : 0;
  for (size_t start = 0; start < size;) {
    char *newline = memchr(buffer + start, '\n', size - start);
    *newline = '\0';
    if (skip) {
      --skip;
    } else {
      term_history_add(&shell->history, buffer + start);
    }
    start = (size_t)(newline - buffer) + 1;
  }
  free(buffer);
}

/* Creates an exclusive temporary name beside the history. */
static enum call_status create_temporary(struct shell *shell, char *name, size_t capacity,
    handle_t *file)
{
  uint64_t stamp = 0;
  if (shell->clock == HANDLE_INVALID || clock_now(shell->clock, &stamp) != CALL_OK) {
    stamp = (uint64_t)(uintptr_t)shell;
  }
  enum call_status status = CALL_ALREADY_EXISTS;
  for (unsigned attempt = 0; attempt < HISTORY_TEMPORARY_ATTEMPTS &&
      status == CALL_ALREADY_EXISTS; ++attempt) {
    snprintf(name, capacity, HISTORY_NAME ".%016llx", (unsigned long long)(stamp + attempt));
    status = directory_create(shell->history_home, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_WRITE, file);
  }
  return status;
}

static enum call_status write_all(handle_t file, const char *bytes, size_t size)
{
  for (size_t offset = 0; offset < size;) {
    size_t written;
    enum call_status status = file_write(file, offset, bytes + offset, size - offset, &written);
    if (status != CALL_OK) {
      return status;
    }
    offset += written;
  }
  return CALL_OK;
}

/* Merge and replace: the newest file contents plus this line go to a new
 * temporary file, renamed over the history. The rename is the commit point;
 * native volumes flush the moved file before replacing, so no sync is needed.
 * Concurrent savers can lose each other's line: the later rename wins. */
static enum call_status save_line(struct shell *shell, const char *line, char *buffer)
{
  size_t size, lines;
  enum call_status status = read_history(shell->history_home, buffer, &size);
  if (status != CALL_OK) {
    return status;
  }
  size = keep_entries(buffer, size, &lines);
  size_t length = strlen(line);
  memcpy(buffer + size, line, length);
  buffer[size + length] = '\n';
  size = trim_entries(buffer, size + length + 1, lines + 1);

  char name[sizeof(HISTORY_NAME) + 18];
  handle_t file;
  status = create_temporary(shell, name, sizeof(name), &file);
  if (status != CALL_OK) {
    return status;
  }
  status = write_all(file, buffer, size);
  if (handle_close(file) != 0 && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status == CALL_OK) {
    status = directory_rename(shell->history_home, name, shell->history_home, HISTORY_NAME,
        DIRECTORY_RENAME_REPLACE);
  }
  if (status != CALL_OK) {
    directory_remove(shell->history_home, name, DIRECTORY_KIND_FILE);
  }
  return status;
}

void shell_history_save(struct shell *shell, const char *line)
{
  if (!shell->history_saving || !valid_entry(line, strlen(line))) {
    return;
  }
  char *buffer = malloc(HISTORY_BUFFER);
  enum call_status status = buffer ? save_line(shell, line, buffer) : CALL_NO_MEMORY;
  free(buffer);
  if (status != CALL_OK) {
    /* One report per shell; later lines stay in memory only. */
    shell->history_saving = false;
    report_directory_error("shell: history not saved", "home://" HISTORY_NAME, status);
  }
}
