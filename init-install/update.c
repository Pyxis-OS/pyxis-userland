#include "update.h"
#include "../installer/coexist.h"
#include "../installer/review.h"
#if INSTALLER_PUBLICATION_SCRATCH
#include "../installer/installed_publication.h"
#endif
#include "../xfer/hash.h"

#include <clock.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>

struct update_candidate {
  struct installer_update_choice choice;
  char label[192];
};

struct update_inventory {
  struct update_candidate *candidates;
  size_t count, capacity;
};

static bool append_candidate(struct update_inventory *inventory,
    const struct install_disk *disk, const struct install_update *update,
    bool coexist, bool rebuild, bool pending)
{
  if (inventory->count == inventory->capacity) {
    size_t maximum = SIZE_MAX / sizeof(*inventory->candidates);
    size_t capacity = inventory->capacity ? inventory->capacity : 8;
    capacity = capacity > maximum / 2 ? maximum : capacity * 2;
    if (capacity <= inventory->count) {
      return false;
    }
    struct update_candidate *grown = realloc(inventory->candidates, capacity * sizeof(*grown));
    if (!grown) {
      return false;
    }
    inventory->candidates = grown;
    inventory->capacity = capacity;
  }
  struct update_candidate *candidate = &inventory->candidates[inventory->count++];
  candidate->choice = (struct installer_update_choice){.disk = disk->info.id,
      .layout = update->layout, .coexist = coexist, .pending = pending};
  char guid[37];
  install_guid_text(update->layout.disk_guid, guid);
  snprintf(candidate->label, sizeof(candidate->label),
      "Disk %llu (%s), %s, ESP %llu, pool %llu%s",
      (unsigned long long)disk->info.id, guid, coexist ? "coexist" : "classic",
      (unsigned long long)update->layout.esp_partition,
      (unsigned long long)update->layout.pool_partition,
      pending ? ", completion Review required" : rebuild ? ", ESP recovery required" : "");
  return true;
}

static const char *esp_refusal(const struct install_review_esp *esp,
    const struct install_layout *layout)
{
  uint64_t clean = MOUNT_STATE_CLEAN_SHUTDOWN | MOUNT_STATE_NO_HARD_ERROR;
  if (!esp->available || !esp->identity_matches || !esp->retired) {
    return "native read-only ESP admission, identity or retirement is unavailable";
  }
  if ((esp->observed.flags & clean) != clean) {
    return "ESP is not clean or reports filesystem errors; coexistence never rebuilds it";
  }
#if !INSTALLER_PUBLICATION_SCRATCH
  const char *reason = NULL;
  if ((esp->menu_absent && esp->menu_status == CALL_NOT_FOUND) ||
      (esp->menu_status == CALL_OK && esp->menu_size == esp->menu_bytes &&
       install_esp_configuration_binding(layout->disk_guid, esp->menu, esp->menu_bytes, &reason))) {
    return NULL;
  }
  return reason ? reason : "native fixed-menu inspection failed or exceeded its read bound";
#else
  (void)layout;
  return NULL;
#endif
}

