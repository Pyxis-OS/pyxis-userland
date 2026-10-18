#include "configuration.h"
#include "../config-editor/render.h"

#include <stdlib.h>
#include <string.h>

static bool digest_equal(const struct config_file_digest *left,
    const struct config_file_digest *right)
{
  return config_digest_valid(left) && config_digest_valid(right) &&
      left->present == right->present && left->size == right->size &&
      !strcmp(left->sha256, right->sha256);
}

static bool copy_blob(struct config_blob *output, const struct config_blob *input)
{
  if (!input || !config_digest_valid(&input->digest) ||
      (input->digest.present ? !input->bytes || input->size != input->digest.size :
      input->bytes || input->size)) {
    return false;
  }
  config_blob_destroy(output);
  return !input->digest.present || (config_blob_set(output, input->bytes, input->size) &&
      digest_equal(&output->digest, &input->digest));
}

static void byte_views(const struct config_blob blobs[CONFIG_LUA_COUNT],
    struct config_bytes views[CONFIG_LUA_COUNT])
{
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    views[i] = (struct config_bytes){(char *)blobs[i].bytes, blobs[i].size};
  }
}

/* Limine has its own boot-path ownership planner. Do not let Lua reconciliation
 * accidentally rewrite an entry from the configuration catalog. */
static bool lua_view(const struct config_catalog *source, struct config_catalog *view)
{
  *view = (struct config_catalog){0};
  if (!source) {
    return true;
  }
  view->entries = calloc(source->count ? source->count : 1, sizeof(*view->entries));
  if (!view->entries) {
    return false;
  }
  view->catalog_id = source->catalog_id;
  view->revision = source->revision;
  for (size_t i = 0; i < source->count; ++i) {
    const struct config_catalog_entry *entry = &source->entries[i];
    if (entry->file != CONFIG_FILE_LIMINE) {
      if (entry->migration.kind == CONFIG_MIGRATION_MOVE &&
          entry->migration.from_file == CONFIG_FILE_LIMINE) {
        return false;
      }
      view->entries[view->count++] = *entry;
    }
  }
  return true;
}

static bool key_is(const struct config_literal_value *key, const char *text)
{
  return key && key->kind == CONFIG_LITERAL_STRING &&
      key->as.string.size == strlen(text) &&
      !memcmp(key->as.string.bytes, text, key->as.string.size);
}

/* Archive machine policy and the protected bootstrap volume remain trusted
 * archive data. Render only editable volumes/spaces into the ESP override. */
static bool boot_defaults(const struct config_blob *archive, struct config_blob *output)
{
  struct config_literal *document = NULL;
  struct config_literal_diagnostic diagnostic;
  if (!config_literal_parse((char *)archive->bytes, archive->size, &document, &diagnostic)) {
    return false;
  }
  const struct config_literal_value *volumes = config_literal_field(document->root, "volumes");
  const struct config_literal_value *spaces = config_literal_field(document->root, "spaces");
  bool ok = volumes && spaces && volumes->kind == CONFIG_LITERAL_TABLE &&
      spaces->kind == CONFIG_LITERAL_TABLE;
  struct config_literal_entry *entries = NULL;
  if (ok) {
    entries = calloc(volumes->as.table.count ? volumes->as.table.count : 1, sizeof(*entries));
    ok = entries != NULL;
  }
  struct config_literal_value selected_volumes = {.kind = CONFIG_LITERAL_TABLE};
  size_t count = 0;
  for (const struct config_literal_entry *entry = ok ? volumes->as.table.entries : NULL;
      entry; entry = entry->next) {
    if (!key_is(entry->key, BOOT_SYSTEM_VOLUME)) {
      entries[count] = *entry;
      entries[count].next = NULL;
      if (count) {
        entries[count - 1].next = &entries[count];
      }
      ++count;
    }
  }
  selected_volumes.as.table.entries = count ? entries : NULL;
  selected_volumes.as.table.count = count;
  struct config_literal_value volume_key = {.kind = CONFIG_LITERAL_STRING,
      .as.string = {"volumes", 7}};
  struct config_literal_value space_key = {.kind = CONFIG_LITERAL_STRING,
      .as.string = {"spaces", 6}};
  struct config_literal_entry space_entry = {.key = &space_key,
      .value = (struct config_literal_value *)spaces};
  struct config_literal_entry volume_entry = {.key = &volume_key,
      .value = &selected_volumes, .next = &space_entry};
  struct config_literal_value root = {.kind = CONFIG_LITERAL_TABLE,
      .as.table = {&volume_entry, 2}};
  char *value = NULL;
  size_t size = 0;
  ok = ok && config_render_value(&root, &value, &size) &&
      size <= CONFIG_FILE_MAX_BYTES - 8;
  if (ok) {
    char *bytes = malloc(size + 9);
    if (!bytes) {
      ok = false;
    } else {
      memcpy(bytes, "return ", 7);
      memcpy(bytes + 7, value, size);
      bytes[size + 7] = '\n';
      bytes[size + 8] = '\0';
      ok = config_blob_set(output, bytes, size + 8);
      free(bytes);
    }
  }
  free(value);
  free(entries);
  config_literal_free(document);
  return ok;
}

