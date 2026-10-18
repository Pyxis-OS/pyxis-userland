#ifndef USERSPACE_INSTALLER_CONFIGURATION_H
#define USERSPACE_INSTALLER_CONFIGURATION_H

#include "../config-editor/plan.h"
#include "../config-editor/storage.h"
#include "../libconfig/selection.h"

struct install_configuration_input {
  const struct config_blob *current; /* CONFIG_ALL_FILES */
  const struct config_blob *record;
  const struct config_blob *archive; /* CONFIG_LUA_COUNT */
  const struct config_blob *catalog;
  const struct config_blob *pool_override; /* NULL means absent */
  const char *kernel_revision, *program_revision;
};

/* Pure owned snapshots: no retained paths, mounts, grants or live handles.
 * Initialize with {0}, then free before preparing again. Catalog views borrow
 * nodes from the owned decoded catalogs; callers must not move this struct.
 * AFTER is the reviewed publication, including unchanged Limine. Archive mode
 * preserves all current Lua/catalog bytes and certifies no ESP configuration.
 * SELECTION owns the complete frozen typed set for the successor handoff. */
struct install_configuration {
  struct config_blob current[CONFIG_ALL_FILES], after[CONFIG_ALL_FILES];
  struct config_blob record, archive[CONFIG_LUA_COUNT], catalog_source, pool_override;
  struct config_catalog *catalog, *old_catalog;
  struct config_catalog lua_catalog, old_lua_catalog;
  struct config_delta_plan delta;
  struct config_selection selection;
  enum config_active_source source;
  enum config_legacy_disposition pool_disposition;
  char kernel_revision[65], program_revision[65];
  bool complete, pool_valid, migrate_pool, pool_conflict, record_refused;
  bool needs_adoption[CONFIG_LUA_COUNT], adopted[CONFIG_LUA_COUNT];
  bool source_reviewed, applicable;
  const char *reason;
};

/* Inputs are borrowed only during the call; their owned bytes and hashes are
 * copied. A prepared/malformed record is retained as a refusal, never treated
 * as absent. True may produce an inapplicable reviewable plan. Shared PSA hash
 * initialization is the caller's responsibility, as for config_blob_set. */
bool install_configuration_prepare(const struct install_configuration_input *input,
    struct install_configuration *plan);
bool install_configuration_choose(struct install_configuration *plan, size_t index,
    enum config_plan_choice choice);
/* Adoption grants only this frozen file's publication; it does not replace
 * its owner values or accept any separately declined required delta. */
bool install_configuration_adopt(struct install_configuration *plan,
    enum config_storage_file file, bool adopt);
/* REVIEWED acknowledges the displayed source change and retained inactive
 * bytes. Final installer Review still supplies the publication consent. */
bool install_configuration_source(struct install_configuration *plan,
    enum config_active_source source, bool reviewed);
/* A valid personal pool migration is preselected. Declining leaves its bytes
 * untouched and requires a separately reviewed source disposition. */
bool install_configuration_pool(struct install_configuration *plan, bool migrate,
    bool reviewed);
void install_configuration_summary(const struct install_configuration *plan, FILE *output);
void install_configuration_review(const struct install_configuration *plan, FILE *output);
/* Compare freshly observed borrowed digests/revisions at the exclusive boundary.
 * This grants no authority and performs no reads or automatic replanning. */
bool install_configuration_revalidate(const struct install_configuration *plan,
    const struct install_configuration_input *input);
void install_configuration_free(struct install_configuration *plan);

#endif
