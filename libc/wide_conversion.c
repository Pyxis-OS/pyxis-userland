#include <errno.h>
#include <stdint.h>
#include <wchar.h>
#include "thread_private.h"

int mbsinit(const mbstate_t *state)
{
  return !state || !state->remaining;
}

static size_t invalid_sequence(mbstate_t *state)
{
  *state = (mbstate_t){0};
  errno = EILSEQ;
  return (size_t)-1;
}

size_t mbrtowc(wchar_t *restrict output, const char *restrict text,
  size_t size, mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_MBR_TOWC];
  if (!state) {
    state = implicit_state;
  }
  if (!text) {
    text = "";
    size = 1;
    output = NULL;
  }
  if (!size) {
    return (size_t)-2;
  }

  const unsigned char *bytes = (const unsigned char *)text;
  size_t consumed = 0;
  if (!state->remaining) {
    unsigned first = bytes[consumed++];
    if (first < 0x80) {
      *state = (mbstate_t){0};
      if (output) {
        *output = (wchar_t)first;
      }
      return first ? 1 : 0;
    }
    if (first >= 0xc2 && first <= 0xdf) {
      *state = (mbstate_t){.value = first & 0x1f, .remaining = 1, .length = 2};
    } else if (first >= 0xe0 && first <= 0xef) {
      *state = (mbstate_t){.value = first & 0x0f, .remaining = 2, .length = 3};
    } else if (first >= 0xf0 && first <= 0xf4) {
      *state = (mbstate_t){.value = first & 0x07, .remaining = 3, .length = 4};
    } else {
      return invalid_sequence(state);
    }
  }

  while (state->remaining && consumed < size) {
    unsigned byte = bytes[consumed++];
    if (byte < 0x80 || byte > 0xbf) {
      return invalid_sequence(state);
    }
    /* Reject invalid prefixes before an incomplete sequence is retained. */
    if (state->remaining == state->length - 1 &&
        ((state->length == 3 &&
          ((state->value == 0 && byte < 0xa0) ||
           (state->value == 0x0d && byte > 0x9f))) ||
         (state->length == 4 &&
          ((state->value == 0 && byte < 0x90) ||
           (state->value == 4 && byte > 0x8f))))) {
      return invalid_sequence(state);
    }
    state->value = (state->value << 6) | (byte & 0x3f);
    --state->remaining;
  }
  if (state->remaining) {
    return (size_t)-2;
  }
  if (output) {
    *output = (wchar_t)state->value;
  }
  *state = (mbstate_t){0};
  return consumed;
}

size_t mbrlen(const char *restrict text, size_t size,
  mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_MBR_LEN];
  return mbrtowc(NULL, text, size, state ? state : implicit_state);
}

size_t wcrtomb(char *restrict output, wchar_t value,
  mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_WCR_TOMB];
  char reset_output[4];
  if (!state) {
    state = implicit_state;
  }
  if (!output) {
    output = reset_output;
    value = 0;
  }
  unsigned scalar = (unsigned)value;
  if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
    return invalid_sequence(state);
  }

  size_t count;
  if (scalar < 0x80) {
    output[0] = (char)scalar;
    count = 1;
  } else if (scalar < 0x800) {
    output[0] = (char)(0xc0 | (scalar >> 6));
    output[1] = (char)(0x80 | (scalar & 0x3f));
    count = 2;
  } else if (scalar < 0x10000) {
    output[0] = (char)(0xe0 | (scalar >> 12));
    output[1] = (char)(0x80 | ((scalar >> 6) & 0x3f));
    output[2] = (char)(0x80 | (scalar & 0x3f));
    count = 3;
  } else {
    output[0] = (char)(0xf0 | (scalar >> 18));
    output[1] = (char)(0x80 | ((scalar >> 12) & 0x3f));
    output[2] = (char)(0x80 | ((scalar >> 6) & 0x3f));
    output[3] = (char)(0x80 | (scalar & 0x3f));
    count = 4;
  }
  *state = (mbstate_t){0};
  return count;
}

wint_t btowc(int byte)
{
  return byte >= 0 && byte < 0x80 ? (wint_t)byte : WEOF;
}

int wctob(wint_t value)
{
  return value < 0x80 ? (int)value : -1;
}

static size_t decode_string(wchar_t *output, const char **source,
  size_t bytes, size_t capacity, mbstate_t *state)
{
  mbstate_t counting_state;
  if (!output) {
    counting_state = *state;
    state = &counting_state;
  }

  const char *text = *source;
  size_t converted = 0;
  while (bytes && (!output || converted < capacity)) {
    wchar_t value;
    size_t count = mbrtowc(&value, text, bytes, state);
    if (count == (size_t)-1) {
      if (output) {
        *source = text;
      }
      return count;
    }
    if (count == (size_t)-2) {
      text += bytes;
      break;
    }
    if (output) {
      output[converted] = value;
    }
    if (!count) {
      if (output) {
        *source = NULL;
      }
      return converted;
    }
    text += count;
    bytes -= count;
    ++converted;
  }
  if (output) {
    *source = text;
  }
  return converted;
}

size_t mbsrtowcs(wchar_t *restrict output, const char **restrict source,
  size_t capacity, mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_MBS_RTOWCS];
  return decode_string(output, source, SIZE_MAX, capacity,
    state ? state : implicit_state);
}

size_t mbsnrtowcs(wchar_t *restrict output, const char **restrict source,
  size_t bytes, size_t capacity, mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_MBS_NRTOWCS];
  return decode_string(output, source, bytes, capacity,
    state ? state : implicit_state);
}

static size_t encode_string(char *output, const wchar_t **source,
  size_t count, size_t capacity, mbstate_t *state)
{
  mbstate_t counting_state;
  if (!output) {
    counting_state = *state;
    state = &counting_state;
  }

  const wchar_t *text = *source;
  size_t converted = 0;
  while (count && (!output || converted < capacity)) {
    char encoded[4];
    mbstate_t next_state = *state;
    size_t bytes = wcrtomb(encoded, *text, &next_state);
    if (bytes == (size_t)-1) {
      *state = next_state;
      if (output) {
        *source = text;
      }
      return bytes;
    }
    if (output && bytes > capacity - converted) {
      break;
    }
    if (output) {
      for (size_t i = 0; i < bytes; ++i) {
        output[converted + i] = encoded[i];
      }
    }
    *state = next_state;
    if (!*text) {
      if (output) {
        *source = NULL;
      }
      return converted;
    }
    converted += bytes;
    ++text;
    --count;
  }
  if (output) {
    *source = text;
  }
  return converted;
}

size_t wcsrtombs(char *restrict output, const wchar_t **restrict source,
  size_t capacity, mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_WCS_RTOMBS];
  return encode_string(output, source, SIZE_MAX, capacity,
    state ? state : implicit_state);
}

size_t wcsnrtombs(char *restrict output, const wchar_t **restrict source,
  size_t count, size_t capacity, mbstate_t *restrict state)
{
  mbstate_t *implicit_state = &libc_thread_record()->conversion_states[LIBC_WCS_NRTOMBS];
  return encode_string(output, source, count, capacity,
    state ? state : implicit_state);
}