static bool inspect_coexist(struct update_inventory *inventory, const struct install_disk *disk,
    handle_t disks, handle_t clock, bool *clean)
{
  struct install_coexist *facts = calloc(1, sizeof(*facts));
  struct install_review review = {0};
  const char *reason = NULL;
  bool success = false;
  if (!facts) {
    fputs("init-install: out of memory inspecting coexistence targets\n", stderr);
    return false;
  }
  if (!install_coexist_inspect(disk, facts, &reason)) {
    fprintf(stderr, "init-install: disk %llu coexist Update refused: %s\n",
        (unsigned long long)disk->info.id, reason ? reason : "inspection unavailable");
    success = true;
    goto done;
  }
  if (!facts->esp_count || !facts->pool_count) {
    fprintf(stderr, "init-install: disk %llu coexist Update unavailable: %s\n",
        (unsigned long long)disk->info.id, !facts->esp_count ? "healthy GPT has no ESP" :
        "healthy GPT has no native pool");
    success = true;
    goto done;
  }
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(2000000000)) {
    goto done;
  }
  /* The existing Review owns native admission, identity comparison and bounded
   * retirement. Its report is discarded; final Review belongs to the child. */
  if (!install_review_prepare(disk, disks, clock, now + UINT64_C(2000000000),
      &facts->gpt, &review)) {
    fprintf(stderr, "init-install: Update ESP inspection failed: %s\n",
        review.reason ? review.reason : "unavailable");
    goto done;
  }
  if (!review.cache_matches) {
    fputs("init-install: GPT changed during Update discovery\n", stderr);
    goto done;
  }
  for (uint32_t p = 0; p < facts->pool_count; ++p) {
    struct install_layout layout;
    install_coexist_layout(&facts->esps[0], &facts->pools[p], &layout);
    struct install_update update;
    if (!install_scan_pool_update(disk, &layout, &update)) {
      fprintf(stderr, "init-install: disk %llu pool %llu inspection failed: %s\n",
          (unsigned long long)disk->info.id, (unsigned long long)layout.pool_partition,
          update.reason ? update.reason : "unavailable");
      goto done;
    }
    if (!update.eligible) {
      fprintf(stderr, "init-install: disk %llu pool %llu Update refused: %s\n",
          (unsigned long long)disk->info.id, (unsigned long long)layout.pool_partition,
          update.reason ? update.reason : "native pool not eligible");
      continue;
    }
    for (uint32_t e = 0; e < facts->esp_count; ++e) {
      install_coexist_layout(&facts->esps[e], &facts->pools[p], &update.layout);
      const char *refusal = "selected ESP has no native read-only admission evidence";
      for (size_t r = 0; r < review.esp_count; ++r) {
        if (review.esps[r].selection.partition == update.layout.esp_partition) {
          refusal = esp_refusal(&review.esps[r], &update.layout);
          break;
        }
      }
      if (refusal) {
        fprintf(stderr, "init-install: disk %llu pool %llu ESP %llu Update refused: %s\n",
            (unsigned long long)disk->info.id, (unsigned long long)update.layout.pool_partition,
            (unsigned long long)update.layout.esp_partition, refusal);
        continue;
      }
      bool pending = false;
#if INSTALLER_PUBLICATION_SCRATCH
      enum install_installed_probe probe = install_installed_probe(disk, &update.layout,
          update.revision, &reason);
      if (probe == INSTALL_INSTALLED_REFUSED) {
        fprintf(stderr, "init-install: disk %llu pool %llu ESP %llu Update refused: %s\n",
            (unsigned long long)disk->info.id, (unsigned long long)update.layout.pool_partition,
            (unsigned long long)update.layout.esp_partition, reason ? reason : "readiness refused");
        continue;
      }
      pending = probe == INSTALL_INSTALLED_PENDING;
#endif
      if (!append_candidate(inventory, disk, &update, true, false, pending)) {
        fputs("init-install: out of memory recording Update targets\n", stderr);
        goto done;
      }
    }
  }
  success = true;
done:
  /* A failed preparation may still retain native views. Never launch a child
   * while any of those views remain uncertain or await retirement. */
  for (size_t i = 0; i < review.esp_count; ++i) {
    if (review.esps[i].admission_status == CALL_OUTCOME_UNKNOWN) {
      *clean = false;
    }
  }
  if (!install_review_destroy(&review)) {
    fputs("init-install: Update inspection cleanup incomplete; installer not launched\n", stderr);
    *clean = false;
    success = false;
  }
  free(facts);
  return success;
}

