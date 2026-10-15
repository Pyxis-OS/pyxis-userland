#include "../common/directory.h"
#include <acpi_tables.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dumps the tables of the acpi:// root. The text form is the Linux acpidump
 * layout that acpixtract reads: a "SIGN @ 0xADDRESS" line, then rows of
 * sixteen hex bytes with their printable ASCII, then an empty line. */

#define DEFAULT_ROOT "acpi://"
#define ROOT_RIGHTS (DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_LOOKUP | \
    DIRECTORY_RIGHT_READ_FILES)
#define OUTPUT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES)
#define CHUNK_BYTES (64u * 1024u)
#define ROW_BYTES 16u
/* "    0000: " + 16 * "XX " + " " + 16 ASCII + "\n" */
#define ROW_TEXT_BYTES (10u + ROW_BYTES * 3u + 1u + ROW_BYTES + 1u)

struct table {
  char *name;
  struct acpi_table_info info;
};

struct listing {
  struct table *tables;
  size_t count, capacity;
};

static int usage(void)
{
  fputs("usage: acpidump [-s | -b DIRECTORY] [ROOT]\n", stderr);
  return EXIT_FAILURE;
}

static int append_table(struct listing *listing, const char *name)
{
  if (listing->count == listing->capacity) {
    size_t capacity = listing->capacity ? listing->capacity * 2 : 16;
    struct table *tables = realloc(listing->tables, capacity * sizeof(*tables));
    if (!tables) {
      return -1;
    }
    listing->tables = tables;
    listing->capacity = capacity;
  }
  char *owned = strdup(name);
  if (!owned) {
    return -1;
  }
  listing->tables[listing->count++] = (struct table){.name = owned};
  return 0;
}

static void free_listing(struct listing *listing)
{
  for (size_t i = 0; i < listing->count; ++i) {
    free(listing->tables[i].name);
  }
  free(listing->tables);
  *listing = (struct listing){0};
}

/* Array order is the listing order; a short name buffer grows on demand. */
static int list_tables(handle_t root, const char *path, struct listing *listing)
{
  size_t capacity = ACPI_TABLE_NAME_MAX + 1;
  char *name = malloc(capacity);
  if (!name) {
    report_directory_error("acpidump", path, CALL_NO_MEMORY);
    return -1;
  }
  int result = -1;
  struct directory_cursor cursor = {0};
  for (;;) {
    struct directory_enumerate_reply entry;
    enum call_status status = directory_enumerate(root, &cursor, name, capacity, &entry);
    if (status != CALL_OK) {
      report_directory_error("acpidump", path, status);
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      result = 0;
      break;
    }
    if (entry.outcome == DIRECTORY_CHANGED) {
      fprintf(stderr, "acpidump: %s: Table list changed during listing\n", path);
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      if (!larger) {
        report_directory_error("acpidump", path, CALL_NO_MEMORY);
        break;
      }
      name = larger;
      capacity = entry.name_size;
      continue;
    }
    if (append_table(listing, name) != 0) {
      report_directory_error("acpidump", path, CALL_NO_MEMORY);
      break;
    }
    cursor = entry.cursor;
  }
  free(name);
  if (result != 0) {
    return result;
  }
  for (size_t i = 0; i < listing->count; ++i) {
    struct table *table = &listing->tables[i];
    enum call_status status = acpi_table_info(root, table->name, &table->info);
    if (status != CALL_OK) {
      report_directory_error("acpidump", table->name, status);
      return -1;
    }
  }
  return 0;
}

static void print_summary(const struct listing *listing)
{
  for (size_t i = 0; i < listing->count; ++i) {
    const struct table *table = &listing->tables[i];
    const char *checksum = !(table->info.flags & ACPI_TABLE_CHECKSUM_CHECKED) ? "unchecked" :
        (table->info.flags & ACPI_TABLE_CHECKSUM_BAD) ? "bad" : "ok";
    printf("%-8s %.4s %8" PRIu64 " 0x%016" PRIx64 " checksum=%s%s%s\n", table->name,
        table->info.signature, table->info.length, table->info.physical_address, checksum,
        (table->info.flags & ACPI_TABLE_RUNTIME_LOADED) ? " runtime-loaded" : "",
        (table->info.flags & ACPI_TABLE_WITHHELD) ? " withheld" : "");
  }
}

