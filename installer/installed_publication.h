#ifndef USERSPACE_INSTALLER_INSTALLED_PUBLICATION_H
#define USERSPACE_INSTALLER_INSTALLED_PUBLICATION_H

#include "configuration.h"
#include "publication.h"
#include "recovery.h"
#include "digest.h"

struct install_plan;

/* One owned part of the ordinary install plan; no second confirmation slot.
 * All snapshots/choices freeze before ready. Execution borrows the plan's view. */
struct install_installed_publication {
  struct install_configuration configuration;
  struct install_recovery recovery;
  struct config_target_facts facts;
  struct config_boot_binding binding;
  struct config_file_digest legacy[INSTALL_BOOT_PAYLOADS];
  uint64_t cluster_bytes, required_bytes, available_bytes;
  bool prepared, fresh, legacy_active, activation_pending;
};

bool install_installed_prepare(struct install_plan *plan);
bool install_installed_hash(const struct install_plan *plan, struct install_digest *digest);
void install_installed_report(const struct install_plan *plan, FILE *output);
/* Admit/revalidate before the first program write; never create publication
 * parents or provision recovery until all consumed current bytes match. */
bool install_installed_admit(struct install_plan *plan);
bool install_installed_publish(struct install_plan *plan);
void install_installed_destroy(struct install_installed_publication *publication);

/* Discovery through the shared FAT reader. ABSENT means no readiness record;
 * malformed/prepared/pending evidence refuses normal Update. */
enum install_installed_probe { INSTALL_INSTALLED_ABSENT, INSTALL_INSTALLED_READY, INSTALL_INSTALLED_REFUSED };
enum install_installed_probe install_installed_probe(const struct install_disk *disk,
    const struct install_layout *layout, char revision[64], const char **reason);

#endif
