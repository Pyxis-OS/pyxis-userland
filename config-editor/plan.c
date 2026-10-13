#include "plan.h"
#include "render.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct plan_edit {
  enum config_file file;
  struct config_literal_span span;
  char *bytes;
  size_t size;
  size_t order;
};

struct plan_edits {
  struct plan_edit *items;
  size_t count;
};

static bool string_is(const struct config_literal_value *value, const char *text)
{
  return value && value->kind == CONFIG_LITERAL_STRING &&
      value->as.string.size == strlen(text) &&
      !memcmp(value->as.string.bytes, text, value->as.string.size);
}

static const struct config_catalog_entry *target_declaration(
    const struct config_catalog *catalog, enum config_file file,
    enum config_entry_kind kind, const char *name)
{
  for (size_t i = 0; catalog && i < catalog->count; ++i) {
    const struct config_catalog_entry *entry = &catalog->entries[i];
    if (entry->file == file && entry->kind == kind && !strcmp(entry->name, name)) {
      return entry;
    }
  }
  return NULL;
}

static bool same_target(const struct config_catalog_entry *a,
    const struct config_catalog_entry *b)
{
  return a && b && a->file == b->file && a->kind == b->kind && !strcmp(a->name, b->name);
}

static const struct config_literal_value *target_table(
    const struct config_plan_file *file, enum config_entry_kind kind)
{
  if (!file->document) {
    return NULL;
  }
  const struct config_literal_value *root = file->document->root;
  if (kind == CONFIG_ENTRY_VOLUME) {
    return config_literal_field(root, "volumes");
  }
  if (kind == CONFIG_ENTRY_SPACE) {
    return config_literal_field(root, "spaces");
  }
  return root;
}

static const struct config_literal_entry *literal_target(
    const struct config_plan_file *file, enum config_entry_kind kind, const char *name)
{
  const struct config_literal_value *table = target_table(file, kind);
  if (!table || table->kind != CONFIG_LITERAL_TABLE) {
    return NULL;
  }
  const struct config_literal_entry *found = NULL;
  for (const struct config_literal_entry *entry = table->as.table.entries;
      entry; entry = entry->next) {
    bool matches = kind == CONFIG_ENTRY_SPACE ?
        string_is(config_literal_field(entry->value, "name"), name) : string_is(entry->key, name);
    if (matches) {
      if (found) {
        return NULL;
      }
      found = entry;
    }
  }
  return found;
}

/* Only whitespace and parser-recognized comments may surround the one entry. */
static bool trivia(const struct config_literal *document, size_t start, size_t end)
{
  while (start < end) {
    unsigned char c = (unsigned char)document->source[start];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
      ++start;
      continue;
    }
    bool comment = false;
    for (size_t i = 0; i < document->comment_count; ++i) {
      struct config_literal_span span = document->comments[i].span;
      if (span.start == start && span.end <= end) {
        start = span.end;
        comment = true;
        break;
      }
    }
    if (!comment) {
      return false;
    }
  }
  return true;
}

static bool certified_span(const struct config_plan_file *file,
    const struct config_owned_span *owned, struct config_literal_span entry)
{
  return entry.start >= owned->content.start && entry.end <= owned->content.end &&
      trivia(file->document, owned->content.start, entry.start) &&
      trivia(file->document, entry.end, owned->content.end);
}

static size_t line_end(const char *bytes, size_t size, size_t at)
{
  while (at < size && bytes[at] != '\n') {
    ++at;
  }
  return at < size ? at + 1 : at;
}

static bool limine_header(const char *bytes, size_t start, size_t end, const char *name)
{
  while (end > start && (bytes[end - 1] == '\r' || bytes[end - 1] == '\n' ||
      bytes[end - 1] == ' ' || bytes[end - 1] == '\t')) {
    --end;
  }
  return end > start && bytes[start] == '/' && end - start == strlen(name) + 1 &&
      !memcmp(bytes + start + 1, name, strlen(name));
}

static bool limine_target(struct config_plan_file *file, const char *name,
    struct config_literal_span *span)
{
  bool found = false;
  size_t end = file->before_size;
  for (size_t at = 0; at < file->before_size;) {
    size_t next = line_end(file->before, file->before_size, at);
    if (file->before[at] == '/') {
      if (found && end == file->before_size) {
        end = at;
      }
      if (limine_header(file->before, at, next, name)) {
        if (found) {
          return false;
        }
        found = true;
        span->start = at;
      }
    }
    at = next;
  }
  if (!found) {
    return false;
  }
  for (size_t i = 0; file->ownership && i < file->ownership->count; ++i) {
    size_t marker = file->ownership->spans[i].span.start;
    if (marker > span->start && marker < end) {
      end = marker;
    }
  }
  span->end = end;
  return true;
}

