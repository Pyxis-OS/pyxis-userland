#include "../common/bootstrap_environment.h"
#include "plan.h"
#include "battery.h"
#include "coexist_ui.h"
#include "publication_frontend.h"
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
  for (;;) {
    struct term_line_result result = term_read_line_default(terminal, prompt, initial, line, capacity);
    if (result.status != TERM_LINE_OK) {
      say("Cancelled. Nothing was written.");
      return false;
    }
    if (!result.limit_reached) {
      return true;
    }
    say("Input was too long; enter the value again.");
  }
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
  const char *const choices[] = {"Restart", "Remain here"};
  struct term_choice_result selected = term_read_choice(terminal, "Restart: ", choices, 2, 0);
  if (selected.status != TERM_LINE_OK || selected.selected != 0) {
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
  const char *const choices[] = {"Install", "Update", "Replay a printed plan", "Cancel"};
  struct term_choice_result selected = term_read_choice(terminal, "Action: ", choices, 4, 0);
  if (selected.status != TERM_LINE_OK || selected.selected == 3) {
    return false;
  }
  *replay = selected.selected == 2;
  *updating = selected.selected == 1;
  return true;
}

static bool choose_mode(struct terminal *terminal, bool *read_the_room)
{
  const char *const choices[] = {"Proceed with marked media", "Read the room", "Cancel"};
  struct term_choice_result selected = term_read_choice(terminal, "Install consent: ", choices, 3, 0);
  if (selected.status != TERM_LINE_OK || selected.selected == 2) {
    return false;
  }
  *read_the_room = selected.selected == 1;
  return true;
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
  if (eligible == SIZE_MAX || eligible + 1 > SIZE_MAX / sizeof(char *) ||
      eligible > SIZE_MAX / sizeof(size_t)) {
    return NULL;
  }
  char **labels = calloc(eligible + 1, sizeof(*labels));
  size_t *indices = malloc(eligible * sizeof(*indices));
  struct target *selected = NULL;
  if (!labels || !indices) {
    goto done;
  }
  size_t row = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!(updating ? targets[i].update.eligible : targets[i].consent.eligible)) {
      continue;
    }
    labels[row] = malloc(96);
    if (!labels[row]) {
      goto done;
    }
    snprintf(labels[row], 96, "Disk %llu (%llu MiB)",
        (unsigned long long)targets[i].info.id,
        (unsigned long long)(targets[i].info.block_count * targets[i].info.block_size / INSTALL_MIB));
    indices[row++] = i;
  }
  labels[eligible] = "Cancel";
  struct term_choice_result choice = term_read_choice(terminal, "Target disk: ",
      (const char *const *)labels, eligible + 1, 0);
  if (choice.status == TERM_LINE_OK && choice.selected < eligible) {
    selected = &targets[indices[choice.selected]];
  }
done:
  if (labels) {
    for (size_t i = 0; i < eligible; ++i) {
      free(labels[i]);
    }
  }
  free(labels);
  free(indices);
  return selected;
}

static bool show_selected_target(const struct target *selected)
{
  install_progress_pause();
  char guid[37];
  install_guid_text(selected->info.gpt_guid, guid);
  return fprintf(stderr, "Selected target: Disk %llu (%llu MiB), GUID %s.\n",
      (unsigned long long)selected->info.id,
      (unsigned long long)(selected->info.block_count * selected->info.block_size / INSTALL_MIB),
      npfs_id_valid(selected->info.gpt_guid) ? guid : "unavailable") >= 0 && !ferror(stderr);
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

static bool new_attempt(handle_t clock, handle_t random, char attempt[33])
{
  uint64_t now;
  uint8_t bytes[16];
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - RANDOM_MAX_WAIT_NS ||
      random_read(random, bytes, sizeof(bytes), now + RANDOM_MAX_WAIT_NS) != CALL_OK) {
    return false;
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    attempt[i * 2] = digits[bytes[i] >> 4];
    attempt[i * 2 + 1] = digits[bytes[i] & 15];
  }
  attempt[32] = '\0';
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

static bool coexist_replay_target(handle_t disks, const struct install_plan_record *record,
    struct disk_info *target)
{
  bool found = false;
  *target = (struct disk_info){0};
  for (uint64_t index = 0;; ++index) {
    struct disk_info info;
    enum call_status status = disks_enumerate(disks, index, &info);
    if (status == CALL_NOT_FOUND) {
      break;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "installer: replay inventory unavailable (status %u)\n", status);
      return false;
    }
    if (memcmp(info.gpt_guid, record->existing_guid, 16) ||
        info.block_size != record->block_size || info.block_count != record->block_count) {
      continue;
    }
    if (found || info.gpt_status != DISK_GPT_HEALTHY) {
      say("installer: coexistence replay identity is ambiguous or lacks healthy GPT");
      return false;
    }
    *target = info;
    found = true;
  }
  if (!found) {
    say("installer: coexistence replay disk GUID and geometry are absent");
  }
  return found;
}

