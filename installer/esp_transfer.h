#ifndef USERSPACE_INSTALLER_ESP_TRANSFER_H
#define USERSPACE_INSTALLER_ESP_TRANSFER_H

#include "installer.h"

/* Internal shared transport; callers translate status into their own reports.
 * Callers validate their existing size/digest policy and supply max_bytes.
 * Parent is an explicit native RAM directory. Success owns an empty FILE whose
 * name is already unlinked. Failure may still return an owned FILE: close it.
 * The parent is borrowed only for this call; it is never closed here. */
enum call_status install_esp_transfer_workspace(handle_t parent, handle_t *file);
/* Hash/copy the exact source extent through bounded READ/WRITE messages.
 * A target of HANDLE_INVALID performs only the source hash check. */
enum call_status install_esp_transfer_stream(const struct install_source *source, handle_t target,
    uint64_t expected_size, const char *expected_sha256, uint64_t max_bytes);
/* Stage memory through the supplied retained RAM FILE, or transfer a native
 * source FILE directly. Each native transfer is at most FILE_TRANSFER_MAX_BYTES.
 * Source handles/memory and all arguments remain borrowed and unchanged. */
enum call_status install_esp_transfer_source(const struct install_source *source, handle_t target,
    handle_t workspace, uint64_t expected_size, const char *expected_sha256, uint64_t max_bytes);
/* Verifying transfers source FAT bytes into destination RAM at offset zero
 * and hashes only confirmed RAM ranges. Otherwise hash confirmed source ranges.
 * Workspace remains owned by the caller through verification and close.
 * No error retry, fallback, sync, truncate of the ESP, or interval management. */
enum call_status install_esp_transfer_file(handle_t source, handle_t destination, bool verifying,
    uint64_t expected_size, const char *expected_sha256, uint64_t max_bytes);

#endif
