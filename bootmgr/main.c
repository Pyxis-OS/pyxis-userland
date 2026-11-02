#include "../common/bootstrap_environment.h"
#include <handle.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uefi.h>

#define ORDER_ENTRIES (UEFI_BOOT_VARIABLE_BYTES / 2)

static handle_t uefi;

static uint16_t get16(const uint8_t *bytes)
{
  return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static void put16(uint8_t *bytes, uint16_t value)
{
  bytes[0] = value;
  bytes[1] = value >> 8;
}

static uint32_t get32(const uint8_t *bytes)
{
  return (uint32_t)get16(bytes) | ((uint32_t)get16(bytes + 2) << 16);
}

static const char *status_text(enum call_status status)
{
  switch (status) {
    case CALL_NOT_FOUND: return "not found";
    case CALL_DENIED: return "denied";
    case CALL_BUSY: return "firmware busy, try again";
    case CALL_UNAVAILABLE: return "firmware variables unavailable";
    case CALL_LIMIT: return "value too large";
    case CALL_IO: return "firmware refused the read";
    case CALL_BAD_REQUEST: return "bad request";
    case CALL_OUTCOME_UNKNOWN:
      return "the write could not be confirmed; inspect with 'bootmgr' before changing anything else";
    default: return "failed";
  }
}

static int failure(const char *operation, enum call_status status)
{
  fprintf(stderr, "bootmgr: %s: %s (status %u)\n", operation, status_text(status), status);
  return EXIT_FAILURE;
}

static int usage(void)
{
  fputs("usage: bootmgr [list]\n"
        "       bootmgr next HEX | bootmgr next --clear\n"
        "       bootmgr order HEX[,HEX...]\n"
        "       bootmgr timeout SECONDS | bootmgr timeout --clear\n"
        "       bootmgr create LABEL --like HEX [--file PATH] [--inactive] [--first]\n"
        "       bootmgr edit HEX [--label LABEL] [--file PATH] [--active | --inactive]\n"
        "       bootmgr delete HEX\n"
        "Numbers are Boot#### hexadecimal numbers. PATH is a firmware file path;\n"
        "'/' stands for '\\'. create copies the device path of the --like entry.\n", stderr);
  return EXIT_FAILURE;
}

static bool parse_hex(const char *text, uint32_t *value)
{
  char *end;
  unsigned long parsed = strtoul(text, &end, 16);
  if (end == text || *end || parsed > 0xffff) {
    return false;
  }
  *value = parsed;
  return true;
}

static enum call_status read_variable(uint32_t kind, uint32_t number, uint8_t *value,
    size_t capacity, size_t *size)
{
  uint32_t attributes;
  return uefi_boot_variable_get(uefi, kind, number, &attributes, value, capacity, size);
}

static enum call_status read_order(uint16_t *order, size_t *count)
{
  uint8_t bytes[UEFI_BOOT_VARIABLE_BYTES];
  size_t size;
  enum call_status status = read_variable(UEFI_VARIABLE_BOOT_ORDER, 0, bytes, sizeof(bytes), &size);
  if (status == CALL_NOT_FOUND) {
    *count = 0;
    return CALL_OK;
  }
  if (status != CALL_OK) {
    return status;
  }
  *count = size / 2;
  for (size_t i = 0; i < *count; ++i) {
    order[i] = get16(bytes + i * 2);
  }
  return CALL_OK;
}

static enum call_status write_order(const uint16_t *order, size_t count)
{
  if (!count) {
    enum call_status status = uefi_boot_variable_delete(uefi, UEFI_VARIABLE_BOOT_ORDER, 0);
    return status == CALL_NOT_FOUND ? CALL_OK : status;
  }
  uint8_t bytes[UEFI_BOOT_VARIABLE_BYTES];
  for (size_t i = 0; i < count; ++i) {
    put16(bytes + i * 2, order[i]);
  }
  return uefi_boot_variable_set(uefi, UEFI_VARIABLE_BOOT_ORDER, 0, bytes, count * 2);
}

static void print_guid(const uint8_t *guid)
{
  printf("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", get32(guid), get16(guid + 4),
      get16(guid + 6), guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14],
      guid[15]);
}