static bool find_target(struct config_plan_file *file,
    const struct config_catalog_entry *entry, struct config_plan_target *target)
{
  *target = (struct config_plan_target){0};
  if (file->print_only) {
    return true;
  }
  const struct config_owned_span *owned = config_owned_find(file->ownership, entry->id);
  if (entry->kind == CONFIG_ENTRY_LIMINE) {
    struct config_literal_span content;
    bool present = owned ? true : limine_target(file, entry->name, &content);
    if (!present) {
      return true;
    }
    if (owned) {
      content = owned->content;
    }
    if (!config_limine_entry_valid(entry->name, file->before + content.start,
        content.end - content.start)) {
      file->print_only = true;
      file->reason = "Limine target is not one complete declared entry";
      return true;
    }
    struct config_literal_value *value = calloc(1, sizeof(*value));
    if (!value) {
      return false;
    }
    *value = (struct config_literal_value){
      .kind = CONFIG_LITERAL_STRING, .span = content,
      .as.string = {file->before + content.start, content.end - content.start},
    };
    *target = (struct config_plan_target){.value = value, .owns_value = true,
        .span = owned ? owned->span : content, .present = true,
        .owned = owned || !file->original_present, .marked = owned != NULL};
    return true;
  }
  const struct config_literal_entry *literal = literal_target(file, entry->kind, entry->name);
  if (owned && (!literal || !certified_span(file, owned, literal->span))) {
    file->print_only = true;
    file->reason = "ownership marker does not enclose exactly its complete declared entry";
    return true;
  }
  if (!literal) {
    return true;
  }
  for (size_t i = 0; file->ownership && i < file->ownership->count; ++i) {
    const struct config_owned_span *other = &file->ownership->spans[i];
    if (other != owned && literal->span.start < other->span.end &&
        literal->span.end > other->span.start) {
      file->print_only = true;
      file->reason = "target entry overlaps another ownership marker";
      return true;
    }
  }
  *target = (struct config_plan_target){.value = literal->value,
      .span = owned ? owned->span : literal->span, .present = true,
      .owned = owned || !file->original_present, .marked = owned != NULL};
  return true;
}

static bool find_destination(struct config_plan_file *file,
    const struct config_catalog_entry *entry, struct config_plan_target *target)
{
  if (entry->migration.kind == CONFIG_MIGRATION_MOVE &&
      entry->migration.from_file == entry->file &&
      (entry->migration.from_kind != entry->kind || strcmp(entry->migration.from_name, entry->name))) {
    struct config_catalog_entry destination = *entry;
    const struct config_owned_span *owned = config_owned_find(file->ownership, entry->id);
    const struct config_literal_entry *literal = entry->kind == CONFIG_ENTRY_LIMINE ? NULL :
        literal_target(file, entry->kind, entry->name);
    if (owned && (!literal || !certified_span(file, owned, literal->span))) {
      /* This ID still marks the declared source of a move, not its destination. */
      destination.id = "";
    }
    return find_target(file, &destination, target);
  }
  return find_target(file, entry, target);
}

static void target_free(struct config_plan_target *target)
{
  if (target->owns_value) {
    free((void *)target->value);
  }
  *target = (struct config_plan_target){0};
}

static enum config_value_origin value_origin(const struct config_delta_plan *plan,
    const struct config_catalog_entry *old_entry, const struct config_plan_target *target)
{
  if (!target->present) {
    return CONFIG_VALUE_ABSENT;
  }
  if (!plan->old_catalog || !old_entry) {
    return CONFIG_VALUE_UNKNOWN;
  }
  return config_literal_equal(target->value, old_entry->default_value) ?
      CONFIG_VALUE_OLD_DEFAULT : CONFIG_VALUE_OWNER_EDIT;
}

static bool migration_consumes(const struct config_catalog *catalog,
    const struct config_catalog_entry *old_entry)
{
  for (size_t i = 0; i < catalog->count; ++i) {
    const struct config_migration *migration = &catalog->entries[i].migration;
    if (migration->kind == CONFIG_MIGRATION_MOVE && migration->from_file == old_entry->file &&
        migration->from_kind == old_entry->kind && !strcmp(migration->from_name, old_entry->name)) {
      return true;
    }
  }
  return false;
}

