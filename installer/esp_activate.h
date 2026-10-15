#ifndef USERSPACE_INSTALLER_ESP_ACTIVATE_H
#define USERSPACE_INSTALLER_ESP_ACTIVATE_H

#include "installer.h"
#include "boot_entry.h"
#include <abi/mount.h>

#define INSTALL_ESP_CLASSIC_PAYLOAD_COUNT 3u

enum install_esp_classic_payload {
  INSTALL_ESP_CLASSIC_EFI, INSTALL_ESP_CLASSIC_KERNEL, INSTALL_ESP_CLASSIC_ARCHIVE,
};

struct install_esp_activation_request {
  handle_t root, control;
  handle_t workspace; /* Borrowed private native RAM parent, used only during stage. */
  struct mount_state_reply expected;
  struct install_source payloads[INSTALL_ESP_CLASSIC_PAYLOAD_COUNT];
  struct config_file_digest expected_payloads[INSTALL_ESP_CLASSIC_PAYLOAD_COUNT];
  const char *configuration;
  size_t configuration_bytes;
  const char *revision;
  size_t revision_bytes;
};

struct install_esp_classic_files;

/* Accepted classic fixed paths: EFI/BOOT/BOOTX64.EFI, boot/caelum.elf and
 * boot/initrd.cpio, in payload order. Creates missing EFI/BOOT and boot/limine
 * parents without making attempt directories. The caller has explicitly
 * authorized classic installation; paths/names confer no authority themselves.
 * Root/control, workspace parent and sources are borrowed; source bytes remain
 * fixed through stage. The helper creates and immediately unlinks a RAM FILE,
 * retaining it for bounded transfers through final verify and close. Expected
 * digests are frozen plan evidence, not ownership credentials.
 * Keep the shared hash context initialized through verify. Initialize *result
 * to NULL; failure may still return owned files/parents and partial changes.
 * Native FILE transfers grow through bounded copies and truncate only after a
 * matching copy hash. Success syncs all files, parents and the mount. No retry,
 * fallback or rollback. The classic UINT32_MAX file-size bound is unchanged.
 * Close any returned result before retiring the caller's view. */
bool install_esp_classic_stage(const struct install_esp_activation_request *request,
    struct install_esp_classic_files **result, enum call_status *status);
/* Borrowed root/control must remain alive. Sources are no longer needed.
 * Reobserve issuer-bound facts, sync, then reopen canonical parents/files and
 * compare retained native object identities, exact sizes and complete hashes.
 * Verification writes only the retained private RAM FILE, never ESP payloads.
 * The caller stages outside its publication interval, then BEGIN/CHECKs before
 * this verify and the small activation below. No interval is extended here. */
bool install_esp_classic_verify(struct install_esp_classic_files *result,
    enum call_status *status);
/* Releases held files/parents and the RAM FILE, and always frees result;
 * never closes the borrowed workspace parent, retires or deletes ESP objects.
 * False reports invalid-handle bookkeeping, not retained authority. The caller
 * clears its pointer on either result and still retires its drained view. */
bool install_esp_classic_close(struct install_esp_classic_files *result);

/* Classic fixed-path publication only: the caller renders the normal/rescue
 * menu and supplies reviewed issuer-bound OBSERVE facts. All handles/text are
 * borrowed. Admission, BEGIN/CHECK/END and retirement remain with the caller.
 * After staging and verifying the held payloads above, writes only
 * boot/revision and boot/limine/limine.conf, then syncs and reopens exact paths
 * for identity/size/byte checks. Directories must already exist. No formatting,
 * rollback, cleanup or retry; failure may follow persistent partial changes.
 * This is not an atomic two-file publication. Status is CALL_OK on success. */
bool install_esp_activate_classic(const struct install_esp_activation_request *request,
    enum call_status *status);

#endif
