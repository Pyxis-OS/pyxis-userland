#include "../common/bootstrap_environment.h"
#include "context.h"
#include "render.h"
#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <random.h>
#include <startup.h>
#include <system_info.h>
#include <literal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define WRITE_RIGHTS (READ_RIGHTS | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_WRITE_FILES)

static bool rights(handle_t handle, uint64_t required)
{
  struct handle_info info;
  return handle != HANDLE_INVALID && handle_query(handle, &info) == CALL_OK &&
      !(required & ~info.rights);
}

static bool read_source(handle_t parent, const char *name, struct config_blob *blob)
{
  handle_t file;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file);
  if (status != CALL_OK) {
    fprintf(stderr, "config: source %s unavailable (status %u)\n", name, status);
    return false;
  }
  uint64_t size, final_size;
  status = file_size(file, &size);
  if (status != CALL_OK || size > CONFIG_FILE_MAX_BYTES) {
    handle_close(file);
    fprintf(stderr, "config: source %s size unavailable or exceeds 1 MiB\n", name);
    return false;
  }
  unsigned char *bytes = malloc((size_t)size + 1);
  bool ok = bytes != NULL;
  size_t offset = 0;
  while (ok && offset < size) {
    size_t count;
    status = file_read(file, offset, bytes + offset, (size_t)size - offset, &count);
    ok = status == CALL_OK && count != 0;
    offset += ok ? count : 0;
  }
  ok = ok && file_size(file, &final_size) == CALL_OK && size == final_size &&
      config_blob_set(blob, bytes, (size_t)size);
  if (handle_close(file) != CALL_OK) {
    ok = false;
  }
  free(bytes);
  return ok;
}

static void guid_text(const uint8_t guid[16], char output[37])
{
  static const uint8_t order[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  static const char hex[] = "0123456789abcdef";
  size_t next = 0;
  for (size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      output[next++] = '-';
    }
    output[next++] = hex[guid[order[i]] >> 4];
    output[next++] = hex[guid[order[i]] & 15];
  }
  output[next] = '\0';
}

static bool observe_target(struct config_editor_context *context, struct config_target_facts *facts)
{
  memset(facts, 0, sizeof(*facts));
  struct mount_state_reply state;
  if (mount_observe(context->view.control, &state) != CALL_OK ||
      (state.flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_FAILED))) {
    return false;
  }
  if (!(state.flags & MOUNT_STATE_IDENTITY_VALID)) {
    return false;
  }
  *facts = (struct config_target_facts){
    .valid = true, .domain = state.domain, .device = state.device,
    .partition = state.partition, .first_block = state.first_block,
    .block_count = state.block_count, .device_block_size = state.device_block_size,
    .device_block_count = state.device_block_count, .partition_attributes = state.partition_attributes,
  };
  memcpy(facts->disk_guid, state.disk_guid, 16);
  memcpy(facts->partition_guid, state.partition_guid, 16);
  memcpy(facts->partition_type, state.partition_type, 16);
  struct file_info_reply metadata;
  if (directory_info(context->view.config_parent, &metadata) != CALL_OK ||
      !(metadata.valid & FILE_INFO_DOMAIN_VALID) || metadata.domain != facts->domain ||
      !config_target_valid(facts)) {
    memset(facts, 0, sizeof(*facts));
    return false;
  }
  if (context->view.limine_parent != HANDLE_INVALID &&
      (directory_info(context->view.limine_parent, &metadata) != CALL_OK ||
       !(metadata.valid & FILE_INFO_DOMAIN_VALID) || metadata.domain != facts->domain)) {
    memset(facts, 0, sizeof(*facts));
    return false;
  }
  return true;
}