static bool add_change(struct config_delta_plan *plan,
    const struct config_catalog_entry *entry, const struct config_catalog_entry *old_entry,
    bool removal)
{
  struct config_plan_change *change = &plan->changes[plan->count++];
  *change = (struct config_plan_change){.old_entry = old_entry, .entry = entry,
      .required = !removal && entry->required, .choice = CONFIG_CHOICE_KEEP};
  struct config_plan_file *file = &plan->files[entry->file];
  /* A stable ID can explicitly move to a different target. Its old marked
   * location is only consumed by an approved declared move, never guessed. */
  if (!find_destination(file, entry, &change->current)) {
    return false;
  }
  change->origin = value_origin(plan, old_entry, &change->current);
  if (change->current.present && entry->migration.kind == CONFIG_MIGRATION_NONE &&
      (change->origin != CONFIG_VALUE_OLD_DEFAULT || (old_entry &&
       same_target(old_entry, entry) && config_literal_equal(old_entry->default_value, entry->default_value)))) {
    /* A changed default does not establish that an existing owner value is
     * invalid. Full typed validation remains the caller's responsibility. */
    change->required = false;
  }
  change->can_apply = !file->print_only;
  change->can_adopt = change->current.present && !change->current.owned && !file->print_only;
  if (removal) {
    change->kind = CONFIG_CHANGE_REMOVE;
    change->can_apply = change->current.present && change->current.owned && !file->print_only;
    change->reason = change->current.owned ? "entry is no longer declared; removal is optional" :
        "entry is no longer declared; unowned bytes are preserved";
    return true;
  }
  if (!file->original_present && entry->migration.kind != CONFIG_MIGRATION_MOVE) {
    change->kind = CONFIG_CHANGE_ADD;
    change->origin = CONFIG_VALUE_ABSENT;
    change->reason = file->print_only ? file->reason : "missing file uses trusted complete defaults";
    if (change->can_apply) {
      change->choice = CONFIG_CHOICE_APPLY;
    }
    return true;
  }
  if (entry->migration.kind == CONFIG_MIGRATION_MOVE) {
    const struct config_migration *migration = &entry->migration;
    const struct config_catalog_entry *source_entry = target_declaration(plan->old_catalog,
        migration->from_file, migration->from_kind, migration->from_name);
    struct config_catalog_entry source = {.id = source_entry ? source_entry->id : entry->id,
        .name = migration->from_name, .file = migration->from_file, .kind = migration->from_kind};
    change->migration_file = migration->from_file;
    if (!find_target(&plan->files[source.file], &source, &change->migration_source)) {
      return false;
    }
    change->kind = CONFIG_CHANGE_MIGRATE;
    change->can_apply &= change->migration_source.present &&
        plan->files[source.file].original_present && !plan->files[source.file].print_only;
    change->reason = "declared entry move requires explicit approval of source and destination";
    return true;
  }
  if (entry->migration.kind != CONFIG_MIGRATION_NONE) {
    change->kind = CONFIG_CHANGE_MIGRATE;
    change->can_apply &= change->current.present;
    change->reason = "declared schema migration requires explicit approval";
    return true;
  }
  if (!change->current.present) {
    change->kind = CONFIG_CHANGE_ADD;
    change->reason = file->print_only ? file->reason : "declared entry is absent";
    if (change->can_apply) {
      change->choice = CONFIG_CHOICE_APPLY;
    }
  } else if (!change->current.owned) {
    change->kind = CONFIG_CHANGE_ADOPT;
    change->reason = "unmarked entry needs explicit adoption or replacement; existing value is retained";
  } else {
    change->kind = CONFIG_CHANGE_DEFAULT;
    change->reason = change->origin == CONFIG_VALUE_OLD_DEFAULT ?
        "revision default changed; current value matches the supplied old default" :
        change->origin == CONFIG_VALUE_OWNER_EDIT ? "owner value is preserved pending replacement approval" :
        "old catalog is unavailable; current value is conservatively an owner edit";
    if (change->origin == CONFIG_VALUE_OLD_DEFAULT) {
      change->choice = CONFIG_CHOICE_APPLY;
    }
  }
  return true;
}

static bool file_snapshot(struct config_plan_file *file, enum config_file which,
    const struct config_plan_input *current, const struct config_plan_input *defaults)
{
  file->original_present = current->present;
  const struct config_plan_input *source = current->present ? current : defaults;
  if (!source || !source->present) {
    file->print_only = true;
    file->reason = "missing file has no trusted complete revision defaults";
    return true;
  }
  if (source->size > CONFIG_LITERAL_SOURCE_LIMIT || (!source->bytes && source->size)) {
    file->print_only = true;
    file->reason = "source exceeds the bounded configuration size";
    return source->size <= CONFIG_LITERAL_SOURCE_LIMIT;
  }
  file->before = malloc(source->size + 1);
  if (!file->before) {
    return false;
  }
  if (source->size) {
    memcpy(file->before, source->bytes, source->size);
  }
  file->before[source->size] = '\0';
  file->before_size = source->size;
  if (which == CONFIG_FILE_LIMINE) {
    if (!config_owned_scan_limine(config_file_name(which), file->before, file->before_size,
        &file->ownership)) {
      file->print_only = true;
      file->reason = "ambiguous Limine ownership markers require printed changes";
    }
    return true;
  }
  struct config_literal_diagnostic diagnostic;
  if (!config_literal_parse(file->before, file->before_size, &file->document, &diagnostic)) {
    file->print_only = true;
    file->reason = "unsupported or invalid Lua stays untouched; use printed changes";
    return true;
  }
  if (!config_owned_scan_lua(config_file_name(which), file->document, &file->ownership)) {
    file->print_only = true;
    file->reason = "ambiguous Lua ownership markers require printed changes";
  }
  return true;
}