static const char hex_digits[] = "0123456789ABCDEF";

/* One row in ACPICA's byte layout; a short final row keeps its column widths. */
static size_t format_row(char *text, size_t offset, const unsigned char *bytes, size_t count)
{
  /* ACPICA's "%8.4X": at least four digits, right-aligned in eight columns. */
  char digits[sizeof(size_t) * 2];
  size_t length = 0;
  for (size_t value = offset; value || length < 4; value >>= 4) {
    digits[length++] = hex_digits[value & 0xf];
  }
  size_t used = 0;
  for (size_t i = length; i < 8; ++i) {
    text[used++] = ' ';
  }
  while (length) {
    text[used++] = digits[--length];
  }
  text[used++] = ':';
  text[used++] = ' ';
  for (size_t i = 0; i < ROW_BYTES; ++i) {
    if (i < count) {
      text[used++] = hex_digits[bytes[i] >> 4];
      text[used++] = hex_digits[bytes[i] & 0xf];
    } else {
      text[used++] = ' ';
      text[used++] = ' ';
    }
    text[used++] = ' ';
  }
  text[used++] = ' ';
  for (size_t i = 0; i < count; ++i) {
    text[used++] = bytes[i] >= 0x20 && bytes[i] <= 0x7e ? (char)bytes[i] : '.';
  }
  text[used++] = '\n';
  return used;
}

/* Reads the whole table through its READ-only FILE and hands each chunk to
 * sink; a table shorter than its listed length is reported, not padded. */
static int read_table(handle_t root, const struct table *table,
    int (*sink)(void *context, const unsigned char *bytes, size_t count, uint64_t offset),
    void *context)
{
  handle_t file;
  enum call_status status = directory_lookup(root, table->name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status != CALL_OK) {
    report_directory_error("acpidump", table->name, status);
    return -1;
  }
  int result = -1;
  unsigned char *chunk = malloc(CHUNK_BYTES);
  if (!chunk) {
    report_directory_error("acpidump", table->name, CALL_NO_MEMORY);
    goto done;
  }
  uint64_t size;
  status = file_size(file, &size);
  if (status != CALL_OK) {
    report_directory_error("acpidump", table->name, status);
    goto done;
  }
  uint64_t offset = 0;
  while (offset < size) {
    size_t capacity = size - offset < CHUNK_BYTES ? (size_t)(size - offset) : CHUNK_BYTES;
    size_t read;
    status = file_read(file, offset, chunk, capacity, &read);
    if (status != CALL_OK) {
      report_directory_error("acpidump", table->name, status);
      goto done;
    }
    if (!read) {
      fprintf(stderr, "acpidump: %s: Short table: %" PRIu64 " of %" PRIu64 " bytes\n",
          table->name, offset, size);
      goto done;
    }
    if (sink(context, chunk, read, offset) != 0) {
      goto done;
    }
    offset += read;
  }
  result = 0;

done:
  free(chunk);
  if (handle_close(file) != 0) {
    report_directory_error("acpidump", table->name, CALL_BAD_HANDLE);
    result = -1;
  }
  return result;
}

struct text_sink {
  char *rows;
};

/* Chunks are multiples of the row width except the last, so rows never span
 * two chunks. One fwrite per chunk keeps the unbuffered console fast. */
static int write_text_rows(void *context, const unsigned char *bytes, size_t count,
    uint64_t offset)
{
  struct text_sink *sink = context;
  size_t used = 0;
  for (size_t row = 0; row < count; row += ROW_BYTES) {
    size_t row_count = count - row < ROW_BYTES ? count - row : ROW_BYTES;
    used += format_row(sink->rows + used, (size_t)(offset + row), bytes + row, row_count);
  }
  if (fwrite(sink->rows, 1, used, stdout) != used) {
    perror("acpidump: stdout");
    return -1;
  }
  return 0;
}

static int dump_text(handle_t root, const struct table *table)
{
  struct text_sink sink = {.rows = malloc((CHUNK_BYTES / ROW_BYTES) * ROW_TEXT_BYTES)};
  if (!sink.rows) {
    report_directory_error("acpidump", table->name, CALL_NO_MEMORY);
    return -1;
  }
  int result = -1;
  if (printf("%.4s @ 0x%016" PRIx64 "\n", table->info.signature,
      table->info.physical_address) < 0) {
    perror("acpidump: stdout");
    goto done;
  }
  if (read_table(root, table, write_text_rows, &sink) != 0) {
    goto done;
  }
  if (fputs("\n", stdout) == EOF) {
    perror("acpidump: stdout");
    goto done;
  }
  result = 0;

done:
  free(sink.rows);
  return result;
}