static void describe_target(struct config_editor_context *context)
{
  char disk[37], partition[37], type[37];
  guid_text(context->target.disk_guid, disk);
  guid_text(context->target.partition_guid, partition);
  guid_text(context->target.partition_type, type);
  snprintf(context->target_text, sizeof(context->target_text),
      "disk=%s;esp=%s;type=%s;attributes=%llu;block-size=%llu;device-blocks=%llu;first=%llu;blocks=%llu",
      disk, partition, type, (unsigned long long)context->target.partition_attributes,
      (unsigned long long)context->target.device_block_size,
      (unsigned long long)context->target.device_block_count,
      (unsigned long long)context->target.first_block,
      (unsigned long long)context->target.block_count);
}

bool config_editor_load(struct config_editor_context *context)
{
  memset(context, 0, sizeof(*context));
  context->view = (struct config_storage_view){
    .config_parent = bootstrap_capability("config_read"),
    .limine_parent = bootstrap_capability("limine_read"),
    .control = bootstrap_capability("control"), .clock = bootstrap_capability("clock"),
  };
  handle_t config_write = bootstrap_capability("config_write"),
      limine_write = bootstrap_capability("limine_write"), writer = bootstrap_capability("writer");
  context->writer = rights(config_write, WRITE_RIGHTS) &&
      rights(writer, MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_WRITE | MOUNT_RIGHT_INTERVAL);
  if (context->writer) {
    context->view.config_parent = config_write;
    context->view.control = writer;
    if (rights(limine_write, WRITE_RIGHTS)) {
      context->view.limine_parent = limine_write;
    }
  }
  handle_t defaults = bootstrap_capability("defaults");
  const char *names[] = {"installed.lua", "session.lua", "network.lua"};
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (!read_source(defaults, names[i], &context->archive[i])) {
      return false;
    }
  }
  /* Source declarations are data, not executable migrations. Missing catalog
   * still permits archive validation; planning reports that it is unavailable. */
  if (read_source(defaults, "catalog.lua", &context->catalog_source)) {
    config_catalog_read("revision catalog", (char *)context->catalog_source.bytes,
        context->catalog_source.size, &context->catalog);
  }
  if (context->view.config_parent == HANDLE_INVALID) {
    fputs("config: no selected ESP view; validating archive defaults only\n", stderr);
    return true;
  }
  struct config_storage_report report = {0};
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (i == CONFIG_LIMINE && context->view.limine_parent == HANDLE_INVALID) {
      continue;
    }
    if (!config_storage_read(&context->view, i, &context->current[i], &report)) {
      fprintf(stderr, "config: read %s failed (status %u)\n", config_storage_file_name(i), report.status);
      return false;
    }
  }
  if (!config_storage_read_record(&context->view, &context->record, &report)) {
    return false;
  }
  struct config_record record;
  bool record_valid = context->record.digest.present &&
      config_record_parse(context->record.bytes, context->record.size, &record) && record.committed;
  context->complete = record_valid;
  for (unsigned i = 0; i < CONFIG_SET_FILES && context->complete; ++i) {
    context->complete = context->current[i].digest.present &&
        context->current[i].size == record.selected[i].size &&
        !strcmp(context->current[i].digest.sha256, record.selected[i].sha256);
  }
  struct config_blob record_again = {0};
  if (!config_storage_read_record(&context->view, &record_again, &report)) {
    return false;
  }
  record_valid &= config_blob_equal(&context->record, &record_again);
  context->complete &= record_valid;
  config_blob_destroy(&record_again);
  /* Current owner edits can invalidate completion without destroying the old
   * catalog's independently recorded default evidence. Never invent history. */
  if (record_valid && context->current[CONFIG_CATALOG].digest.present &&
      context->current[CONFIG_CATALOG].size == record.selected[CONFIG_CATALOG].size &&
      !strcmp(context->current[CONFIG_CATALOG].digest.sha256, record.selected[CONFIG_CATALOG].sha256)) {
    config_catalog_read("previous committed catalog", (char *)context->current[CONFIG_CATALOG].bytes,
        context->current[CONFIG_CATALOG].size, &context->old_catalog);
  }
  if (observe_target(context, &context->target)) {
    describe_target(context);
  } else {
    fputs("config: stable issuer-bound ESP facts unavailable; no publication or replay\n", stderr);
  }
  return true;
}