static bool optional_equal(const char *a, const char *b)
{
  return a && b ? !strcmp(a, b) : a == b;
}

static bool migration_equal(const struct config_migration *a, const struct config_migration *b)
{
  if (a->kind != b->kind) {
    return false;
  }
  switch (a->kind) {
  case CONFIG_MIGRATION_NONE: return true;
  case CONFIG_MIGRATION_REPLACE: return config_literal_equal(a->value, b->value);
  case CONFIG_MIGRATION_MOVE:
    return a->from_file == b->from_file && a->from_kind == b->from_kind &&
        optional_equal(a->from_name, b->from_name);
  case CONFIG_MIGRATION_RENAME_FIELD:
    return optional_equal(a->from, b->from) && optional_equal(a->to, b->to);
  }
  return false;
}

static bool declaration_changed(const struct config_catalog_entry *old_entry,
    const struct config_catalog_entry *entry)
{
  return !old_entry || !same_target(old_entry, entry) ||
      !config_literal_equal(old_entry->default_value, entry->default_value) ||
      (!old_entry->required && entry->required) || !migration_equal(&old_entry->migration, &entry->migration);
}

bool config_delta_plan_build(const struct config_catalog *old_catalog,
    const struct config_catalog *catalog,
    const struct config_plan_input current[CONFIG_FILE_COUNT],
    const struct config_plan_input defaults[CONFIG_FILE_COUNT],
    struct config_delta_plan *plan)
{
  *plan = (struct config_delta_plan){.old_catalog = old_catalog, .catalog = catalog};
  if (!catalog || catalog->count > CONFIG_CATALOG_ENTRY_LIMIT ||
      (old_catalog && old_catalog->count > CONFIG_CATALOG_ENTRY_LIMIT)) {
    goto fail;
  }
  plan->changes = calloc(CONFIG_PLAN_CHANGE_LIMIT, sizeof(*plan->changes));
  if (!plan->changes) {
    goto fail;
  }
  for (size_t i = 0; i < CONFIG_FILE_COUNT; ++i) {
    if (!file_snapshot(&plan->files[i], (enum config_file)i, &current[i], defaults ? &defaults[i] : NULL)) {
      goto fail;
    }
  }
  for (size_t i = 0; i < catalog->count; ++i) {
    const struct config_catalog_entry *entry = &catalog->entries[i];
    const struct config_catalog_entry *old_entry = old_catalog ? config_catalog_find(old_catalog, entry->id) : NULL;
    struct config_plan_target target;
    if (!find_destination(&plan->files[entry->file], entry, &target)) {
      goto fail;
    }
    bool changed = declaration_changed(old_entry, entry) || !target.present || !target.owned ||
        !plan->files[entry->file].original_present;
    /* Without old defaults, an equal current value needs no replacement; an
     * unowned equal value still requires explicit adoption of its exact bytes. */
    if (!old_entry && target.present && target.owned &&
        config_literal_equal(target.value, entry->default_value) &&
        entry->migration.kind == CONFIG_MIGRATION_NONE && plan->files[entry->file].original_present) {
      changed = false;
    }
    target_free(&target);
    if (changed && !add_change(plan, entry, old_entry, false)) {
      goto fail;
    }
  }
  for (size_t i = 0; old_catalog && i < old_catalog->count; ++i) {
    const struct config_catalog_entry *entry = &old_catalog->entries[i];
    if (!config_catalog_find(catalog, entry->id) && !migration_consumes(catalog, entry) &&
        !add_change(plan, entry, entry, true)) {
      goto fail;
    }
  }
  if (!config_delta_plan_rebuild(plan)) {
    goto fail;
  }
  return true;
fail:
  fputs("config: cannot construct the bounded configuration delta plan\n", stderr);
  config_delta_plan_free(plan);
  return false;
}

static bool append_edit(struct plan_edits *edits, enum config_file file,
    struct config_literal_span span, char *bytes, size_t size)
{
  struct plan_edit *grown = realloc(edits->items, (edits->count + 1) * sizeof(*grown));
  if (!grown) {
    free(bytes);
    return false;
  }
  edits->items = grown;
  edits->items[edits->count] = (struct plan_edit){file, span, bytes, size, edits->count};
  ++edits->count;
  return true;
}

struct renamed_value {
  struct config_literal_value value, key, replacement;
  struct config_literal_entry *entries;
};

