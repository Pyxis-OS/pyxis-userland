#ifndef USERSPACE_PACKAGE_MAINTENANCE_H
#define USERSPACE_PACKAGE_MAINTENANCE_H

#include <abi/handle.h>
#include <stdbool.h>
#include <bundle_launch.h>
#include <package.h>
#include <string.h>

#define PACKAGE_MANAGEMENT_SOURCE "package_management"
#define PACKAGE_INSPECT_SOURCE "package_inspect"
#define PACKAGE_CONFIG_SOURCE "package_config"
#define PACKAGE_CONFIG_WRITE_SOURCE "package_config_write"
/* Selected only for a package_upgrade space: the boot ESP issuer with
 * OBSERVE|UPGRADE_BOOT_ESP, and boot-init's READ-only boot evidence. */
#define PACKAGE_UPGRADE_SOURCE "boot_esp_upgrade"
#define PACKAGE_EVIDENCE_SOURCE "boot_esp_evidence"

/* Trusted session successors carry these selected sources privately. Ordinary
 * children receive only sources explicitly requested by their bundle. */
static inline bool package_launch_source(const char *name)
{
  return !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_SOURCE) ||
      !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_MANAGEMENT_SOURCE) ||
      !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_INSPECT_SOURCE) ||
      !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_CONFIG_SOURCE) ||
      !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_CONFIG_WRITE_SOURCE) ||
      !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_UPGRADE_SOURCE) ||
      !strcmp(name, BUNDLE_SOURCE_PREFIX PACKAGE_EVIDENCE_SOURCE);
}

/* Trusted boot only, before any provider or package consumer starts. Missing
 * handles are allowed; cleanup failure reports locally and never stops boot. */
void package_boot_maintain(handle_t packages, handle_t http_work, handle_t clock);

#endif
