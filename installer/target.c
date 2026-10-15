#include "target.h"
#include "coexist.h"
#include "digest.h"

#include <handle.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t esp_type[16] = {
  0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
  0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
static const uint8_t pool_type[16] = {
  0xa3, 0x94, 0x81, 0x1a, 0x07, 0x8a, 0xff, 0x4d,
  0x83, 0x0e, 0x4c, 0xb4, 0xed, 0x7a, 0xac, 0x00,
};

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

static bool layout_matches(const struct install_layout *a, const struct install_layout *b)
{
  return a->esp_partition == b->esp_partition && a->pool_partition == b->pool_partition &&
      a->esp_start == b->esp_start && a->esp_bytes == b->esp_bytes &&
      a->pool_start == b->pool_start && a->pool_bytes == b->pool_bytes &&
      a->esp_attributes == b->esp_attributes && a->pool_attributes == b->pool_attributes &&
      !memcmp(a->disk_guid, b->disk_guid, 16) && !memcmp(a->esp_guid, b->esp_guid, 16) &&
      !memcmp(a->pool_guid, b->pool_guid, 16);
}

static void selection(struct install_target *target, bool esp)
{
  const struct install_layout *layout = &target->record.layout;
  struct disk_partition_selection *output = esp ? &target->esp : &target->pool;
  *output = (struct disk_partition_selection){
    .id = target->disk.info.id,
    .partition = esp ? layout->esp_partition : layout->pool_partition,
    .first_block = (esp ? layout->esp_start : layout->pool_start) / target->disk.info.block_size,
    .block_count = (esp ? layout->esp_bytes : layout->pool_bytes) / target->disk.info.block_size,
    .device_block_size = target->disk.info.block_size,
    .device_block_count = target->disk.info.block_count,
    .attributes = esp ? layout->esp_attributes : layout->pool_attributes,
  };
  memcpy(output->disk_guid, layout->disk_guid, 16);
  memcpy(output->partition_guid, esp ? layout->esp_guid : layout->pool_guid, 16);
  memcpy(output->partition_type, esp ? esp_type : pool_type, 16);
}

static bool partition_matches(const struct install_gpt_partition *partition,
    const struct disk_partition_selection *selected)
{
  return partition->entry == selected->partition &&
      partition->first_block == selected->first_block &&
      partition->block_count == selected->block_count &&
      partition->attributes == selected->attributes &&
      !memcmp(partition->guid, selected->partition_guid, 16) &&
      !memcmp(partition->type_guid, selected->partition_type, 16);
}

static bool selected_gpt(const struct install_target *target)
{
  const struct install_review_gpt *facts = target->review.gpt;
  if (!facts || !facts->mbr_protective || !facts->copies_agree ||
      facts->status[0] != INSTALL_REVIEW_GPT_VALID ||
      facts->status[1] != INSTALL_REVIEW_GPT_VALID) {
    return false;
  }
  const struct install_gpt *gpt = &facts->maps[0];
  if (memcmp(gpt->disk_guid, target->record.existing_guid, 16) ||
      memcmp(gpt->disk_guid, target->record.layout.disk_guid, 16)) {
    return false;
  }
  bool esp_found = false, pool_found = false;
  bool append = target->record.coexist && !target->record.updating;
  for (uint32_t i = 0; i < gpt->partition_count; ++i) {
    const struct install_gpt_partition *partition = &gpt->partitions[i];
    esp_found |= partition_matches(partition, &target->esp);
    pool_found |= partition_matches(partition, &target->pool);
    if (append && (partition->entry == target->pool.partition ||
        !memcmp(partition->guid, target->pool.partition_guid, 16) ||
        (target->pool.first_block < partition->first_block + partition->block_count &&
         partition->first_block < target->pool.first_block + target->pool.block_count))) {
      return false;
    }
  }
  if (!append) {
    return esp_found && pool_found;
  }
  return esp_found && target->pool.partition <= gpt->slot_count &&
      target->pool.first_block >= gpt->first_usable &&
      target->pool.first_block <= gpt->last_usable &&
      target->pool.block_count <= gpt->last_usable - target->pool.first_block + 1;
}