static bool rename_field(const struct config_literal_value *value, const char *from,
    const char *to, struct renamed_value *renamed)
{
  *renamed = (struct renamed_value){0};
  if (!value || value->kind != CONFIG_LITERAL_TABLE || !config_literal_field(value, from) ||
      (strcmp(from, to) && config_literal_field(value, to))) {
    return false;
  }
  renamed->value = *value;
  renamed->key = (struct config_literal_value){.kind = CONFIG_LITERAL_STRING,
      .as.string = {(char *)to, strlen(to)}};
  size_t count = value->as.table.count;
  renamed->entries = calloc(count ? count : 1, sizeof(*renamed->entries));
  if (!renamed->entries) {
    return false;
  }
  size_t i = 0;
  for (const struct config_literal_entry *entry = value->as.table.entries;
      entry; entry = entry->next, ++i) {
    renamed->entries[i] = *entry;
    if (string_is(entry->key, from)) {
      renamed->entries[i].key = &renamed->key;
    }
    renamed->entries[i].next = i + 1 < count ? &renamed->entries[i + 1] : NULL;
  }
  renamed->value.as.table.entries = renamed->entries;
  return true;
}

static bool space_move_value(const struct config_literal_value *value, const char *name,
    struct renamed_value *renamed)
{
  if (!rename_field(value, "name", "name", renamed)) {
    return false;
  }
  renamed->replacement = (struct config_literal_value){.kind = CONFIG_LITERAL_STRING,
      .as.string = {(char *)name, strlen(name)}};
  for (struct config_literal_entry *entry = renamed->value.as.table.entries;
      entry; entry = entry->next) {
    if (string_is(entry->key, "name")) {
      entry->value = &renamed->replacement;
    }
  }
  return true;
}

static bool render_change(const struct config_plan_change *change, char **bytes, size_t *size)
{
  const struct config_catalog_entry *entry = change->entry;
  const struct config_literal_value *value = change->choice == CONFIG_CHOICE_ADOPT ?
      change->current.value : entry->default_value;
  struct renamed_value renamed = {0};
  if (change->choice == CONFIG_CHOICE_APPLY && change->kind == CONFIG_CHANGE_MIGRATE) {
    if (entry->migration.kind == CONFIG_MIGRATION_REPLACE) {
      value = entry->migration.value;
    } else if (entry->migration.kind == CONFIG_MIGRATION_MOVE) {
      value = change->migration_source.value;
    } else if (entry->migration.kind == CONFIG_MIGRATION_RENAME_FIELD) {
      if (!rename_field(change->current.value, entry->migration.from, entry->migration.to, &renamed)) {
        return false;
      }
      value = &renamed.value;
    }
  }
  struct renamed_value moved_space = {0};
  bool shape = value != NULL;
  if (shape && entry->kind == CONFIG_ENTRY_VOLUME) {
    shape = value->kind == CONFIG_LITERAL_TABLE;
  } else if (shape && entry->kind == CONFIG_ENTRY_SPACE) {
    if (change->choice == CONFIG_CHOICE_APPLY && change->kind == CONFIG_CHANGE_MIGRATE &&
        entry->migration.kind == CONFIG_MIGRATION_MOVE) {
      shape = space_move_value(value, entry->name, &moved_space);
      value = &moved_space.value;
    } else {
      shape = string_is(config_literal_field(value, "name"), entry->name);
    }
  }
  bool rendered = shape && config_render_entry(entry, value, bytes, size);
  free(moved_space.entries);
  free(renamed.entries);
  return rendered;
}

/* Inserts before existing entries, so no separator or other byte belonging to
 * an unowned existing entry needs to change. The rendered entry owns its comma. */
static bool insertion(struct config_delta_plan *plan, const struct config_catalog_entry *entry,
    struct config_literal_span *span, bool *needs_container)
{
  struct config_plan_file *file = &plan->files[entry->file];
  *needs_container = false;
  if (entry->kind == CONFIG_ENTRY_LIMINE) {
    *span = (struct config_literal_span){file->before_size, file->before_size};
    return true;
  }
  const struct config_literal_value *table = target_table(file, entry->kind);
  if (!table && (entry->kind == CONFIG_ENTRY_VOLUME || entry->kind == CONFIG_ENTRY_SPACE)) {
    table = file->document ? file->document->root : NULL;
    *needs_container = true;
  }
  if (!table || table->kind != CONFIG_LITERAL_TABLE || table->span.end <= table->span.start + 1) {
    return false;
  }
  *span = (struct config_literal_span){table->span.start + 1, table->span.start + 1};
  return true;
}

static bool with_newline(char **bytes, size_t *size)
{
  if (*size >= CONFIG_LITERAL_SOURCE_LIMIT) {
    return false;
  }
  char *grown = realloc(*bytes, *size + 2);
  if (!grown) {
    return false;
  }
  memmove(grown + 1, grown, *size);
  grown[0] = '\n';
  grown[++*size] = '\0';
  *bytes = grown;
  return true;
}

