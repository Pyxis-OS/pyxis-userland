#ifndef USERSPACE_UEFI_H
#define USERSPACE_UEFI_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <abi/uefi.h>

/* Borrowed UEFI grant. Success means the fixed Pyxis boot option and BootOrder
 * were read back after every effective write. OUTCOME_UNKNOWN means inspect
 * firmware state before preparing another registration attempt. */
enum call_status uefi_boot_register(handle_t uefi,
    const struct uefi_boot_register_request *request);

#endif
