#include <abi/memory.h>
#include <disk.h>
#include <handle.h>
#include <pyxis_fs/fat32_write.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#define WORKSPACE_MAX_BYTES (32u * 1024u * 1024u)
#define STEP_LIMIT UINT64_C(67108864)
#define SECTOR_LIMIT UINT64_C(16777216)
#define DIRECTORY_DEPTH_LIMIT 64u
#define COMMAND_BYTES 4096u
#define FILE_MAX_BYTES 4096u
#define RECEIPT_COUNT 32u
#define RECEIPT_CHAIN_CLUSTERS (FILE_MAX_BYTES / FAT32_SECTOR_BYTES)
#define WRITER_DISK_RIGHTS (DISK_RIGHT_INFO | DISK_RIGHT_READ | DISK_RIGHT_WRITE | DISK_RIGHT_RELEASE)

struct disk_source {
  handle_t disk;
  uint64_t first, sectors;
  enum call_status last_error;
};

static bool read_sectors(void *context, uint64_t first, uint32_t count, void *bytes)
{
  struct disk_source *source = context;
  if (!count || count > DISK_IO_MAX_BYTES / FAT32_SECTOR_BYTES || first < source->first ||
      first - source->first >= source->sectors ||
      count > source->sectors - (first - source->first) ||
      first > UINT64_MAX / FAT32_SECTOR_BYTES) {
    source->last_error = CALL_BAD_REQUEST;
    return false;
  }
  source->last_error = disk_read(source->disk, first * FAT32_SECTOR_BYTES,
      bytes, (size_t)count * FAT32_SECTOR_BYTES);
  return source->last_error == CALL_OK;
}

static bool write_sectors(void *context, uint64_t first, uint32_t count, const void *bytes)
{
  struct disk_source *source = context;
  if (!count || count > DISK_IO_MAX_BYTES / FAT32_SECTOR_BYTES || first < source->first ||
      first - source->first >= source->sectors ||
      count > source->sectors - (first - source->first) ||
      first > UINT64_MAX / FAT32_SECTOR_BYTES) {
    source->last_error = CALL_BAD_REQUEST;
    return false;
  }
  source->last_error = disk_write(source->disk, first * FAT32_SECTOR_BYTES,
      bytes, (size_t)count * FAT32_SECTOR_BYTES);
  return source->last_error == CALL_OK;
}

static bool flush_disk(void *context)
{
  struct disk_source *source = context;
  source->last_error = disk_flush(source->disk);
  return source->last_error == CALL_OK;
}

static void report(enum fat32_status status, const struct disk_source *source)
{
  fprintf(stderr, "fat32-write: %s", fat32_status_string(status));
  if (status == FAT32_IO) {
    fprintf(stderr, " (disk status %u)", source->last_error);
  }
  fputc('\n', stderr);
}

static bool decimal_entry(const char *text, uint32_t *entry)
{
  uint32_t number = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || number > (UINT32_MAX - (*text - '0')) / 10) {
      return false;
    }
    number = number * 10 + (*text - '0');
  }
  *entry = number;
  return number != 0;
}

static void info(const struct fat32_volume *volume)
{
  printf("GPT entry %u: first sector %llu, partition sectors %llu\n"
      "FAT32: sectors %u, sectors/cluster %u, clusters %u, FATs %u, FAT sectors %u\n"
      "Root cluster %u, clean shutdown %s, no hard error %s\n",
      volume->gpt_entry, (unsigned long long)volume->partition_first,
      (unsigned long long)volume->partition_sectors, volume->volume_sectors,
      volume->sectors_per_cluster, volume->cluster_count, volume->fat_count,
      volume->fat_sectors, volume->root_cluster,
      volume->clean_shutdown ? "yes" : "no", volume->no_hard_error ? "yes" : "no");
}

