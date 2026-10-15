#ifndef USERSPACE_ACPI_TABLES_H
#define USERSPACE_ACPI_TABLES_H

#include <abi/acpi_tables.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Describe one table of the acpi_tables root (the acpi:// binding) by its
 * listed name. Requires ENUMERATE on the directory. A name the root does not
 * hold is NOT_FOUND; any other directory returns BAD_OPERATION. Failure clears
 * *info. Listing and reading use the ordinary directory and file helpers. */
enum call_status acpi_table_info(handle_t directory, const char *name,
    struct acpi_table_info *info);

#endif
