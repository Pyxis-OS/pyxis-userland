#include "coexist_ui.h"
#include "coexist.h"

#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool decimal(const char *text, uint64_t *value)
{
  if (!*text) {
    return false;
  }
  uint64_t number = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || number > (UINT64_MAX - (*text - '0')) / 10) {
      return false;
    }
    number = number * 10 + (*text - '0');
  }
  *value = number;
  return true;
}

static bool read_line(struct terminal *terminal, const char *prompt, const char *initial,
    char line[32], struct install_coexist_choice *choice)
{
  if (fflush(stderr) != 0) {
    choice->reason = "installer selection output failed";
    return false;
  }
  for (;;) {
    struct term_line_result result = term_read_line_default(terminal, prompt, initial, line, 32);
    if (result.status != TERM_LINE_OK) {
      choice->reason = "selection cancelled or interrupted";
      return false;
    }
    if (!result.limit_reached) {
      return true;
    }
    fputs("Input was too long; enter the value again.\n", stderr);
  }
}

static bool list_disks(handle_t disks, struct install_coexist_choice *choice)
{
  fputs("Coexistence uses existing ESPs and unallocated space; Ctrl+C cancels a value prompt.\n", stderr);
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    enum call_status status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      if (ferror(stderr)) {
        choice->reason = "installer inventory output failed";
        return false;
      }
      return !ferror(stderr);
    }
    if (status != CALL_OK) {
      fprintf(stderr, "installer: disk inventory failed (status %u)\n", status);
      choice->reason = "disk inventory is incomplete or unavailable";
      return false;
    }
    fprintf(stderr, "Disk ID %llu: %llu blocks of %llu bytes, preparation=%llu GPT=%llu flags=%llx\n",
        (unsigned long long)info.id, (unsigned long long)info.block_count,
        (unsigned long long)info.block_size, (unsigned long long)info.preparation,
        (unsigned long long)info.gpt_status, (unsigned long long)info.flags);
  }
}

static bool show_gpt(const struct install_gpt *gpt)
{
  char disk_guid[37];
  install_guid_text(gpt->disk_guid, disk_guid);
  fprintf(stderr, "Healthy GPT disk GUID %s; usable blocks %llu..%llu; %u slots, %u partitions.\n",
      disk_guid, (unsigned long long)gpt->first_usable, (unsigned long long)gpt->last_usable,
      gpt->slot_count, gpt->partition_count);
  for (uint32_t i = 0; i < gpt->partition_count; ++i) {
    const struct install_gpt_partition *partition = &gpt->partitions[i];
    char guid[37], type[37];
    install_guid_text(partition->guid, guid);
    install_guid_text(partition->type_guid, type);
    fprintf(stderr, "  Entry %llu: first=%llu count=%llu GUID=%s type=%s attrs=%016llx name_utf16=\"",
        (unsigned long long)partition->entry, (unsigned long long)partition->first_block,
        (unsigned long long)partition->block_count, guid, type,
        (unsigned long long)partition->attributes);
    for (unsigned unit = 0; unit < INSTALL_GPT_NAME_UNITS; ++unit) {
      uint16_t value = partition->name[unit];
      if (value >= 32 && value <= 126 && value != '\\' && value != '"') {
        fputc(value, stderr);
      } else {
        fprintf(stderr, "\\u%04x", value);
      }
    }
    fputs("\"\n", stderr);
  }
  return !ferror(stderr);
}

static const struct disk_partition_selection *choose_partition(struct terminal *terminal,
    const struct disk_partition_selection *partitions, uint32_t count, bool esp,
    struct install_coexist_choice *choice)
{
  if (!count) {
    choice->reason = esp ? "healthy GPT has no ESP" : "healthy GPT has no native pool for Update";
    return NULL;
  }
  for (uint32_t i = 0; i < count; ++i) {
    fprintf(stderr, "%s GPT entry %llu: first=%llu count=%llu\n", esp ? "ESP" : "Native pool",
        (unsigned long long)partitions[i].partition, (unsigned long long)partitions[i].first_block,
        (unsigned long long)partitions[i].block_count);
  }
  if (esp && count == 1) {
    return &partitions[0];
  }
  if (!esp) {
    fputs("A native pool type is not ownership evidence; explicitly select the pool to inspect for Update.\n", stderr);
  }
  char line[32];
  for (;;) {
    if (!read_line(terminal, esp ? "ESP GPT entry: " : "Native pool GPT entry: ", "", line, choice)) {
      return NULL;
    }
    uint64_t entry;
    if (decimal(line, &entry)) {
      for (uint32_t i = 0; i < count; ++i) {
        if (partitions[i].partition == entry) {
          return &partitions[i];
        }
      }
    }
    fputs("Enter one of the listed GPT entry numbers.\n", stderr);
  }
}

