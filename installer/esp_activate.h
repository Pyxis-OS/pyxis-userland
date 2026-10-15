#ifndef USERSPACE_INSTALLER_ESP_ACTIVATE_H
#define USERSPACE_INSTALLER_ESP_ACTIVATE_H

#include "installer.h"
#include <abi/mount.h>

struct install_esp_activation_request {
  handle_t root, control;
  struct mount_state_reply expected;
  const char *configuration;
  size_t configuration_bytes;
  const char *revision;
  size_t revision_bytes;
};

/* Classic targets only: the caller renders the explicit normal/rescue menu and
 * supplies reviewed issuer-bound OBSERVE facts. All handles/text are borrowed.
 * Admission, BEGIN/CHECK/END and retirement remain with the caller. Writes only
 * boot/revision and boot/limine/limine.conf, then syncs and reopens exact paths
 * for identity/size/byte checks. Directories must already exist. No formatting,
 * rollback, cleanup or retry; failure may follow persistent partial changes.
 * This is not an atomic two-file publication. Status is CALL_OK on success. */
bool install_esp_activate_classic(const struct install_esp_activation_request *request,
    enum call_status *status);

#endif
