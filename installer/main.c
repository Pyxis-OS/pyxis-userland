#include "installer.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <path.h>
#include <pyxis_fs/npfs.h>
#include <random.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#define JOURNAL_DEFAULT_MIN_MIB UINT64_C(8)
#define JOURNAL_MAX_MIB UINT64_C(1024)

struct target {
  struct disk_info info;
  struct install_consent consent;
};

static bool read_line(struct terminal *terminal, const char *prompt, const char *initial,
    char *line, size_t capacity)
{
  struct term_line_result result = term_read_line_initial(terminal, prompt, initial, line, capacity);
  if (result.status != TERM_LINE_OK) {
    puts("Installation cancelled.");
    return false;
  }
  if (result.limit_reached) {
    puts("Input was too long; enter the value again.");
    return false;
  }
  return true;
}

static bool decimal(const char *text, uint64_t *value)
{
  if (!*text) {
    return false;
  }
  uint64_t result = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || result > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    result = result * 10 + (*text - '0');
  }
  *value = result;
  return true;
}

static bool choose_mode(struct terminal *terminal, bool *read_the_room)
{
  puts("1. Proceed with installation\n2. Read the room");
  char line[16];
  for (;;) {
    if (!read_line(terminal, "Choice: ", "", line, sizeof(line))) {
      return false;
    }
    if (!strcmp(line, "1") || !strcmp(line, "2")) {
      *read_the_room = line[0] == '2';
      return true;
    }
    puts("Enter 1 or 2.");
  }
}

static const char *unavailable_reason(const struct disk_info *info)
{
  if (info->preparation != DISK_READY) {
    return info->preparation == DISK_UNSUPPORTED ? "unsupported device" : "device setup failed";
  }
  if (info->flags & DISK_FLAG_MOUNTED) {
    return "a pool is mounted or retained";
  }
  if (info->flags & DISK_FLAG_CLAIMED) {
    return "another raw writer holds the disk";
  }
  if (!(info->flags & DISK_FLAG_WRITABLE)) {
    return "read-only device";
  }
  if (!(info->flags & DISK_FLAG_FLUSH_SUPPORTED)) {
    return "device cannot flush";
  }
  if (info->flags & DISK_FLAG_WRITE_FAILED) {
    return "device write failure; reboot required";
  }
  if ((info->block_size != 512 && info->block_size != 4096) ||
      info->block_count > UINT64_MAX / info->block_size) {
    return "unsupported geometry";
  }
  return NULL;
}

static bool pool_header(const struct install_layout *layout, uint64_t journal_mib,
    const uint8_t pool_id[16], struct npfs_header *header)
{
  if (!journal_mib || journal_mib > JOURNAL_MAX_MIB ||
      npfs_header_layout(layout->pool_bytes / NPFS_BLOCK_SIZE,
        journal_mib * INSTALL_MIB / NPFS_BLOCK_SIZE, pool_id, header) != NPFS_OK) {
    return false;
  }
  uint64_t reserved = 2 + header->bitmap_blocks + header->volume_blocks + header->journal_blocks;
  return header->pool_blocks >= reserved && header->pool_blocks - reserved >= 2;
}

static void destroy_targets(struct target *targets, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    install_consent_destroy(&targets[i].consent);
  }
  free(targets);
}

