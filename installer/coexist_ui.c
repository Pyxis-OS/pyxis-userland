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

struct disk_choice {
  uint64_t id;
  char label[192];
};

static bool choose_disk(struct terminal *terminal, handle_t disks,
    struct install_coexist_choice *choice, uint64_t *id)
{
  struct disk_choice *options = NULL;
  const char **labels = NULL;
  size_t count = 0, capacity = 0;
  bool success = false;
  fputs("Coexistence uses existing ESPs and unallocated space; Ctrl+C cancels a prompt.\n", stderr);
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    enum call_status status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      break;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "installer: disk inventory failed (status %u)\n", status);
      choice->reason = "disk inventory is incomplete or unavailable";
      goto done;
    }
    if (count == capacity) {
      size_t maximum = SIZE_MAX / sizeof(*options);
      size_t next = capacity ? capacity : 8;
      next = next > maximum / 2 ? maximum : next * 2;
      if (next <= count) {
        choice->reason = "disk inventory exceeds allocation capacity";
        goto done;
      }
      struct disk_choice *grown = realloc(options, next * sizeof(*grown));
      if (!grown) {
        choice->reason = "out of memory listing target disks";
        goto done;
      }
      options = grown;
      capacity = next;
    }
    struct disk_choice *option = &options[count++];
    option->id = info.id;
    snprintf(option->label, sizeof(option->label),
        "Disk %llu: %llu blocks of %llu bytes, preparation=%llu GPT=%llu flags=%llx",
        (unsigned long long)info.id, (unsigned long long)info.block_count,
        (unsigned long long)info.block_size, (unsigned long long)info.preparation,
        (unsigned long long)info.gpt_status, (unsigned long long)info.flags);
  }
  if (!count) {
    choice->reason = "no target disks are available";
    goto done;
  }
  if (count > SIZE_MAX / sizeof(*labels) - 1 ||
      !(labels = calloc(count + 1, sizeof(*labels)))) {
    choice->reason = "out of memory preparing disk choices";
    goto done;
  }
  for (size_t i = 0; i < count; ++i) {
    labels[i] = options[i].label;
  }
  labels[count] = "Cancel";
  if (fflush(stderr) != 0) {
    choice->reason = "installer inventory output failed";
    goto done;
  }
  struct term_choice_result result = term_read_choice(terminal, "Choose target disk",
      labels, count + 1, 0);
  if (result.status != TERM_LINE_OK || result.selected >= count) {
    choice->reason = "disk selection cancelled or interrupted";
    goto done;
  }
  *id = options[result.selected].id;
  success = true;
done:
  free(labels);
  free(options);
  return success;
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
  if (count > INSTALL_GPT_MAX_PARTITIONS) {
    choice->reason = "partition choice count exceeds GPT inspection capacity";
    return NULL;
  }
  char (*storage)[128] = calloc(count, sizeof(*storage));
  const char **labels = calloc((size_t)count + 1, sizeof(*labels));
  const struct disk_partition_selection *selected = NULL;
  if (!storage || !labels) {
    choice->reason = "out of memory listing partition choices";
    goto done;
  }
  for (uint32_t i = 0; i < count; ++i) {
    snprintf(storage[i], sizeof(storage[i]), "%s GPT entry %llu: first=%llu count=%llu",
        esp ? "ESP" : "Native pool", (unsigned long long)partitions[i].partition,
        (unsigned long long)partitions[i].first_block,
        (unsigned long long)partitions[i].block_count);
    labels[i] = storage[i];
  }
  labels[count] = "Cancel";
  if (!esp) {
    fputs("A native pool type is not ownership evidence; select the pool to inspect for Update.\n", stderr);
  }
  if (fflush(stderr) != 0) {
    choice->reason = "installer partition listing output failed";
    goto done;
  }
  struct term_choice_result result = term_read_choice(terminal,
      esp ? "Choose EFI System Partition" : "Choose native pool", labels, (size_t)count + 1, 0);
  if (result.status != TERM_LINE_OK || result.selected >= count) {
    choice->reason = "partition selection cancelled or interrupted";
    goto done;
  }
  selected = &partitions[result.selected];