static bool print_name(const char *name, size_t bytes)
{
  for (size_t i = 0; i < bytes; ++i) {
    unsigned char byte = (unsigned char)name[i];
    if (byte == 0xc2 && i + 1 < bytes && (unsigned char)name[i + 1] >= 0x80 &&
        (unsigned char)name[i + 1] <= 0x9f) {
      if (fprintf(stdout, "\\u%04x", (unsigned char)name[++i]) < 0) {
        return false;
      }
    } else if (byte >= 0x20 && byte != 0x7f && byte != '\\') {
      if (fputc(byte, stdout) == EOF) {
        return false;
      }
    } else if (fprintf(stdout, "\\x%02x", byte) < 0) {
      return false;
    }
  }
  return true;
}

static bool list_entry(void *context, const struct fat32_entry *entry)
{
  (void)context;
  if (printf("%c %10u ", entry->attributes & FAT32_ATTR_DIRECTORY ? 'd' : 'f',
      entry->file_bytes) < 0 || !print_name(entry->name, entry->name_bytes)) {
    return false;
  }
  if (entry->has_long_name) {
    if (entry->alias_ascii) {
      if (fputs(" [alias: ", stdout) == EOF || !print_name(entry->alias, entry->alias_bytes) ||
          fputc(']', stdout) == EOF) {
        return false;
      }
    } else if (fputs(" [non-ASCII short alias]", stdout) == EOF) {
      return false;
    }
  }
  return fputc('\n', stdout) != EOF;
}

static bool read_bytes(void *context, const void *bytes, size_t length)
{
  (void)context;
  return fwrite(bytes, 1, length, stdout) == length;
}

static bool allocate_workspace(struct fat32_workspace *workspace, size_t bytes)
{
  if (!bytes || bytes > WORKSPACE_MAX_BYTES) {
    fputs("fat32-write: LIMIT (workspace exceeds the 32 MiB policy)\n", stderr);
    return false;
  }
  free(workspace->memory);
  workspace->memory = NULL;
  workspace->bytes = 0;
  workspace->memory = malloc(bytes);
  if (!workspace->memory) {
    fputs("fat32-write: NO_MEMORY\n", stderr);
    return false;
  }
  workspace->bytes = bytes;
  return true;
}

static bool payload_bytes(void *context, uint32_t offset, void *bytes, size_t length)
{
  const char *text = context;
  size_t available = strlen(text);
  if (offset > available || length > available - offset) {
    return false;
  }
  memcpy(bytes, text + offset, length);
  return true;
}

static bool ascii_payload(const char *text)
{
  for (; *text; ++text) {
    if ((unsigned char)*text < 0x20 || (unsigned char)*text > 0x7e) {
      return false;
    }
  }
  return true;
}

static void show_receipts(const struct fat32_writer *writer)
{
  for (uint32_t i = 0; i < writer->receipt_count; ++i) {
    const struct fat32_receipt *receipt = &writer->receipts[i];
    if (receipt->valid) {
      printf("receipt %u: %s, %s, %u bytes, %u clusters\n", i + 1,
          receipt->path, receipt->directory ? "directory" : "file",
          receipt->file_bytes, receipt->chain_count);
    }
  }
}

static const char *phase_name(uint8_t phase)
{
  switch (phase) {
  case FAT32_WRITE_PREFLIGHT: return "preflight";
  case FAT32_WRITE_DATA: return "data";
  case FAT32_WRITE_FAT: return "FAT";
  case FAT32_WRITE_DIRECTORY: return "directory";
  case FAT32_WRITE_RECLAIM: return "reclaim";
  case FAT32_WRITE_FSINFO: return "FSInfo";
  }
  return "unknown";
}

static void print_chain(const char *label, const uint32_t *chain, uint32_t count)
{
  fprintf(stderr, "%s:", label);
  for (uint32_t i = 0; i < count; ++i) {
    fprintf(stderr, " %u", chain[i]);
  }
  fputc('\n', stderr);
}