static bool declarations_valid(const struct install_configuration *plan)
{
  struct config_bytes archive[CONFIG_LUA_COUNT];
  byte_views(plan->archive, archive);
  for (unsigned replacements = 0; replacements < 2; ++replacements) {
    char *bytes[CONFIG_LUA_COUNT] = {0};
    struct config_bytes declared[CONFIG_LUA_COUNT] = {0};
    bool ok = true;
    for (size_t i = 0; i < CONFIG_LUA_COUNT && ok; ++i) {
      ok = config_render_defaults(&plan->lua_catalog, i, replacements,
          &bytes[i], &declared[i].size);
      declared[i].bytes = bytes[i];
    }
    struct config_selection selection = {0};
    ok = ok && config_selection_validate(archive, declared, &selection);
    config_selection_free(&selection);
    for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
      free(bytes[i]);
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool refresh(struct install_configuration *plan)
{
  plan->applicable = false;
  config_selection_free(&plan->selection);
  for (size_t i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!copy_blob(&plan->after[i], &plan->current[i])) {
      return false;
    }
  }
  if (plan->record_refused) {
    plan->reason = "prepared or malformed record requires manual disposition";
    return true;
  }
  struct config_bytes archive[CONFIG_LUA_COUNT], selected[CONFIG_LUA_COUNT];
  byte_views(plan->archive, archive);
  plan->pool_disposition = plan->pool_override.digest.present ?
      CONFIG_LEGACY_INACTIVE_RECOVERY : CONFIG_LEGACY_NONE;
  if (plan->source == CONFIG_ACTIVE_ARCHIVE) {
    const char *reason;
    bool valid = config_selection_choose(archive, archive, false, false,
        &plan->selection, &reason);
    plan->applicable = valid && plan->source_reviewed;
    plan->reason = !valid ? "trusted archive triple fails validation" :
        !plan->source_reviewed ? "archive activation and inactive recovery require explicit Review" :
        "whole archive selected; all ESP Lua/catalog bytes retained unchanged";
    return true;
  }
  if (plan->pool_override.digest.present && !plan->pool_valid && !plan->complete) {
    plan->reason = "unsupported personal override requires reviewed whole-archive selection";
    return true;
  }
  if (!config_delta_plan_rebuild(&plan->delta)) {
    return false;
  }
  bool adopted = true;
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    adopted &= !plan->needs_adoption[i] || plan->adopted[i];
    const struct config_plan_file *file = &plan->delta.files[i];
    if (file->after_present && !config_blob_set(&plan->after[i], file->after, file->after_size)) {
      return false;
    }
  }
  if (!copy_blob(&plan->after[CONFIG_CATALOG], &plan->catalog_source)) {
    return false;
  }
  byte_views(plan->after, selected);
  bool valid = config_selection_validate(archive, selected, &plan->selection);
  plan->applicable = valid && adopted && plan->delta.applicable && plan->source_reviewed;
  plan->reason = !valid ? "selected ESP triple is unsupported or statically invalid" :
      !adopted ? "unrecorded owner files require explicit adoption" :
      !plan->delta.applicable ? "a required delta is declined or only printable" :
      !plan->source_reviewed ? "source disposition requires explicit Review" :
      "complete reconciled ESP selection passes static validation";
  return true;
}

