#ifndef USERSPACE_INSTALLER_INSTALLED_PUBLICATION_H
#define USERSPACE_INSTALLER_INSTALLED_PUBLICATION_H

#include "boot_publisher.h"

struct install_plan;

/* The installer's front end to the shared boot publisher. The plan supplies
 * the reviewed target and sources; execution borrows the plan's view. */
bool install_installed_prepare(struct install_plan *plan);
bool install_installed_hash(const struct install_plan *plan, struct install_digest *digest);
void install_installed_report(const struct install_plan *plan, FILE *output, bool verbose);
/* Admit/revalidate before the first program write; never create publication
 * parents or provision recovery until all consumed current bytes match. */
bool install_installed_admit(struct install_plan *plan);
bool install_installed_publish(struct install_plan *plan);
void install_installed_destroy(struct boot_publication *publication);

/* Discovery through the shared FAT reader. PENDING routes an inherited committed
 * record to a fresh completion Review; it proves no writable completion is safe.
 * Prepared/malformed records and failed inspection refuse normal Update. */
/* ABSENT: no readable ESP. UNRECORDED: a readable ESP without a boot record,
 * normal for Coexist; a Classic install always has one (decision 28). */
enum install_installed_probe {
  INSTALL_INSTALLED_ABSENT, INSTALL_INSTALLED_UNRECORDED, INSTALL_INSTALLED_READY,
  INSTALL_INSTALLED_PENDING, INSTALL_INSTALLED_REFUSED,
};
enum install_installed_probe install_installed_probe(const struct install_disk *disk,
    const struct install_layout *layout, char revision[64], const char **reason);

#endif
