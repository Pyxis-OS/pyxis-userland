#include <errno.h>
#include <startup.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_info.h>

#define DEFAULT_DATABASE "app://share/hwdata/pci.ids"
/* pci.ids field widths in hexadecimal digits. */
#define VENDOR_DIGITS 4
#define DEVICE_DIGITS 4
#define CLASS_DIGITS 2

struct entry {
  struct system_info_pci_function function;
  /* Owned database labels; NULL when the database has no entry. */
  char *vendor_name;
  char *device_name;
  char *class_name;
  char *subclass_name;
};

enum section { SECTION_NONE, SECTION_VENDOR, SECTION_CLASS };

static int usage(void)
{
  fputs("usage: lspci [-n] [-i FILE]\n", stderr);
  return EXIT_FAILURE;
}

static int hex_value(char character)
{
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

/* An ID of exactly the given width, two spaces and a nonempty label. */
static bool parse_id(const char *text, unsigned digits, unsigned *id, const char **name)
{
  unsigned value = 0;
  for (unsigned i = 0; i < digits; ++i) {
    int digit = hex_value(text[i]);
    if (digit < 0) {
      return false;
    }
    value = value << 4 | (unsigned)digit;
  }
  if (text[digits] != ' ' || text[digits + 1] != ' ' || !text[digits + 2]) {
    return false;
  }
  *id = value;
  *name = text + digits + 2;
  return true;
}

/* The first label for an ID wins; later duplicates are ignored. */
static bool save_name(char **slot, const char *name)
{
  if (*slot) {
    return true;
  }
  *slot = strdup(name);
  return *slot != NULL;
}

static void free_names(struct entry *entries, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    free(entries[i].vendor_name);
    free(entries[i].device_name);
    free(entries[i].class_name);
    free(entries[i].subclass_name);
    entries[i].vendor_name = entries[i].device_name = NULL;
    entries[i].class_name = entries[i].subclass_name = NULL;
  }
}

/* One pass over the text database. Subsystem and programming-interface lines
 * are skipped, and malformed lines simply provide no names. Returns zero or
 * an errno value; names found before a failure remain owned by entries. */
static int load_names(const char *path, struct entry *entries, size_t count)
{
  FILE *database = fopen(path, "r");
  if (!database) {
    return errno;
  }
  char *line = NULL;
  size_t capacity = 0;
  enum section section = SECTION_NONE;
  unsigned current = 0;
  int error = 0;
  ssize_t length;
  errno = 0;
  while ((length = getline(&line, &capacity, database)) >= 0) {
    if (length && line[length - 1] == '\n') {
      line[--length] = '\0';
    }
    unsigned id;
    const char *name;
    bool saved = true;
    if (!line[0] || line[0] == '#' || (line[0] == '\t' && line[1] == '\t')) {
      continue;
    }
    if (line[0] == '\t') {
      /* A device within a vendor, or a subclass within a class. */
      unsigned digits = section == SECTION_VENDOR ? DEVICE_DIGITS : CLASS_DIGITS;
      if (section == SECTION_NONE || !parse_id(line + 1, digits, &id, &name)) {
        continue;
      }
      for (size_t i = 0; i < count; ++i) {
        const struct system_info_pci_function *function = &entries[i].function;
        if (section == SECTION_VENDOR && function->vendor_id == current &&
            function->device_id == id) {
          saved = save_name(&entries[i].device_name, name) && saved;
        } else if (section == SECTION_CLASS && function->base_class == current &&
            function->subclass == id) {
          saved = save_name(&entries[i].subclass_name, name) && saved;
        }
      }
    } else if (line[0] == 'C' && line[1] == ' ' &&
        parse_id(line + 2, CLASS_DIGITS, &current, &name)) {
      section = SECTION_CLASS;
      for (size_t i = 0; i < count; ++i) {
        if (entries[i].function.base_class == current) {
          saved = save_name(&entries[i].class_name, name) && saved;
        }
      }
    } else if (parse_id(line, VENDOR_DIGITS, &current, &name)) {
      section = SECTION_VENDOR;
      for (size_t i = 0; i < count; ++i) {
        if (entries[i].function.vendor_id == current) {
          saved = save_name(&entries[i].vendor_name, name) && saved;
        }
      }
    } else {
      section = SECTION_NONE;
    }
    if (!saved) {
      error = ENOMEM;
      break;
    }
  }
  if (!error && ferror(database)) {
    error = errno ? errno : EIO;
  }
  free(line);
  if (fclose(database) != 0 && !error) {
    error = errno;
  }
  return error;
}

