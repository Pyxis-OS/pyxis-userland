#ifndef CONFIG_EDITOR_PLAN_H
#define CONFIG_EDITOR_PLAN_H

#include "catalog.h"
#include "ownership.h"
#include <stdio.h>

#define CONFIG_PLAN_CHANGE_LIMIT (2 * CONFIG_CATALOG_ENTRY_LIMIT)

struct config_plan_input {
  const char *bytes;
  size_t size;
  bool present;
};

enum config_change_kind {
  CONFIG_CHANGE_ADD, CONFIG_CHANGE_DEFAULT, CONFIG_CHANGE_ADOPT,
  CONFIG_CHANGE_MIGRATE, CONFIG_CHANGE_REMOVE,
};
enum config_value_origin {
  CONFIG_VALUE_ABSENT, CONFIG_VALUE_OLD_DEFAULT, CONFIG_VALUE_OWNER_EDIT,
  CONFIG_VALUE_UNKNOWN,
};
enum config_plan_choice {
  CONFIG_CHOICE_KEEP, CONFIG_CHOICE_APPLY, CONFIG_CHOICE_ADOPT, CONFIG_CHOICE_REMOVE,
};

struct config_plan_target {
  const struct literal_value *value;
  struct literal_span span;
  bool present, owned, marked, owns_value;
};

struct config_plan_change {
  const struct config_catalog_entry *old_entry, *entry;
  enum config_change_kind kind;
  enum config_value_origin origin;
  enum config_plan_choice choice;
  struct config_plan_target current, migration_source;
  enum config_file migration_file;
  /* Required delta, distinct from entry->required disposition. KEEP/ADOPT do
   * not satisfy an established required replacement or schema migration. */
  bool required, can_apply, can_adopt;
  const char *reason;
};

struct config_plan_file {
  /* Owned snapshot; missing files use a supplied trusted full-file default as
   * their editing template. original_present records which case was supplied. */
  char *before, *after;
  size_t before_size, after_size;
  bool original_present, after_present, print_only, changed, relevant;
  struct literal *document;
  struct config_owned_set *ownership;
  const char *reason;
};

struct config_delta_plan {
  /* Catalogs are borrowed and must outlive the plan, including all rebuilds. */
  const struct config_catalog *old_catalog, *catalog;
  struct config_plan_file files[CONFIG_FILE_COUNT];
  struct config_plan_change *changes;
  size_t count;
  bool applicable;
};

/* Pure byte planning, without file/volume/authority calls. CURRENT and DEFAULTS
 * are borrowed for the call; the plan owns snapshots and outputs. A missing file
 * may only be created from the caller's trusted complete revision defaults.
 * Unsupported Lua or ambiguous/mismatched markers keep that file untouched and
 * allow printed proposals only. Success does not certify the whole Lua triple:
 * the caller must run shared typed/merge validation on the rebuilt outputs.
 * False reports an allocation/size failure and leaves PLAN empty. */
bool config_delta_plan_build(const struct config_catalog *old_catalog,
    const struct config_catalog *catalog,
    const struct config_plan_input current[CONFIG_FILE_COUNT],
    const struct config_plan_input defaults[CONFIG_FILE_COUNT],
    struct config_delta_plan *plan);
/* Rebuild from immutable snapshots after changing choices. Inapplicability is
 * ordinary success with applicable=false; output still displays chosen changes.
 * Every source span refers to before, so publication can bind exact consumed
 * bytes/hashes without trusting a printed plan to grant authority. */
bool config_delta_plan_rebuild(struct config_delta_plan *plan);
bool config_delta_plan_choose(struct config_delta_plan *plan, size_t index,
    enum config_plan_choice choice);
void config_delta_plan_free(struct config_delta_plan *plan);
void config_delta_plan_review(const struct config_delta_plan *plan, size_t index, FILE *stream);
const char *config_plan_choice_name(enum config_plan_choice choice);

#endif
