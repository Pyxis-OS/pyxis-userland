#ifndef USERSPACE_SYSTEM_INFO_H
#define USERSPACE_SYSTEM_INFO_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <abi/system_info.h>

/* Borrowed READ grant. All queries leave the caller's output unchanged on
 * failure. Empty build_revision or brand strings mean unavailable information;
 * memory describes the global allocator, not installed RAM or process usage. */
enum call_status system_info_get_identity(handle_t system_info,
    struct system_info_identity *identity);
enum call_status system_info_get_cpu(handle_t system_info, struct system_info_cpu *cpu);
enum call_status system_info_get_memory(handle_t system_info, struct system_info_memory *memory);

/* PCI inventory state and the number of retained functions. Unavailable
 * inventories report zero functions. */
enum call_status system_info_get_pci(handle_t system_info, struct system_info_pci *pci);
/* One retained function, for indices below function_count; later indices
 * return NOT_FOUND. Order is stable for the boot but otherwise unspecified. */
enum call_status system_info_get_pci_function(handle_t system_info, uint64_t index,
    struct system_info_pci_function *function);

#endif