static void print_path(const uint8_t *path, size_t size)
{
  size_t offset = 0;
  struct uefi_path_node node;
  for (bool first = true; uefi_path_next(path, size, &offset, &node); first = false) {
    if (!first) {
      putchar('/');
    }
    struct uefi_hard_drive drive;
    if (uefi_path_hard_drive(&node, &drive)) {
      printf("HD(%u,%s,", drive.partition_number,
          drive.signature_type == UEFI_SIGNATURE_GPT ? "GPT" :
          drive.signature_type == UEFI_SIGNATURE_MBR ? "MBR" : "?");
      if (drive.signature_type == UEFI_SIGNATURE_GPT) {
        print_guid(drive.signature);
      } else {
        printf("%08x", get32(drive.signature));
      }
      printf(",0x%llx,0x%llx)", (unsigned long long)drive.first_lba,
          (unsigned long long)drive.block_count);
    } else if (node.type == UEFI_PATH_MEDIA && node.subtype == UEFI_PATH_FILE) {
      fputs("File(", stdout);
      for (size_t i = 0; i + 1 < node.data_size; i += 2) {
        uint16_t unit = get16(node.data + i);
        if (!unit) {
          break;
        }
        putchar(unit < 0x80 ? (int)unit : '?');
      }
      putchar(')');
    } else {
      printf("Path(%u,%u", node.type, node.subtype);
      for (size_t i = 0; i < node.data_size; ++i) {
        printf("%s%02x", i ? "" : ",", node.data[i]);
      }
      putchar(')');
    }
  }
}

static enum call_status read_entry(uint32_t number, uint8_t *bytes, size_t *size, struct uefi_load_option *entry)
{
  enum call_status status = read_variable(UEFI_VARIABLE_BOOT_ENTRY, number, bytes,
      UEFI_BOOT_VARIABLE_BYTES, size);
  if (status == CALL_OK && !uefi_load_option_parse(bytes, *size, entry)) {
    return CALL_BAD_REQUEST;
  }
  return status;
}

static void print_entry(uint32_t number)
{
  uint8_t bytes[UEFI_BOOT_VARIABLE_BYTES];
  size_t size;
  struct uefi_load_option entry;
  enum call_status status = read_entry(number, bytes, &size, &entry);
  printf("Boot%04X%c ", number, status == CALL_OK && (entry.attributes & UEFI_LOAD_OPTION_ACTIVE) ? '*' : ' ');
  if (status != CALL_OK) {
    printf("(unreadable: %s)\n", status == CALL_BAD_REQUEST ? "malformed load option" : status_text(status));
    return;
  }
  char label[128];
  uefi_load_option_label(&entry, label, sizeof(label));
  printf("%s\t", label);
  print_path(entry.path, entry.path_size);
  if (entry.data_size) {
    printf(" +%zu data bytes", entry.data_size);
  }
  putchar('\n');
}

static int list(void)
{
  uint8_t bytes[UEFI_BOOT_VARIABLE_BYTES];
  size_t size;
  enum call_status status = read_variable(UEFI_VARIABLE_BOOT_CURRENT, 0, bytes, sizeof(bytes), &size);
  if (status == CALL_OK && size == 2) {
    printf("BootCurrent: %04X\n", get16(bytes));
  } else if (status != CALL_NOT_FOUND) {
    return failure("BootCurrent", status);
  }
  status = read_variable(UEFI_VARIABLE_BOOT_NEXT, 0, bytes, sizeof(bytes), &size);
  if (status == CALL_OK && size == 2) {
    printf("BootNext: %04X\n", get16(bytes));
  } else if (status != CALL_NOT_FOUND) {
    return failure("BootNext", status);
  }
  status = read_variable(UEFI_VARIABLE_TIMEOUT, 0, bytes, sizeof(bytes), &size);
  if (status == CALL_OK && size == 2) {
    printf("Timeout: %u seconds\n", get16(bytes));
  } else if (status != CALL_NOT_FOUND) {
    return failure("Timeout", status);
  }
  uint16_t order[ORDER_ENTRIES];
  size_t count;
  status = read_order(order, &count);
  if (status != CALL_OK) {
    return failure("BootOrder", status);
  }
  fputs("BootOrder: ", stdout);
  for (size_t i = 0; i < count; ++i) {
    printf("%s%04X", i ? "," : "", order[i]);
  }
  putchar('\n');
  for (size_t i = 0; i < count; ++i) {
    print_entry(order[i]);
  }
  /* Entries outside BootOrder, found by scanning every number in windows. */
  for (uint32_t first = 0; first < UEFI_BOOT_ENTRY_NUMBERS; first += UEFI_BOOT_ENTRY_WINDOW) {
    uint8_t bitmap[UEFI_BOOT_ENTRY_WINDOW / 8];
    status = uefi_boot_entries(uefi, first, UEFI_BOOT_ENTRY_WINDOW, bitmap);
    if (status != CALL_OK) {
      return failure("scan", status);
    }
    for (uint32_t n = 0; n < UEFI_BOOT_ENTRY_WINDOW; ++n) {
      if (!(bitmap[n / 8] & (1u << (n % 8)))) {
        continue;
      }
      bool ordered = false;
      for (size_t i = 0; i < count; ++i) {
        ordered |= order[i] == first + n;
      }
      if (!ordered) {
        print_entry(first + n);
      }
    }
  }
  return EXIT_SUCCESS;
}