static bool profile_matches(handle_t disks, bool coexist)
{
  uint64_t rights, transport;
  uint64_t required = DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_OPEN_READ_ONLY |
      DISKS_RIGHT_OPEN_POOL;
#if INSTALLER_PUBLICATION_SCRATCH
  required |= DISKS_RIGHT_OPEN_OWNED_ESP;
#else
  required |= DISKS_RIGHT_MOUNT_READ_ONLY | DISKS_RIGHT_MOUNT_READ_WRITE;
#endif
  required |= coexist ? DISKS_RIGHT_APPEND_POOL : DISKS_RIGHT_ERASE | DISKS_RIGHT_REBUILD_ESP;
  uint64_t forbidden = coexist ? DISKS_RIGHT_ERASE | DISKS_RIGHT_REBUILD_ESP |
      DISKS_RIGHT_OPEN | DISKS_RIGHT_OPEN_PARTITION : 0;
  if (handle_rights(disks, &rights, &transport) != CALL_OK || transport ||
      (rights & required) != required || (rights & forbidden)) {
    say("installer: selected behavior does not match delegated installer authority");
    return false;
  }
  return true;
}

static const char *consent_word(const struct install_plan_record *record)
{
  return record->updating ? "update" : record->coexist ? "append" : "wipe";
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
  bool print_only = false, dry_run = false, replay = false, verbose = false, coexist = false;
#if INSTALLER_PUBLICATION_SCRATCH
  bool scratch_publication = false;
#endif
  const char *explicit_consent = NULL, *selected_action = NULL, *selected_guid = NULL;
  uint64_t selected_target = 0, selected_esp = 0, selected_pool = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
      verbose = true;
#if INSTALLER_PUBLICATION_SCRATCH
    } else if (!strcmp(argv[i], "--scratch-publication")) {
      scratch_publication = true;
#endif
    } else if (!strncmp(argv[i], "--selected-action=", 18) && !selected_action) {
      selected_action = argv[i] + 18;
    } else if (!strncmp(argv[i], "--selected-target=", 18)) {
      if (selected_target || !decimal(argv[i] + 18, &selected_target) || !selected_target) {
        say("Invalid or repeated trusted target selection.");
        return EXIT_FAILURE;
      }
    } else if (!strncmp(argv[i], "--selected-esp=", 15)) {
      if (selected_esp || !decimal(argv[i] + 15, &selected_esp) || !selected_esp) {
        say("Invalid or repeated trusted esp selection.");
        return EXIT_FAILURE;
      }
    } else if (!strncmp(argv[i], "--selected-pool=", 16)) {
      if (selected_pool || !decimal(argv[i] + 16, &selected_pool) || !selected_pool) {
        say("Invalid or repeated trusted pool selection.");
        return EXIT_FAILURE;
      }
    } else if (!strncmp(argv[i], "--selected-disk-guid=", 21) && !selected_guid) {
      selected_guid = argv[i] + 21;
    } else if (!strcmp(argv[i], "--plan")) {
      print_only = true;
    } else if (!strcmp(argv[i], "--dry-run")) {
      dry_run = true;
    } else if (!strcmp(argv[i], "--apply")) {
      replay = true;
    } else if (!strcmp(argv[i], "--coexist") && !coexist) {
      coexist = true;
    } else if (!strncmp(argv[i], "--confirm=", 10) && !explicit_consent) {
      explicit_consent = argv[i] + 10;
    } else {
      fputs("usage: installer [--coexist] [-v | --verbose] [--plan | --apply] [--dry-run] [--confirm=wipe|append|update]\n", stderr);
      return EXIT_FAILURE;
    }
  }
  bool selected_update = selected_action && !strcmp(selected_action, "update");
  bool selected_install = selected_action && !strcmp(selected_action, "install");
  bool any_target = selected_target || selected_esp || selected_pool || selected_guid;
  if ((selected_action && !selected_update && !selected_install) ||
      (selected_update && (!selected_target || !selected_esp || !selected_pool ||
        !selected_guid || strlen(selected_guid) != 36)) ||
      (any_target && !selected_update) || (selected_action && replay)) {
    say("installer: invalid startup selection; nothing was written");
    return EXIT_FAILURE;
  }
  if ((print_only && replay) || (explicit_consent &&
        (!replay || print_only || dry_run ||
          (strcmp(explicit_consent, "wipe") && strcmp(explicit_consent, "append") &&
              strcmp(explicit_consent, "update"))))) {
    fputs("installer: invalid plan/apply options\n", stderr);
    return EXIT_FAILURE;
  }