static void show_recovery(const struct fat32_writer *writer)
{
  const struct fat32_recovery *recovery = &writer->recovery;
  fprintf(stderr, "fat32-write: writes stopped (%s); target %s, phase %s, failed sector %llu, "
      "old first cluster %u, staged first cluster %u, staged clusters %u\n",
      fat32_status_string(recovery->failure), recovery->target, phase_name(recovery->phase),
      (unsigned long long)recovery->failed_sector,
      recovery->old_first, recovery->staged_first, recovery->staged_clusters);
  print_chain("Staged chain", recovery->staged_chain, recovery->staged_clusters);
  print_chain("Allocated clusters", recovery->allocated_chain, recovery->allocated_clusters);
  for (uint32_t i = 0; i < writer->receipt_count; ++i) {
    const struct fat32_receipt *receipt = &writer->receipts[i];
    if (receipt->valid && strcmp(receipt->path, recovery->target) == 0) {
      print_chain("Receipt chain", receipt->chain, receipt->chain_count);
    }
  }
  fprintf(stderr, "Retained prepared sector images: %zu\n", recovery->prepared_sector_count);
  fputs("No rollback, replay or cleanup was attempted. Preserve this media for external repair.\n",
      stderr);
}

static void help(void)
{
  puts("Commands: info, check, list /PATH, read /PATH, receipts, mkdir /PATH,");
  puts("  create /PATH = ASCII TEXT, append RECEIPT = ASCII TEXT,");
  puts("  replace RECEIPT = ASCII TEXT, extend RECEIPT BYTES, quit.");
  puts("Receipt numbers authorize only objects created in this session; existing files are never adopted.");
}

static enum fat32_status mutate(struct fat32_writer *writer, char *line)
{
  struct fat32_receipt *receipt = NULL;
  enum fat32_status status;
  if (strncmp(line, "mkdir ", 6) == 0) {
    status = fat32_create(writer, line + 6, strlen(line + 6), true, NULL, NULL, 0, &receipt);
  } else if (strncmp(line, "create ", 7) == 0 || strncmp(line, "append ", 7) == 0 ||
      strncmp(line, "replace ", 8) == 0) {
    bool create = line[0] == 'c', append = line[0] == 'a';
    char *target = line + (create || append ? 7 : 8);
    char *separator = strstr(target, " = ");
    if (!separator) {
      return FAT32_INVALID;
    }
    *separator = '\0';
    char *payload = separator + 3;
    if (!ascii_payload(payload)) {
      return FAT32_UNSUPPORTED;
    }
    if (create) {
      status = fat32_create(writer, target, strlen(target), false, payload_bytes,
          payload, strlen(payload), &receipt);
    } else {
      uint32_t number;
      if (!decimal_entry(target, &number) || number > writer->receipt_count) {
        return FAT32_INVALID;
      }
      receipt = &writer->receipts[number - 1];
      status = append ? fat32_append(writer, receipt, payload_bytes, payload, strlen(payload)) :
          fat32_replace(writer, receipt, payload_bytes, payload, strlen(payload));
    }
  } else if (strncmp(line, "extend ", 7) == 0) {
    char *target = line + 7, *separator = strchr(target, ' ');
    uint32_t number, bytes;
    if (!separator) {
      return FAT32_INVALID;
    }
    *separator = '\0';
    if (!decimal_entry(target, &number) || number > writer->receipt_count ||
        !decimal_entry(separator + 1, &bytes)) {
      return FAT32_INVALID;
    }
    receipt = &writer->receipts[number - 1];
    status = fat32_extend(writer, receipt, bytes);
  } else {
    help();
    return FAT32_INVALID;
  }
  if (status == FAT32_OK && receipt) {
    printf("durable OK; receipt %u: %s, %u bytes\n",
        (unsigned)(receipt - writer->receipts) + 1, receipt->path, receipt->file_bytes);
  }
  return status;
}