static bool selected_esp_available(const struct install_target *target)
{
  for (size_t i = 0; i < target->review.esp_count; ++i) {
    const struct install_review_esp *esp = &target->review.esps[i];
    if (!memcmp(&esp->selection, &target->esp, sizeof(target->esp))) {
      return esp->available && esp->identity_matches && esp->retired;
    }
  }
  return false;
}

static bool fresh_coexist(struct install_target *target)
{
  struct install_coexist *facts = calloc(1, sizeof(*facts));
  if (!facts) {
    target->reason = "out of memory checking the frozen coexistence proposal";
    return false;
  }
  struct disk_partition_selection proposed = {0};
  bool ok = install_coexist_inspect(&target->disk, facts, &target->reason) &&
      install_coexist_choose(facts, target->record.layout.pool_guid,
          target->record.layout.pool_bytes, &proposed, &target->reason) &&
      !memcmp(&proposed, &target->pool, sizeof(proposed));
  if (!ok && !target->reason) {
    target->reason = "the frozen pool slot, gap, size or GUID no longer matches the proposal";
  }
  free(facts);
  return ok;
}

static bool inspect(struct install_target *target, struct install_prior *prior,
    struct install_consent *consent, uint64_t *free_blocks)
{
  const struct install_plan_record *record = &target->record;
  struct install_update update = {0};
  if (record->coexist) {
    target->reason = "coexistence selections differ from healthy GPT or the selected ESP is unavailable";
    if (!selected_gpt(target) || !selected_esp_available(target)) {
      return false;
    }
    if (!record->updating) {
      return fresh_coexist(target);
    }
    target->reason = "selected native pool is not eligible for Update";
    if (!install_scan_pool_update(&target->disk, &record->layout, &update) || !update.eligible) {
      if (update.reason) {
        target->reason = update.reason;
      }
      return false;
    }
  } else if (record->updating) {
    target->reason = "classic Update target no longer matches the frozen layout";
    if (!install_scan_update(&target->disk, &update) || !update.eligible ||
        !layout_matches(&update.layout, &record->layout) || !selected_gpt(target)) {
      if (!update.eligible && update.reason) {
        target->reason = update.reason;
      }
      return false;
    }
    const char *reason = NULL;
    enum install_esp_state state = install_esp_inspect(&target->disk, &update.layout,
        update.revision, &reason);
    target->reason = reason ? reason : "classic ESP recovery choice or previous revision changed";
    if (state == INSTALL_ESP_REFUSED || (state == INSTALL_ESP_REBUILD) != record->esp_rebuild ||
        (!record->esp_rebuild && !selected_esp_available(target))) {
      return false;
    }
  } else {
    target->reason = "classic Install consent facts changed or no longer permit installation";
    if (!install_scan_consent(&target->disk, record->read_the_room, consent) ||
        !consent->eligible || memcmp(consent->existing_guid, record->existing_guid, 16)) {
      if (!consent->eligible && consent->reason) {
        target->reason = consent->reason;
      }
      return false;
    }
    return true;
  }
  if (!layout_matches(&update.layout, &record->layout) ||
      strcmp(update.revision, record->previous)) {
    target->reason = "Update layout or previous revision differs from the frozen plan";
    return false;
  }
  *free_blocks = update.free_blocks;
  target->reason = "prior program inventory inspection failed or requires explicit legacy consent";
  return install_prior_read(&target->disk, &update, record->legacy, prior) && !prior->needs_legacy;
}

/* Review displays boot-scoped numeric device IDs. Canonicalize only these two
 * authored fields, preserving every other byte, including escaped names. */
