#ifndef USERSPACE_INSTALLER_PACKAGES_H
#define USERSPACE_INSTALLER_PACKAGES_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

/* Install fetches the base package set (docs/wip/pkg-upgrade.md, decisions 63
 * and 67) by launching boot://pkg.pxb with the archive's repository
 * configuration and the HTTP and HTTPS providers installer-init started. */
#define INSTALL_BASE_PACKAGE "base"

/* pkg check base: the repository offers base for this system. Needs no
 * packages volume; runs before Review, so a refusal writes nothing. */
bool install_packages_check(void);

/* pkg --yes install base into the packages volume of the new pool. */
bool install_packages_base(handle_t disk, uint64_t pool_partition);

struct install_plan;
/* Classic Install records the active set's boot identity; Classic Update
 * stages a complete target-ABI set and durable pending proof before ESP writes. */
bool install_packages_boot_mark(const struct install_plan *plan);
bool install_packages_update_stage(const struct install_plan *plan);

#endif
