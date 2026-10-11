#include <abi/memory.h>
#include <disk.h>
#include <handle.h>
#include <pyxis_fs/fat32.h>
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

static const char *status_name(enum fat32_status status)
{
  switch (status) {
  case FAT32_OK: return "OK";
  case FAT32_INVALID: return "INVALID";
  case FAT32_CORRUPT: return "CORRUPT";
  case FAT32_UNSUPPORTED: return "UNSUPPORTED";
  case FAT32_IO: return "IO";
  case FAT32_NO_MEMORY: return "NO_MEMORY";
  case FAT32_LIMIT: return "LIMIT";
  case FAT32_NAME_TOO_LONG: return "NAME_TOO_LONG";
  case FAT32_NOT_FOUND: return "NOT_FOUND";
  case FAT32_NOT_DIRECTORY: return "NOT_DIRECTORY";
  case FAT32_IS_DIRECTORY: return "IS_DIRECTORY";
  case FAT32_OUTPUT: return "OUTPUT";
  }
  return "UNKNOWN";
}

static void report(enum fat32_status status, const struct disk_source *source)
{
  fprintf(stderr, "fat32-inspect: %s", status_name(status));
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
    if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
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
  if (entry->has_long_name && (fputs(" [alias: ", stdout) == EOF ||
      !print_name(entry->alias, entry->alias_bytes) || fputc(']', stdout) == EOF)) {
    return false;
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
    fputs("fat32-inspect: LIMIT (workspace exceeds the 32 MiB policy)\n", stderr);
    return false;
  }
  void *memory = malloc(bytes);
  if (!memory) {
    fputs("fat32-inspect: NO_MEMORY\n", stderr);
    return false;
  }
  free(workspace->memory);
  workspace->memory = memory;
  workspace->bytes = bytes;
  return true;
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
  uint32_t entry;
  uint64_t rights, transport;
  if (argc != 2 || !decimal_entry(argv[1], &entry) || disk == HANDLE_INVALID ||
      startup_resource_count() != 2 || startup_resource("memory") == HANDLE_INVALID ||
      startup_root_count() || startup_namespace() != HANDLE_INVALID ||
      startup_working_directory_count() || input.protocol != PROTOCOL_CONSOLE ||
      output.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    fputs("fat32-inspect: requires a selected disk, GPT entry and explicit console profile\n", stderr);
    goto done;
  }
  enum call_status native = handle_rights(disk, &rights, &transport);
  if (native != CALL_OK || rights != (DISK_RIGHT_INFO | DISK_RIGHT_READ) || transport) {
    fputs("fat32-inspect: disk must carry exactly INFO|READ authority\n", stderr);
    goto done;
  }
  struct disk_info disk_info;
  native = disk_get_info(disk, &disk_info);
  if (native != CALL_OK) {
    fprintf(stderr, "fat32-inspect: disk info failed (status %u)\n", native);
    goto done;
  }
  if (disk_info.preparation != DISK_READY || disk_info.block_size != FAT32_SECTOR_BYTES ||
      !disk_info.block_count || disk_info.block_count > UINT64_MAX / FAT32_SECTOR_BYTES) {
    fputs("fat32-inspect: UNSUPPORTED disk geometry; requires ready 512-byte blocks\n", stderr);
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
  status = fat32_workspace_size(&volume, DIRECTORY_DEPTH_LIMIT, &bytes);
  if (status != FAT32_OK) {
    report(status, &backing);
    goto done;
  }
  if (!allocate_workspace(&workspace, bytes)) {
    goto done;
  }
  info(&volume);
  puts("Admission checks geometry; use check for whole-volume allocation validation.");
  puts("Commands: info, check, list /PATH, read /PATH, quit. Read streams raw file bytes.");
  char line[COMMAND_BYTES];
  result = EXIT_SUCCESS;
  for (;;) {
    if (fflush(stdout) != 0) {
      result = EXIT_FAILURE;
      break;
    }
    struct term_line_result command = term_read_line(&terminal, "fat32> ", line, sizeof(line));
    if (command.status == TERM_LINE_EOF) {
      break;
    }
    if (command.status == TERM_LINE_CANCELLED) {
      continue;
    }
    if (command.status != TERM_LINE_OK) {
      fprintf(stderr, "fat32-inspect: input failed (status %u)\n", command.error);
      result = EXIT_FAILURE;
      break;
    }
    if (command.limit_reached) {
      fputs("fat32-inspect: command too long; enter it again\n", stderr);
      continue;
    }
    if (strcmp(line, "quit") == 0) {
      break;
    }
    if (strcmp(line, "info") == 0) {
      info(&volume);
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
      fputs("fat32-inspect: expected info, check, list /PATH, read /PATH, or quit\n", stderr);
      continue;
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
  free(workspace.memory);
  if (disk != HANDLE_INVALID && handle_close(disk) != 0) {
    fputs("fat32-inspect: disk close failed\n", stderr);
    result = EXIT_FAILURE;
  }
  return result;
}