static bool container_edit(struct plan_edits *edits, enum config_entry_kind kind,
    enum config_file file, struct config_literal_span span, char *bytes, size_t size)
{
  const char *prefix = kind == CONFIG_ENTRY_VOLUME ? "\nvolumes = {\n" : "\nspaces = {\n";
  const char suffix[] = "},\n";
  /* One synthetic parent table collects all additions to a previously absent
   * container, preventing duplicate root keys when several entries are added. */
  for (size_t i = 0; i < edits->count; ++i) {
    struct plan_edit *edit = &edits->items[i];
    size_t prefix_size = strlen(prefix);
    if (edit->file == file && edit->span.start == span.start && edit->span.end == span.end &&
        edit->size >= prefix_size + sizeof(suffix) - 1 &&
        !memcmp(edit->bytes, prefix, prefix_size)) {
      size_t old_size = edit->size;
      if (size > CONFIG_LITERAL_SOURCE_LIMIT - old_size) {
        free(bytes);
        return false;
      }
      char *grown = realloc(edit->bytes, old_size + size + 1);
      if (!grown) {
        free(bytes);
        return false;
      }
      size_t at = old_size - (sizeof(suffix) - 1);
      memmove(grown + at + size, grown + at, sizeof(suffix));
      memcpy(grown + at, bytes, size);
      free(bytes);
      edit->bytes = grown;
      edit->size += size;
      return true;
    }
  }
  size_t prefix_size = strlen(prefix);
  if (size > CONFIG_LITERAL_SOURCE_LIMIT - prefix_size - (sizeof(suffix) - 1)) {
    free(bytes);
    return false;
  }
  size_t total = prefix_size + size + sizeof(suffix) - 1;
  char *wrapped = malloc(total + 1);
  if (!wrapped) {
    free(bytes);
    return false;
  }
  memcpy(wrapped, prefix, prefix_size);
  memcpy(wrapped + prefix_size, bytes, size);
  memcpy(wrapped + prefix_size + size, suffix, sizeof(suffix));
  free(bytes);
  return append_edit(edits, file, span, wrapped, total);
}

static bool adoption_bytes(const struct config_delta_plan *plan,
    const struct config_plan_change *change, char **output, size_t *size)
{
  const struct config_plan_file *file = &plan->files[change->entry->file];
  struct config_literal_span span = change->current.span;
  size_t content_size = span.end - span.start;
  const char *prefix = change->entry->kind == CONFIG_ENTRY_LIMINE ? "# pyxis:" : "-- pyxis:";
  const struct config_literal_entry *literal = change->entry->kind == CONFIG_ENTRY_LIMINE ? NULL :
      literal_target(file, change->entry->kind, change->entry->name);
  bool separator = literal && literal->span.end == literal->value->span.end;
  size_t id_size = strlen(change->entry->id), prefix_size = strlen(prefix);
  size_t overhead = 2 * (prefix_size + id_size) + 20;
  if (content_size > CONFIG_LITERAL_SOURCE_LIMIT - overhead) {
    return false;
  }
  char *bytes = malloc(content_size + overhead + 1);
  if (!bytes) {
    return false;
  }
  size_t at = (size_t)sprintf(bytes, "\n%sbegin %s\n", prefix, change->entry->id);
  memcpy(bytes + at, file->before + span.start, content_size);
  at += content_size;
  if (separator) {
    bytes[at++] = ',';
  }
  if (!at || bytes[at - 1] != '\n') {
    bytes[at++] = '\n';
  }
  at += (size_t)sprintf(bytes + at, "%send %s\n", prefix, change->entry->id);
  *output = bytes;
  *size = at;
  return true;
}

static bool selected_edit(struct config_delta_plan *plan, struct config_plan_change *change,
    struct plan_edits *edits)
{
  struct config_plan_file *file = &plan->files[change->entry->file];
  if (file->print_only || (change->kind == CONFIG_CHANGE_MIGRATE &&
      change->entry->migration.kind == CONFIG_MIGRATION_MOVE &&
      plan->files[change->migration_file].print_only)) {
    return true;
  }
  if (change->choice < CONFIG_CHOICE_KEEP || change->choice > CONFIG_CHOICE_REMOVE) {
    plan->applicable = false;
    return true;
  }
  if (change->required && change->choice == CONFIG_CHOICE_ADOPT) {
    plan->applicable = false;
  }
  if (change->choice == CONFIG_CHOICE_KEEP) {
    if (change->required) {
      plan->applicable = false;
    }
    /* A declined optional addition to a newly created trusted template removes
     * only its declared entry, since no original owner bytes exist in that file. */
    if (!file->original_present && change->kind == CONFIG_CHANGE_ADD && change->current.present) {
      return append_edit(edits, change->entry->file, change->current.span, NULL, 0);
    }
    return true;
  }
  if (change->choice == CONFIG_CHOICE_REMOVE) {
    if (change->kind != CONFIG_CHANGE_REMOVE || !change->can_apply) {
      plan->applicable = false;
      return true;
    }
    return append_edit(edits, change->entry->file, change->current.span, NULL, 0);
  }
  if ((change->choice == CONFIG_CHOICE_ADOPT && !change->can_adopt) ||
      (change->choice == CONFIG_CHOICE_APPLY && !change->can_apply)) {
    plan->applicable = false;
    return true;
  }
  char *bytes = NULL;
  size_t size = 0;
  bool rendered = change->choice == CONFIG_CHOICE_ADOPT ?
      adoption_bytes(plan, change, &bytes, &size) : render_change(change, &bytes, &size);
  if (!rendered) {
    change->can_apply = false;
    change->reason = "declared migration is not applicable to the current complete entry";
    plan->applicable = false;
    return true;
  }
  struct config_literal_span span = change->current.span;
  bool needs_container = false;
  if (!change->current.present) {
    if (!insertion(plan, change->entry, &span, &needs_container)) {
      free(bytes);
      change->can_apply = false;
      change->reason = "declared target has no valid insertion table";
      plan->applicable = false;
      return true;
    }
    if (needs_container) {
      if (!container_edit(edits, change->entry->kind, change->entry->file, span, bytes, size)) {
        return false;
      }
    } else {
      if (!with_newline(&bytes, &size)) {
        free(bytes);
        return false;
      }
      if (!append_edit(edits, change->entry->file, span, bytes, size)) {
        return false;
      }
    }
  } else {
    if (!change->current.marked && change->choice != CONFIG_CHOICE_ADOPT &&
        !with_newline(&bytes, &size)) {
      free(bytes);
      return false;
    }
    if (!append_edit(edits, change->entry->file, span, bytes, size)) {
      return false;
    }
  }
  if (change->choice == CONFIG_CHOICE_APPLY && change->kind == CONFIG_CHANGE_MIGRATE &&
      change->entry->migration.kind == CONFIG_MIGRATION_MOVE) {
    struct config_literal_span source = change->migration_source.span;
    if (change->migration_file != change->entry->file || source.start != span.start || source.end != span.end) {
      if (!append_edit(edits, change->migration_file, source, NULL, 0)) {
        return false;
      }
    }
  }
  return true;
}

