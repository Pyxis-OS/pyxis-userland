#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

static unsigned digit_value(wchar_t character)
{
  if (character >= L'0' && character <= L'9') {
    return character - L'0';
  }
  if (character >= L'a' && character <= L'z') {
    return character - L'a' + 10;
  }
  if (character >= L'A' && character <= L'Z') {
    return character - L'A' + 10;
  }
  return 36;
}

static const wchar_t *number_start(const wchar_t *text)
{
  while (*text >= 0 && *text <= 127 && isspace((unsigned char)*text)) {
    ++text;
  }
  if (*text == L'+' || *text == L'-') {
    ++text;
  }
  return text;
}

static const wchar_t *integer_end(const wchar_t *text, int base)
{
  const wchar_t *cursor = number_start(text);
  if (*cursor == L'0') {
    if ((base == 0 || base == 16) && (cursor[1] == L'x' || cursor[1] == L'X') &&
        digit_value(cursor[2]) < 16) {
      base = 16;
      cursor += 2;
    } else if ((base == 0 || base == 2) && (cursor[1] == L'b' || cursor[1] == L'B') &&
               digit_value(cursor[2]) < 2) {
      base = 2;
      cursor += 2;
    } else if (!base) {
      base = 8;
    }
  }
  if (!base) {
    base = 10;
  }
  while (digit_value(*cursor) < (unsigned)base) {
    ++cursor;
  }
  return cursor;
}

static wchar_t ascii_lower(wchar_t character)
{
  return character >= L'A' && character <= L'Z' ? character + L'a' - L'A' : character;
}

static const wchar_t *float_end(const wchar_t *text)
{
  const wchar_t *cursor = number_start(text);
  if (ascii_lower(*cursor) == L'i') {
    const char *word = "infinity";
    while (*word && ascii_lower(*cursor) == *word) {
      ++word;
      ++cursor;
    }
    return cursor;
  }
  if (ascii_lower(*cursor) == L'n') {
    const char *word = "nan";
    while (*word && ascii_lower(*cursor) == *word) {
      ++word;
      ++cursor;
    }
    if (!*word && *cursor == L'(') {
      ++cursor;
      while (digit_value(*cursor) < 36 || *cursor == L'_') {
        ++cursor;
      }
      if (*cursor == L')') {
        ++cursor;
      }
    }
    return cursor;
  }

  unsigned base = 10;
  if (*cursor == L'0' && (cursor[1] == L'x' || cursor[1] == L'X')) {
    base = 16;
    cursor += 2;
  }
  while (digit_value(*cursor) < base) {
    ++cursor;
  }
  if (*cursor == L'.') {
    ++cursor;
    while (digit_value(*cursor) < base) {
      ++cursor;
    }
  }
  wchar_t exponent = base == 16 ? L'p' : L'e';
  if (ascii_lower(*cursor) == exponent) {
    ++cursor;
    if (*cursor == L'+' || *cursor == L'-') {
      ++cursor;
    }
    while (digit_value(*cursor) < 10) {
      ++cursor;
    }
  }
  return cursor;
}

/* Only the ASCII subject candidate is copied. The narrow converter decides
 * incomplete-prefix rollback, rounding and range errors; offsets stay 1:1. */
static char *narrow_number(const wchar_t *text, wchar_t **end, bool floating,
  int base, char *local, size_t capacity)
{
  if (end) {
    *end = (wchar_t *)text;
  }
  if (!floating && base && (base < 2 || base > 36)) {
    errno = EINVAL;
    return NULL;
  }
  const wchar_t *limit = floating ? float_end(text) : integer_end(text, base);
  size_t count = limit - text;
  if (count == SIZE_MAX) {
    errno = ENOMEM;
    return NULL;
  }
  char *buffer = count < capacity ? local : malloc(count + 1);
  if (!buffer) {
    return NULL;
  }
  for (size_t index = 0; index < count; ++index) {
    buffer[index] = (char)text[index];
  }
  buffer[count] = '\0';
  return buffer;
}

long wcstol(const wchar_t *restrict text, wchar_t **restrict end, int base)
{
  char local[128];
  char *buffer = narrow_number(text, end, false, base, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  long result = strtol(buffer, &tail, base);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}

unsigned long wcstoul(const wchar_t *restrict text, wchar_t **restrict end, int base)
{
  char local[128];
  char *buffer = narrow_number(text, end, false, base, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  unsigned long result = strtoul(buffer, &tail, base);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}

long long wcstoll(const wchar_t *restrict text, wchar_t **restrict end, int base)
{
  char local[128];
  char *buffer = narrow_number(text, end, false, base, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  long long result = strtoll(buffer, &tail, base);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}

unsigned long long wcstoull(const wchar_t *restrict text, wchar_t **restrict end, int base)
{
  char local[128];
  char *buffer = narrow_number(text, end, false, base, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  unsigned long long result = strtoull(buffer, &tail, base);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}

float wcstof(const wchar_t *restrict text, wchar_t **restrict end)
{
  char local[128];
  char *buffer = narrow_number(text, end, true, 0, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  float result = strtof(buffer, &tail);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}

double wcstod(const wchar_t *restrict text, wchar_t **restrict end)
{
  char local[128];
  char *buffer = narrow_number(text, end, true, 0, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  double result = strtod(buffer, &tail);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}

long double wcstold(const wchar_t *restrict text, wchar_t **restrict end)
{
  char local[128];
  char *buffer = narrow_number(text, end, true, 0, local, sizeof(local));
  if (!buffer) {
    return 0;
  }
  char *tail;
  long double result = strtold(buffer, &tail);
  if (end) {
    *end = (wchar_t *)text + (tail - buffer);
  }
  if (buffer != local) {
    free(buffer);
  }
  return result;
}