static bool build_delta(struct install_configuration *plan)
{
  struct {
    const struct config_catalog_entry *entry;
    enum config_plan_choice choice;
  } choices[CONFIG_PLAN_CHANGE_LIMIT];
  size_t choice_count = 0;
  for (size_t i = 0; i < plan->delta.count; ++i) {
    const struct config_plan_change *change = &plan->delta.changes[i];
    const struct config_catalog_entry *entry = change->entry;
    if (entry->file != CONFIG_FILE_BOOT && !(entry->migration.kind == CONFIG_MIGRATION_MOVE &&
        entry->migration.from_file == CONFIG_FILE_BOOT)) {
      choices[choice_count].entry = entry;
      choices[choice_count++].choice = change->choice;
    }
  }
  plan->applicable = false;
  config_delta_plan_free(&plan->delta);
  struct config_blob boot = {0};
  if (!boot_defaults(&plan->archive[CONFIG_LUA_BOOT], &boot)) {
    return false;
  }
  struct config_plan_input current[CONFIG_FILE_COUNT] = {0};
  struct config_plan_input defaults[CONFIG_FILE_COUNT] = {0};
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    const struct config_blob *source = &plan->current[i];
    if (i == CONFIG_BOOT && plan->migrate_pool) {
      source = &plan->pool_override;
    }
    current[i] = (struct config_plan_input){(char *)source->bytes, source->size,
        source->digest.present && source->size != 0};
    source = i == CONFIG_BOOT ? &boot : &plan->archive[i];
    defaults[i] = (struct config_plan_input){(char *)source->bytes, source->size, true};
  }
  bool ok = config_delta_plan_build(plan->old_catalog ? &plan->old_lua_catalog : NULL,
      &plan->lua_catalog, current, defaults, &plan->delta);
  config_blob_destroy(&boot);
  if (!ok) {
    return false;
  }
  for (size_t i = 0; i < plan->delta.count; ++i) {
    struct config_plan_change *change = &plan->delta.changes[i];
    bool pool = change->entry->file == CONFIG_FILE_BOOT && plan->migrate_pool;
    bool unadopted = plan->needs_adoption[change->entry->file] &&
        !plan->adopted[change->entry->file];
    if (unadopted) {
      change->choice = CONFIG_CHOICE_KEEP;
    } else if (change->required && change->can_apply) {
      change->choice = CONFIG_CHOICE_APPLY;
    } else if (pool && change->current.present) {
      change->choice = change->can_adopt ? CONFIG_CHOICE_ADOPT : CONFIG_CHOICE_KEEP;
    }
    /* Pool selection changes the boot snapshot and moves consuming it. All
     * other entry snapshots and declarations remain identical across rebuilds. */
    for (size_t j = 0; j < choice_count; ++j) {
      if (change->entry == choices[j].entry) {
        change->choice = choices[j].choice;
        break;
      }
    }
  }
  return refresh(plan);
}

