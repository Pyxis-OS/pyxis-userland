#include "ls.h"
#include "../common/directory.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLUMN_GAP 2
#define COLOR_DIRECTORY "\x1b[94m"
#define COLOR_PROGRAM "\x1b[92m"
#define COLOR_SCRIPT "\x1b[96m"
#define COLOR_FILE "\x1b[39m"

int ls_print_text(const char *text, bool terminal)
{
  if (!terminal) {
    return fputs(text, stdout) == EOF ? -1 : 0;
  }
  /* One cell per byte, matching the terminal's ASCII presentation. Names
   * cannot inject controls or change the column geometry. Pipes stay raw. */
  char buffer[256];
  while (*text) {
    size_t count = 0;
    while (*text && count < sizeof(buffer)) {
      unsigned char byte = *text++;
      buffer[count++] = byte >= ' ' && byte <= '~' ? byte : '?';
    }
    if (fwrite(buffer, 1, count, stdout) != count) {
      return -1;
    }
  }
  return 0;
}

int ls_print_name(const struct ls_entry *entry, bool terminal)
{
  if (terminal) {
    const char *color = COLOR_FILE;
    switch (entry->kind) {
    case LS_DIRECTORY: color = COLOR_DIRECTORY; break;
    case LS_PROGRAM: color = COLOR_PROGRAM; break;
    case LS_SCRIPT: color = COLOR_SCRIPT; break;
    case LS_FILE: break;
    }
    if (fputs(color, stdout) == EOF) {
      return -1;
    }
  }
  int result = ls_print_text(entry->name, terminal);
  if (result == 0 && entry->kind == LS_DIRECTORY && fputc('/', stdout) == EOF) {
    result = -1;
  }
  if (terminal && fputs(COLOR_FILE, stdout) == EOF) {
    result = -1;
  }
  return result;
}

static int print_padding(size_t count)
{
  static const char spaces[] = "                                ";
  while (count != 0) {
    size_t chunk = count < sizeof(spaces) - 1 ? count : sizeof(spaces) - 1;
    if (fwrite(spaces, 1, chunk, stdout) != chunk) {
      return -1;
    }
    count -= chunk;
  }
  return 0;
}

static size_t layout_columns(const struct ls_listing *listing, size_t columns,
    size_t *widths, size_t count)
{
  for (; count > 1; --count) {
    memset(widths, 0, count * sizeof(*widths));
    for (size_t i = 0; i < listing->count; ++i) {
      size_t column = i % count;
      if (listing->entries[i].width > widths[column]) {
        widths[column] = listing->entries[i].width;
      }
    }
    size_t remaining = columns;
    bool fits = true;
    for (size_t column = 0; column < count; ++column) {
      size_t gap = column == 0 ? 0 : COLUMN_GAP;
      if (gap > remaining || widths[column] > remaining - gap) {
        fits = false;
        break;
      }
      remaining -= gap + widths[column];
    }
    if (fits) {
      return count;
    }
  }
  return 1;
}

static size_t size_width(const struct ls_listing *listing)
{
  size_t width = 1;
  for (size_t i = 0; i < listing->count; ++i) {
    uint64_t size = listing->entries[i].size;
    size_t digits = 1;
    while (size >= 10) {
      ++digits;
      size /= 10;
    }
    if (digits > width) {
      width = digits;
    }
  }
  return width;
}

static int print_details(const struct ls_entry *entry, size_t width)
{
  const char *kind;
  switch (entry->kind) {
  case LS_DIRECTORY: kind = "directory"; break;
  case LS_PROGRAM: kind = "program"; break;
  case LS_SCRIPT: kind = "script"; break;
  default: kind = "file"; break;
  }
  char size[21];
  if (entry->kind == LS_DIRECTORY) {
    strcpy(size, "-");
  } else if (!entry->size_known) {
    strcpy(size, "?");
  } else {
    snprintf(size, sizeof(size), "%" PRIu64, entry->size);
  }
  return printf("%-9s %*s ", kind, (int)width, size) < 0 ? -1 : 0;
}

int ls_print_listing(const struct ls_listing *listing, const struct ls_output *output)
{
  size_t count = 1;
  size_t *widths = NULL;
  if (output->terminal && !output->one_per_line && !output->long_listing &&
      output->columns > 1 && listing->count > 1) {
    /* Every column needs at least one name byte and two separating spaces. */
    size_t possible = (output->columns - 1) / (1 + COLUMN_GAP) + 1;
    if (possible > listing->count) {
      possible = listing->count;
    }
    if (possible > SIZE_MAX / sizeof(*widths) ||
        !(widths = malloc(possible * sizeof(*widths)))) {
      report_directory_error("ls", "column layout", CALL_NO_MEMORY);
      return -1;
    }
    count = layout_columns(listing, output->columns, widths, possible);
  }

  int result = 0;
  size_t digits = output->long_listing ? size_width(listing) : 0;
  for (size_t i = 0; i < listing->count; ++i) {
    const struct ls_entry *entry = &listing->entries[i];
    if ((output->long_listing && print_details(entry, digits) != 0) ||
        ls_print_name(entry, output->terminal) != 0) {
      result = -1;
      break;
    }
    size_t column = i % count;
    if (column + 1 == count || i + 1 == listing->count) {
      if (fputc('\n', stdout) == EOF) {
        result = -1;
        break;
      }
    } else {
      size_t padding = widths[column] - entry->width + COLUMN_GAP;
      if (print_padding(padding) != 0) {
        result = -1;
        break;
      }
    }
  }
  free(widths);
  if (result != 0) {
    perror("ls: stdout");
  }
  return result;
}
