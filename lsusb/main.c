#include <errno.h>
#include <startup.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_info.h>

#define DEFAULT_DATABASE "app://share/hwdata/usb.ids"
#define DATABASE_LINE_CAPACITY 1024
#define ID_DIGITS 4

struct device_entry {
  struct system_info_usb_device device;
  /* Owned descriptive labels; absent IDs retain numeric identification. */
  char *vendor_name;
  char *product_name;
};

struct snapshot {
  struct system_info_usb inventory;
  struct system_info_usb_controller *controllers;
  struct device_entry *devices;
  struct system_info_usb_interface *interfaces;
};

static int usage(void)
{
  fputs("usage: lsusb [-n] [-i FILE]\n", stderr);
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

static bool parse_id(const char *text, unsigned *id, const char **name)
{
  unsigned value = 0;
  for (unsigned i = 0; i < ID_DIGITS; ++i) {
    int digit = hex_value(text[i]);
    if (digit < 0) {
      return false;
    }
    value = value << 4 | (unsigned)digit;
  }
  if (text[ID_DIGITS] != ' ' || text[ID_DIGITS + 1] != ' ' || !text[ID_DIGITS + 2]) {
    return false;
  }
  *id = value;
  *name = text + ID_DIGITS + 2;
  return true;
}

/* Consume whole records with bounded storage. Oversized records and embedded
 * NULs give no label; a truncated prefix is never used as a database entry. */
static int read_line(FILE *database, char *line, size_t capacity)
{
  size_t length = 0;
  bool invalid = false, present = false;
  int character;
  while ((character = fgetc(database)) != EOF) {
    present = true;
    if (character == '\n') {
      break;
    }
    if (!character || length == capacity - 1) {
      invalid = true;
    } else {
      line[length++] = (char)character;
    }
  }
  line[length] = '\0';
  if (ferror(database)) {
    return -1;
  }
  if (!present) {
    return 0;
  }
  return invalid ? 2 : 1;
}

static bool save_name(char **slot, const char *name)
{
  if (*slot) {
    return true;
  }
  *slot = strdup(name);
  return *slot != NULL;
}

static void free_names(struct snapshot *snapshot)
{
  for (size_t i = 0; i < snapshot->inventory.device_count; ++i) {
    free(snapshot->devices[i].vendor_name);
    free(snapshot->devices[i].product_name);
    snapshot->devices[i].vendor_name = NULL;
    snapshot->devices[i].product_name = NULL;
  }
}

/* Only top-level four-digit vendors and one-tab products participate. The
 * class and other top-level sections clear vendor context; two-tab interface
 * records never supply product names. First labels win. */
static int load_names(const char *path, struct snapshot *snapshot)
{
  FILE *database = fopen(path, "r");
  if (!database) {
    return errno ? errno : EIO;
  }
  char line[DATABASE_LINE_CAPACITY];
  bool vendor_section = false;
  unsigned vendor = 0;
  int error = 0, record;
  errno = 0;
  while ((record = read_line(database, line, sizeof(line))) > 0) {
    if (record == 2) {
      vendor_section = false;
      continue;
    }
    if (!line[0] || line[0] == '#' || (line[0] == '\t' && line[1] == '\t')) {
      continue;
    }
    unsigned id;
    const char *name;
    bool product = line[0] == '\t';
    if (product) {
      if (!vendor_section || !parse_id(line + 1, &id, &name)) {
        continue;
      }
    } else {
      vendor_section = parse_id(line, &id, &name);
      if (!vendor_section) {
        continue;
      }
      vendor = id;
    }
    for (size_t i = 0; i < snapshot->inventory.device_count; ++i) {
      struct device_entry *entry = &snapshot->devices[i];
      if (!(entry->device.flags & SYSTEM_INFO_USB_DEVICE_IDENTIFIED) ||
          entry->device.vendor_id != vendor ||
          (product && entry->device.product_id != id)) {
        continue;
      }
      if (!save_name(product ? &entry->product_name : &entry->vendor_name, name)) {
        error = ENOMEM;
        break;
      }
    }
    if (error) {
      break;
    }
  }
  if (!error && record < 0) {
    error = errno ? errno : EIO;
  }
  if (fclose(database) != 0 && !error) {
    error = errno ? errno : EIO;
  }
  return error;
}

static void free_snapshot(struct snapshot *snapshot)
{
  if (snapshot->devices) {
    free_names(snapshot);
  }
  free(snapshot->controllers);
  free(snapshot->devices);
  free(snapshot->interfaces);
}

static bool load_snapshot(handle_t system_info, struct snapshot *snapshot)
{
  const struct system_info_usb *inventory = &snapshot->inventory;
  if (inventory->controller_count > SIZE_MAX / sizeof(*snapshot->controllers) ||
      inventory->device_count > SIZE_MAX / sizeof(*snapshot->devices) ||
      inventory->interface_count > SIZE_MAX / sizeof(*snapshot->interfaces)) {
    fputs("lsusb: USB inventory too large\n", stderr);
    return false;
  }
  snapshot->controllers = calloc(inventory->controller_count ? inventory->controller_count : 1,
      sizeof(*snapshot->controllers));
  snapshot->devices = calloc(inventory->device_count ? inventory->device_count : 1,
      sizeof(*snapshot->devices));
  snapshot->interfaces = calloc(inventory->interface_count ? inventory->interface_count : 1,
      sizeof(*snapshot->interfaces));
  if (!snapshot->controllers || !snapshot->devices || !snapshot->interfaces) {
    fputs("lsusb: cannot allocate inventory\n", stderr);
    return false;
  }
  enum call_status status;
  for (size_t i = 0; i < inventory->controller_count; ++i) {
    status = system_info_get_usb_controller(system_info, i, &snapshot->controllers[i]);
    if (status != CALL_OK) {
      fprintf(stderr, "lsusb: USB controller %zu query failed (status %u)\n", i, status);
      return false;
    }
  }
  for (size_t i = 0; i < inventory->device_count; ++i) {
    struct system_info_usb_device *device = &snapshot->devices[i].device;
    status = system_info_get_usb_device(system_info, i, device);
    if (status != CALL_OK) {
      fprintf(stderr, "lsusb: USB device %zu query failed (status %u)\n", i, status);
      return false;
    }
    if (device->controller_index >= inventory->controller_count ||
        device->interface_first > inventory->interface_count ||
        device->interface_count > inventory->interface_count - device->interface_first ||
        device->root_port > snapshot->controllers[device->controller_index].root_port_count) {
      fputs("lsusb: invalid USB device association\n", stderr);
      return false;
    }
  }
  for (size_t i = 0; i < inventory->interface_count; ++i) {
    struct system_info_usb_interface *interface = &snapshot->interfaces[i];
    status = system_info_get_usb_interface(system_info, i, interface);
    if (status != CALL_OK) {
      fprintf(stderr, "lsusb: USB interface %zu query failed (status %u)\n", i, status);
      return false;
    }
    if (interface->device_index >= inventory->device_count) {
      fputs("lsusb: invalid USB interface association\n", stderr);
      return false;
    }
    const struct system_info_usb_device *device =
        &snapshot->devices[interface->device_index].device;
    if (i < device->interface_first || i - device->interface_first >= device->interface_count) {
      fputs("lsusb: invalid USB interface range\n", stderr);
      return false;
    }
  }
  /* Validate each device range too, so overlapping ranges cannot print another
   * device's otherwise valid interface record. */
  for (size_t i = 0; i < inventory->device_count; ++i) {
    const struct system_info_usb_device *device = &snapshot->devices[i].device;
    for (size_t j = 0; j < device->interface_count; ++j) {
      if (snapshot->interfaces[device->interface_first + j].device_index != i) {
        fputs("lsusb: invalid USB interface ownership\n", stderr);
        return false;
      }
    }
  }
  return true;
}

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

static const char *controller_state(uint32_t state)
{
  switch (state) {
    case SYSTEM_INFO_USB_CONTROLLER_UNSUPPORTED: return "unsupported";
    case SYSTEM_INFO_USB_CONTROLLER_FAILED: return "failed";
    case SYSTEM_INFO_USB_CONTROLLER_INCOMPLETE: return "incomplete";
    case SYSTEM_INFO_USB_CONTROLLER_COMPLETE: return "complete";
  }
  return "unknown";
}

static const char *device_speed(uint8_t speed)
{
  switch (speed) {
    case SYSTEM_INFO_USB_SPEED_UNKNOWN: return "unknown";
    case SYSTEM_INFO_USB_SPEED_LOW: return "low";
    case SYSTEM_INFO_USB_SPEED_FULL: return "full";
    case SYSTEM_INFO_USB_SPEED_HIGH: return "high";
    case SYSTEM_INFO_USB_SPEED_SUPER: return "super";
  }
  return "unknown";
}

static void print_controller(const struct system_info_usb_controller *controller)
{
  const struct system_info_pci_function *pci = &controller->pci;
  printf("Controller %04x:%02x:%02x.%x [%04x:%04x]: %s, root ports ",
      pci->segment, pci->bus, pci->device, pci->function, pci->vendor_id, pci->device_id,
      controller_state(controller->state));
  if (controller->root_port_count) {
    printf("%u\n", controller->root_port_count);
  } else {
    fputs("unknown\n", stdout);
  }
}

static void print_device(const struct device_entry *entry, bool numeric)
{
  const struct system_info_usb_device *device = &entry->device;
  printf("  Port %u speed %s: ", device->root_port, device_speed(device->speed));
  if (!(device->flags & SYSTEM_INFO_USB_DEVICE_IDENTIFIED)) {
    fputs("unidentified", stdout);
  } else if (numeric) {
    printf("%04x:%04x", device->vendor_id, device->product_id);
  } else {
    if (entry->vendor_name) {
      put_label(entry->vendor_name);
      putchar(' ');
    }
    put_label(entry->product_name ? entry->product_name : "Device");
    printf(" [%04x:%04x]", device->vendor_id, device->product_id);
  }
  if (device->flags & SYSTEM_INFO_USB_DEVICE_HUB) {
    fputs(" (hub; descendants unavailable)", stdout);
  }
  if (device->flags & SYSTEM_INFO_USB_DEVICE_INCOMPLETE) {
    fputs(" (incomplete)", stdout);
  }
  putchar('\n');
}

static void print_interface(const struct system_info_usb_interface *interface)
{
  printf("    Config %u interface %u alt %u class %02x subclass %02x protocol %02x endpoints %u\n",
      interface->configuration, interface->number, interface->alternate, interface->class,
      interface->subclass, interface->protocol, interface->endpoint_count);
}

static int write_error(void)
{
  return ferror(stdout) ? (errno ? errno : EIO) : 0;
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
    fputs("lsusb: system information authority unavailable\n", stderr);
    return EXIT_FAILURE;
  }
  struct snapshot snapshot = {0};
  enum call_status status = system_info_get_usb(system_info, &snapshot.inventory);
  if (status != CALL_OK) {
    fprintf(stderr, "lsusb: USB inventory query failed (status %u)\n", status);
    return EXIT_FAILURE;
  }
  if (snapshot.inventory.state == SYSTEM_INFO_USB_UNAVAILABLE) {
    fputs("lsusb: USB inventory unavailable\n", stderr);
    return EXIT_FAILURE;
  }
  if (snapshot.inventory.state == SYSTEM_INFO_USB_INITIALIZING) {
    fputs("lsusb: USB inventory initializing\n", stderr);
    return EXIT_FAILURE;
  }
  if (!load_snapshot(system_info, &snapshot)) {
    free_snapshot(&snapshot);
    return EXIT_FAILURE;
  }
  if (!numeric) {
    int error = load_names(database, &snapshot);
    if (error) {
      fprintf(stderr, "lsusb: %s: %s; showing numeric IDs\n", database, strerror(error));
      free_names(&snapshot);
      numeric = true;
    }
  }
  int result = EXIT_SUCCESS, error = 0;
  errno = 0;
  for (size_t i = 0; i < snapshot.inventory.controller_count && !error; ++i) {
    print_controller(&snapshot.controllers[i]);
    error = write_error();
    for (size_t j = 0; j < snapshot.inventory.device_count && !error; ++j) {
      const struct device_entry *entry = &snapshot.devices[j];
      if (entry->device.controller_index != i) {
        continue;
      }
      print_device(entry, numeric);
      error = write_error();
      for (size_t k = 0; k < entry->device.interface_count && !error; ++k) {
        print_interface(&snapshot.interfaces[entry->device.interface_first + k]);
        error = write_error();
      }
    }
  }
  if (snapshot.inventory.state == SYSTEM_INFO_USB_INCOMPLETE) {
    fputs("lsusb: USB inventory incomplete; listed observations are valid boot records\n", stderr);
    result = EXIT_FAILURE;
  }
  free_snapshot(&snapshot);
  if (fclose(stdout) != 0 && !error) {
    error = errno ? errno : EIO;
  }
  if (error) {
    fprintf(stderr, "lsusb: stdout: %s\n", strerror(error));
    result = EXIT_FAILURE;
  }
  return result;
}