bool install_configuration_prepare(const struct install_configuration_input *input,
    struct install_configuration *plan)
{
  if (!input || !plan || !input->current || !input->record || !input->archive ||
      !input->catalog || !config_revision_valid(input->kernel_revision) ||
      !config_revision_valid(input->program_revision)) {
    return false;
  }
  *plan = (struct install_configuration){.source = CONFIG_ACTIVE_ESP, .source_reviewed = true};
  strcpy(plan->kernel_revision, input->kernel_revision);
  strcpy(plan->program_revision, input->program_revision);
  for (size_t i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!copy_blob(&plan->current[i], &input->current[i])) {
      goto fail;
    }
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (!input->archive[i].digest.present || !copy_blob(&plan->archive[i], &input->archive[i])) {
      goto fail;
    }
  }
  if (!copy_blob(&plan->record, input->record) ||
      !copy_blob(&plan->catalog_source, input->catalog) ||
      (input->pool_override && !copy_blob(&plan->pool_override, input->pool_override)) ||
      !plan->catalog_source.digest.present ||
      !config_catalog_read("trusted source catalog", (char *)plan->catalog_source.bytes,
          plan->catalog_source.size, &plan->catalog) ||
      !lua_view(plan->catalog, &plan->lua_catalog) || !declarations_valid(plan)) {
    goto fail;
  }
  struct config_record record = {0};
  bool recorded = plan->record.digest.present &&
      config_record_parse(plan->record.bytes, plan->record.size, &record) && record.committed;
  plan->record_refused = plan->record.digest.present && !recorded;
  bool history = recorded && (!record.boot.present || record.boot.source == CONFIG_ACTIVE_ESP) &&
      digest_equal(&record.selected[CONFIG_CATALOG], &plan->current[CONFIG_CATALOG].digest);
  if (history) {
    history = config_catalog_read("previous committed catalog",
        (char *)plan->current[CONFIG_CATALOG].bytes, plan->current[CONFIG_CATALOG].size,
        &plan->old_catalog) && !strcmp(plan->old_catalog->revision, record.revision);
    if (!history) {
      config_catalog_free(plan->old_catalog);
      plan->old_catalog = NULL;
    }
  }
  if (!lua_view(plan->old_catalog, &plan->old_lua_catalog)) {
    goto fail;
  }
  /* Proposed payload revisions differ during an ordinary upgrade. Established
   * default provenance and static compatibility decide which owner values to
   * reconcile; the resulting publication binds the new payload pair. */
  plan->complete = history;
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    plan->complete &= digest_equal(&record.selected[i], &plan->current[i].digest);
    plan->needs_adoption[i] = !history && plan->current[i].digest.present && plan->current[i].size;
  }
  struct config_bytes archive[CONFIG_LUA_COUNT], current[CONFIG_LUA_COUNT];
  byte_views(plan->archive, archive);
  byte_views(plan->current, current);
  struct config_selection selection = {0};
  if (plan->complete) {
    plan->complete = config_selection_validate(archive, current, &selection);
    config_selection_free(&selection);
  }
  if (plan->pool_override.digest.present) {
    struct config_bytes proposed[CONFIG_LUA_COUNT] = {
      {(char *)plan->pool_override.bytes, plan->pool_override.size},
      archive[CONFIG_LUA_SESSION], archive[CONFIG_LUA_NETWORK],
    };
    plan->pool_valid = config_selection_validate(archive, proposed, &selection);
    config_selection_free(&selection);
    /* Owner ESP bytes take precedence too: a missing record never permits
     * migration to overwrite an existing nonempty handwritten boot file. */
    plan->migrate_pool = plan->pool_valid && !plan->complete &&
        (!plan->current[CONFIG_BOOT].digest.present || !plan->current[CONFIG_BOOT].size);
    plan->pool_conflict = plan->current[CONFIG_BOOT].digest.present &&
        !config_blob_equal(&plan->current[CONFIG_BOOT], &plan->pool_override);
    if (!plan->pool_valid) {
      plan->source_reviewed = false;
    }
  }
  if (!build_delta(plan)) {
    goto fail;
  }
  return true;
fail:
  install_configuration_free(plan);
  return false;
}

bool install_configuration_choose(struct install_configuration *plan, size_t index,
    enum config_plan_choice choice)
{
  return plan && !plan->record_refused && config_delta_plan_choose(&plan->delta, index, choice) &&
      refresh(plan);
}

bool install_configuration_adopt(struct install_configuration *plan,
    enum config_storage_file file, bool adopt)
{
  if (!plan || (unsigned)file >= CONFIG_LUA_COUNT || !plan->needs_adoption[file] ||
      plan->record_refused) {
    return false;
  }
  plan->adopted[file] = adopt;
  return refresh(plan);
}

bool install_configuration_source(struct install_configuration *plan,
    enum config_active_source source, bool reviewed)
{
  if (!plan || plan->record_refused ||
      (source != CONFIG_ACTIVE_ESP && source != CONFIG_ACTIVE_ARCHIVE)) {
    return false;
  }
  plan->source = source;
  plan->source_reviewed = reviewed;
  return refresh(plan);
}

bool install_configuration_pool(struct install_configuration *plan, bool migrate, bool reviewed)
{
  if (!plan || plan->record_refused || !plan->pool_override.digest.present ||
      (migrate && (!plan->pool_valid || plan->complete ||
       (plan->current[CONFIG_BOOT].digest.present && plan->current[CONFIG_BOOT].size)))) {
    return false;
  }
  plan->migrate_pool = migrate;
  plan->source_reviewed = reviewed;
  return build_delta(plan);
}

static const struct config_literal_value *named_entry(
    const struct config_literal_value *table, const char *name, size_t size, bool space)
{
  if (!table || table->kind != CONFIG_LITERAL_TABLE) {
    return NULL;
  }
  for (const struct config_literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    const struct config_literal_value *key = space ? config_literal_field(entry->value, "name") : entry->key;
    if (key && key->kind == CONFIG_LITERAL_STRING && key->as.string.size == size &&
        !memcmp(key->as.string.bytes, name, size)) {
      return entry->value;
    }
  }
  return NULL;
}

