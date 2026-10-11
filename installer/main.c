#include "plan.h"
#include "../xfer/hash.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <path.h>
#include <power.h>
#include <pyxis_fs/npfs.h>
#include <random.h>
#include <startup.h>
#include <system_info.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#define JOURNAL_DEFAULT_MIN_MIB UINT64_C(8)
#define JOURNAL_MAX_MIB UINT64_C(1024)

static void say(const char *text)
{
  fprintf(stderr, "%s\n", text);
}

struct target {
  struct disk_info info;
  struct install_consent consent;
  struct install_update update;
  bool boot_rebuild;
};

static bool read_line(struct terminal *terminal, const char *prompt, const char *initial,
    char *line, size_t capacity)
{
  struct term_line_result result = term_read_line_initial(terminal, prompt, initial, line, capacity);
  if (result.status != TERM_LINE_OK) {
    say("Cancelled. Nothing was written.");
    return false;
  }
  if (result.limit_reached) {
    say("Input was too long; enter the value again.");
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

/* Offered after the final success message only. Restart flushes the mounted
 * pools. A failed call leaves the system running and the installation done. */
static void offer_restart(struct terminal *terminal, handle_t power, bool updating)
{
  if (power == HANDLE_INVALID) {
    return;
  }
  fprintf(stderr, "Remove the %s medium before the machine restarts.\n", updating ? "live" : "install");
  char line[16];
  struct term_line_result result = term_read_line(terminal,
      "Press Enter to restart, or type stay to remain here: ", line, sizeof(line));
  if (result.status != TERM_LINE_OK || result.length) {
    say("Not restarting.");
    return;
  }
  install_progress_begin("Restarting (includes pool sync)", INSTALL_PROGRESS_NONE, NULL);
  enum call_status status = power_restart(power);
  install_progress_finish();
  fprintf(stderr, "installer: restart failed (status %u)\n"
      "The %s is complete; restart the machine yourself after removing the medium.\n",
      status, updating ? "update" : "installation");
}

static bool choose_action(struct terminal *terminal, bool *updating, bool *replay)
{
  say("1. Install\n2. Update\n3. Replay a printed plan");
  char line[16];
  for (;;) {
    if (!read_line(terminal, "Choice: ", "", line, sizeof(line))) {
      return false;
    }
    if (!strcmp(line, "1") || !strcmp(line, "2") || !strcmp(line, "3")) {
      *replay = line[0] == '3';
      *updating = line[0] == '2';
      return true;
    }
    say("Enter 1, 2 or 3.");
  }
}

static bool choose_mode(struct terminal *terminal, bool *read_the_room)
{
  say("1. Proceed with installation\n2. Read the room");
  char line[16];
  for (;;) {
    if (!read_line(terminal, "Choice: ", "", line, sizeof(line))) {
      return false;
    }
    if (!strcmp(line, "1") || !strcmp(line, "2")) {
      *read_the_room = line[0] == '2';
      return true;
    }
    say("Enter 1 or 2.");
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

static bool inspect_update(const struct install_disk *disk, struct install_update *update,
    bool *boot_rebuild)
{
  *boot_rebuild = false;
  if (!install_scan_update(disk, update)) {
    return false;
  }
  if (!update->eligible) {
    return true;
  }
  enum install_esp_state state = install_esp_inspect(disk, &update->layout,
      update->revision, &update->reason);
  update->eligible = state != INSTALL_ESP_REFUSED;
  *boot_rebuild = state == INSTALL_ESP_REBUILD;
  if (state == INSTALL_ESP_VALID) {
    update->reason = "recognized Pyxis installation";
  }
  return true;
}

static bool inventory(handle_t disks, bool read_the_room, bool updating, struct target **output,
    size_t *count)
{
  *output = NULL;
  *count = 0;
  for (uint64_t index = 0;; ++index) {
    install_progress_begin("Enumerating target disks", INSTALL_PROGRESS_NONE, NULL);
    struct disk_info info;
    enum call_status status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      return true;
    }
    if (status != CALL_OK || *count == SIZE_MAX / sizeof(**output)) {
      install_progress_pause();
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
    const char **reason_out = updating ? &target->update.reason : &target->consent.reason;
    const char *reason = unavailable_reason(&info);
    if (reason) {
      *reason_out = reason;
      continue;
    }
    struct install_disk disk = {.info = info, .bytes = info.block_count * info.block_size};
    struct install_layout layout = {0};
    struct npfs_header header;
    const uint8_t placeholder_id[16] = {1};
    if (!updating && (!install_layout_plan(&disk, &layout) ||
        !pool_header(&layout, 1, placeholder_id, &header))) {
      *reason_out = "too small for a 512 MiB ESP and writable pool";
      continue;
    }
    install_progress_begin("Opening disk for read-only inspection", INSTALL_PROGRESS_NONE, NULL);
    status = disks_open(disks, info.id, DISK_ACCESS_READ_ONLY, &disk.handle);
    if (status != CALL_OK) {
      *reason_out = "raw inspection unavailable";
      continue;
    }
    bool inspected;
    if (updating) {
      inspected = inspect_update(&disk, &target->update, &target->boot_rebuild);
    } else {
      inspected = install_scan_consent(&disk, read_the_room, &target->consent);
    }
    install_progress_begin("Closing inspected disk handle", INSTALL_PROGRESS_NONE, NULL);
    bool closed = handle_close(disk.handle) == 0;
    if (!inspected || !closed) {
      if (updating) {
        target->update.eligible = false;
      } else {
        target->consent.eligible = false;
      }
      *reason_out = "inspection failed; eligibility cannot be established";
    }
  }
}

static struct target *choose_target(struct terminal *terminal, struct target *targets,
    size_t count, bool updating)
{
  size_t eligible = 0, single = 0;
  for (size_t i = 0; i < count; ++i) {
    struct target *target = &targets[i];
    bool offered = updating ? target->update.eligible : target->consent.eligible;
    bool degraded = updating ? target->update.degraded : target->consent.degraded;
    const char *reason = updating ? target->update.reason : target->consent.reason;
    char guid[37];
    install_guid_text(target->info.gpt_guid, guid);
    fprintf(stderr, "%zu. Disk %llu: %llu MiB, GUID %s; %s: ", i + 1,
        (unsigned long long)target->info.id,
        (unsigned long long)(target->info.block_count * target->info.block_size / INSTALL_MIB),
        npfs_id_valid(target->info.gpt_guid) ? guid : "unavailable",
        offered ? "eligible" : "ineligible");
    if (updating && offered && target->boot_rebuild) {
      fprintf(stderr, "Pyxis installation (boot files damaged: %s)", reason);
    } else {
      fputs(reason, stderr);
    }
    fprintf(stderr, "%s\n", degraded ? " (degraded metadata)" : "");
    if (updating && offered) {
      fprintf(stderr, "   Installed kernel revision: %s\n", target->update.revision);
      if (target->boot_rebuild) {
        say("   Boot files damaged or missing; Update will rebuild them.");
      }
    }
    if (offered) {
      ++eligible;
      single = i;
    }
  }
  if (!eligible) {
    say("No eligible disks. Nothing was written.");
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
        (updating ? targets[number - 1].update.eligible : targets[number - 1].consent.eligible)) {
      return &targets[number - 1];
    }
    say("Enter the number of an eligible disk.");
  }
}

static bool new_ids(handle_t clock, handle_t random, struct install_layout *layout,
    uint8_t pool_id[16], uint8_t volume_id[16])
{
  install_progress_begin("Obtaining fresh disk and volume identities", INSTALL_PROGRESS_NONE, NULL);
  uint64_t now;
  uint8_t bytes[80];
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - RANDOM_MAX_WAIT_NS ||
      random_read(random, bytes, sizeof(bytes), now + RANDOM_MAX_WAIT_NS) != CALL_OK) {
    install_progress_pause();
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
    say("Enter a size from 1 to 1024 MiB that leaves room for pool metadata and files.");
  }
}

static struct target *replay_target(struct target *targets, size_t count,
    const struct install_plan_record *record)
{
  if (!record->portable) {
    fputs("installer: this target has no portable identity; use interactive planning\n", stderr);
    return NULL;
  }
  struct target *match = NULL;
  for (size_t i = 0; i < count; i++) {
    const struct disk_info *info = &targets[i].info;
    if (memcmp(info->gpt_guid, record->existing_guid, 16)) {
      continue;
    }
    if (match || (info->gpt_status != DISK_GPT_HEALTHY && info->gpt_status != DISK_GPT_DEGRADED)) {
      fputs("installer: replay target identity is ambiguous or unavailable\n", stderr);
      return NULL;
    }
    match = &targets[i];
  }
  bool eligible = match && (record->updating ? match->update.eligible : match->consent.eligible);
  if (!eligible) {
    fputs("installer: reviewed target is absent or no longer eligible\n", stderr);
    return NULL;
  }
  return match;
}

static bool record_matches(const struct install_plan_record *expected,
    const struct install_plan_record *actual)
{
  return !strcmp(expected->revision, actual->revision) &&
      !strcmp(expected->source_hash, actual->source_hash) &&
      !strcmp(expected->target_hash, actual->target_hash);
}

int main(int argc, char **argv)
{
  bool print_only = false, dry_run = false, replay = false;
  const char *explicit_consent = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--plan")) {
      print_only = true;
    } else if (!strcmp(argv[i], "--dry-run")) {
      dry_run = true;
    } else if (!strcmp(argv[i], "--apply")) {
      replay = true;
    } else if (!strncmp(argv[i], "--confirm=", 10) && !explicit_consent) {
      explicit_consent = argv[i] + 10;
    } else {
      fputs("usage: installer [--plan | --apply] [--dry-run] [--confirm=wipe|update]\n", stderr);
      return EXIT_FAILURE;
    }
  }
  if ((print_only && replay) || (explicit_consent &&
        (!replay || print_only || dry_run ||
          (strcmp(explicit_consent, "wipe") && strcmp(explicit_consent, "update"))))) {
    fputs("installer: invalid plan/apply options\n", stderr);
    return EXIT_FAILURE;
  }
  struct terminal terminal = {
    .input = startup_resource("input"), .output = startup_resource("output"),
  };
  handle_t disks = startup_resource("disks"), boot = startup_root("boot");
  handle_t clock = startup_resource("clock"), random = startup_resource("random");
  handle_t kernel = startup_resource("boot_kernel"), archive = startup_resource("boot_archive");
  struct install_plan plan = {.disk.handle = HANDLE_INVALID,
    .efi.handle = HANDLE_INVALID, .template.handle = HANDLE_INVALID};
  struct install_plan_record expected = {0};
  struct target *targets = NULL;
  size_t count = 0;
  bool updating = false, read_the_room = false, crypto = false;
  int result = EXIT_FAILURE;
  if (disks == HANDLE_INVALID || boot == HANDLE_INVALID || clock == HANDLE_INVALID ||
      random == HANDLE_INVALID || kernel == HANDLE_INVALID || archive == HANDLE_INVALID ||
      terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID) {
    fputs("installer: missing install-mode authority\n", stderr);
    goto done;
  }
  if (!replay && !choose_action(&terminal, &updating, &replay)) {
    goto done;
  }
  if (replay) {
    say("Supply one framed printed plan on standard input (ending with end).");
    if (!install_plan_record_parse(stdin, &expected)) {
      fputs("installer: invalid, incomplete or unsupported plan; nothing was written\n", stderr);
      goto done;
    }
    updating = expected.updating;
    read_the_room = expected.read_the_room;
  } else if (!updating && !choose_mode(&terminal, &read_the_room)) {
    goto done;
  }
  install_progress_plain_stream(clock, stderr);
  install_progress_begin("Initializing revision hashing", INSTALL_PROGRESS_NONE, NULL);
  crypto = hash_init(clock, random);
  if (!crypto || !install_plan_sources(&plan, disks, boot, kernel, archive, clock,
        startup_resource("system_info")) ||
      !inventory(disks, read_the_room, updating, &targets, &count)) {
    fputs("installer: sources or inventory unavailable; nothing was written\n", stderr);
    goto done;
  }
  char revision[64];
  strcpy(revision, plan.record.revision);
  struct target *selected = replay ? replay_target(targets, count, &expected) :
      choose_target(&terminal, targets, count, updating);
  if (!selected) {
    goto done;
  }
  if (replay) {
    plan.record = expected;
    strcpy(plan.record.revision, revision);
  } else {
    plan.record.updating = updating;
    plan.record.read_the_room = read_the_room;
    plan.record.block_count = selected->info.block_count;
    plan.record.block_size = selected->info.block_size;
    const uint8_t *existing_guid = updating ? selected->update.layout.disk_guid :
        selected->consent.existing_guid;
    memcpy(plan.record.existing_guid, existing_guid, 16);
    plan.record.portable = npfs_id_valid(existing_guid);
    if (updating) {
      plan.record.layout = selected->update.layout;
      strcpy(plan.record.previous, selected->update.revision);
    } else {
      struct install_disk disk = {.info = selected->info,
        .bytes = selected->info.block_count * selected->info.block_size};
      if (!install_layout_plan(&disk, &plan.record.layout) ||
          !new_ids(clock, random, &plan.record.layout, plan.record.pool_id, plan.record.volume_id) ||
          !choose_journal(&terminal, &plan.record.layout, plan.record.pool_id, &plan.header)) {
        goto done;
      }
      plan.record.journal_mib = plan.header.journal_blocks * NPFS_BLOCK_SIZE / INSTALL_MIB;
    }
  }
  bool prepared = install_plan_prepare(&plan, &selected->info);
  if (!prepared && plan.prior.needs_legacy && !replay) {
    install_progress_pause();
    say("The previous revision has no valid manifest. Read and hash its programs once?");
    char choice[16];
    if (!read_line(&terminal, "Type scan to build a read-only inventory, or cancel: ",
          "", choice, sizeof(choice)) || strcmp(choice, "scan")) {
      goto done;
    }
    plan.record.legacy = true;
    prepared = install_plan_prepare(&plan, &selected->info);
  }
  if (!prepared || (replay && !record_matches(&expected, &plan.record))) {
    install_progress_pause();
    fputs("installer: plan cannot be prepared, no longer matches, or exceeds capacity;\n"
        "nothing was written. Build and review a new plan.\n", stderr);
    goto done;
  }
  if (!install_plan_report(&plan, stderr)) {
    goto done;
  }
  if (!print_only && !dry_run && !replay) {
    char action[16];
    if (!read_line(&terminal, "Review action: 1 apply, 2 print plan, 3 dry-run: ",
          "1", action, sizeof(action))) {
      goto done;
    }
    if (!strcmp(action, "2")) {
      print_only = true;
    } else if (!strcmp(action, "3")) {
      dry_run = true;
    } else if (strcmp(action, "1")) {
      say("Cancelled. Nothing was written.");
      goto done;
    }
  }
  if (print_only) {
    result = install_plan_record_format(stdout, &plan.record) ? EXIT_SUCCESS : EXIT_FAILURE;
    goto done;
  }
  if (dry_run) {
    say("Dry-run complete. Nothing was written.");
    result = EXIT_SUCCESS;
    goto done;
  }
  char confirmation[16];
  bool interactive_consent = !explicit_consent;
  if (!explicit_consent) {
    if (!read_line(&terminal, updating ? "Type update to confirm this plan: " :
          "Type wipe to confirm this plan: ", "", confirmation, sizeof(confirmation))) {
      goto done;
    }
    explicit_consent = confirmation;
  }
  if (!install_plan_execute(&plan, explicit_consent)) {
    goto done;
  }
  result = EXIT_SUCCESS;
  if (interactive_consent) {
    offer_restart(&terminal, startup_resource("power"), updating);
  }

done:
  install_progress_finish();
  install_plan_destroy(&plan);
  destroy_targets(targets, count);
  if (crypto) {
    hash_close();
  }
  return result;
}