static int edit_order(const void *a, const void *b)
{
  const struct plan_edit *left = a, *right = b;
  if (left->file != right->file) {
    return left->file < right->file ? -1 : 1;
  }
  if (left->span.start != right->span.start) {
    return left->span.start < right->span.start ? -1 : 1;
  }
  /* Zero-length insertions at the start of a replaced span occur first. */
  if (left->span.end != right->span.end) {
    return left->span.end < right->span.end ? -1 : 1;
  }
  return left->order < right->order ? -1 : left->order != right->order;
}

static bool file_rebuild(struct config_delta_plan *plan, enum config_file which,
    const struct plan_edits *edits)
{
  struct config_plan_file *file = &plan->files[which];
  bool untouched = file->print_only;
  if (!file->print_only) {
    file->reason = NULL;
  }
  size_t size = file->before_size, consumed = 0;
  for (size_t i = 0; i < edits->count; ++i) {
    const struct plan_edit *edit = &edits->items[i];
    if (edit->file != which) {
      continue;
    }
    if (edit->span.start < consumed || edit->span.end < edit->span.start ||
        edit->span.end > file->before_size) {
      untouched = true;
      file->reason = "selected declarations overlap; only printed changes are permitted";
      plan->applicable = false;
      break;
    }
    consumed = edit->span.end;
    size_t removed = edit->span.end - edit->span.start;
    if (removed > size || edit->size > CONFIG_LITERAL_SOURCE_LIMIT - (size - removed)) {
      return false;
    }
    size = size - removed + edit->size;
  }
  if (untouched) {
    size = file->before_size;
  }
  char *after = malloc(size + 1);
  if (!after) {
    return false;
  }
  size_t source = 0, output = 0;
  if (!untouched) {
    for (size_t i = 0; i < edits->count; ++i) {
      const struct plan_edit *edit = &edits->items[i];
      if (edit->file != which) {
        continue;
      }
      size_t copied = edit->span.start - source;
      memcpy(after + output, file->before + source, copied);
      output += copied;
      if (edit->size) {
        memcpy(after + output, edit->bytes, edit->size);
        output += edit->size;
      }
      source = edit->span.end;
    }
  }
  if (file->before_size > source) {
    memcpy(after + output, file->before + source, file->before_size - source);
    output += file->before_size - source;
  }
  after[output] = '\0';
  free(file->after);
  file->after = after;
  file->after_size = output;
  file->after_present = file->original_present || (!untouched && file->before != NULL);
  file->changed = file->after_present != file->original_present ||
      output != file->before_size || (output && memcmp(file->before, after, output));
  return true;
}

bool config_delta_plan_rebuild(struct config_delta_plan *plan)
{
  struct plan_edits edits = {0};
  plan->applicable = true;
  for (size_t i = 0; i < CONFIG_FILE_COUNT; ++i) {
    if (plan->files[i].print_only) {
      plan->applicable = false;
    }
  }
  bool built = true;
  for (size_t i = 0; i < plan->count && built; ++i) {
    built = selected_edit(plan, &plan->changes[i], &edits);
  }
  if (built) {
    if (edits.count > 1) {
      qsort(edits.items, edits.count, sizeof(*edits.items), edit_order);
    }
    for (size_t i = 0; i < CONFIG_FILE_COUNT && built; ++i) {
      built = file_rebuild(plan, (enum config_file)i, &edits);
    }
  }
  for (size_t i = 0; i < edits.count; ++i) {
    free(edits.items[i].bytes);
  }
  free(edits.items);
  if (!built) {
    plan->applicable = false;
    fputs("config: cannot rebuild the bounded configuration delta plan\n", stderr);
  }
  return built;
}

