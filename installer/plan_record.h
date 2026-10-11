#ifndef USERSPACE_INSTALLER_PLAN_RECORD_H
#define USERSPACE_INSTALLER_PLAN_RECORD_H

#include "installer.h"

#include <stdio.h>

#define INSTALL_PLAN_RECORD_MAX_BYTES (UINT64_C(16) * INSTALL_MIB)

/* Data only: hashes bind inspection inputs, not authority or authenticity.
 * Existing GUIDs bind logical disks; a zero GUID makes a plan nonportable. */
struct install_plan_record {
  bool updating, read_the_room, legacy, portable;
  uint64_t block_count, block_size, journal_mib;
  uint8_t existing_guid[16], pool_id[16], volume_id[16];
  struct install_layout layout;
  char revision[64], previous[64];
  char source_hash[65], target_hash[65];
};

bool install_plan_record_valid(const struct install_plan_record *record);
/* Strict canonical ASCII lines, in fixed order, framed by
 * "pyxis-install-plan 1\n" and "end\n". Revision bytes and IDs use lowercase
 * hex. Parsing stops at the end marker and changes OUTPUT only on success;
 * any following input belongs to the caller, never to this plan. */
bool install_plan_record_parse(FILE *input, struct install_plan_record *output);
/* Validate before output and report buffered I/O errors through fflush.
 * The caller must keep review/progress output off this stream. */
bool install_plan_record_format(FILE *output, const struct install_plan_record *record);

#endif