struct file_sink {
  handle_t file;
  const char *name;
};

static int write_file_chunk(void *context, const unsigned char *bytes, size_t count,
    uint64_t offset)
{
  struct file_sink *sink = context;
  while (count) {
    size_t written;
    enum call_status status = file_write(sink->file, offset, bytes, count, &written);
    if (status != CALL_OK || !written) {
      report_directory_error("acpidump", sink->name, status == CALL_OK ? CALL_IO : status);
      return -1;
    }
    bytes += written;
    count -= written;
    offset += written;
  }
  return 0;
}

/* Exclusive creation: an existing name is reported and never replaced. */
static int dump_binary(handle_t root, const struct table *table, handle_t parent,
    const char *directory)
{
  struct file_sink sink = {.name = table->name};
  enum call_status status = directory_create(parent, table->name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_WRITE, &sink.file);
  if (status == CALL_ALREADY_EXISTS) {
    fprintf(stderr, "acpidump: %s/%s: Exists; not replaced\n", directory, table->name);
    return -1;
  }
  if (status != CALL_OK) {
    report_directory_error("acpidump", table->name, status);
    return -1;
  }
  int result = read_table(root, table, write_file_chunk, &sink);
  if (handle_close(sink.file) != 0) {
    report_directory_error("acpidump", table->name, CALL_BAD_HANDLE);
    result = -1;
  }
  return result;
}

int main(int argc, char **argv)
{
  bool summary = false;
  const char *binary_directory = NULL;
  const char *root_path = DEFAULT_ROOT;
  int operands = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-s")) {
      summary = true;
    } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
      binary_directory = argv[++i];
    } else if (argv[i][0] == '-' && argv[i][1]) {
      return usage();
    } else if (operands++) {
      return usage();
    } else {
      root_path = argv[i];
    }
  }
  if (summary && binary_directory) {
    return usage();
  }

  handle_t root;
  enum call_status status = resolve_directory(root_path, ROOT_RIGHTS, &root);
  if (status != CALL_OK) {
    report_directory_error("acpidump", root_path, status);
    return EXIT_FAILURE;
  }
  handle_t parent = HANDLE_INVALID;
  if (binary_directory) {
    status = resolve_directory(binary_directory, OUTPUT_RIGHTS, &parent);
    if (status != CALL_OK) {
      report_directory_error("acpidump", binary_directory, status);
      handle_close(root);
      return EXIT_FAILURE;
    }
  }

  int result = EXIT_SUCCESS;
  struct listing listing = {0};
  if (list_tables(root, root_path, &listing) != 0) {
    result = EXIT_FAILURE;
  } else if (summary) {
    print_summary(&listing);
  } else {
    bool withheld = false;
    for (size_t i = 0; i < listing.count; ++i) {
      const struct table *table = &listing.tables[i];
      if (table->info.flags & ACPI_TABLE_WITHHELD) {
        withheld = true;
        continue;
      }
      int dumped = binary_directory ? dump_binary(root, table, parent, binary_directory) :
          dump_text(root, table);
      if (dumped != 0) {
        result = EXIT_FAILURE;
      }
    }
    if (withheld) {
      fputs("acpidump: withheld, not dumped:", stderr);
      for (size_t i = 0; i < listing.count; ++i) {
        if (listing.tables[i].info.flags & ACPI_TABLE_WITHHELD) {
          fprintf(stderr, " %s", listing.tables[i].name);
        }
      }
      fputs("\n", stderr);
    }
  }
  if (fflush(stdout) == EOF || ferror(stdout)) {
    perror("acpidump: stdout");
    result = EXIT_FAILURE;
  }
  free_listing(&listing);
  if (parent != HANDLE_INVALID && handle_close(parent) != 0) {
    report_directory_error("acpidump", binary_directory, CALL_BAD_HANDLE);
    result = EXIT_FAILURE;
  }
  if (handle_close(root) != 0) {
    report_directory_error("acpidump", root_path, CALL_BAD_HANDLE);
    result = EXIT_FAILURE;
  }
  return result;
}