static bool inventory(handle_t disks, bool read_the_room, struct target **output,
    size_t *count)
{
  *output = NULL;
  *count = 0;
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    enum call_status status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      return true;
    }
    if (status != CALL_OK || *count == SIZE_MAX / sizeof(**output)) {
      fprintf(stderr, "installer: cannot enumerate disks (status %u)\n", status);
      return false;
    }
    struct target *grown = realloc(*output, (*count + 1) * sizeof(**output));
    if (!grown) {
      return false;
    }
    *output = grown;
    struct target *target = &grown[(*count)++];
    *target = (struct target){.info = info};
    const char *reason = unavailable_reason(&info);
    if (reason) {
      target->consent.reason = reason;
      continue;
    }
    struct install_disk disk = {.info = info, .bytes = info.block_count * info.block_size};
    struct install_layout layout = {0};
    struct npfs_header header;
    const uint8_t placeholder_id[16] = {1};
    if (!install_layout_plan(&disk, &layout) || !pool_header(&layout, 1, placeholder_id, &header)) {
      target->consent.reason = "too small for a 512 MiB ESP and writable pool";
      continue;
    }
    status = disks_open(disks, info.id, DISK_ACCESS_READ_ONLY, &disk.handle);
    if (status != CALL_OK) {
      target->consent.reason = "raw inspection unavailable";
      continue;
    }
    bool inspected = install_scan_consent(&disk, read_the_room, &target->consent);
    bool closed = handle_close(disk.handle) == 0;
    if (!inspected || !closed) {
      target->consent.eligible = false;
      target->consent.reason = "inspection failed; consent cannot be established";
    }
  }
}

static struct target *choose_target(struct terminal *terminal, struct target *targets,
    size_t count)
{
  size_t eligible = 0, single = 0;
  for (size_t i = 0; i < count; ++i) {
    struct target *target = &targets[i];
    char guid[37];
    install_guid_text(target->info.gpt_guid, guid);
    printf("%zu. Disk %llu: %llu MiB, GUID %s; %s: %s%s\n", i + 1,
        (unsigned long long)target->info.id,
        (unsigned long long)(target->info.block_count * target->info.block_size / INSTALL_MIB),
        npfs_id_valid(target->info.gpt_guid) ? guid : "unavailable",
        target->consent.eligible ? "eligible" : "ineligible", target->consent.reason,
        target->consent.degraded ? " (degraded metadata)" : "");
    if (target->consent.eligible) {
      ++eligible;
      single = i;
    }
  }
  if (!eligible) {
    puts("No eligible disks. Nothing was written.");
    return NULL;
  }
  if (eligible == 1) {
    return &targets[single];
  }
  char line[32];
  for (;;) {
    uint64_t number;
    if (!read_line(terminal, "Disk number: ", "", line, sizeof(line))) {
      return NULL;
    }
    if (decimal(line, &number) && number > 0 && number <= count &&
        targets[number - 1].consent.eligible) {
      return &targets[number - 1];
    }
    puts("Enter the number of an eligible disk.");
  }
}

static bool source_open(const char *path, struct install_source *source)
{
  source->handle = HANDLE_INVALID;
  handle_t directories[4];
  char component[64];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 4,
    .component = component, .component_capacity = sizeof(component),
  };
  enum call_status status = path_resolve(NULL, path, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &source->handle);
  if (status != CALL_OK || file_size(source->handle, &source->bytes) != CALL_OK || !source->bytes) {
    fprintf(stderr, "installer: cannot read source %s\n", path);
    return false;
  }
  return true;
}

