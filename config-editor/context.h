#ifndef CONFIG_EDITOR_CONTEXT_H
#define CONFIG_EDITOR_CONTEXT_H

#include "plan.h"
#include "replay.h"
#include "storage.h"
#include "../libconfig/selection.h"

struct config_editor_context {
  struct config_storage_view view;
  struct config_target_facts target;
  char target_text[CONFIG_REPLAY_TARGET_MAX + 1];
  struct config_blob current[CONFIG_ALL_FILES], record, catalog_source;
  struct config_blob archive[CONFIG_LUA_COUNT];
  struct config_catalog *catalog, *old_catalog;
  struct config_delta_plan plan;
  bool writer, complete;
};

bool config_editor_load(struct config_editor_context *context);
bool config_editor_plan(struct config_editor_context *context);
bool config_editor_validate(struct config_editor_context *context, bool planned);
void config_editor_hashes(const struct config_editor_context *context,
    struct config_replay_input inputs[CONFIG_REPLAY_INPUTS]);
bool config_editor_publish(struct config_editor_context *context);
void config_editor_close(struct config_editor_context *context);

#endif
