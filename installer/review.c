#include "review.h"
#include "plan_record.h"

#include <clock.h>
#include <directory.h>
#include <handle.h>
#include <mount.h>
#include <pyxis_fs/fat32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVIEW_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE)

static const uint8_t esp_type[16] = {
  0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
  0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

static bool text(struct install_review *review, const char *format, ...)
{
  va_list arguments, measured;
  va_start(arguments, format);
  va_copy(measured, arguments);
  int count = vsnprintf(NULL, 0, format, measured);
  va_end(measured);
  if (count < 0 || (size_t)count > INSTALL_PLAN_RECORD_MAX_BYTES - review->text_bytes) {
    review->reason = "target Review exceeds the plan-record text budget or formatting failed";
    va_end(arguments);
    return false;
  }
  size_t needed = review->text_bytes + (size_t)count + 1;
  if (needed > review->text_capacity) {
    size_t capacity = review->text_capacity ? review->text_capacity : 4096;
    while (capacity < needed) {
      capacity *= 2;
    }
    if (capacity > INSTALL_PLAN_RECORD_MAX_BYTES + 1) {
      capacity = INSTALL_PLAN_RECORD_MAX_BYTES + 1;
    }
    char *grown = realloc(review->text, capacity);
    if (!grown) {
      review->reason = "out of memory preparing target Review text";
      va_end(arguments);
      return false;
    }
    review->text = grown;
    review->text_capacity = capacity;
  }
  vsnprintf(review->text + review->text_bytes, needed - review->text_bytes, format, arguments);
  va_end(arguments);
  review->text_bytes += (size_t)count;
  return true;
}

static bool escaped_name(struct install_review *review, const char *name, size_t bytes)
{
  if (!text(review, "\"")) {
    return false;
  }
  for (size_t i = 0; i < bytes; ++i) {
    unsigned char byte = (unsigned char)name[i];
    if (byte < 32 || byte > 126 || byte == '\\' || byte == '"') {
      if (!text(review, "\\x%02x", byte)) {
        return false;
      }
    } else if (!text(review, "%c", byte)) {
      return false;
    }
  }
  return text(review, "\"");
}

static bool same_gpt(const struct install_gpt *a, const struct install_gpt *b)
{
  if (a->first_usable != b->first_usable || a->last_usable != b->last_usable ||
      a->slot_count != b->slot_count || a->partition_count != b->partition_count ||
      memcmp(a->disk_guid, b->disk_guid, 16)) {
    return false;
  }
  for (uint32_t i = 0; i < a->partition_count; ++i) {
    const struct install_gpt_partition *left = &a->partitions[i], *right = &b->partitions[i];
    if (left->entry != right->entry || left->first_block != right->first_block ||
        left->block_count != right->block_count || left->attributes != right->attributes ||
        memcmp(left->type_guid, right->type_guid, 16) || memcmp(left->guid, right->guid, 16) ||
        memcmp(left->name, right->name, sizeof(left->name))) {
      return false;
    }
  }
  return true;
}

static const char *copy_status(enum install_review_copy_status status)
{
  switch (status) {
  case INSTALL_REVIEW_GPT_ABSENT: return "absent";
  case INSTALL_REVIEW_GPT_VALID: return "valid";
  case INSTALL_REVIEW_GPT_DAMAGED: return "damaged";
  case INSTALL_REVIEW_GPT_UNSUPPORTED: return "unsupported";
  case INSTALL_REVIEW_GPT_UNAVAILABLE: return "unavailable";
  }
  return "unknown";
}

static bool map_text(struct install_review *review, unsigned copy)
{
  const struct install_gpt *map = &review->gpt->maps[copy];
  if (review->gpt->status[copy] != INSTALL_REVIEW_GPT_VALID) {
    return text(review, "GPT %s: entries unknown; no complete entry enumeration\n",
        copy ? "backup" : "primary");
  }
  char disk_guid[37];
  install_guid_text(map->disk_guid, disk_guid);
  if (!text(review, "GPT %s: disk_guid=%s usable=%llu..%llu slots=%u nonempty=%u\n",
      copy ? "backup" : "primary", disk_guid, (unsigned long long)map->first_usable,
      (unsigned long long)map->last_usable, map->slot_count, map->partition_count)) {
    return false;
  }
  for (uint32_t i = 0; i < map->partition_count; ++i) {
    const struct install_gpt_partition *partition = &map->partitions[i];
    char guid[37], type[37];
    install_guid_text(partition->guid, guid);
    install_guid_text(partition->type_guid, type);
    if (!text(review, "  entry=%llu first=%llu count=%llu last=%llu guid=%s type=%s attrs=%016llx name_utf16=\"",
        (unsigned long long)partition->entry, (unsigned long long)partition->first_block,
        (unsigned long long)partition->block_count,
        (unsigned long long)(partition->first_block + partition->block_count - 1), guid, type,
        (unsigned long long)partition->attributes)) {
      return false;
    }
    for (unsigned unit = 0; unit < INSTALL_GPT_NAME_UNITS; ++unit) {
      uint16_t value = partition->name[unit];
      if (value >= 32 && value <= 126 && value != '\\' && value != '"') {
        if (!text(review, "%c", value)) {
          return false;
        }
      } else if (!text(review, "\\u%04x", value)) {
        return false;
      }
    }
    if (!text(review, "\"\n")) {
      return false;
    }
  }
  return true;
}

static void collect_esps(const struct install_disk *disk, struct install_review *review)
{
  for (unsigned copy = 0; copy < 2; ++copy) {
    const struct install_gpt *map = &review->gpt->maps[copy];
    for (uint32_t i = 0; i < map->partition_count; ++i) {
      const struct install_gpt_partition *partition = &map->partitions[i];
      if (memcmp(partition->type_guid, esp_type, 16)) {
        continue;
      }
      struct disk_partition_selection selection = {.id = disk->info.id,
          .partition = partition->entry, .first_block = partition->first_block,
          .block_count = partition->block_count, .device_block_size = disk->info.block_size,
          .device_block_count = disk->info.block_count, .attributes = partition->attributes};
      memcpy(selection.disk_guid, map->disk_guid, 16);
      memcpy(selection.partition_guid, partition->guid, 16);
      memcpy(selection.partition_type, partition->type_guid, 16);
      bool duplicate = false;
      for (size_t j = 0; j < review->esp_count; ++j) {
        duplicate |= !memcmp(&selection, &review->esps[j].selection, sizeof(selection));
      }
      if (!duplicate) {
        struct install_review_esp *esp = &review->esps[review->esp_count++];
        esp->selection = selection;
        esp->admission_status = esp->observe_status = esp->content_status =
            esp->retire_status = CALL_UNAVAILABLE;
      }
    }
  }
}

static bool identity_matches(const struct mount_state_reply *state,
    const struct disk_partition_selection *selection)
{
  return (state->flags & MOUNT_STATE_IDENTITY_VALID) &&
      !(state->flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_READ_WRITE | MOUNT_STATE_FAILED)) &&
      state->device == selection->id && state->partition == selection->partition &&
      state->first_block == selection->first_block && state->block_count == selection->block_count &&
      state->device_block_size == selection->device_block_size &&
      state->device_block_count == selection->device_block_count &&
      state->partition_attributes == selection->attributes &&
      !memcmp(state->disk_guid, selection->disk_guid, 16) &&
      !memcmp(state->partition_guid, selection->partition_guid, 16) &&
      !memcmp(state->partition_type, selection->partition_type, 16);
}

