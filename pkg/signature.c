#include "signature.h"
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#define SHA256_BYTES 32

static int digits(const char *text, size_t count)
{
  int value = 0;
  for (size_t i = 0; i < count; ++i) {
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

/* SIGNED_AT is already checked as YYYY-MM-DDTHH:MM:SSZ. */
/* Only expiry may fail: it is judged at the signing time instead. Any other
 * error, with or without flags, refuses. */
static bool chained(mbedtls_x509_crt *leaf, mbedtls_x509_crt *trusted)
{
  uint32_t flags = 0;
  int status = mbedtls_x509_crt_verify(leaf, trusted, NULL, NULL, &flags, NULL, NULL);
  uint32_t dated = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
  return status == 0 ||
      (status == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags && !(flags & ~dated));
}

/* ECDSA on secp256r1 alone, not another 256-bit curve. */
static bool p256(const mbedtls_pk_context *key)
{
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  bool ok = mbedtls_pk_get_psa_attributes(key, PSA_KEY_USAGE_VERIFY_HASH, &attributes) == 0 &&
      psa_get_key_type(&attributes) == PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1) &&
      psa_get_key_bits(&attributes) == 256 &&
      mbedtls_pk_can_do_psa(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), PSA_KEY_USAGE_VERIFY_HASH);
  psa_reset_key_attributes(&attributes);
  return ok;
}

static bool within(const mbedtls_x509_crt *certificate, const mbedtls_x509_time *at)
{
  return mbedtls_x509_time_cmp(&certificate->valid_from, at) <= 0 &&
      mbedtls_x509_time_cmp(at, &certificate->valid_to) <= 0;
}

bool pkg_signature_verify(const char *anchor, size_t anchor_size,
    const char *signer, size_t signer_size, const uint8_t *index, size_t index_size,
    const uint8_t *signature, size_t signature_size, const char *signed_at,
    const char **reason)
{
  mbedtls_x509_crt trusted, leaf;
  mbedtls_x509_crt_init(&trusted);
  mbedtls_x509_crt_init(&leaf);
  mbedtls_x509_time at = {
    .year = digits(signed_at, 4), .mon = digits(signed_at + 5, 2),
    .day = digits(signed_at + 8, 2), .hour = digits(signed_at + 11, 2),
    .min = digits(signed_at + 14, 2), .sec = digits(signed_at + 17, 2),
  };
  bool ok = false;
  uint8_t hash[SHA256_BYTES];
  size_t hash_size = 0;
  if (mbedtls_x509_crt_parse(&trusted, (const unsigned char *)anchor, anchor_size) != 0 ||
      trusted.next) {
    *reason = "the pinned signing CA (" PKG_SIGNING_ANCHOR ") is unreadable";
  } else if (mbedtls_x509_crt_parse(&leaf, (const unsigned char *)signer, signer_size) != 0 ||
      leaf.next) {
    *reason = "the signer certificate is not one PEM certificate";
  } else if (!chained(&leaf, &trusted)) {
    *reason = "the signer is not issued by the Pyxis Software Signing CA";
  } else if (mbedtls_x509_crt_get_ca_istrue(&leaf) != 0) {
    *reason = "the signer certificate is not CA:FALSE";
  } else if (!mbedtls_x509_crt_has_ext_type(&leaf, MBEDTLS_X509_EXT_KEY_USAGE) ||
      mbedtls_x509_crt_check_key_usage(&leaf, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
      !mbedtls_x509_crt_has_ext_type(&leaf, MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE) ||
      mbedtls_x509_crt_check_extended_key_usage(&leaf, MBEDTLS_OID_CODE_SIGNING,
          MBEDTLS_OID_SIZE(MBEDTLS_OID_CODE_SIGNING)) != 0) {
    *reason = "the signer certificate lacks Digital Signature or Code Signing";
  } else if (!within(&leaf, &at) || !within(&trusted, &at)) {
    *reason = "the signer or the CA is not valid at the index's signed time";
  } else if (!p256(&leaf.pk)) {
    *reason = "the signer key is not ECDSA P-256";
  } else if (psa_hash_compute(PSA_ALG_SHA_256, index, index_size, hash, sizeof(hash),
      &hash_size) != PSA_SUCCESS || hash_size != sizeof(hash)) {
    *reason = "cannot hash the index";
  } else if (mbedtls_pk_verify(&leaf.pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
      signature, signature_size) != 0) {
    *reason = "the index signature does not verify";
  } else {
    ok = true;
  }
  mbedtls_x509_crt_free(&leaf);
  mbedtls_x509_crt_free(&trusted);
  return ok;
}

bool pkg_sha256_text(const void *bytes, size_t size, char text[65])
{
  uint8_t hash[SHA256_BYTES];
  size_t hash_size = 0;
  if (psa_hash_compute(PSA_ALG_SHA_256, bytes, size, hash, sizeof(hash), &hash_size) !=
      PSA_SUCCESS || hash_size != sizeof(hash)) {
    return false;
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(hash); ++i) {
    text[2 * i] = digits[hash[i] >> 4];
    text[2 * i + 1] = digits[hash[i] & 15];
  }
  text[64] = 0;
  return true;
}
