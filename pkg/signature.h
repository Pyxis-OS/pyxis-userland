#ifndef USERSPACE_PKG_SIGNATURE_H
#define USERSPACE_PKG_SIGNATURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The only certificate pkg trusts for signed repositories (decision 71). */
#define PKG_SIGNING_ANCHOR "boot://share/pyxis/trust/software-signing-ca.pem"

/* Checks a signed repository index (docs/wip/pkg-upgrade.md, decision 71):
 * SIGNER chains to ANCHOR alone, has CA:FALSE, Digital Signature and Code
 * Signing, both certificates are valid at SIGNED_AT (the index's own time, not
 * this system's clock), and SIGNATURE is SIGNER's DER ECDSA P-256 signature over
 * SHA-256 of INDEX. Both PEM buffers end in a NUL counted in their sizes. PSA
 * crypto must be initialized. False sets *REASON to the step that failed. */
bool pkg_signature_verify(const char *anchor, size_t anchor_size,
    const char *signer, size_t signer_size, const uint8_t *index, size_t index_size,
    const uint8_t *signature, size_t signature_size, const char *signed_at,
    const char **reason);

/* TEXT is the lower-case hex SHA-256 of BYTES. PSA crypto must be initialized. */
bool pkg_sha256_text(const void *bytes, size_t size, char text[65]);

#endif