static bool inspect_esp(handle_t issuer, struct install_review_esp *esp,
    struct install_review *review)
{
  char guid[37];
  install_guid_text(esp->selection.partition_guid, guid);
  if (!text(review, "ESP entry=%llu guid=%s\n", (unsigned long long)esp->selection.partition, guid)) {
    return false;
  }
  struct mount_fat32_reply mounted;
  esp->admission_status = disks_open_fat32(issuer, esp->selection.id, esp->selection.partition,
      MOUNT_ACCESS_READ_ONLY, REVIEW_DIRECTORY_RIGHTS, esp->selection.disk_guid, &mounted);
  esp->root = mounted.root;
  esp->control = mounted.control;
  if (!text(review, "  RO admission status=%u\n", esp->admission_status)) {
    return false;
  }
  if (esp->admission_status != CALL_OK) {
    if (esp->admission_status == CALL_OUTCOME_UNKNOWN) {
      review->reason = "RO admission outcome unknown; target exclusion may remain";
      return false;
    }
    return text(review, "  EFI entries unavailable: native RO FAT admission failed\n");
  }
  esp->observe_status = mount_observe(esp->control, &esp->observed);
  esp->identity_matches = esp->observe_status == CALL_OK &&
      identity_matches(&esp->observed, &esp->selection);
  if (!text(review, "  OBSERVE status=%u identity_matches=%u\n", esp->observe_status,
      esp->identity_matches)) {
    return false;
  }
  if (esp->observe_status == CALL_OK) {
    char disk_guid[37], partition_guid[37], type[37];
    install_guid_text(esp->observed.disk_guid, disk_guid);
    install_guid_text(esp->observed.partition_guid, partition_guid);
    install_guid_text(esp->observed.partition_type, type);
    if (!text(review, "  bound device=%llu entry=%llu first=%llu count=%llu block_size=%llu device_blocks=%llu attrs=%016llx disk_guid=%s partition_guid=%s type=%s identity_valid=%u\n",
        (unsigned long long)esp->observed.device, (unsigned long long)esp->observed.partition,
        (unsigned long long)esp->observed.first_block, (unsigned long long)esp->observed.block_count,
        (unsigned long long)esp->observed.device_block_size,
        (unsigned long long)esp->observed.device_block_count,
        (unsigned long long)esp->observed.partition_attributes, disk_guid, partition_guid, type,
        !!(esp->observed.flags & MOUNT_STATE_IDENTITY_VALID))) {
      return false;
    }
  }
  if (!esp->identity_matches) {
    return text(review, "  EFI entries unavailable: bound identity or RO instance state did not match\n");
  }
  esp->content_status = directory_lookup(esp->root, "EFI", DIRECTORY_KIND_DIRECTORY,
      REVIEW_DIRECTORY_RIGHTS, &esp->efi);
  if (esp->content_status == CALL_NOT_FOUND) {
    esp->efi_absent = esp->available = true;
    return text(review, "  EFI absent (native lookup status=%u)\n", esp->content_status);
  }
  if (esp->content_status != CALL_OK) {
    return text(review, "  EFI unavailable: lookup status=%u\n", esp->content_status);
  }
  if (!text(review, "  EFI direct entries (no recursive content inventory):\n")) {
    return false;
  }
  struct directory_cursor cursor = {0};
  char name[FAT32_NAME_MAX + 1];
  for (;;) {
    struct directory_enumerate_reply entry;
    esp->content_status = directory_enumerate(esp->efi, &cursor, name, sizeof(name), &entry);
    if (esp->content_status != CALL_OK) {
      return text(review, "  EFI enumeration unavailable: status=%u\n", esp->content_status);
    }
    if (entry.outcome == DIRECTORY_END) {
      esp->available = true;
      return text(review, "  EFI enumeration complete\n");
    }
    if (entry.outcome != DIRECTORY_ENTRY) {
      return text(review, "  EFI enumeration incomplete: outcome=%llu required_name_bytes=%llu\n",
          (unsigned long long)entry.outcome, (unsigned long long)entry.name_size);
    }
    const char *kind = entry.kind == DIRECTORY_KIND_DIRECTORY ? "directory" :
        entry.kind == DIRECTORY_KIND_FILE ? "file" : "other";
    if (!text(review, "    %s EFI/", kind) ||
        !escaped_name(review, name, entry.name_size - 1) || !text(review, "\n")) {
      return false;
    }
    cursor = entry.cursor;
  }
}