int main(int argc, char **argv)
{
  handle_t disk = startup_resource("disk");
  struct startup_stream input = startup_stream(STARTUP_STDIN);
  struct startup_stream output = startup_stream(STARTUP_STDOUT);
  struct startup_stream error = startup_stream(STARTUP_STDERR);
  struct terminal terminal = {.input = input.handle, .output = output.handle};
  struct fat32_workspace workspace = {
    .step_limit = STEP_LIMIT, .sector_limit = SECTOR_LIMIT,
    .directory_depth_limit = DIRECTORY_DEPTH_LIMIT,
  };
  struct disk_source backing = {.disk = disk};
  int result = EXIT_FAILURE;
  bool release_disk = false;
  struct fat32_writer writer = {0};
  struct fat32_receipt *receipts = NULL;
  uint32_t *chains = NULL;
  uint32_t entry;
  uint64_t rights, transport;
  if (argc != 2 || !decimal_entry(argv[1], &entry) || disk == HANDLE_INVALID ||
      startup_resource_count() != 2 || startup_resource("memory") == HANDLE_INVALID ||
      startup_root_count() || startup_namespace() != HANDLE_INVALID ||
      startup_working_directory_count() || input.protocol != PROTOCOL_CONSOLE ||
      output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("fat32-write: requires a selected disk, GPT entry and explicit console profile\n", stderr);
    goto done;
  }
  enum call_status native = handle_rights(disk, &rights, &transport);
  if (native != CALL_OK || rights != WRITER_DISK_RIGHTS || transport) {
    fputs("fat32-write: disk must carry exactly INFO|READ|WRITE|RELEASE authority\n", stderr);
    goto done;
  }
  release_disk = true;
  struct disk_info disk_info;
  native = disk_get_info(disk, &disk_info);
  if (native != CALL_OK) {
    fprintf(stderr, "fat32-write: disk info failed (status %u)\n", native);
    goto done;
  }
  if (disk_info.preparation != DISK_READY || disk_info.block_size != FAT32_SECTOR_BYTES ||
      !disk_info.block_count || disk_info.block_count > UINT64_MAX / FAT32_SECTOR_BYTES) {
    fputs("fat32-write: UNSUPPORTED disk geometry; requires ready 512-byte blocks\n", stderr);
    goto done;
  }
  uint64_t required_flags = DISK_FLAG_WRITABLE | DISK_FLAG_FLUSH_SUPPORTED | DISK_FLAG_CLAIMED;
  if ((disk_info.flags & required_flags) != required_flags ||
      (disk_info.flags & DISK_FLAG_WRITE_FAILED)) {
    fputs("fat32-write: requires a live writable claim and real flush support without device failure\n", stderr);
    goto done;
  }
  backing.sectors = disk_info.block_count;
  struct fat32_source source = {
    .read = read_sectors, .context = &backing, .sectors = disk_info.block_count,
    .sector_bytes = FAT32_SECTOR_BYTES,
    .max_read_sectors = DISK_IO_MAX_BYTES / FAT32_SECTOR_BYTES,
  };
  if (!allocate_workspace(&workspace, fat32_open_workspace_size())) {
    goto done;
  }
  struct fat32_volume volume;
  enum fat32_status status = fat32_open(&source, entry, &workspace, &volume);
  if (status != FAT32_OK) {
    report(status, &backing);
    goto done;
  }
  /* Admission needs GPT sectors; every later backing read stays in this extent. */
  backing.first = volume.partition_first;
  backing.sectors = volume.partition_sectors;
  size_t bytes;
  status = fat32_write_workspace_size(&volume, DIRECTORY_DEPTH_LIMIT, FILE_MAX_BYTES, &bytes);
  if (status != FAT32_OK) {
    report(status, &backing);
    goto done;
  }
  if (!allocate_workspace(&workspace, bytes)) {
    goto done;
  }
  receipts = calloc(RECEIPT_COUNT, sizeof(*receipts));
  chains = calloc(RECEIPT_COUNT * RECEIPT_CHAIN_CLUSTERS, sizeof(*chains));
  if (!receipts || !chains) {
    report(FAT32_NO_MEMORY, &backing);
    goto done;
  }
  for (unsigned i = 0; i < RECEIPT_COUNT; ++i) {
    receipts[i].chain = chains + i * RECEIPT_CHAIN_CLUSTERS;
    receipts[i].chain_capacity = RECEIPT_CHAIN_CLUSTERS;
  }
  struct fat32_write_io io = {.write = write_sectors, .flush = flush_disk, .context = &backing};
  status = fat32_writer_open(&writer, &volume, &io, &workspace, FILE_MAX_BYTES,
      receipts, RECEIPT_COUNT);
  if (status != FAT32_OK) {
    report(status, &backing);
    goto done;
  }
  info(&volume);
  puts("GPT and volume revalidated under the selected claim. Each mutation preflights whole-volume integrity.");
  printf("Session limits: %u receipts, %u-byte files, %u clusters per receipt; no restart ownership.\n",
      RECEIPT_COUNT, FILE_MAX_BYTES, RECEIPT_CHAIN_CLUSTERS);
  help();
  char line[COMMAND_BYTES];
  result = EXIT_SUCCESS;
  for (;;) {
    if (fflush(stdout) != 0) {
      report(FAT32_OUTPUT, &backing);
      result = EXIT_FAILURE;
      break;
    }
    struct term_line_result command = term_read_line(&terminal, "fat32-write> ", line, sizeof(line));
    if (command.status == TERM_LINE_EOF) {
      break;
    }
    if (command.status == TERM_LINE_CANCELLED) {
      continue;
    }
    if (command.status != TERM_LINE_OK) {
      fprintf(stderr, "fat32-write: input failed (status %u)\n", command.error);
      result = EXIT_FAILURE;
      break;
    }
    if (command.limit_reached) {
      fputs("fat32-write: command too long; enter it again\n", stderr);
      continue;
    }
    if (strcmp(line, "quit") == 0) {
      break;
    }
    if (strcmp(line, "info") == 0) {
      info(&volume);
      continue;
    }
    if (strcmp(line, "receipts") == 0) {
      show_receipts(&writer);
      continue;
    }
    if (strcmp(line, "help") == 0) {
      help();
      continue;
    }
    if (writer.stopped) {
      show_recovery(&writer);
      fputs("Only info, receipts and quit are available; recovery workspace is retained.\n", stderr);
      continue;
    }
    if (strcmp(line, "check") == 0) {
      struct fat32_check_report checked;
      status = fat32_check(&volume, &workspace, &checked);
      if (status == FAT32_OK) {
        printf("check: OK; files %llu, directories %llu, allocated %llu, free %llu, bad %llu; "
            "all names ASCII %s\n", (unsigned long long)checked.files,
            (unsigned long long)checked.directories, (unsigned long long)checked.allocated_clusters,
            (unsigned long long)checked.free_clusters, (unsigned long long)checked.bad_clusters,
            checked.all_names_ascii ? "yes" : "no");
      }
    } else if (strncmp(line, "list ", 5) == 0) {
      status = fat32_list(&volume, line + 5, strlen(line + 5), &workspace, list_entry, NULL);
    } else if (strncmp(line, "read ", 5) == 0) {
      status = fat32_read(&volume, line + 5, strlen(line + 5), &workspace, read_bytes, NULL);
    } else {
      status = mutate(&writer, line);
      if (writer.stopped) {
        result = EXIT_FAILURE;
        show_recovery(&writer);
      }
    }
    if (fflush(stdout) != 0) {
      status = FAT32_OUTPUT;
    }
    if (status != FAT32_OK) {
      report(status, &backing);
      if (status == FAT32_OUTPUT) {
        result = EXIT_FAILURE;
        break;
      }
    }
  }

done:
  if (release_disk) {
    enum call_status released = disk_release(disk);
    if (released != CALL_OK) {
      fprintf(stderr, "fat32-write: release failed (status %u); claim ended, durability unconfirmed\n",
          released);
      result = EXIT_FAILURE;
    } else {
      puts("Selected partition claim released.");
    }
  }
  free(chains);
  free(receipts);
  free(workspace.memory);
  if (disk != HANDLE_INVALID && handle_close(disk) != 0) {
    fputs("fat32-write: disk close failed\n", stderr);
    result = EXIT_FAILURE;
  }
  return result;
}