static void selected_layout(const struct disk_partition_selection *esp,
    const struct disk_partition_selection *pool, struct install_layout *layout)
{
  *layout = (struct install_layout){.esp_partition = esp->partition,
      .pool_partition = pool->partition,
      .esp_start = esp->first_block * esp->device_block_size,
      .esp_bytes = esp->block_count * esp->device_block_size,
      .pool_start = pool->first_block * pool->device_block_size,
      .pool_bytes = pool->block_count * pool->device_block_size,
      .esp_attributes = esp->attributes, .pool_attributes = pool->attributes};
  memcpy(layout->disk_guid, pool->disk_guid, 16);
  memcpy(layout->esp_guid, esp->partition_guid, 16);
  memcpy(layout->pool_guid, pool->partition_guid, 16);
}

bool install_coexist_ui_choose(struct terminal *terminal, handle_t disks, bool updating,
    const uint8_t new_pool_guid[16], struct install_coexist_choice *choice)
{
  if (!choice) {
    return false;
  }
  *choice = (struct install_coexist_choice){.updating = updating,
      .reason = "missing terminal, fresh pool GUID or selection memory"};
  if (!terminal || (!updating && !new_pool_guid)) {
    return false;
  }
  struct install_coexist *facts = calloc(1, sizeof(*facts));
  if (!facts) {
    return false;
  }
  bool success = false;
  struct install_disk disk = {.handle = HANDLE_INVALID};
  if (!list_disks(disks, choice)) {
    goto done;
  }
  uint64_t id;
  char line[32];
  for (;;) {
    if (!read_line(terminal, "Explicit target disk ID: ", "", line, choice)) {
      goto done;
    }
    if (decimal(line, &id) && id) {
      break;
    }
    fputs("Enter an explicit nonzero disk ID.\n", stderr);
  }
  enum call_status status = disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk.handle);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: target RO open failed (status %u)\n", status);
    choice->reason = "target read-only inspection could not be opened";
    goto done;
  }
  status = disk_get_info(disk.handle, &disk.info);
  if (status != CALL_OK || disk.info.id != id) {
    fprintf(stderr, "installer: target INFO unavailable or mismatched (status %u)\n", status);
    choice->reason = "target identity could not be established";
    goto done;
  }
  choice->info = disk.info;
  if (!disk.info.block_size || disk.info.block_count > UINT64_MAX / disk.info.block_size) {
    choice->reason = "target disk geometry overflows its byte extent";
    goto done;
  }
  disk.bytes = disk.info.block_count * disk.info.block_size;
  if (!install_coexist_inspect(&disk, facts, &choice->reason)) {
    goto done;
  }
  if (!show_gpt(&facts->gpt)) {
    choice->reason = "installer partition listing output failed";
    goto done;
  }
  const struct disk_partition_selection *esp = choose_partition(terminal,
      facts->esps, facts->esp_count, true, choice);
  if (!esp) {
    goto done;
  }
  struct disk_partition_selection proposed;
  const struct disk_partition_selection *pool;
  if (updating) {
    pool = choose_partition(terminal, facts->pools, facts->pool_count, false, choice);
    if (!pool) {
      goto done;
    }
  } else {
    if (!install_coexist_choose(facts, new_pool_guid, 0, &proposed, &choice->reason)) {
      goto done;
    }
    uint64_t maximum_mib = facts->gap.block_count * disk.info.block_size / INSTALL_MIB;
    fprintf(stderr, "Largest aligned gap: first=%llu, %llu MiB; unused GPT entry=%llu.\n",
        (unsigned long long)facts->gap.first_block, (unsigned long long)maximum_mib,
        (unsigned long long)facts->gap.partition);
    char initial[32];
    snprintf(initial, sizeof(initial), "%llu", (unsigned long long)(facts->default_bytes / INSTALL_MIB));
    for (;;) {
      if (!read_line(terminal, "New pool size (MiB): ", initial, line, choice)) {
        goto done;
      }
      uint64_t mib;
      if (decimal(line, &mib) && mib && mib <= maximum_mib &&
          install_coexist_choose(facts, new_pool_guid, mib * INSTALL_MIB,
              &proposed, &choice->reason)) {
        break;
      }
      fprintf(stderr, "Enter a positive whole MiB size, at most %llu.\n",
          (unsigned long long)maximum_mib);
    }
    pool = &proposed;
  }
  selected_layout(esp, pool, &choice->layout);
  if (updating && (!install_scan_pool_update(&disk, &choice->layout, &choice->update) ||
      !choice->update.eligible)) {
    choice->reason = choice->update.reason;
    goto done;
  }
  fputs("Selected GPT identities are frozen for planning; ESP admission and final Review remain pending.\n", stderr);
  success = !ferror(stderr);
  choice->reason = success ? NULL : "installer selection output failed";
done:
  free(facts);
  if (disk.handle != HANDLE_INVALID && handle_close(disk.handle) != 0) {
    choice->pending_inspection = disk.handle;
    choice->reason = "read-only inspection handle close failed; caller retains cleanup ownership";
    success = false;
  }
  return success;
}