static bool inspect_disk(struct update_inventory *inventory, const struct install_disk *disk,
    handle_t disks, handle_t clock, bool *clean)
{
  struct install_update update;
  if (!install_scan_update(disk, &update)) {
    fprintf(stderr, "init-install: disk %llu Update inspection failed: %s\n",
        (unsigned long long)disk->info.id, update.reason ? update.reason : "unavailable");
    return false;
  }
  if (!update.eligible) {
    fprintf(stderr, "init-install: disk %llu classic Update unavailable: %s\n",
        (unsigned long long)disk->info.id, update.reason ? update.reason : "native layout not eligible");
    return inspect_coexist(inventory, disk, disks, clock, clean);
  }
  /* A recognized classic layout retains classic recovery and its exact grant.
   * An ESP refusal never falls back to the coexistence profile. */
#if INSTALLER_PUBLICATION_SCRATCH
  enum install_installed_probe probe = install_installed_probe(disk, &update.layout,
      update.revision, &update.reason);
  if (probe == INSTALL_INSTALLED_PENDING) {
    return append_candidate(inventory, disk, &update, false, false, true);
  }
  if (probe == INSTALL_INSTALLED_REFUSED) {
    fprintf(stderr, "init-install: disk %llu classic Update refused: %s\n",
        (unsigned long long)disk->info.id, update.reason ? update.reason : "readiness refused");
    return true;
  }
#endif
  enum install_esp_state state = install_esp_inspect(disk, &update.layout,
      update.revision, &update.reason);
  if (state == INSTALL_ESP_REFUSED) {
    fprintf(stderr, "init-install: disk %llu classic Update refused: %s\n",
        (unsigned long long)disk->info.id, update.reason ? update.reason : "ESP not eligible");
    return true;
  }
  return append_candidate(inventory, disk, &update, false, state == INSTALL_ESP_REBUILD, false);
}

bool installer_update_choose(struct terminal *terminal, handle_t disks, handle_t clock,
    handle_t random, struct installer_update_choice *choice, bool *back, bool *clean)
{
  struct update_inventory inventory = {0};
  bool success = false;
  bool hashing = hash_init(clock, random);
  *back = false;
  *clean = true;
  *choice = (struct installer_update_choice){0};
  if (!hashing) {
    fputs("init-install: cannot initialize Update inspection\n", stderr);
    goto done;
  }
  puts("Discovering existing Pyxis installations (read-only)...");
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    enum call_status status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      break;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "init-install: disk inventory incomplete (status %u)\n", status);
      goto done;
    }
    struct install_disk disk = {.handle = HANDLE_INVALID};
    status = disks_open(disks, info.id, DISK_ACCESS_READ_ONLY, &disk.handle);
    if (status != CALL_OK) {
      fprintf(stderr, "init-install: disk %llu unavailable for read-only inspection (status %u)\n",
          (unsigned long long)info.id, status);
      continue;
    }
    status = disk_get_info(disk.handle, &disk.info);
    bool inspected = false;
    if (status == CALL_OK && disk.info.id == info.id && disk.info.block_size &&
        disk.info.block_count <= UINT64_MAX / disk.info.block_size) {
      disk.bytes = disk.info.block_count * disk.info.block_size;
      inspected = inspect_disk(&inventory, &disk, disks, clock, clean);
    } else {
      fprintf(stderr, "init-install: disk %llu identity or geometry inspection failed (status %u)\n",
          (unsigned long long)info.id, status);
    }
    if (handle_close(disk.handle) != 0) {
      fputs("init-install: read-only disk close failed; installer not launched\n", stderr);
      *clean = false;
      goto done;
    }
    if (!inspected) {
      fputs("init-install: Update inspection failed; returning to installer actions\n", stderr);
      goto done;
    }
  }
  if (!inventory.count) {
    puts("No eligible existing Pyxis installation was found.");
    *back = true;
    success = true;
    goto done;
  }
  if (inventory.count > SIZE_MAX / sizeof(char *) - 1) {
    goto done;
  }
  const char **labels = calloc(inventory.count + 1, sizeof(*labels));
  if (!labels) {
    goto done;
  }
  for (size_t i = 0; i < inventory.count; ++i) {
    labels[i] = inventory.candidates[i].label;
  }
  labels[inventory.count] = "Back";
  if (fflush(stdout) == 0 && fflush(stderr) == 0) {
    struct term_choice_result result = term_read_choice(terminal,
        "Choose the existing installation to update", labels, inventory.count + 1, 0);
    if (result.status == TERM_LINE_OK && result.selected < inventory.count) {
      *choice = inventory.candidates[result.selected].choice;
      success = true;
    } else if (result.status == TERM_LINE_CANCELLED ||
        (result.status == TERM_LINE_OK && result.selected == inventory.count)) {
      *back = true;
      success = true;
    }
  }
  free(labels);
done:
  hash_close();
  free(inventory.candidates);
  return success;
}