static char *boot_configuration(const struct install_source *source, const uint8_t guid[16],
    size_t *bytes)
{
  if (source->bytes > SIZE_MAX - 512) {
    return NULL;
  }
  size_t size = (size_t)source->bytes;
  char *input = malloc(size + 1), *output = malloc(size + 512);
  if (!input || !output || !install_source_read(source, 0, input, size)) {
    free(input);
    free(output);
    return NULL;
  }
  input[size] = '\0';
  if (memchr(input, '\0', size)) {
    free(input);
    free(output);
    return NULL;
  }
  char disk_guid[37], normal[256];
  install_guid_text(guid, disk_guid);
  snprintf(normal, sizeof(normal),
      "  cmdline: init=app://init-idle init.primary=app://init-installed mount.disk=%s\n", disk_guid);
  bool skip = false, timeout = false, command = false, install_entry = false;
  size_t used = 0;
  char *line = input;
  while (*line) {
    char *end = strchr(line, '\n');
    size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length == strlen("/Install Pyxis") && !memcmp(line, "/Install Pyxis", length)) {
      skip = true;
      install_entry = true;
    } else if (length && line[0] == '/') {
      skip = false;
    }
    bool default_entry = length >= strlen("default_entry:") &&
      !memcmp(line, "default_entry:", strlen("default_entry:"));
    if (!skip && !default_entry) {
      const char *replacement = NULL;
      if (length == strlen("# PYXIS_BOOT_MENU_TIMEOUT") &&
          !memcmp(line, "# PYXIS_BOOT_MENU_TIMEOUT", length)) {
        if (timeout) {
          goto invalid;
        }
        timeout = true;
        replacement = "timeout: 0\n";
      } else if (length == strlen("# PYXIS_NORMAL_COMMAND_LINE") &&
          !memcmp(line, "# PYXIS_NORMAL_COMMAND_LINE", length)) {
        if (command) {
          goto invalid;
        }
        command = true;
        replacement = normal;
      }
      size_t copy = replacement ? strlen(replacement) : length;
      if (copy + 1 > size + 512 - used) {
        goto invalid;
      }
      memcpy(output + used, replacement ? replacement : line, copy);
      used += copy;
      if (!replacement) {
        output[used++] = '\n';
      }
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  if (!timeout || !command || !install_entry) {
    goto invalid;
  }
  output[used] = '\0';
  *bytes = used;
  free(input);
  return output;

invalid:
  free(input);
  free(output);
  return NULL;
}

static bool new_ids(handle_t clock, handle_t random, struct install_layout *layout,
    uint8_t pool_id[16], uint8_t volume_id[16])
{
  uint64_t now;
  uint8_t bytes[80];
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - RANDOM_MAX_WAIT_NS ||
      random_read(random, bytes, sizeof(bytes), now + RANDOM_MAX_WAIT_NS) != CALL_OK) {
    fputs("installer: fresh random identities unavailable\n", stderr);
    return false;
  }
  for (unsigned i = 0; i < 5; ++i) {
    bytes[i * 16 + 7] = (bytes[i * 16 + 7] & 0x0f) | 0x40;
    bytes[i * 16 + 8] = (bytes[i * 16 + 8] & 0x3f) | 0x80;
  }
  memcpy(layout->disk_guid, bytes, 16);
  memcpy(layout->esp_guid, bytes + 16, 16);
  memcpy(layout->pool_guid, bytes + 32, 16);
  memcpy(pool_id, bytes + 48, 16);
  memcpy(volume_id, bytes + 64, 16);
  return true;
}

static bool choose_journal(struct terminal *terminal, const struct install_layout *layout,
    const uint8_t pool_id[16], struct npfs_header *header)
{
  uint64_t unit = 128 * INSTALL_MIB;
  uint64_t standard = layout->pool_bytes / unit + (layout->pool_bytes % unit != 0);
  if (standard < JOURNAL_DEFAULT_MIN_MIB) {
    standard = JOURNAL_DEFAULT_MIN_MIB;
  }
  if (standard > JOURNAL_MAX_MIB) {
    standard = JOURNAL_MAX_MIB;
  }
  char initial[32], line[32];
  snprintf(initial, sizeof(initial), "%llu", (unsigned long long)standard);
  for (;;) {
    uint64_t journal;
    if (!read_line(terminal, "Journal size (MiB): ", initial, line, sizeof(line))) {
      return false;
    }
    if (decimal(line, &journal) && pool_header(layout, journal, pool_id, header)) {
      return true;
    }
    puts("Enter a size from 1 to 1024 MiB that leaves room for pool metadata and files.");
  }
}