static void review_value(FILE *output, const char *label,
    const struct config_literal_value *value)
{
  char *bytes = NULL;
  size_t size = 0;
  if (value && config_render_value(value, &bytes, &size)) {
    fprintf(output, "  %s: ", label);
    fwrite(bytes, 1, size, output);
    fputc('\n', output);
    free(bytes);
  }
}

static void review_pool(const struct install_configuration *plan, FILE *output)
{
  if (!plan->pool_valid) {
    return;
  }
  struct config_literal *pool = NULL, *esp = NULL;
  struct config_literal_diagnostic diagnostic;
  if (!config_literal_parse((char *)plan->pool_override.bytes, plan->pool_override.size,
      &pool, &diagnostic)) {
    return;
  }
  if (plan->current[CONFIG_BOOT].digest.present) {
    config_literal_parse((char *)plan->current[CONFIG_BOOT].bytes,
        plan->current[CONFIG_BOOT].size, &esp, &diagnostic);
  }
  for (unsigned group = 0; group < 2; ++group) {
    const char *name = group ? "spaces" : "volumes";
    const struct config_literal_value *table = config_literal_field(pool->root, name);
    const struct config_literal_value *current = esp ? config_literal_field(esp->root, name) : NULL;
    for (const struct config_literal_entry *entry = table && table->kind == CONFIG_LITERAL_TABLE ?
        table->as.table.entries : NULL; entry; entry = entry->next) {
      const struct config_literal_value *key = group ? config_literal_field(entry->value, "name") : entry->key;
      if (!key || key->kind != CONFIG_LITERAL_STRING) {
        continue;
      }
      const struct config_literal_value *existing = named_entry(current,
          key->as.string.bytes, key->as.string.size, group != 0);
      fprintf(output, "Pool %s %.*s: %s\n", group ? "space" : "volume",
          (int)key->as.string.size, key->as.string.bytes,
          existing && !config_literal_equal(existing, entry->value) ? "conflicts with ESP; no merge" :
          existing ? "matches ESP" : "personal entry");
      review_value(output, "pool value", entry->value);
      if (existing) {
        review_value(output, "retained ESP value", existing);
      }
    }
  }
  config_literal_free(esp);
  config_literal_free(pool);
}

void install_configuration_review(const struct install_configuration *plan, FILE *output)
{
  fprintf(output, "Configuration source: %s; kernel %s; programs %s\n",
      plan->source == CONFIG_ACTIVE_ESP ? "complete ESP set" : "whole archive",
      plan->kernel_revision, plan->program_revision);
  fprintf(output, "Configuration: %s (%s)\n", plan->applicable ? "applicable" : "blocked",
      plan->reason ? plan->reason : "not prepared");
  fprintf(output, "Trusted revision catalog: %llu bytes SHA256 %s\n",
      (unsigned long long)plan->catalog_source.digest.size, plan->catalog_source.digest.sha256);
  if (plan->source == CONFIG_ACTIVE_ARCHIVE && plan->current[CONFIG_CATALOG].digest.present) {
    fprintf(output, "catalog.lua retained unchanged, inactive: %llu bytes SHA256 %s\n",
        (unsigned long long)plan->current[CONFIG_CATALOG].digest.size,
        plan->current[CONFIG_CATALOG].digest.sha256);
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (plan->needs_adoption[i]) {
      fprintf(output, "%s owner bytes: %s; %llu bytes SHA256 %s\n",
          config_storage_file_name(i), plan->adopted[i] ? "explicit adoption" : "Keep",
          (unsigned long long)plan->current[i].digest.size, plan->current[i].digest.sha256);
    }
    if (plan->source == CONFIG_ACTIVE_ARCHIVE && plan->current[i].digest.present) {
      fprintf(output, "%s retained unchanged, inactive: %llu bytes SHA256 %s\n",
          config_storage_file_name(i), (unsigned long long)plan->current[i].digest.size,
          plan->current[i].digest.sha256);
    }
  }
  if (plan->pool_override.digest.present) {
    fprintf(output, "system://config/boot.lua: %llu bytes SHA256 %s; %s; after publication retained unchanged as inactive recovery\n",
        (unsigned long long)plan->pool_override.digest.size, plan->pool_override.digest.sha256,
        plan->migrate_pool ? "migration selected" : plan->pool_valid ? "migration not selected" :
        "unsupported/invalid; printed proposals only");
    if (plan->pool_conflict) {
      fputs("Pool/ESP boot inputs differ; ESP owner bytes take precedence, no silent merge\n", output);
    }
    review_pool(plan, output);
  }
  for (size_t i = 0; i < plan->delta.count; ++i) {
    fprintf(output, "%zu. ", i + 1);
    config_delta_plan_review(&plan->delta, i, output);
  }
  fputs("CPU presence, mounted volumes, timezone files and live network availability: not checked\n", output);
}