static bool close_reference(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  if (handle_close(*handle) != 0) {
    return false;
  }
  *handle = HANDLE_INVALID;
  return true;
}

bool install_review_cleanup(struct install_review *review, handle_t clock, uint64_t deadline_ns)
{
  if (!review) {
    return false;
  }
  bool complete = true;
  for (size_t i = 0; i < review->esp_count; ++i) {
    struct install_review_esp *esp = &review->esps[i];
    bool closed = close_reference(&esp->efi);
    closed = close_reference(&esp->root) && closed;
    if (!closed) {
      review->reason = "RO FAT data reference close failed; caller retains cleanup ownership";
      complete = false;
      continue;
    }
    if (esp->control == HANDLE_INVALID) {
      if (esp->admission_status == CALL_OUTCOME_UNKNOWN) {
        complete = false;
      }
      continue;
    }
    while (!esp->retired) {
      if (esp->retirement_attempted && esp->retire_status != CALL_BUSY) {
        complete = false;
        break;
      }
      esp->retirement_attempted = true;
      esp->retire_status = mount_retire(esp->control);
      if (esp->retire_status == CALL_OK) {
        esp->retired = true;
        break;
      }
      if (esp->retire_status != CALL_BUSY) {
        review->reason = "RO FAT retirement failed; caller retains its control";
        complete = false;
        break;
      }
      uint64_t now;
      if (clock_now(clock, &now) != CALL_OK || now >= deadline_ns) {
        review->reason = "RO FAT retirement drain deadline unavailable or reached; caller retains its control";
        complete = false;
        break;
      }
    }
    if (esp->retired && !close_reference(&esp->control)) {
      review->reason = "retired FAT control close failed; caller retains its control";
      complete = false;
    }
  }
  return complete;
}