bool config_delta_plan_choose(struct config_delta_plan *plan, size_t index,
    enum config_plan_choice choice)
{
  if (index >= plan->count || choice < CONFIG_CHOICE_KEEP || choice > CONFIG_CHOICE_REMOVE) {
    return false;
  }
  struct config_plan_change *change = &plan->changes[index];
  if ((choice == CONFIG_CHOICE_ADOPT && !change->can_adopt) ||
      (choice == CONFIG_CHOICE_APPLY && (!change->can_apply || change->kind == CONFIG_CHANGE_REMOVE)) ||
      (choice == CONFIG_CHOICE_REMOVE && (!change->can_apply || change->kind != CONFIG_CHANGE_REMOVE))) {
    return false;
  }
  change->choice = choice;
  return config_delta_plan_rebuild(plan);
}

void config_delta_plan_free(struct config_delta_plan *plan)
{
  for (size_t i = 0; i < plan->count; ++i) {
    target_free(&plan->changes[i].current);
    target_free(&plan->changes[i].migration_source);
  }
  free(plan->changes);
  for (size_t i = 0; i < CONFIG_FILE_COUNT; ++i) {
    struct config_plan_file *file = &plan->files[i];
    config_owned_free(file->ownership);
    config_literal_free(file->document);
    free(file->before);
    free(file->after);
  }
  *plan = (struct config_delta_plan){0};
}

const char *config_plan_choice_name(enum config_plan_choice choice)
{
  switch (choice) {
  case CONFIG_CHOICE_KEEP: return "keep";
  case CONFIG_CHOICE_APPLY: return "apply";
  case CONFIG_CHOICE_ADOPT: return "adopt";
  case CONFIG_CHOICE_REMOVE: return "remove";
  }
  return "invalid";
}

static void review_value(FILE *stream, const char *label, const struct config_literal_value *value)
{
  fprintf(stream, "  %s: ", label);
  if (!value) {
    fputs("absent\n", stream);
    return;
  }
  char *bytes = NULL;
  size_t size = 0;
  if (!config_render_value(value, &bytes, &size)) {
    fputs("unavailable\n", stream);
    return;
  }
  fwrite(bytes, 1, size, stream);
  fputc('\n', stream);
  free(bytes);
}

void config_delta_plan_review(const struct config_delta_plan *plan, size_t index, FILE *stream)
{
  if (index >= plan->count) {
    return;
  }
  const struct config_plan_change *change = &plan->changes[index];
  static const char *const origins[] = {"absent", "old default", "owner edit", "unknown; preserve owner value"};
  fprintf(stream, "%s: %s/%s (%s, %s), choice=%s\n", change->entry->id,
      config_file_name(change->entry->file), change->entry->name,
      change->required ? "required" : "optional", origins[change->origin],
      config_plan_choice_name(change->choice));
  fprintf(stream, "  reason: %s\n", change->reason ? change->reason : "unavailable");
  review_value(stream, "old default", change->old_entry ? change->old_entry->default_value : NULL);
  if (plan->files[change->entry->file].original_present) {
    review_value(stream, "current", change->current.value);
  } else {
    review_value(stream, "current", NULL);
    review_value(stream, "trusted new-file template", change->current.value);
  }
  if (change->entry->migration.kind == CONFIG_MIGRATION_MOVE) {
    review_value(stream, "migration source", change->migration_source.value);
  }
  review_value(stream, "new default", change->kind == CONFIG_CHANGE_REMOVE ? NULL : change->entry->default_value);
  if (change->entry->migration.kind == CONFIG_MIGRATION_REPLACE) {
    review_value(stream, "declared replacement", change->entry->migration.value);
  } else if (change->entry->migration.kind == CONFIG_MIGRATION_RENAME_FIELD) {
    fprintf(stream, "  field migration: %s -> %s\n", change->entry->migration.from,
        change->entry->migration.to);
  }
  if (change->migration_source.present) {
    fprintf(stream, "  migration source bytes: %s %zu..%zu (%s)\n",
        config_file_name(change->migration_file), change->migration_source.span.start,
        change->migration_source.span.end,
        change->migration_source.owned ? "owned" : "explicit source adoption required");
  }
  if (change->current.present) {
    fprintf(stream, "  %s bytes: %zu..%zu (%s)\n",
        plan->files[change->entry->file].original_present ? "consumed" : "trusted template",
        change->current.span.start, change->current.span.end,
        change->current.owned ? "owned" : "explicit adoption required");
  }
}