done:
  free(labels);
  free(storage);
  return selected;
}

static bool open_disk(handle_t disks, uint64_t id, struct install_disk *disk,
    struct install_coexist_choice *choice)
{
  enum call_status status = disks_open(disks, id, DISK_ACCESS_READ_ONLY, &disk->handle);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: target RO open failed (status %u)\n", status);
    choice->reason = "target read-only inspection could not be opened";
    return false;
  }
  status = disk_get_info(disk->handle, &disk->info);
  if (status != CALL_OK || disk->info.id != id) {
    fprintf(stderr, "installer: target INFO unavailable or mismatched (status %u)\n", status);
    choice->reason = "target identity could not be established";
    return false;
  }
  choice->info = disk->info;
  if (!disk->info.block_size || disk->info.block_count > UINT64_MAX / disk->info.block_size) {
    choice->reason = "target disk geometry overflows its byte extent";
    return false;
  }
  disk->bytes = disk->info.block_count * disk->info.block_size;
  return true;
}

static bool close_disk(struct install_disk *disk, struct install_coexist_choice *choice)
{
  if (disk->handle != HANDLE_INVALID && handle_close(disk->handle) != 0) {
    choice->pending_inspection = disk->handle;
    choice->reason = "read-only inspection handle close failed; caller retains cleanup ownership";
    return false;
  }
  return true;
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
  uint64_t id;
  char line[32];
  if (!choose_disk(terminal, disks, choice, &id) || !open_disk(disks, id, &disk, choice)) {
    goto done;
  }
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
  install_coexist_layout(esp, pool, &choice->layout);
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
  return close_disk(&disk, choice) && success;
}

static const struct disk_partition_selection *find_partition(
    const struct disk_partition_selection *partitions, uint32_t count, uint64_t entry)
{
  for (uint32_t i = 0; i < count; ++i) {
    if (partitions[i].partition == entry) {
      return &partitions[i];
    }
  }
  return NULL;
}

bool install_coexist_ui_selected(handle_t disks, uint64_t target, uint64_t esp_entry,
    uint64_t pool_entry, const char *guid, struct install_coexist_choice *choice)
{
  if (!choice) {
    return false;
  }
  *choice = (struct install_coexist_choice){.updating = true,
      .reason = "missing selected target, partition entries or disk GUID"};
  if (!target || !esp_entry || !pool_entry || !guid) {
    return false;
  }
  struct install_disk disk = {.handle = HANDLE_INVALID};
  struct install_coexist *facts = calloc(1, sizeof(*facts));
  bool success = false;
  if (!facts) {
    choice->reason = "out of memory revalidating selected installation";
    goto done;
  }
  if (!open_disk(disks, target, &disk, choice) ||
      !install_coexist_inspect(&disk, facts, &choice->reason)) {
    goto done;
  }
  char current_guid[37];
  install_guid_text(facts->gpt.disk_guid, current_guid);
  if (strcmp(current_guid, guid)) {
    choice->reason = "selected disk GUID differs from the current healthy GPT";
    goto done;
  }
  const struct disk_partition_selection *esp = find_partition(facts->esps,
      facts->esp_count, esp_entry);
  const struct disk_partition_selection *pool = find_partition(facts->pools,
      facts->pool_count, pool_entry);
  if (!esp || !pool) {
    choice->reason = "selected ESP or native pool entry is absent from the current healthy GPT";
    goto done;
  }
  install_coexist_layout(esp, pool, &choice->layout);
  if (!install_scan_pool_update(&disk, &choice->layout, &choice->update) ||
      !choice->update.eligible) {
    choice->reason = choice->update.reason;
    goto done;
  }
  choice->reason = NULL;
  success = true;
done:
  free(facts);
  return close_disk(&disk, choice) && success;
}