#if INSTALLER_PUBLICATION_SCRATCH
  if (scratch_publication) {
    if (print_only || dry_run || replay || explicit_consent) {
      fputs("installer: scratch publication requires its own Review\n", stderr);
      return EXIT_FAILURE;
    }
    struct terminal terminal = {.input = bootstrap_stdio(ENVIRONMENT_STDIN).handle,
      .output = bootstrap_stdio(ENVIRONMENT_STDOUT).handle};
    return install_publication_scratch(&terminal, verbose);
  }
#endif
  struct terminal terminal = {
    .input = bootstrap_capability("input"), .output = bootstrap_capability("output"),
  };
  handle_t disks = bootstrap_capability("disks"), boot = bootstrap_capability("boot");
  handle_t clock = bootstrap_capability("clock"), random = bootstrap_capability("random");
  handle_t kernel = bootstrap_capability("boot_kernel"), archive = bootstrap_capability("boot_archive");
  handle_t payload_workspace = bootstrap_capability("config_snapshot_parent");
  struct install_plan plan = {.disk.handle = HANDLE_INVALID,
    .efi.handle = HANDLE_INVALID};
  struct install_plan_record expected = {0};
  struct install_coexist_choice coexist_choice = {0};
  struct target coexist_target = {0};
  struct target *targets = NULL;
  size_t count = 0;
  bool updating = selected_update, read_the_room = false, crypto = false;
  int result = EXIT_FAILURE;
  if (disks == HANDLE_INVALID || boot == HANDLE_INVALID || clock == HANDLE_INVALID ||
      random == HANDLE_INVALID || kernel == HANDLE_INVALID || archive == HANDLE_INVALID ||
      terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID ||
      payload_workspace == HANDLE_INVALID) {
    fputs("installer: missing install-mode authority\n", stderr);
    goto done;
  }
  if (!profile_matches(disks, coexist)) {
    goto done;
  }
  if (!install_battery_check(bootstrap_capability("system_info"), stderr)) {
    goto done;
  }
  if (!replay && !selected_action && !choose_action(&terminal, &updating, &replay)) {
    goto done;
  }
  if (replay) {
    say("Supply one framed printed plan on standard input (ending with end).");
    if (!install_plan_record_parse(stdin, &expected)) {
      fputs("installer: invalid, incomplete or unsupported plan; nothing was written\n", stderr);
      goto done;
    }
    if (expected.coexist != coexist ||
        (explicit_consent && strcmp(explicit_consent, consent_word(&expected)))) {
      say("installer: printed plan profile or confirmation does not match this installer invocation");
      goto done;
    }
    updating = expected.updating;
    read_the_room = expected.read_the_room;
  } else if (!updating && !coexist && !choose_mode(&terminal, &read_the_room)) {
    goto done;
  }
  install_progress_plain_stream(clock, stderr, bootstrap_capability("installer_diagnostics"),
      verbose || print_only);
  install_progress_begin("Initializing revision hashing", INSTALL_PROGRESS_NONE, NULL);
  crypto = hash_init(clock, random);
  if (!crypto || !install_plan_sources(&plan, disks, boot, kernel, archive, clock,
        bootstrap_capability("system_info"))) {
    install_progress_pause();
    fputs("installer: sources or inventory unavailable; nothing was written\n", stderr);
    goto done;
  }
