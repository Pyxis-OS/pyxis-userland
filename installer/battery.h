#ifndef INSTALLER_BATTERY_H
#define INSTALLER_BATTERY_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdio.h>

/* Read the same ACPI snapshot as the power widget; no firmware operation or
 * retained authority. Failure prints a refusal before consent or any write. */
bool install_battery_check(handle_t system_info, FILE *output);

#endif