static int compare_entries(const void *left, const void *right)
{
  const struct system_info_pci_function *a = &((const struct entry *)left)->function;
  const struct system_info_pci_function *b = &((const struct entry *)right)->function;
  uint64_t first = (uint64_t)a->segment << 24 | (uint64_t)a->bus << 16 |
      (uint64_t)a->device << 8 | a->function;
  uint64_t second = (uint64_t)b->segment << 24 | (uint64_t)b->bus << 16 |
      (uint64_t)b->device << 8 | b->function;
  return first < second ? -1 : first > second;
}

/* Database labels are descriptive text; keep terminal output unambiguous. */
static void put_label(const char *text)
{
  for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
    if (*cursor == '\\') {
      fputs("\\\\", stdout);
    } else if (*cursor >= ' ' && *cursor <= '~') {
      putchar(*cursor);
    } else {
      printf("\\x%02x", *cursor);
    }
  }
}

static void print_entry(const struct entry *entry, bool segments, bool numeric)
{
  const struct system_info_pci_function *function = &entry->function;
  if (segments) {
    printf("%04x:", function->segment);
  }
  printf("%02x:%02x.%x ", function->bus, function->device, function->function);
  if (numeric) {
    printf("%02x%02x: %04x:%04x", function->base_class, function->subclass,
        function->vendor_id, function->device_id);
  } else {
    const char *class_name = entry->subclass_name ? entry->subclass_name :
        entry->class_name ? entry->class_name : "Class";
    put_label(class_name);
    printf(" [%02x%02x]: ", function->base_class, function->subclass);
    if (entry->vendor_name) {
      put_label(entry->vendor_name);
      putchar(' ');
    }
    put_label(entry->device_name ? entry->device_name : "Device");
    printf(" [%04x:%04x]", function->vendor_id, function->device_id);
  }
  if (function->revision) {
    printf(" (rev %02x)", function->revision);
  }
  putchar('\n');
}

int main(int argc, char **argv)
{
  bool numeric = false;
  const char *database = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-n") && !numeric) {
      numeric = true;
    } else if (!strcmp(argv[i], "-i") && !database && i + 1 < argc) {
      database = argv[++i];
    } else {
      return usage();
    }
  }
  if (!database) {
    database = DEFAULT_DATABASE;
  }

  handle_t system_info = startup_resource("system_info");
  if (system_info == HANDLE_INVALID) {
    fputs("lspci: system information authority unavailable\n", stderr);
    return EXIT_FAILURE;
  }
  struct system_info_pci inventory;
  enum call_status status = system_info_get_pci(system_info, &inventory);
  if (status != CALL_OK) {
    fprintf(stderr, "lspci: PCI inventory query failed (status %u)\n", status);
    return EXIT_FAILURE;
  }
  if (inventory.state == SYSTEM_INFO_PCI_UNAVAILABLE) {
    fputs("lspci: PCI inventory unavailable\n", stderr);
    return EXIT_FAILURE;
  }
  if (inventory.function_count > SIZE_MAX / sizeof(struct entry)) {
    fputs("lspci: PCI inventory too large\n", stderr);
    return EXIT_FAILURE;
  }

  size_t count = (size_t)inventory.function_count;
  struct entry *entries = calloc(count ? count : 1, sizeof(*entries));
  if (!entries) {
    fputs("lspci: cannot allocate inventory\n", stderr);
    return EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  bool segments = false;
  for (size_t i = 0; i < count; ++i) {
    status = system_info_get_pci_function(system_info, i, &entries[i].function);
    if (status != CALL_OK) {
      fprintf(stderr, "lspci: PCI function %zu query failed (status %u)\n", i, status);
      free(entries);
      return EXIT_FAILURE;
    }
    segments |= entries[i].function.segment != 0;
  }
  qsort(entries, count, sizeof(*entries), compare_entries);

  if (!numeric) {
    int error = load_names(database, entries, count);
    if (error) {
      fprintf(stderr, "lspci: %s: %s; showing numeric IDs\n", database, strerror(error));
      free_names(entries, count);
      numeric = true;
    }
  }
  /* stdout is unbuffered and fclose reports only descriptor closure, so a
   * failed write is visible only through the stream error indicator. */
  int write_error = 0;
  errno = 0;
  for (size_t i = 0; i < count && !write_error; ++i) {
    print_entry(&entries[i], segments, numeric);
    if (ferror(stdout)) {
      write_error = errno ? errno : EIO;
    }
  }
  free_names(entries, count);
  free(entries);

  if (inventory.state == SYSTEM_INFO_PCI_INCOMPLETE) {
    fputs("lspci: PCI inventory incomplete; listed functions are valid\n", stderr);
    result = EXIT_FAILURE;
  }
  if (fclose(stdout) != 0 && !write_error) {
    write_error = errno;
  }
  if (write_error) {
    fprintf(stderr, "lspci: stdout: %s\n", strerror(write_error));
    result = EXIT_FAILURE;
  }
  return result;
}