static void archive_bytes(const struct config_editor_context *context,
    struct config_bytes archive[CONFIG_LUA_COUNT])
{
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    archive[i] = (struct config_bytes){(char *)context->archive[i].bytes, context->archive[i].size};
  }
}

#define POOL_BOOT_PATH "system://config/boot.lua"

/* Installed boot reads spaces from the pool file, not the ESP copy, so check
 * that file the way boot does: declarative literal data over the archive
 * defaults. False only for a present file that boot would ignore. */
static bool validate_pool(const struct boot_config *defaults, struct boot_config *override,
    struct boot_plan *plan)
{
  FILE *file = fopen(POOL_BOOT_PATH, "rb");
  if (!file) {
    if (errno == ENOENT) {
      fputs("config: no " POOL_BOOT_PATH "; boot starts the stock spaces\n", stderr);
    } else {
      fputs("config: " POOL_BOOT_PATH ": not checked (no readable system:// root)\n", stderr);
    }
    return boot_plan_build(defaults, NULL, plan, NULL, NULL);
  }
  char *bytes = malloc(LITERAL_SOURCE_LIMIT + 1);
  size_t size = bytes ? fread(bytes, 1, LITERAL_SOURCE_LIMIT + 1, file) : 0;
  bool read = bytes && !ferror(file);
  fclose(file);
  struct config_failure failure = {0};
  bool ok = false;
  if (!read) {
    config_failure_format(&failure, "cannot read");
  } else if (size > LITERAL_SOURCE_LIMIT) {
    config_failure_format(&failure, "exceeds %d bytes", LITERAL_SOURCE_LIMIT);
  } else {
    ok = boot_config_read_literal_bytes_reason(POOL_BOOT_PATH, bytes, size, override,
        &failure) == BOOT_CONFIG_OK &&
        boot_plan_build(defaults, override, plan, NULL, &failure);
  }
  free(bytes);
  if (ok) {
    fputs("config: " POOL_BOOT_PATH " is valid; boot reads it for spaces\n", stderr);
    return true;
  }
  fprintf(stderr, "config: " POOL_BOOT_PATH " is invalid: %s; boot ignores it and starts "
      "the stock spaces\n", failure.text[0] ? failure.text : "invalid");
  boot_config_free(override);
  boot_plan_build(defaults, NULL, plan, NULL, NULL);
  return false;
}

bool config_editor_validate(struct config_editor_context *context, bool planned)
{
  struct config_bytes archive[CONFIG_LUA_COUNT], esp[CONFIG_LUA_COUNT];
  archive_bytes(context, archive);
  bool any = false, all = true;
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    bool present = planned ? context->plan.files[i].after_present : context->current[i].digest.present;
    esp[i] = (struct config_bytes){
      planned ? context->plan.files[i].after : (char *)context->current[i].bytes,
      planned ? context->plan.files[i].after_size : context->current[i].size,
    };
    any |= present;
    all &= present;
  }
  struct config_selection selection;
  bool ok;
  if (planned || any) {
    ok = all && config_selection_validate(archive, esp, &selection);
    if (ok) {
      fputs("config: ESP triple is statically valid\n", stderr);
    } else {
      /* Boot then uses the archive triple; the pool file still applies. */
      fputs("config: ESP triple is missing, unsupported or statically invalid\n", stderr);
      const char *reason;
      if (!config_selection_choose(archive, archive, false, false, &selection, &reason)) {
        goto preview;
      }
    }
  } else {
    const char *reason;
    ok = config_selection_choose(archive, esp, false, false, &selection, &reason);
    if (!ok) {
      return false;
    }
    fprintf(stderr, "config: %s\n", reason);
  }
  struct boot_config pool = {0};
  struct boot_plan spaces = {0};
  ok = validate_pool(&selection.defaults, &pool, &spaces) && ok;
  for (size_t i = 0; i < spaces.space_count; ++i) {
    fprintf(stderr, "config: space %s from %s, init %s\n", spaces.spaces[i]->name,
        spaces.space_sources[i] == BOOT_ENTRY_OVERRIDE ? "pool override" : "archive default",
        spaces.spaces[i]->init);
  }
  for (size_t i = 0; i < spaces.volume_count; ++i) {
    fprintf(stderr, "config: volume %s from %s\n", spaces.volumes[i]->name,
        spaces.volume_sources[i] == BOOT_ENTRY_OVERRIDE ? "pool override" : "archive default");
  }
  struct system_info_cpu cpu;
  if (system_info_get_cpu(bootstrap_capability("system_info"), &cpu) == CALL_OK && cpu.online_count) {
    for (size_t i = 0; i < spaces.space_count; ++i) {
      const struct boot_space *space = spaces.spaces[i];
      for (size_t j = 0; j < space->cpu_count; ++j) {
        if (space->cpus[j] >= cpu.online_count) {
          fprintf(stderr, "config: space %s names absent CPU %llu\n", space->name,
              (unsigned long long)space->cpus[j]);
          ok = false;
        }
      }
    }
  } else {
    fputs("config: CPU presence: not checked (read authority/inventory unavailable)\n", stderr);
  }
  fputs("config: mounted volumes, packaged timezone data and live network availability: not checked\n", stderr);
  boot_plan_free(&spaces);
  boot_config_free(&pool);
  config_selection_free(&selection);

