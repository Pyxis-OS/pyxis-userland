#ifndef USERSPACE_PKG_SIGNATURE_H
#define USERSPACE_PKG_SIGNATURE_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The only certificate pkg trusts for signed repositories (decision 71). */
#define PKG_SIGNING_ANCHOR "boot://share/pyxis/trust/software-signing-ca.pem"

/* Borrow the named clock/random grants to initialize signature crypto.
 * Keep initialized through all signature verification; close before exit. */
bool pkg_crypto_init(handle_t clock, handle_t random);
void pkg_crypto_close(void);

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

/* TEXT is the lower-case hex SHA-256 of BYTES; no crypto setup is required. */
bool pkg_sha256_text(const void *bytes, size_t size, char text[65]);

#endif