static bool hash_review(struct install_digest *digest, const struct install_review *review)
{
  static const char *const prefixes[] = {"Target Review: disk=", "  bound device="};
  const char *cursor = review->text, *end = cursor + review->text_bytes;
  while (cursor < end) {
    const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
    const char *line_end = newline ? newline + 1 : end;
    const char *number = NULL;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
      size_t length = strlen(prefixes[i]);
      if ((size_t)(line_end - cursor) >= length && !memcmp(cursor, prefixes[i], length)) {
        number = cursor + length;
        break;
      }
    }
    if (number) {
      const char *suffix = number;
      while (suffix < line_end && *suffix >= '0' && *suffix <= '9') {
        ++suffix;
      }
      static const char identity[] = "<selected-device>";
      if (suffix == number || !install_digest_update(digest, cursor, (size_t)(number - cursor)) ||
          !install_digest_update(digest, identity, sizeof(identity) - 1) ||
          !install_digest_update(digest, suffix, (size_t)(line_end - suffix))) {
        return false;
      }
    } else if (!install_digest_update(digest, cursor, (size_t)(line_end - cursor))) {
      return false;
    }
    cursor = line_end;
  }
  return true;
}

static bool fingerprint(struct install_target *target, const char raw[65], char output[65])
{
  static const char domain[] = "pyxis-install-target-1";
  struct install_digest digest = {0};
  bool ok = install_digest_begin(&digest) &&
      install_digest_update(&digest, domain, sizeof(domain)) &&
      install_digest_update(&digest, raw, 64) && hash_review(&digest, &target->review) &&
      install_digest_finish(&digest, output);
  install_digest_abort(&digest);
  return ok;
}

bool install_target_prepare(struct install_target *target, handle_t disks, handle_t clock,
    const struct disk_info *info, const struct install_plan_record *record,
    uint64_t deadline_ns, struct install_prior *prior, struct install_consent *consent,
    uint64_t *free_blocks, char target_hash[65])
{
  if (!target || target->initialized || !info || !record || !prior || !consent ||
      !free_blocks || !target_hash) {
    return false;
  }
  target->initialized = true;
  target->disks = disks;
  target->clock = clock;
  target->record = *record;
  target->status = CALL_BAD_REQUEST;
  target->reason = "invalid frozen target geometry or choices";
  *free_blocks = 0;
  target_hash[0] = '\0';
  struct install_plan_record checked = *record;
  memset(checked.source_hash, '0', 64);
  memset(checked.target_hash, '0', 64);
  checked.source_hash[64] = checked.target_hash[64] = '\0';
  if (!info->id || (info->block_size != 512 && info->block_size != 4096) ||
      info->block_count != record->block_count ||
      info->block_size != record->block_size || !install_plan_record_valid(&checked)) {
    return false;
  }
  target->status = disks_open(disks, info->id, DISK_ACCESS_READ_ONLY, &target->disk.handle);
  if (target->status != CALL_OK) {
    target->reason = "cannot open the reviewed device read-only";
    return false;
  }
  target->status = disk_get_info(target->disk.handle, &target->disk.info);
  if (target->status != CALL_OK || target->disk.info.id != info->id ||
      target->disk.info.block_count != record->block_count ||
      target->disk.info.block_size != record->block_size) {
    target->reason = "fresh device identity or geometry differs from the frozen target";
    if (target->status == CALL_OK) {
      target->status = CALL_BAD_REQUEST;
    }
    return false;
  }
  uint64_t required = DISK_FLAG_WRITABLE | DISK_FLAG_FLUSH_SUPPORTED;
  if (target->disk.info.preparation != DISK_READY ||
      (target->disk.info.flags & required) != required ||
      (target->disk.info.flags & DISK_FLAG_WRITE_FAILED)) {
    target->reason = "target is not ready and writable with reliable flush support";
    target->status = CALL_READ_ONLY;
    return false;
  }
  target->disk.bytes = record->block_count * record->block_size;
  selection(target, true);
  selection(target, false);
  if (!record->updating) {
    target->review_kind = record->coexist ? INSTALL_TARGET_REVIEW_APPEND : INSTALL_TARGET_REVIEW_ERASE;
    target->status = record->coexist ?
        disks_review_append(disks, &target->disk.info, &target->review_handle) :
        disks_review_erase(disks, &target->disk.info, &target->review_handle);
    if (target->status != CALL_OK) {
      target->reason = "cannot retain the exact GPT review for Apply";
      return false;
    }
  } else if (record->esp_rebuild) {
    target->status = disks_review_esp(disks, &target->esp, &target->rebuild_review);
    if (target->status != CALL_OK) {
      target->reason = "cannot retain the reviewed classic ESP rebuild grant";
      return false;
    }
  }
  struct install_disk review_disk = target->disk;
  if (target->review_handle != HANDLE_INVALID) {
    review_disk.handle = target->review_handle;
  } else if (target->rebuild_review != HANDLE_INVALID) {
    review_disk.handle = target->rebuild_review;
  }
  char raw[65];
  target->reason = "read-only target inspection or fingerprint preparation failed";
  bool ok = install_read_fingerprint_begin();
  if (ok) {
    ok = install_review_prepare(&review_disk, disks, clock, deadline_ns, NULL, &target->review) &&
        inspect(target, prior, consent, free_blocks);
  }
  if (ok) {
    ok = install_read_fingerprint_finish(raw) && fingerprint(target, raw, target_hash);
  } else {
    install_read_fingerprint_abort();
  }
  if (!ok) {
    if (!target->review.prepared && target->review.reason) {
      target->reason = target->review.reason;
    }
    target->status = CALL_UNAVAILABLE;
    return false;
  }
  if (!close_reference(&target->disk.handle)) {
    target->reason = "read-only disk close failed; handle remains owned";
    target->status = CALL_UNAVAILABLE;
    return false;
  }
  memcpy(target->record.target_hash, target_hash, 65);
  target->status = CALL_OK;
  target->reason = NULL;
  target->prepared = true;
  return true;
}