preview:
  if (!planned && context->view.config_parent != HANDLE_INVALID) {
    struct config_selection selected;
    const char *reason;
    if (!config_selection_choose(archive, esp, context->complete, false, &selected, &reason)) {
      return false;
    }
    fprintf(stderr, "config: loader preview: %s\n", reason);
    config_selection_free(&selected);
  }
  return ok;
}

bool config_editor_plan(struct config_editor_context *context)
{
  if (!context->catalog) {
    fputs("config: revision catalog unavailable; printed changes only\n", stderr);
    return false;
  }
  struct config_bytes archive[CONFIG_LUA_COUNT];
  archive_bytes(context, archive);
  for (unsigned replacements = 0; replacements < 2; ++replacements) {
    char *bytes[CONFIG_LUA_COUNT] = {0};
    struct config_bytes declared[CONFIG_LUA_COUNT] = {0};
    bool valid = true;
    for (unsigned i = 0; i < CONFIG_LUA_COUNT && valid; ++i) {
      valid = config_render_defaults(context->catalog, i, replacements, &bytes[i], &declared[i].size);
      declared[i].bytes = bytes[i];
    }
    struct config_selection selection;
    if (valid) {
      valid = config_selection_validate(archive, declared, &selection);
      if (valid) {
        config_selection_free(&selection);
      }
    }
    for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
      free(bytes[i]);
    }
    if (!valid) {
      fputs("config: revision declarations fail shared typed/merge validation\n", stderr);
      return false;
    }
  }
  struct config_plan_input current[CONFIG_FILE_COUNT], defaults[CONFIG_FILE_COUNT] = {0};
  static const char empty_boot[] = "return { volumes = {}, spaces = {} }\n";
  defaults[CONFIG_FILE_BOOT] = (struct config_plan_input){empty_boot, sizeof(empty_boot) - 1, true};
  for (unsigned i = CONFIG_FILE_SESSION; i <= CONFIG_FILE_NETWORK; ++i) {
    defaults[i] = (struct config_plan_input){(char *)context->archive[i].bytes, context->archive[i].size, true};
  }
  static const unsigned slots[] = {CONFIG_BOOT, CONFIG_SESSION, CONFIG_NETWORK, CONFIG_LIMINE};
  for (unsigned i = 0; i < CONFIG_FILE_COUNT; ++i) {
    current[i] = (struct config_plan_input){(char *)context->current[slots[i]].bytes,
        context->current[slots[i]].size, context->current[slots[i]].digest.present};
  }
  return config_delta_plan_build(context->old_catalog, context->catalog, current, defaults, &context->plan);
}

