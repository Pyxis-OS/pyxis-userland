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

/* The immutable name selected by boot init. Only boot init's initial grant
 * holds SET_HOSTNAME_ONCE; every child receives READ alone. */
enum call_status system_info_get_hostname(handle_t system_info,
    struct system_info_hostname *hostname);
enum call_status system_info_set_hostname_once(handle_t system_info,
    const struct system_info_hostname *hostname);

/* PCI inventory state and the number of retained functions. Unavailable
 * inventories report zero functions. */
enum call_status system_info_get_pci(handle_t system_info, struct system_info_pci *pci);
/* One retained function, for indices below function_count; later indices
 * return NOT_FOUND. Order is stable for the boot but otherwise unspecified. */
enum call_status system_info_get_pci_function(handle_t system_info, uint64_t index,
    struct system_info_pci_function *function);

/* Immutable boot USB snapshot. INITIALIZING and UNAVAILABLE carry zero counts;
 * indexed queries use global indices and return NOT_FOUND beyond those counts.
 * No query rescans hardware, sends transfers or adds device authority. */
enum call_status system_info_get_usb(handle_t system_info, struct system_info_usb *usb);
enum call_status system_info_get_usb_controller(handle_t system_info, uint64_t index,
    struct system_info_usb_controller *controller);
enum call_status system_info_get_usb_device(handle_t system_info, uint64_t index,
    struct system_info_usb_device *device);
enum call_status system_info_get_usb_interface(handle_t system_info, uint64_t index,
    struct system_info_usb_interface *interface);

/* Power-source state from the kernel's latest ACPI poll, a few seconds old at
 * most; sample_ns is zero before the first poll or without ACPI. Battery
 * records are indexed below battery_count; later indices return NOT_FOUND.
 * Numeric battery fields may be SYSTEM_INFO_BATTERY_UNKNOWN. */
enum call_status system_info_get_power(handle_t system_info, struct system_info_power *power);
enum call_status system_info_get_battery(handle_t system_info, uint64_t index,
    struct system_info_battery *battery);

#endif