bool install_target_destroy(struct install_target *target, uint64_t deadline_ns)
{
  if (!target) {
    return false;
  }
  target->prepared = false;
  bool cleaned = true;
  if (target->pending_recheck) {
    if (install_target_destroy(target->pending_recheck, deadline_ns)) {
      free(target->pending_recheck);
      target->pending_recheck = NULL;
    } else {
      cleaned = false;
    }
  }
  if (!install_review_cleanup(&target->review, target->clock, deadline_ns) ||
      !install_review_destroy(&target->review)) {
    cleaned = false;
  }
  if (!close_reference(&target->disk.handle)) {
    cleaned = false;
  }
  if (!close_reference(&target->rebuild_review)) {
    cleaned = false;
  }
  if (!close_reference(&target->review_handle)) {
    cleaned = false;
  }
  if (cleaned) {
    memset(target, 0, sizeof(*target));
  } else {
    target->reason = "target cleanup remains incomplete; all pending handles remain owned";
  }
  return cleaned;
}

bool install_target_revalidate(struct install_target *target, uint64_t deadline_ns)
{
  if (!target || !target->prepared || target->pending_recheck) {
    return false;
  }
  struct install_target *fresh = calloc(1, sizeof(*fresh));
  if (!fresh) {
    target->prepared = false;
    target->reason = "out of memory preparing a fresh target recheck";
    return false;
  }
  target->pending_recheck = fresh;
  struct install_prior prior = {0};
  struct install_consent consent = {0};
  uint64_t free_blocks = 0;
  char hash[65];
  bool ok = install_target_prepare(fresh, target->disks, target->clock, &target->disk.info,
      &target->record, deadline_ns, &prior, &consent, &free_blocks, hash) &&
      !strcmp(hash, target->record.target_hash);
  install_prior_destroy(&prior);
  install_consent_destroy(&consent);
  if (install_target_destroy(fresh, deadline_ns)) {
    free(fresh);
    target->pending_recheck = NULL;
  } else {
    ok = false;
  }
  if (!ok) {
    target->prepared = false;
    target->status = CALL_UNAVAILABLE;
    target->reason = "fresh target inspection changed or could not complete and clean up";
  }
  return ok;
}
