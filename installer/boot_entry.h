#ifndef INSTALLER_BOOT_ENTRY_H
#define INSTALLER_BOOT_ENTRY_H

#include "boot_storage.h"
#include <stdio.h>

struct install_source;

struct install_boot_entry_paths {
  const char *kernel, *archive;
};

/* A menu's owned Pyxis entries. LEGACY_PREVIOUS describes a previous entry
 * found in a 0.0.9 menu; new menus never create one. ENTIRE_PROVEN allows
 * recovery to replace marked entries after proving the whole menu equal to
 * the committed record's digest-recorded previous menu. */
struct install_boot_menu {
  const struct install_boot_entry_paths *normal, *legacy_previous;
  bool entire_proven;
};

/* Pure planning over borrowed frozen bytes. Paths must be validated relative
 * kernel/archive paths, such as install_esp_payload_path output. INHERITED is
 * NULL when no committed record describes the menu, which is then never
 * replaced; otherwise the caller has certified its boot bindings and payload
 * evidence against this disk. The GUID names that same
 * reviewed target. These inputs confer no I/O authority or boot readiness.
 *
 * Only exact inherited Pyxis entries are replaceable: normal and rescue must
 * match INHERITED's normal pair, and a legacy previous entry its recorded pair,
 * unless ENTIRE_PROVEN rests on a digest-recorded whole menu. A legacy previous
 * entry is removed; markers alone do not establish ownership. An ambiguous menu is retained
 * byte-for-byte, with PENDING true. Inputs remain frozen and distinct from
 * caller-owned SELECTED, initialized as a config_blob. Keep the shared hash
 * context ready. Missing current input clears SELECTED and remains pending.
 * No target files are opened or changed. */
bool install_boot_entry_plan(const struct config_blob *current,
    const struct install_boot_menu *inherited, const uint8_t disk_guid[16],
    uint64_t pool_partition, const struct install_boot_menu *next,
    struct config_blob *selected, bool *pending);

/* Whether CURRENT is exactly the menu COMMITTED publishes. Same inputs and
 * hash context as install_boot_entry_plan. */
bool install_boot_entry_current(const struct config_blob *current,
    const struct install_boot_menu *committed, const uint8_t disk_guid[16],
    uint64_t pool_partition, bool *is_current);

/* Pure manual proposal: unmarked normal/rescue entries, no global directives.
 * Borrow validated relative PATHS and GUIDs, distinct from initialized owned
 * PAIR; keep the shared hash context ready. ESP_GUID NULL uses boot():/;
 * otherwise guid(ESP_GUID):/ names the GPT partition. DISK_GUID and
 * POOL_PARTITION always supply mount.disk and mount.pool. Success replaces
 * PAIR; failure leaves it unchanged. This confers no ownership, boot readiness
 * or activation authority and performs no I/O. */
bool install_boot_entry_render(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, const uint8_t esp_guid[16],
    struct config_blob *pair);
/* Fresh, exclusively created menu: owned normal/rescue pair plus the authored
 * globals. Uses the same boot-relative spelling recognized by later Review. */
bool install_boot_entry_render_owned(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, struct config_blob *menu);
void install_boot_entry_print(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], uint64_t pool_partition, FILE *output);

/* Caller owns source bytes/handles and the initialized shared hash context.
 * Success hashes exactly source->bytes; failure leaves OUTPUT absent. */
bool install_boot_payload_digest(const struct install_source *source,
    struct config_file_digest *output);

#endif