static int set_next(int argc, char **argv)
{
  if (argc == 1 && !strcmp(argv[0], "--clear")) {
    enum call_status status = uefi_boot_variable_delete(uefi, UEFI_VARIABLE_BOOT_NEXT, 0);
    return status == CALL_OK || status == CALL_NOT_FOUND ? EXIT_SUCCESS : failure("clear BootNext", status);
  }
  uint32_t number;
  if (argc != 1 || !parse_hex(argv[0], &number)) {
    return usage();
  }
  uint8_t bytes[UEFI_BOOT_VARIABLE_BYTES];
  size_t size;
  struct uefi_load_option entry;
  enum call_status status = read_entry(number, bytes, &size, &entry);
  if (status != CALL_OK) {
    return failure("no such boot entry", status);
  }
  uint8_t value[2];
  put16(value, number);
  status = uefi_boot_variable_set(uefi, UEFI_VARIABLE_BOOT_NEXT, 0, value, sizeof(value));
  return status == CALL_OK ? EXIT_SUCCESS : failure("set BootNext", status);
}

static int set_order(int argc, char **argv)
{
  if (argc != 1) {
    return usage();
  }
  uint16_t order[ORDER_ENTRIES];
  size_t count = 0;
  char *text = argv[0], *save = NULL;
  for (char *item = strtok_r(text, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
    uint32_t number;
    if (count == ORDER_ENTRIES || !parse_hex(item, &number)) {
      return usage();
    }
    order[count++] = number;
  }
  enum call_status status = write_order(order, count);
  return status == CALL_OK ? EXIT_SUCCESS : failure("set BootOrder", status);
}

static int set_timeout(int argc, char **argv)
{
  if (argc == 1 && !strcmp(argv[0], "--clear")) {
    enum call_status status = uefi_boot_variable_delete(uefi, UEFI_VARIABLE_TIMEOUT, 0);
    return status == CALL_OK || status == CALL_NOT_FOUND ? EXIT_SUCCESS : failure("clear Timeout", status);
  }
  char *end;
  unsigned long seconds = argc == 1 ? strtoul(argv[0], &end, 10) : 0;
  if (argc != 1 || end == argv[0] || *end || seconds > 0xffff) {
    return usage();
  }
  uint8_t value[2];
  put16(value, seconds);
  enum call_status status = uefi_boot_variable_set(uefi, UEFI_VARIABLE_TIMEOUT, 0, value, sizeof(value));
  return status == CALL_OK ? EXIT_SUCCESS : failure("set Timeout", status);
}

/* Builds a load option into bytes; path and file are optional replacements. */
static size_t build_entry(uint8_t *bytes, uint32_t attributes, const char *label,
    const uint8_t *path, size_t path_size, const char *file)
{
  uint8_t nodes[UEFI_BOOT_VARIABLE_BYTES];
  size_t nodes_size = 0;
  /* Keep the source nodes up to the end node, swapping the file node. */
  size_t offset = 0;
  struct uefi_path_node node;
  while (uefi_path_next(path, path_size, &offset, &node)) {
    if (node.type == UEFI_PATH_MEDIA && node.subtype == UEFI_PATH_FILE && file) {
      continue;
    }
    size_t length = node.data_size + 4;
    if (nodes_size + length > sizeof(nodes)) {
      return 0;
    }
    memcpy(nodes + nodes_size, node.data - 4, length);
    nodes_size += length;
  }
  if (file) {
    size_t units = strlen(file) + 1;
    size_t length = 4 + units * 2;
    if (nodes_size + length + 4 > sizeof(nodes)) {
      return 0;
    }
    nodes[nodes_size] = UEFI_PATH_MEDIA;
    nodes[nodes_size + 1] = UEFI_PATH_FILE;
    put16(nodes + nodes_size + 2, length);
    for (size_t i = 0; i < units; ++i) {
      put16(nodes + nodes_size + 4 + i * 2, file[i] == '/' ? '\\' : (uint8_t)file[i]);
    }
    nodes_size += length;
  }
  nodes[nodes_size] = UEFI_PATH_END;
  nodes[nodes_size + 1] = UEFI_PATH_END_ENTIRE;
  put16(nodes + nodes_size + 2, 4);
  nodes_size += 4;
  size_t label_units = strlen(label) + 1;
  size_t total = 6 + label_units * 2 + nodes_size;
  if (total > UEFI_BOOT_VARIABLE_BYTES) {
    return 0;
  }
  put16(bytes, attributes);
  put16(bytes + 2, attributes >> 16);
  put16(bytes + 4, nodes_size);
  for (size_t i = 0; i < label_units; ++i) {
    put16(bytes + 6 + i * 2, (uint8_t)label[i]);
  }
  memcpy(bytes + 6 + label_units * 2, nodes, nodes_size);
  return total;
}

static int create(int argc, char **argv)
{
  if (argc < 3 || argv[0][0] == '-') {
    return usage();
  }
  const char *label = argv[0], *file = NULL;
  uint32_t like = 0, attributes = UEFI_LOAD_OPTION_ACTIVE;
  bool have_like = false, first = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--like") && i + 1 < argc && parse_hex(argv[i + 1], &like)) {
      have_like = true;
      ++i;
    } else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
      file = argv[++i];
    } else if (!strcmp(argv[i], "--inactive")) {
      attributes = 0;
    } else if (!strcmp(argv[i], "--first")) {
      first = true;
    } else {
      return usage();
    }
  }
  if (!have_like) {
    return usage();
  }
  uint8_t source[UEFI_BOOT_VARIABLE_BYTES], bytes[UEFI_BOOT_VARIABLE_BYTES];
  size_t size;
  struct uefi_load_option entry;
  enum call_status status = read_entry(like, source, &size, &entry);
  if (status != CALL_OK) {
    return failure("--like entry", status);
  }
  size_t total = build_entry(bytes, attributes, label, entry.path, entry.path_size, file);
  if (!total) {
    fputs("bootmgr: the entry does not fit in one variable\n", stderr);
    return EXIT_FAILURE;
  }
  /* The first number whose Boot#### does not exist. */
  uint32_t number = UEFI_BOOT_ENTRY_NUMBERS;
  for (uint32_t window = 0; window < UEFI_BOOT_ENTRY_NUMBERS && number == UEFI_BOOT_ENTRY_NUMBERS;
      window += UEFI_BOOT_ENTRY_WINDOW) {
    uint8_t bitmap[UEFI_BOOT_ENTRY_WINDOW / 8];
    status = uefi_boot_entries(uefi, window, UEFI_BOOT_ENTRY_WINDOW, bitmap);
    if (status != CALL_OK) {
      return failure("scan", status);
    }
    for (uint32_t n = 0; n < UEFI_BOOT_ENTRY_WINDOW; ++n) {
      if (!(bitmap[n / 8] & (1u << (n % 8)))) {
        number = window + n;
        break;
      }
    }
  }
  if (number == UEFI_BOOT_ENTRY_NUMBERS) {
    fputs("bootmgr: every Boot#### number is in use\n", stderr);
    return EXIT_FAILURE;
  }
  status = uefi_boot_variable_set(uefi, UEFI_VARIABLE_BOOT_ENTRY, number, bytes, total);
  if (status != CALL_OK) {
    return failure("create entry", status);
  }
  uint16_t order[ORDER_ENTRIES];
  size_t count;
  status = read_order(order, &count);
  if (status == CALL_OK && count == ORDER_ENTRIES) {
    status = CALL_LIMIT;
  }
  if (status == CALL_OK) {
    if (first) {
      memmove(order + 1, order, count * sizeof(*order));
      order[0] = number;
    } else {
      order[count] = number;
    }
    status = write_order(order, count + 1);
  }
  if (status != CALL_OK) {
    fprintf(stderr, "bootmgr: Boot%04X was created but BootOrder was not updated\n", number);
    return failure("update BootOrder", status);
  }
  printf("Boot%04X created\n", number);
  return EXIT_SUCCESS;
}

