#ifndef INSTALLER_BOOT_ENTRY_H
#define INSTALLER_BOOT_ENTRY_H

#include "../config-editor/storage.h"
#include <stdio.h>

#define INSTALL_BOOT_LEGACY_PAYLOADS 3u

struct install_source;

struct install_boot_entry_paths {
  const char *kernel, *archive;
};

/* Pure planning over borrowed frozen bytes. Paths must be validated relative
 * kernel/archive paths, such as install_esp_payload_path output. PREVIOUS is
 * NULL for legacy admission; otherwise the caller has certified its boot
 * binding and payload evidence against this disk. The GUID names that same
 * reviewed target. These inputs confer no I/O authority or boot readiness.
 *
 * Only exact inherited Pyxis entries are replaceable. Markers alone do not
 * establish ownership. An ambiguous menu is retained byte-for-byte, with
 * PENDING true; LEGACY_PAIR_VERIFIED comes from the read-only certifier below.
 * Inputs remain frozen and distinct from caller-owned SELECTED, initialized as
 * a config_blob. Keep the shared hash context ready. Missing current input
 * clears SELECTED and remains pending. No target files are opened or changed. */
bool install_boot_entry_plan(const struct config_blob *current,
    const struct install_boot_entry_paths *previous, const uint8_t disk_guid[16],
    const struct install_boot_entry_paths *next, bool legacy_pair_verified,
    struct config_blob *selected, bool *adoption, bool *pending);

/* Pure manual proposal: unmarked normal/rescue entries, no global directives.
 * Borrow validated relative PATHS and GUIDs, distinct from initialized owned
 * PAIR; keep the shared hash context ready. ESP_GUID NULL uses boot():/;
 * otherwise guid(ESP_GUID):/ names the GPT partition. DISK_GUID always supplies
 * mount.disk. Success replaces PAIR; failure leaves it unchanged. This confers
 * no ownership, boot readiness or activation authority and performs no I/O. */
bool install_boot_entry_render(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], const uint8_t esp_guid[16], struct config_blob *pair);
void install_boot_entry_print(const struct install_boot_entry_paths *paths,
    const uint8_t disk_guid[16], FILE *output);

/* Caller owns source bytes/handles and the initialized shared hash context.
 * Success hashes exactly source->bytes; failure leaves OUTPUT absent. */
bool install_boot_payload_digest(const struct install_source *source,
    struct config_file_digest *output);

/* Borrow an already admitted native read-only root. Certify exactly
 * boot/caelum.elf, boot/initrd.cpio and boot/revision using the existing ELF,
 * rescue archive and revision checks plus complete hashes, in that order.
 * True means eligible content; missing, invalid or failed inspection returns
 * false with every digest absent. No file handles escape. This is sampled
 * content evidence, not mount identity, ownership/adoption consent or authority;
 * the caller binds the root/GUID and revalidates before its own publication.
 * The caller keeps the shared hash context initialized throughout inspection. */
bool install_boot_legacy_certify(handle_t root,
    struct config_file_digest payloads[INSTALL_BOOT_LEGACY_PAYLOADS]);

#endif