bool install_configuration_revalidate(const struct install_configuration *plan,
    const struct install_configuration_input *input)
{
  if (!plan || !plan->applicable || !input || !input->current || !input->record ||
      !input->archive || !input->catalog || !config_revision_valid(input->kernel_revision) ||
      !config_revision_valid(input->program_revision) ||
      strcmp(plan->kernel_revision, input->kernel_revision) ||
      strcmp(plan->program_revision, input->program_revision) ||
      !digest_equal(&plan->record.digest, &input->record->digest) ||
      !digest_equal(&plan->catalog_source.digest, &input->catalog->digest)) {
    return false;
  }
  for (size_t i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!digest_equal(&plan->current[i].digest, &input->current[i].digest)) {
      return false;
    }
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (!digest_equal(&plan->archive[i].digest, &input->archive[i].digest)) {
      return false;
    }
  }
  struct config_file_digest absent = {0};
  return digest_equal(&plan->pool_override.digest,
      input->pool_override ? &input->pool_override->digest : &absent);
}

void install_configuration_free(struct install_configuration *plan)
{
  if (!plan) {
    return;
  }
  config_selection_free(&plan->selection);
  config_delta_plan_free(&plan->delta);
  free(plan->lua_catalog.entries);
  free(plan->old_lua_catalog.entries);
  config_catalog_free(plan->catalog);
  config_catalog_free(plan->old_catalog);
  for (size_t i = 0; i < CONFIG_ALL_FILES; ++i) {
    config_blob_destroy(&plan->current[i]);
    config_blob_destroy(&plan->after[i]);
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    config_blob_destroy(&plan->archive[i]);
  }
  config_blob_destroy(&plan->record);
  config_blob_destroy(&plan->catalog_source);
  config_blob_destroy(&plan->pool_override);
  *plan = (struct install_configuration){0};
}

void install_configuration_summary(const struct install_configuration *plan, FILE *output)
{
  size_t required = 0, selected = 0;
  for (size_t i = 0; i < plan->delta.count; ++i) {
    required += plan->delta.changes[i].required;
    selected += plan->delta.changes[i].choice != CONFIG_CHOICE_KEEP;
  }
  fprintf(output, "Configuration source: %s; %s.\nReason: %s.\n",
      plan->source == CONFIG_ACTIVE_ESP ? "complete ESP set" : "whole archive",
      plan->applicable ? "applicable" : "blocked", plan->reason ? plan->reason : "not prepared");
  fprintf(output, "ESP deltas: %zu required, %zu selected / %zu total%s.\n", required,
      selected, plan->delta.count, plan->source == CONFIG_ACTIVE_ARCHIVE ? "; inactive" : "");
  for (unsigned i = 0; i < CONFIG_LUA_COUNT; ++i) {
    if (plan->needs_adoption[i]) {
      fprintf(output, "%s owner bytes: %s%s.\n", config_storage_file_name(i),
          plan->adopted[i] ? "adoption selected" : "Keep; adoption required for ESP",
          plan->source == CONFIG_ACTIVE_ARCHIVE ? "; retained inactive" : "");
    }
    const struct config_plan_file *file = &plan->delta.files[i];
    if (file->print_only && file->relevant) {
      fprintf(output, "%s: %s.\n", config_storage_file_name(i),
          file->reason ? file->reason : "printed changes only");
    }
  }
  if (plan->pool_override.digest.present) {
    fprintf(output, "Pool boot override: %s; original kept as inactive recovery after publication.\n",
        plan->source == CONFIG_ACTIVE_ARCHIVE ? "migration inactive; archive selected" :
        plan->migrate_pool ? "migration selected" : plan->pool_valid ? "migration not selected" :
        "unsupported/invalid; printed proposals only");
    if (plan->pool_conflict) {
      fputs("Pool/ESP boot inputs differ; no silent merge.\n", output);
    }
  }
  fputs("CPU/volume/timezone/network: not checked. Details and numbered choices: plan.\n", output);
}