static int edit(int argc, char **argv)
{
  uint32_t number;
  if (argc < 2 || !parse_hex(argv[0], &number)) {
    return usage();
  }
  const char *label = NULL, *file = NULL;
  int active = -1;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--label") && i + 1 < argc) {
      label = argv[++i];
    } else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
      file = argv[++i];
    } else if (!strcmp(argv[i], "--active")) {
      active = 1;
    } else if (!strcmp(argv[i], "--inactive")) {
      active = 0;
    } else {
      return usage();
    }
  }
  uint8_t source[UEFI_BOOT_VARIABLE_BYTES], bytes[UEFI_BOOT_VARIABLE_BYTES];
  size_t size;
  struct uefi_load_option entry;
  enum call_status status = read_entry(number, source, &size, &entry);
  if (status != CALL_OK) {
    return failure("no such boot entry", status);
  }
  uint32_t attributes = entry.attributes;
  if (active >= 0) {
    attributes = (attributes & ~UEFI_LOAD_OPTION_ACTIVE) | (active ? UEFI_LOAD_OPTION_ACTIVE : 0);
  }
  char current[128];
  uefi_load_option_label(&entry, current, sizeof(current));
  size_t total = build_entry(bytes, attributes, label ? label : current, entry.path,
      entry.path_size, file);
  if (!total || total + entry.data_size > sizeof(bytes)) {
    fputs("bootmgr: the entry does not fit in one variable\n", stderr);
    return EXIT_FAILURE;
  }
  memcpy(bytes + total, entry.data, entry.data_size);
  status = uefi_boot_variable_set(uefi, UEFI_VARIABLE_BOOT_ENTRY, number, bytes, total + entry.data_size);
  return status == CALL_OK ? EXIT_SUCCESS : failure("edit entry", status);
}

