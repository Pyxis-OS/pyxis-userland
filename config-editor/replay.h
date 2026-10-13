#ifndef CONFIG_EDITOR_REPLAY_H
#define CONFIG_EDITOR_REPLAY_H

#include "plan.h"

#define CONFIG_REPLAY_INPUTS 10
#define CONFIG_REPLAY_TARGET_MAX 512

struct config_replay_input {
  bool present;
  char sha256[65];
};
struct config_replay_choice {
  char id[CONFIG_ENTRY_ID_LIMIT + 1];
  enum config_plan_choice choice;
};
struct config_replay {
  char target[CONFIG_REPLAY_TARGET_MAX + 1];
  struct config_replay_input inputs[CONFIG_REPLAY_INPUTS];
  struct config_replay_choice choices[CONFIG_PLAN_CHANGE_LIMIT];
  size_t count;
};

/* The target is normalized issuer evidence, never a selector or authority.
 * Inputs bind every consumed file, old record, catalog and archive default.
 * Replay always rebuilds from current sources and then applies exact choices;
 * mismatch refuses instead of replanning an already confirmed operation. */
bool config_replay_write(FILE *stream, const char *target,
    const struct config_replay_input inputs[CONFIG_REPLAY_INPUTS],
    const struct config_delta_plan *plan);
/* Consume one end-delimited frame, leaving subsequent confirmation input. */
bool config_replay_read(FILE *stream, struct config_replay *replay);
bool config_replay_matches(const struct config_replay *replay, const char *target,
    const struct config_replay_input inputs[CONFIG_REPLAY_INPUTS],
    struct config_delta_plan *plan);

#endif