bool install_review_destroy(struct install_review *review)
{
  if (!review) {
    return false;
  }
  for (size_t i = 0; i < review->esp_count; ++i) {
    const struct install_review_esp *esp = &review->esps[i];
    if (esp->root != HANDLE_INVALID || esp->efi != HANDLE_INVALID ||
        esp->control != HANDLE_INVALID) {
      return false;
    }
  }
  free(review->text);
  free(review->gpt);
  free(review->esps);
  memset(review, 0, sizeof(*review));
  return true;
}

bool install_review_prepare(const struct install_disk *disk, handle_t issuer,
    handle_t clock, uint64_t deadline_ns, const struct install_gpt *cache,
    struct install_review *review)
{
  if (!review) {
    return false;
  }
  if (review->text || review->gpt || review->esps) {
    review->reason = "destroy the previous owned Review before preparing another";
    return false;
  }
  review->reason = "missing target disk or out of memory preparing target Review";
  if (!disk || !disk->info.id) {
    return false;
  }
  review->gpt = calloc(1, sizeof(*review->gpt));
  review->esps = calloc(2 * INSTALL_GPT_MAX_PARTITIONS, sizeof(*review->esps));
  if (!review->gpt || !review->esps ||
      !install_review_gpt_read(disk, review->gpt, &review->reason)) {
    return false;
  }
  bool healthy = review->gpt->mbr_protective && review->gpt->copies_agree;
  review->blank = review->gpt->mbr_zero &&
      review->gpt->status[0] == INSTALL_REVIEW_GPT_ABSENT &&
      review->gpt->status[1] == INSTALL_REVIEW_GPT_ABSENT;
  review->metadata_complete = healthy || review->blank;
  review->cache_matches = cache && healthy && same_gpt(&review->gpt->maps[0], cache);
  bool success = text(review, "Target Review: disk=%llu block_size=%llu block_count=%llu\n",
      (unsigned long long)disk->info.id, (unsigned long long)disk->info.block_size,
      (unsigned long long)disk->info.block_count) &&
      text(review, "MBR: read=%u protective=%u zero=%u\n", review->gpt->mbr_read,
          review->gpt->mbr_protective, review->gpt->mbr_zero) &&
      text(review, "GPT: primary=%s backup=%s copies_agree=%u metadata_complete=%u blank=%u\n",
          copy_status(review->gpt->status[0]), copy_status(review->gpt->status[1]),
          review->gpt->copies_agree, review->metadata_complete, review->blank) &&
      text(review, "Planning GPT cache: %s\n", !cache ? "not supplied" :
          !healthy ? "not comparable" : review->cache_matches ? "matches" : "differs");
  if (success && review->blank) {
    success = text(review, "Blank GPT framing: no partition entries\n");
  } else if (success) {
    success = map_text(review, 0) && map_text(review, 1);
  }
  collect_esps(disk, review);
  for (size_t i = 0; success && i < review->esp_count; ++i) {
    success = inspect_esp(issuer, &review->esps[i], review);
  }
  bool cleaned = install_review_cleanup(review, clock, deadline_ns);
  for (size_t i = 0; success && i < review->esp_count; ++i) {
    const struct install_review_esp *esp = &review->esps[i];
    success = text(review, "ESP entry=%llu: available=%u retire_status=%u retired=%u cleanup_pending=%u\n",
        (unsigned long long)esp->selection.partition, esp->available, esp->retire_status,
        esp->retired, esp->root != HANDLE_INVALID || esp->efi != HANDLE_INVALID ||
            esp->control != HANDLE_INVALID);
  }
  if (!review->esp_count && success) {
    success = text(review, "No ESP among known valid GPT entries%s\n",
        review->metadata_complete ? "" : "; unknown entries may include an ESP");
  }
  review->prepared = success && cleaned;
  if (review->prepared) {
    review->reason = NULL;
  }
  return review->prepared;
}
