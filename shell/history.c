#include <pyxis/working_path.h>
#include "shell.h"
#include "../common/directory.h"
#include <abi/directory.h>
#include <abi/file.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_NAME ".history"
#define HISTORY_SAVE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)
/* Bytes read per call while scanning the file from its end. */
#define HISTORY_CHUNK 4096

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

static enum call_status read_exact(handle_t file, uint64_t offset, char *buffer, size_t size)
{
  for (size_t count = 0; count < size;) {
    size_t read;
    enum call_status status = file_read(file, offset + count, buffer + count, size - count, &read);
    if (status != CALL_OK) {
      return status;
    }
    if (!read) {
      return CALL_IO;
    }
    count += read;
  }
  return CALL_OK;
}

struct history_tail {
  /* Newest first; each is a valid entry. */
  char *entries[TERM_HISTORY_ENTRIES];
  size_t count;
  /* The line being read, last byte first, until the LF before it. */
  char reversed[SHELL_LINE_CAPACITY];
  size_t length;
  bool overlong;
  /* Bytes after the last LF belong to a line that was never finished. */
  bool unfinished;
};

/* A line ends at the LF just scanned. Longer than a valid entry, or invalid,
 * it is skipped like anything else the editor would not have accepted. */
static void finish_line(struct history_tail *tail)
{
  if (tail->length && !tail->overlong) {
    char *entry = malloc(tail->length + 1);
    if (entry) {
      for (size_t i = 0; i < tail->length; ++i) {
        entry[i] = tail->reversed[tail->length - 1 - i];
      }
      entry[tail->length] = '\0';
      if (valid_entry(entry, tail->length)) {
        tail->entries[tail->count++] = entry;
      } else {
        free(entry);
      }
    }
  }
  tail->length = 0;
  tail->overlong = false;
}

/* Reads the file backwards in chunks and stops as soon as it holds the newest
 * TERM_HISTORY_ENTRIES valid entries, so the file's length does not matter and
 * only a few kilobytes are read for a long history. A missing file is empty. */
static enum call_status read_tail(handle_t home, struct history_tail *tail)
{
  handle_t file;
  enum call_status status = directory_lookup(home, HISTORY_NAME, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status == CALL_NOT_FOUND) {
    return CALL_OK;
  }
  if (status != CALL_OK) {
    return status;
  }
  uint64_t end;
  status = file_size(file, &end);
  tail->unfinished = true;
  char chunk[HISTORY_CHUNK];
  while (status == CALL_OK && end && tail->count < TERM_HISTORY_ENTRIES) {
    size_t size = end < HISTORY_CHUNK ? (size_t)end : HISTORY_CHUNK;
    end -= size;
    status = read_exact(file, end, chunk, size);
    for (size_t i = size; status == CALL_OK && i && tail->count < TERM_HISTORY_ENTRIES; --i) {
      char byte = chunk[i - 1];
      if (tail->unfinished) {
        tail->unfinished = byte != '\n';
      } else if (byte == '\n') {
        finish_line(tail);
      } else if (tail->length < SHELL_LINE_CAPACITY) {
        tail->reversed[tail->length++] = byte;
      } else {
        tail->overlong = true;
      }
    }
  }
  if (status == CALL_OK && !end && !tail->unfinished && tail->count < TERM_HISTORY_ENTRIES) {
    finish_line(tail);
  }
  if (handle_close(file) != 0 && status == CALL_OK) {
    status = CALL_IO;
  }
  return status;
}

void shell_history_load(struct shell *shell)
{
  handle_t home = HANDLE_INVALID;
  environment_capability(shell->environment, "home", &home);
  if (home == HANDLE_INVALID) {
    return;
  }
  uint64_t rights;
  shell->history_home = home;
  shell->history_saving = handle_rights(home, &rights, NULL) == CALL_OK &&
      (rights & HISTORY_SAVE_RIGHTS) == HISTORY_SAVE_RIGHTS;
  struct history_tail *tail = calloc(1, sizeof(*tail));
  if (!tail) {
    return;
  }
  if (read_tail(home, tail) == CALL_OK) {
    for (size_t i = tail->count; i; --i) {
      term_history_add(&shell->history, tail->entries[i - 1]);
    }
  }
  for (size_t i = 0; i < tail->count; ++i) {
    free(tail->entries[i]);
  }
  free(tail);
}

/* Opens the history for appending, creating it when it is missing. Another
 * shell may create it first. Reading is for the last byte only. */
static enum call_status open_for_append(handle_t home, handle_t *file)
{
  enum call_status status = directory_lookup(home, HISTORY_NAME, DIRECTORY_KIND_FILE,
      FILE_RIGHTS, file);
  if (status == CALL_NOT_FOUND) {
    status = directory_create(home, HISTORY_NAME, DIRECTORY_KIND_FILE, FILE_RIGHTS, file);
    if (status == CALL_ALREADY_EXISTS) {
      status = directory_lookup(home, HISTORY_NAME, DIRECTORY_KIND_FILE, FILE_RIGHTS, file);
    }
  }
  return status;
}

/* True when the file ends inside a line, which only a failed save leaves. */
static enum call_status ends_inside_line(handle_t file, bool *inside)
{
  uint64_t size;
  char last = '\n';
  enum call_status status = file_size(file, &size);
  if (status == CALL_OK && size) {
    status = read_exact(file, size - 1, &last, 1);
  }
  *inside = status == CALL_OK && last != '\n';
  return status;
}

/* One FILE_APPEND selects the end of file and writes the line in a single
 * mutation of the file, so lines from concurrent shells never overwrite or
 * split each other: a record is at most SHELL_LINE_CAPACITY bytes plus its
 * two LFs, well inside the 4080 bytes one append carries. The file only grows;
 * nothing is trimmed or replaced. A short write (a full volume) is finished
 * once and otherwise reported. It, or a crash, can leave the file ending
 * inside a line; the next save sees that from the last byte and starts its
 * record with an LF, so two entries never merge. The torn line stays in the
 * file as an entry of its own. */
static enum call_status save_line(struct shell *shell, const char *line)
{
  char record[SHELL_LINE_CAPACITY + 2];
  size_t size = 1;
  size_t length = strlen(line);
  memcpy(record + size, line, length);
  size += length;
  record[0] = '\n';
  record[size++] = '\n';

  handle_t file;
  enum call_status status = open_for_append(shell->history_home, &file);
  if (status != CALL_OK) {
    return status;
  }
  bool inside;
  status = ends_inside_line(file, &inside);
  /* The leading LF is written only when the file ends inside a line. */
  size_t done = inside ? 0 : 1;
  while (status == CALL_OK && done < size) {
    uint64_t offset;
    size_t written;
    status = file_append(file, record + done, size - done, &offset, &written);
    done += written;
  }
  if (status == CALL_OK) {
    status = file_sync(file);
  }
  if (handle_close(file) != 0 && status == CALL_OK) {
    status = CALL_IO;
  }
  return status;
}

void shell_history_save(struct shell *shell, const char *line)
{
  if (!shell->history_saving || !valid_entry(line, strlen(line))) {
    return;
  }
  enum call_status status = save_line(shell, line);
  if (status != CALL_OK) {
    /* One report per shell; later lines stay in memory only. */
    shell->history_saving = false;
    report_directory_error("shell: history not saved", "home://" HISTORY_NAME, status);
  }
}
