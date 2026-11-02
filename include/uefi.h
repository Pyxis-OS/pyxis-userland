#ifndef USERSPACE_UEFI_H
#define USERSPACE_UEFI_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <abi/syscall.h>
#include <abi/uefi.h>

/* Borrowed UEFI grant. Success means the fixed Pyxis boot option and BootOrder
 * were read back after every effective write. OUTCOME_UNKNOWN means inspect
 * firmware state before preparing another registration attempt. */
enum call_status uefi_boot_register(handle_t uefi,
    const struct uefi_boot_register_request *request);

/* Load options and device paths, as stored in Boot#### variables. This is the
 * one parser: bootmgr and any boot-device identity use it. Pointers refer into
 * the caller's bytes, which must outlive the structs. Nothing here touches the
 * firmware. */
#define UEFI_PATH_MEDIA 4u
#define UEFI_PATH_HARD_DRIVE 1u
#define UEFI_PATH_FILE 4u
#define UEFI_PATH_END 0x7fu
#define UEFI_PATH_END_ENTIRE 0xffu
#define UEFI_LOAD_OPTION_ACTIVE UINT32_C(1)
#define UEFI_SIGNATURE_MBR 1u
#define UEFI_SIGNATURE_GPT 2u

struct uefi_load_option {
  uint32_t attributes;
  const uint8_t *description; /* little-endian UCS-2, without its NUL */
  size_t description_units;
  const uint8_t *path; /* device path nodes, through the end node */
  size_t path_size;
  const uint8_t *data; /* optional data after the path */
  size_t data_size;
};

struct uefi_path_node {
  uint8_t type, subtype;
  const uint8_t *data; /* node bytes after the 4-byte header */
  size_t data_size;
};

/* A media hard-drive node: the partition the firmware boots from. For a GPT
 * partition the signature is its unique partition GUID in on-disk byte order. */
struct uefi_hard_drive {
  uint32_t partition_number;
  uint64_t first_lba, block_count;
  uint8_t signature[16];
  uint8_t signature_type; /* UEFI_SIGNATURE_MBR or UEFI_SIGNATURE_GPT */
};

/* Splits a Boot#### value. False when it is too short or its lengths overrun. */
bool uefi_load_option_parse(const void *bytes, size_t size, struct uefi_load_option *option);
/* Writes the description as ASCII ('?' for other characters), always NUL
 * terminated, truncating to capacity; returns the characters stored. */
size_t uefi_load_option_label(const struct uefi_load_option *option, char *label, size_t capacity);
/* Steps through nodes from *offset (start at 0). False at the end node or at a
 * malformed node (length under 4 or past the path). */
bool uefi_path_next(const uint8_t *path, size_t size, size_t *offset, struct uefi_path_node *node);
bool uefi_path_hard_drive(const struct uefi_path_node *node, struct uefi_hard_drive *drive);

/* Boot manager (UEFI_RIGHT_BOOT_MANAGE). The kind selects the variable and, for
 * UEFI_VARIABLE_BOOT_ENTRY, number selects Boot####. get stores up to capacity
 * bytes and sets *size; NOT_FOUND means the variable is absent and LIMIT that
 * the value does not fit. set and delete succeed only after the firmware
 * variable was read back; OUTCOME_UNKNOWN means inspect it before retrying.
 * entries sets bit n of bitmap (UEFI_BOOT_ENTRY_WINDOW / 8 bytes) when Boot####
 * for first + n exists. */
enum call_status uefi_boot_variable_get(handle_t uefi, uint32_t kind, uint32_t number,
    uint32_t *attributes, void *value, size_t capacity, size_t *size);
enum call_status uefi_boot_variable_set(handle_t uefi, uint32_t kind, uint32_t number,
    const void *value, size_t size);
enum call_status uefi_boot_variable_delete(handle_t uefi, uint32_t kind, uint32_t number);
enum call_status uefi_boot_entries(handle_t uefi, uint32_t first, uint32_t count,
    uint8_t bitmap[UEFI_BOOT_ENTRY_WINDOW / 8]);

#endif
