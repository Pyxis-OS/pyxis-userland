#ifndef USERSPACE_INSTALLER_PUBLICATION_H
#define USERSPACE_INSTALLER_PUBLICATION_H

#include "../config-editor/storage.h"
#include "installer.h"
#include "esp_payload.h"

#define INSTALL_BOOT_PAYLOADS 3u
#define INSTALL_BOOT_PAYLOAD_MAX_BYTES UINT64_C(1073741824)
#define INSTALL_BOOT_MANIFEST_NAME "ownership.record"

/* All handles are borrowed from the already admitted native view. No mount,
 * format, raw write, authority creation or boot-time activation occurs here.
 * Source bytes and expected bytes are frozen review inputs. EXPECTED is the
 * current owned menu and SELECTED the reviewed one; the caller renders it. */
struct install_publication_request {
  handle_t esp_root;
  handle_t workspace; /* Borrowed private native RAM parent, stage only. */
  struct mount_state_reply expected_target;
  struct config_storage_view view;
  struct config_target_facts facts;
  const struct config_blob *expected;
  const struct config_blob *expected_record;
  const struct config_blob *selected;
  const char *transaction;
  struct config_boot_binding binding;
  struct install_source sources[INSTALL_BOOT_PAYLOADS];
  /* Supplied only after explicit admission of the exact canonical legacy
   * boot pair, in caelum.elf/initrd.cpio/revision order under boot/. */
  bool legacy_active;
  struct config_file_digest legacy_expected[INSTALL_BOOT_PAYLOADS];
};

/* Internal retained staging evidence, never caller-authored authority. */
struct install_staged_boot {
  handle_t root;
  struct install_esp_payloads *payloads;
  struct config_boot_binding binding;
  bool legacy_active;
  handle_t legacy_files[INSTALL_BOOT_PAYLOADS];
  struct config_file_digest legacy_expected[INSTALL_BOOT_PAYLOADS];
};

/* WORKSPACE is a borrowed private native RAM parent. Each admission hashes all
 * bytes through an owned unlinked RAM transfer FILE and closes it before return. */
bool install_publication_legacy_check(handle_t root, handle_t workspace,
    const struct config_file_digest expected[INSTALL_BOOT_PAYLOADS],
    struct config_storage_report *report);
/* Internal pending-activation admission: retain exact legacy identities and
 * verify the reviewed bytes before any ESP mutation. Close returned handles. */
bool install_publication_legacy_hold(struct install_staged_boot *boot, handle_t workspace,
    uint64_t domain, struct config_storage_report *report);
bool install_publication_publish(const struct install_publication_request *request,
    struct config_storage_report *report);
bool install_publication_verify(const struct install_staged_boot *boot,
    uint64_t domain, struct config_storage_report *report);
bool install_publication_cleanup(const struct install_staged_boot *boot,
    const struct config_boot_binding *obsolete, const struct config_storage_view *view,
    const struct config_target_facts *facts, uint64_t interval,
    struct config_storage_report *report);

#endif
