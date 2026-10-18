#ifndef USERSPACE_INSTALLER_ARCHIVE_CONFIG_H
#define USERSPACE_INSTALLER_ARCHIVE_CONFIG_H

#include "literal.h"
#include <stdint.h>

#define INSTALL_ARCHIVE_CONFIG_MAX_BYTES LITERAL_SOURCE_LIMIT

/* Rebind system's npfs selector and a reserved native home sharing its old
 * partition; separate explicit and RAM roots remain unchanged. Literal data
 * only. Success
 * with *output NULL preserves the original bytes and sets *output_size to size.
 * Otherwise the caller owns the rendered replacement. Failure clears outputs. */
bool install_archive_config_rebind(const char *bytes, size_t size, uint64_t pool_partition,
    char **output, size_t *output_size);

#endif