#if INSTALLER_PUBLICATION_SCRATCH
  plan.workspace = payload_workspace;
#endif
  char revision[64];
  strcpy(revision, plan.record.revision);
  install_progress_pause();
  struct target *selected;
  if (coexist) {
    if (replay) {
      if (!coexist_replay_target(disks, &expected, &coexist_target.info)) {
        goto done;
      }
    } else if (selected_update) {
      if (!install_coexist_ui_selected(disks, selected_target, selected_esp, selected_pool,
          selected_guid, &coexist_choice)) {
        fprintf(stderr, "installer: selected installation changed or is unavailable: %s\n",
            coexist_choice.reason ? coexist_choice.reason : "selection refused");
        goto done;
      }
      coexist_target.info = coexist_choice.info;
      coexist_target.update = coexist_choice.update;
    } else {
      struct install_layout fresh_ids = {0};
      if (!updating && !new_ids(clock, random, &fresh_ids,
          plan.record.pool_id, plan.record.volume_id)) {
        goto done;
      }
      if (!install_coexist_ui_choose(&terminal, disks, updating,
          updating ? NULL : fresh_ids.pool_guid, &coexist_choice)) {
        fprintf(stderr, "installer: coexistence selection unavailable: %s\n",
            coexist_choice.reason ? coexist_choice.reason : "selection failed");
        goto done;
      }
      coexist_target.info = coexist_choice.info;
      coexist_target.update = coexist_choice.update;
    }
    selected = &coexist_target;
  } else {
    if (!inventory(disks, read_the_room, updating, &targets, &count)) {
      say("installer: target inventory unavailable; nothing was written");
      goto done;
    }
    if (selected_update) {
      selected = NULL;
      for (size_t i = 0; i < count; ++i) {
        struct target *candidate = &targets[i];
        char guid[37];
        install_guid_text(candidate->info.gpt_guid, guid);
        if (candidate->info.id == selected_target && !strcmp(guid, selected_guid) &&
            candidate->update.eligible &&
            candidate->update.layout.esp_partition == selected_esp &&
            candidate->update.layout.pool_partition == selected_pool) {
          if (selected) {
            say("installer: selected installation is ambiguous; nothing was written");
            goto done;
          }
          selected = candidate;
        }
      }
      if (!selected) {
        say("installer: selected installation changed or is unavailable; nothing was written");
        goto done;
      }
    } else {
      selected = replay ? replay_target(targets, count, &expected) :
          choose_target(&terminal, targets, count, updating);
    }
  }
  if (!selected) {
    goto done;
  }
  if (replay) {
    plan.record = expected;
    strcpy(plan.record.revision, revision);
  } else {
    plan.record.updating = updating;
    plan.record.coexist = coexist;
    plan.record.read_the_room = read_the_room;
    plan.record.esp_rebuild = !coexist && updating && selected->boot_rebuild;
    memset(plan.record.attempt, '0', 32);
    plan.record.attempt[32] = '\0';
    if ((coexist || INSTALLER_PUBLICATION_SCRATCH) && !new_attempt(clock, random, plan.record.attempt)) {
      say("installer: fresh payload attempt identity unavailable; nothing was written");
      goto done;
    }
    plan.record.block_count = selected->info.block_count;
    plan.record.block_size = selected->info.block_size;
    const uint8_t *existing_guid = coexist ? coexist_choice.layout.disk_guid :
        updating ? selected->update.layout.disk_guid : selected->consent.existing_guid;
    memcpy(plan.record.existing_guid, existing_guid, 16);
    plan.record.portable = npfs_id_valid(existing_guid);
    if (updating) {
      plan.record.layout = selected->update.layout;
      strcpy(plan.record.previous, selected->update.revision);
    } else {
      struct install_disk disk = {.info = selected->info,
        .bytes = selected->info.block_count * selected->info.block_size};
      if (coexist) {
        plan.record.layout = coexist_choice.layout;
      } else if (!install_layout_plan(&disk, &plan.record.layout) ||
          !new_ids(clock, random, &plan.record.layout, plan.record.pool_id, plan.record.volume_id)) {
        goto done;
      }
      if (!show_selected_target(selected) ||
          !choose_journal(&terminal, &plan.record.layout, plan.record.pool_id, &plan.header)) {
        goto done;
      }
      plan.record.journal_mib = plan.header.journal_blocks * NPFS_BLOCK_SIZE / INSTALL_MIB;
    }
  }
  bool prepared = install_plan_prepare(&plan, &selected->info);
  if (!replay && updating && !coexist && plan.record.esp_rebuild) {
    for (size_t i = 0; i < plan.target.review.esp_count; ++i) {
      const struct install_review_esp *esp = &plan.target.review.esps[i];
      uint64_t clean = MOUNT_STATE_CLEAN_SHUTDOWN | MOUNT_STATE_NO_HARD_ERROR;
      if (esp->selection.partition == plan.record.layout.esp_partition &&
          esp->available && esp->identity_matches && esp->retired &&
          (esp->observed.flags & clean) == clean) {
        say("Native ESP validation passed; keeping the existing filesystem.");
        plan.record.esp_rebuild = false;
        prepared = install_plan_prepare(&plan, &selected->info);
        break;
      }
    }
  }
  if (!prepared && plan.prior.needs_legacy && !replay && !coexist) {
    if (!show_selected_target(selected)) {
      goto done;
    }
    say("The previous revision has no valid manifest. Read and hash its programs once?");
    const char *const choices[] = {"Read and hash the previous programs", "Cancel"};
    struct term_choice_result decision = term_read_choice(&terminal, "Previous inventory: ",
        choices, 2, 1);
    if (decision.status != TERM_LINE_OK || decision.selected != 0) {
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
  plan.payload_request.workspace = payload_workspace;
  plan.classic_request.workspace = payload_workspace;
  install_progress_pause();
  if (!install_plan_report(&plan, stderr, verbose || print_only)) {
    goto done;
  }
  if (!print_only && !dry_run && !replay) {
    const char *const choices[] = {"Apply", "Print plan", "Dry-run", "Cancel"};
    struct term_choice_result decision = term_read_choice(&terminal, "Review action: ",
        choices, 4, 0);
    if (decision.status != TERM_LINE_OK || decision.selected == 3) {
      say("Cancelled. Nothing was written.");
      goto done;
    }
    print_only = decision.selected == 1;
    dry_run = decision.selected == 2;
    if (print_only && !verbose && !install_plan_report(&plan, stderr, true)) {
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
  const char *required_consent = consent_word(&plan.record);
  if (!explicit_consent) {
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Type %s to confirm this plan: ", required_consent);
    for (;;) {
      if (!read_line(&terminal, prompt, "", confirmation, sizeof(confirmation))) {
        goto done;
      }
      if (!strcmp(confirmation, required_consent)) {
        break;
      }
      say("Confirmation did not match; enter the displayed word, or Ctrl+C to cancel.");
    }
    explicit_consent = confirmation;
  }
  if (strcmp(explicit_consent, required_consent)) {
    say("installer: confirmation does not match the frozen plan's action; nothing was written");
    goto done;
  }
  if (!install_plan_execute(&plan, explicit_consent)) {
    goto done;
  }
  result = EXIT_SUCCESS;
  if (interactive_consent && !coexist) {
    offer_restart(&terminal, bootstrap_capability("power"), updating);
  }

done:
  install_progress_finish();
  install_plan_destroy(&plan);
  destroy_targets(targets, count);
  if (coexist_choice.pending_inspection != HANDLE_INVALID &&
      handle_close(coexist_choice.pending_inspection) != 0) {
    say("installer: pending read-only inspection handle close failed");
    result = EXIT_FAILURE;
  }
  if (crypto) {
    hash_close();
  }
  return result;
}
