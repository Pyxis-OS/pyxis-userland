#include "hash.h"
#include <sha256.h>
#include <string.h>

static struct sha256_context context;
static bool active;

bool digest_begin(void)
{
  sha256_init(&context);
  active = true;
  return true;
}

bool digest_update(const void *bytes, size_t size)
{
  if (!active || !sha256_update(&context, bytes, size)) {
    digest_abort();
    return false;
  }
  return true;
}

bool digest_finish(char digest[65])
{
  unsigned char hash[SHA256_DIGEST_BYTES];
  bool ok = active && sha256_finish(&context, hash);
  digest_abort();
  if (!ok) {
    return false;
  }
  static const char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(hash); index++) {
    digest[index * 2] = hex[hash[index] >> 4];
    digest[index * 2 + 1] = hex[hash[index] & 15];
  }
  digest[64] = 0;
  return true;
}

void digest_abort(void)
{
  context = (struct sha256_context){0};
  active = false;
}

static unsigned hex_lower(unsigned byte)
{
  return byte >= 'A' && byte <= 'F' ? byte + 'a' - 'A' : byte;
}

bool digest_valid(const char *digest)
{
  if (!digest || strlen(digest) != 64) {
    return false;
  }
  for (; *digest; digest++) {
    unsigned byte = hex_lower((unsigned char)*digest);
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool digest_equal(const char *expected, const char *actual)
{
  if (!digest_valid(expected) || !digest_valid(actual)) {
    return false;
  }
  for (size_t index = 0; index < 64; index++) {
    if (hex_lower((unsigned char)expected[index]) != hex_lower((unsigned char)actual[index])) {
      return false;
    }
  }
  return true;
}