static int delete_entry(int argc, char **argv)
{
  uint32_t number;
  if (argc != 1 || !parse_hex(argv[0], &number)) {
    return usage();
  }
  enum call_status status = uefi_boot_variable_delete(uefi, UEFI_VARIABLE_BOOT_ENTRY, number);
  if (status != CALL_OK) {
    return failure("delete entry", status);
  }
  uint16_t order[ORDER_ENTRIES];
  size_t count;
  status = read_order(order, &count);
  if (status != CALL_OK) {
    fprintf(stderr, "bootmgr: Boot%04X was deleted but BootOrder was not read\n", number);
    return failure("read BootOrder", status);
  }
  size_t kept = 0;
  for (size_t i = 0; i < count; ++i) {
    if (order[i] != number) {
      order[kept++] = order[i];
    }
  }
  if (kept != count) {
    status = write_order(order, kept);
    if (status != CALL_OK) {
      fprintf(stderr, "bootmgr: Boot%04X was deleted but BootOrder was not updated\n", number);
      return failure("update BootOrder", status);
    }
  }
  return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
  uefi = bootstrap_capability("uefi");
  if (uefi == HANDLE_INVALID) {
    fputs("bootmgr: this space has no boot manager authority\n", stderr);
    return EXIT_FAILURE;
  }
  const char *command = argc > 1 ? argv[1] : "list";
  if (!strcmp(command, "list")) {
    return list();
  }
  if (!strcmp(command, "next")) {
    return set_next(argc - 2, argv + 2);
  }
  if (!strcmp(command, "order")) {
    return set_order(argc - 2, argv + 2);
  }
  if (!strcmp(command, "timeout")) {
    return set_timeout(argc - 2, argv + 2);
  }
  if (!strcmp(command, "create")) {
    return create(argc - 2, argv + 2);
  }
  if (!strcmp(command, "edit")) {
    return edit(argc - 2, argv + 2);
  }
  if (!strcmp(command, "delete")) {
    return delete_entry(argc - 2, argv + 2);
  }
  return usage();
}
