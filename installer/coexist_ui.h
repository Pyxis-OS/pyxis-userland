#ifndef USERSPACE_INSTALLER_COEXIST_UI_H
#define USERSPACE_INSTALLER_COEXIST_UI_H

#include "installer.h"
#include <term.h>

struct install_coexist_choice {
  struct disk_info info;
  struct install_layout layout;
  bool updating;
  struct install_update update;
  const char *reason;
  /* Normally invalid; caller owns this RO handle if its close failed. */
  handle_t pending_inspection;
};

/* Read-only interactive selection for the caller's Install/Update action.
 * New Install requires a caller-generated pool GUID; Update ignores it.
 * Success freezes selected GPT identities/extents and leaves no open handles.
 * Update validates only the selected native pool; active revision remains
 * unknown. ESP admission, source capacity and final Review remain caller work.
 * False includes cancellation or unavailable inspection; inspect reason and
 * close pending_inspection when noninvalid. No mount, replay or writes occur.
 * Output must be fresh, with no pending handle from a previous invocation. */
bool install_coexist_ui_choose(struct terminal *terminal, handle_t disks, bool updating,
    const uint8_t new_pool_guid[16], struct install_coexist_choice *choice);

/* Revalidate the trusted selector's exact Update choice without input or
 * fallback. GUID is the canonical disk text emitted by install_guid_text;
 * target and both GPT entries must still identify eligible native metadata.
 * ESP admission and final Review remain caller work. Output and failed-close
 * ownership follow choose above; no pool mount, replay or write occurs. */
bool install_coexist_ui_selected(handle_t disks, uint64_t target, uint64_t esp_entry,
    uint64_t pool_entry, const char *guid, struct install_coexist_choice *choice);

#endif