void config_editor_hashes(const struct config_editor_context *context,
    struct config_replay_input inputs[CONFIG_REPLAY_INPUTS])
{
  const struct config_blob *blobs[] = {
    &context->current[CONFIG_BOOT], &context->current[CONFIG_SESSION],
    &context->current[CONFIG_NETWORK], &context->current[CONFIG_LIMINE],
    &context->current[CONFIG_CATALOG], &context->record, &context->catalog_source,
    &context->archive[CONFIG_LUA_BOOT], &context->archive[CONFIG_LUA_SESSION], &context->archive[CONFIG_LUA_NETWORK],
  };
  static const char absent_hash[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  for (size_t i = 0; i < CONFIG_REPLAY_INPUTS; ++i) {
    inputs[i].present = blobs[i]->digest.present;
    memcpy(inputs[i].sha256, blobs[i]->digest.present ? blobs[i]->digest.sha256 : absent_hash, 65);
  }
}

bool config_editor_publish(struct config_editor_context *context)
{
  if (!context->writer || !context->target.valid || !context->plan.applicable ||
      !config_editor_validate(context, true)) {
    fputs("config: publication refused: authority, issuer evidence or validated plan unavailable\n", stderr);
    return false;
  }
  struct config_target_facts current_target;
  if (!observe_target(context, &current_target) || !config_target_equal(&context->target, &current_target)) {
    fputs("config: target changed; create a new plan\n", stderr);
    return false;
  }
  struct config_blob selected[CONFIG_ALL_FILES] = {0};
  static const unsigned slots[] = {CONFIG_BOOT, CONFIG_SESSION, CONFIG_NETWORK, CONFIG_LIMINE};
  bool ok = true;
  for (unsigned i = 0; i < CONFIG_FILE_COUNT && ok; ++i) {
    if (context->plan.files[i].after_present) {
      ok = config_blob_set(&selected[slots[i]], context->plan.files[i].after, context->plan.files[i].after_size);
    }
  }
  ok = ok && config_blob_set(&selected[CONFIG_CATALOG], context->catalog_source.bytes, context->catalog_source.size);
  uint64_t now;
  unsigned char random[16];
  char transaction[33];
  static const char hex[] = "0123456789abcdef";
  ok = ok && clock_now(context->view.clock, &now) == CALL_OK && now <= UINT64_MAX - UINT64_C(2000000000) &&
      random_read(bootstrap_capability("random"), random, sizeof(random), now + UINT64_C(2000000000)) == CALL_OK;
  if (ok) {
    for (size_t i = 0; i < sizeof(random); ++i) {
      transaction[2 * i] = hex[random[i] >> 4];
      transaction[2 * i + 1] = hex[random[i] & 15];
    }
    transaction[32] = '\0';
    struct config_storage_report report;
    ok = config_storage_publish(&context->view, &current_target, context->current, &context->record,
        selected, transaction, context->catalog->revision, &report);
    if (ok) {
      fputs("config: complete configuration set committed and verified\n", stderr);
    } else {
      fprintf(stderr, "config: stopped at phase %u status %u, artifact %s%s; no retry or rollback\n",
          report.phase, report.status, report.artifact[0] ? report.artifact : "none",
          report.committed ? ", committed; cleanup incomplete" : "");
    }
  }
  for (size_t i = 0; i < CONFIG_ALL_FILES; ++i) {
    config_blob_destroy(&selected[i]);
  }
  return ok;
}

void config_editor_close(struct config_editor_context *context)
{
  config_delta_plan_free(&context->plan);
  config_catalog_free(context->old_catalog);
  config_catalog_free(context->catalog);
  for (size_t i = 0; i < CONFIG_ALL_FILES; ++i) {
    config_blob_destroy(&context->current[i]);
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    config_blob_destroy(&context->archive[i]);
  }
  config_blob_destroy(&context->catalog_source);
  config_blob_destroy(&context->record);
  memset(context, 0, sizeof(*context));
}