static bool marker_verify(const struct install_disk *disk)
{
  handle_t root = HANDLE_INVALID, marker = HANDLE_INVALID;
  bool ok = disk_open_volume(disk->handle, 2, "system",
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &root) == CALL_OK &&
      directory_lookup(root, "SAFE_TO_WIPE", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &marker) == CALL_OK;
  uint64_t size;
  if (ok) {
    ok = file_size(marker, &size) == CALL_OK && size == 0;
  }
  if (marker != HANDLE_INVALID) {
    ok = handle_close(marker) == 0 && ok;
  }
  if (root != HANDLE_INVALID) {
    ok = handle_close(root) == 0 && ok;
  }
  return ok;
}

int main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1) {
    fputs("installer takes no arguments\n", stderr);
    return EXIT_FAILURE;
  }
  struct terminal terminal = {
    .input = startup_resource("input"), .output = startup_resource("output"),
  };
  handle_t disks = startup_resource("disks"), app = startup_root("app");
  handle_t clock = startup_resource("clock"), random = startup_resource("random");
  struct install_source kernel = {.handle = startup_resource("boot_kernel")};
  struct install_source archive = {.handle = startup_resource("boot_archive")};
  struct install_source efi = {.handle = HANDLE_INVALID}, template = {.handle = HANDLE_INVALID};
  struct install_disk disk = {.handle = HANDLE_INVALID};
  struct target *targets = NULL;
  size_t count = 0;
  char *configuration = NULL;
  struct install_esp *esp = NULL;
  bool claimed = false, attempted_write = false;
  int result = EXIT_FAILURE;
  if (disks == HANDLE_INVALID || app == HANDLE_INVALID || clock == HANDLE_INVALID ||
      random == HANDLE_INVALID || kernel.handle == HANDLE_INVALID || archive.handle == HANDLE_INVALID ||
      terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID) {
    fputs("installer: missing install-mode authority\n", stderr);
    goto done;
  }
  bool read_the_room;
  if (!choose_mode(&terminal, &read_the_room)) {
    goto done;
  }
  if (!source_open("app://share/installer/BOOTX64.EFI", &efi) ||
      !source_open("app://share/installer/limine.conf.template", &template) ||
      file_size(kernel.handle, &kernel.bytes) != CALL_OK || !kernel.bytes ||
      file_size(archive.handle, &archive.bytes) != CALL_OK || !archive.bytes ||
      !inventory(disks, read_the_room, &targets, &count)) {
    fputs("installer: sources or disk inventory unavailable; nothing was written\n", stderr);
    goto done;
  }
  struct target *selected = choose_target(&terminal, targets, count);
  if (!selected) {
    goto done;
  }
  disk.info = selected->info;
  disk.bytes = disk.info.block_count * disk.info.block_size;
  struct install_layout layout = {0};
  uint8_t pool_id[16], volume_id[16];
  struct npfs_header header;
  if (!install_layout_plan(&disk, &layout) || !new_ids(clock, random, &layout, pool_id, volume_id)) {
    goto done;
  }
  char guid[37];
  install_guid_text(disk.info.gpt_guid, guid);
  printf("Disk %llu: %llu MiB, GUID %s\n", (unsigned long long)disk.info.id,
      (unsigned long long)(disk.bytes / INSTALL_MIB),
      npfs_id_valid(disk.info.gpt_guid) ? guid : "unavailable");
  puts("The entire disk will be rebuilt. Volumes to destroy:");
  puts(selected->consent.volume_labels ? selected->consent.volume_labels : "  none readable");
  if (!choose_journal(&terminal, &layout, pool_id, &header)) {
    goto done;
  }
  size_t configuration_bytes;
  configuration = boot_configuration(&template, layout.disk_guid, &configuration_bytes);
  if (!configuration) {
    fputs("installer: boot template cannot be filled\n", stderr);
    goto done;
  }
  esp = install_esp_plan(&disk, &layout, &efi, &kernel, &archive,
      configuration, configuration_bytes, npfs_get_u32(layout.esp_guid));
  if (!esp) {
    fputs("installer: boot sources do not fit the fresh ESP\n", stderr);
    goto done;
  }
  printf("New layout: 512 MiB ESP, %llu MiB pool, %llu MiB journal; system volume.\n",
      (unsigned long long)(layout.pool_bytes / INSTALL_MIB),
      (unsigned long long)(header.journal_blocks * NPFS_BLOCK_SIZE / INSTALL_MIB));
  char confirmation[16];
  if (!read_line(&terminal, "Type wipe to continue: ", "", confirmation, sizeof(confirmation)) ||
      strcmp(confirmation, "wipe")) {
    puts("Nothing was written.");
    goto done;
  }
  enum call_status status = disks_open(disks, disk.info.id, DISK_ACCESS_READ_WRITE, &disk.handle);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: cannot claim disk (status %u); nothing was written\n", status);
    goto done;
  }
  claimed = true;
  struct install_consent current = {0};
  bool consent_ok = install_scan_consent(&disk, read_the_room, &current) && current.eligible &&
      current.pools == selected->consent.pools && current.volumes == selected->consent.volumes &&
      !strcmp(current.reason, selected->consent.reason) &&
      !strcmp(current.volume_labels ? current.volume_labels : "",
        selected->consent.volume_labels ? selected->consent.volume_labels : "");
  install_consent_destroy(&current);
  if (!consent_ok) {
    fputs("installer: consent changed or cannot be revalidated; nothing was written\n", stderr);
    goto done;
  }
  struct clock_wall_reading wall;
  bool created_valid = clock_wall_now(clock, &wall) == CALL_OK;
  int64_t created_ns = created_valid ? npfs_timestamp(wall.seconds, wall.nanoseconds) : 0;
  puts("Writing EFI boot files...");
  attempted_write = true;
  if (!install_esp_write(esp)) {
    goto done;
  }
  puts("Formatting system pool...");
  if (!install_pool_format(&disk, &layout, &header, volume_id, created_ns, created_valid)) {
    goto done;
  }
  puts("Writing GPT...");
  if (!install_gpt_write(&disk, &layout)) {
    goto done;
  }
  status = disk_flush(disk.handle);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: final sync failed (status %u)\n", status);
    goto done;
  }
  status = disk_release(disk.handle);
  claimed = false;
  if (status != CALL_OK) {
    fprintf(stderr, "installer: raw release/GPT rescan failed (status %u)\n", status);
    goto done;
  }
  struct disk_info verified;
  if (disk_get_info(disk.handle, &verified) != CALL_OK || verified.gpt_status != DISK_GPT_HEALTHY ||
      memcmp(verified.gpt_guid, layout.disk_guid, 16)) {
    fputs("installer: new GPT read-back failed\n", stderr);
    goto done;
  }
  puts("Checking EFI files and reopening system read-only...");
  if (!install_esp_verify(esp) || !marker_verify(&disk)) {
    fputs("installer: read-back check failed\n", stderr);
    goto done;
  }
  puts("installed\nBoot from this disk after removing the install medium.\n"
      "Deleting system://SAFE_TO_WIPE marks this installation as final.");
  result = EXIT_SUCCESS;

done:
  if (claimed) {
    enum call_status status = disk_release(disk.handle);
    if (status != CALL_OK) {
      fprintf(stderr, "installer: cleanup release failed (status %u)\n", status);
    }
  }
  if (attempted_write && result != EXIT_SUCCESS) {
    fputs("Installation failed; the disk may be partially rebuilt.\n"
        "Boot the live image again, then run the installer and choose Read the room to reinstall this disk.\n",
        stderr);
  }
  install_esp_destroy(esp);
  free(configuration);
  destroy_targets(targets, count);
  if (disk.handle != HANDLE_INVALID) {
    handle_close(disk.handle);
  }
  if (efi.handle != HANDLE_INVALID) {
    handle_close(efi.handle);
  }
  if (template.handle != HANDLE_INVALID) {
    handle_close(template.handle);
  }
  return result;
}
