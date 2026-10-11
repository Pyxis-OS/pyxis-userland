#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum length_modifier { LENGTH_INT, LENGTH_HH, LENGTH_H, LENGTH_L, LENGTH_LL,
                       LENGTH_J, LENGTH_Z, LENGTH_T, LENGTH_BIG_L };

struct conversion {
  bool left, plus, space, alternate, zero;
  int width, precision;
  enum length_modifier length;
};

struct wide_output {
  wchar_t *buffer;
  size_t capacity, length;
};

static int put_character(struct wide_output *out, wchar_t character)
{
  if (out->length >= out->capacity - 1 || out->length == INT_MAX) {
    return EOVERFLOW;
  }
  out->buffer[out->length++] = character;
  return 0;
}

static int decimal(const wchar_t **format)
{
  int value = 0;
  while (**format >= L'0' && **format <= L'9') {
    int digit = *(*format)++ - L'0';
    if (value > (INT_MAX - digit) / 10) {
      return -1;
    }
    value = value * 10 + digit;
  }
  return value;
}

static int parse_conversion(const wchar_t **format, va_list *args, struct conversion *spec)
{
  for (;;) {
    switch (**format) {
      case L'-': spec->left = true; break;
      case L'+': spec->plus = true; break;
      case L' ': spec->space = true; break;
      case L'#': spec->alternate = true; break;
      case L'0': spec->zero = true; break;
      default: goto width;
    }
    ++*format;
  }

width:
  if (**format == L'*') {
    ++*format;
    spec->width = va_arg(*args, int);
    if (spec->width == INT_MIN) {
      return EOVERFLOW;
    }
    if (spec->width < 0) {
      spec->left = true;
      spec->width = -spec->width;
    }
  } else if ((spec->width = decimal(format)) < 0) {
    return EOVERFLOW;
  }
  if (**format == L'.') {
    ++*format;
    if (**format == L'*') {
      ++*format;
      spec->precision = va_arg(*args, int);
      if (spec->precision < 0) {
        spec->precision = -1;
      }
    } else if ((spec->precision = decimal(format)) < 0) {
      return EOVERFLOW;
    }
  }
  switch (**format) {
    case L'h':
      ++*format;
      spec->length = LENGTH_H;
      if (**format == L'h') {
        ++*format;
        spec->length = LENGTH_HH;
      }
      break;
    case L'l':
      ++*format;
      spec->length = LENGTH_L;
      if (**format == L'l') {
        ++*format;
        spec->length = LENGTH_LL;
      }
      break;
    case L'L': ++*format; spec->length = LENGTH_BIG_L; break;
    case L'j': ++*format; spec->length = LENGTH_J; break;
    case L'z': ++*format; spec->length = LENGTH_Z; break;
    case L't': ++*format; spec->length = LENGTH_T; break;
  }
  return 0;
}

static char *write_decimal(char *cursor, unsigned value)
{
  char digits[10];
  size_t count = 0;
  do {
    digits[count++] = '0' + value % 10;
    value /= 10;
  } while (value);
  while (count) {
    *cursor++ = digits[--count];
  }
  return cursor;
}

static void numeric_format(char *buffer, const struct conversion *spec, char kind)
{
  char *cursor = buffer;
  *cursor++ = '%';
  if (spec->left) {
    *cursor++ = '-';
  }
  if (spec->plus) {
    *cursor++ = '+';
  }
  if (spec->space) {
    *cursor++ = ' ';
  }
  if (spec->alternate) {
    *cursor++ = '#';
  }
  if (spec->zero) {
    *cursor++ = '0';
  }
  if (spec->width) {
    cursor = write_decimal(cursor, spec->width);
  }
  if (spec->precision >= 0) {
    *cursor++ = '.';
    cursor = write_decimal(cursor, spec->precision);
  }
  switch (spec->length) {
    case LENGTH_HH: *cursor++ = 'h'; [[fallthrough]];
    case LENGTH_H: *cursor++ = 'h'; break;
    case LENGTH_LL: *cursor++ = 'l'; [[fallthrough]];
    case LENGTH_L: *cursor++ = 'l'; break;
    case LENGTH_J: *cursor++ = 'j'; break;
    case LENGTH_Z: *cursor++ = 'z'; break;
    case LENGTH_T: *cursor++ = 't'; break;
    case LENGTH_BIG_L: *cursor++ = 'L'; break;
    default: break;
  }
  *cursor++ = kind;
  *cursor = '\0';
}

static int consume_numeric(va_list *args, enum length_modifier length, wchar_t kind)
{
  if (kind == L'f' || kind == L'F' || kind == L'e' || kind == L'E' ||
      kind == L'g' || kind == L'G' || kind == L'a' || kind == L'A') {
    if (length == LENGTH_BIG_L) {
      (void)va_arg(*args, long double);
    } else if (length == LENGTH_INT || length == LENGTH_L) {
      (void)va_arg(*args, double);
    } else {
      return EINVAL;
    }
    return 0;
  }
  if (kind == L'p') {
    if (length != LENGTH_INT) {
      return EINVAL;
    }
    (void)va_arg(*args, void *);
    return 0;
  }
  bool signed_value = kind == L'd' || kind == L'i';
  if (!signed_value && kind != L'u' && kind != L'o' && kind != L'x' && kind != L'X') {
    return EINVAL;
  }
  switch (length) {
    case LENGTH_HH:
    case LENGTH_H: (void)va_arg(*args, int); break;
    case LENGTH_INT:
      if (signed_value) {
        (void)va_arg(*args, int);
      } else {
        (void)va_arg(*args, unsigned int);
      }
      break;
    case LENGTH_L:
      if (signed_value) {
        (void)va_arg(*args, long);
      } else {
        (void)va_arg(*args, unsigned long);
      }
      break;
    case LENGTH_LL:
      if (signed_value) {
        (void)va_arg(*args, long long);
      } else {
        (void)va_arg(*args, unsigned long long);
      }
      break;
    case LENGTH_J:
      if (signed_value) {
        (void)va_arg(*args, intmax_t);
      } else {
        (void)va_arg(*args, uintmax_t);
      }
      break;
    case LENGTH_Z:
    case LENGTH_T:
      if (signed_value) {
        (void)va_arg(*args, ptrdiff_t);
      } else {
        (void)va_arg(*args, size_t);
      }
      break;
    default: return EINVAL;
  }
  return 0;
}

static int numeric_conversion(struct wide_output *out, const struct conversion *spec,
  wchar_t kind, va_list *args)
{
  char format[32];
  numeric_format(format, spec, (char)kind);
  va_list copy;
  va_copy(copy, *args);
  int error = consume_numeric(args, spec->length, kind);
  if (error) {
    va_end(copy);
    return error;
  }
  int count = vsnprintf(NULL, 0, format, copy);
  if (count < 0) {
    va_end(copy);
    return errno;
  }
  if ((size_t)count >= out->capacity - out->length ||
      (size_t)count > (size_t)INT_MAX - out->length) {
    va_end(copy);
    return EOVERFLOW;
  }
  /* Numeric output is ASCII. Character access may use the destination's
   * object representation; expand backwards so unread bytes survive. The
   * fitting wide-character count also bounds the temporary byte result. */
  char *buffer = (char *)(out->buffer + out->length);
  int written = vsnprintf(buffer, (size_t)count + 1, format, copy);
  va_end(copy);
  if (written != count) {
    error = written < 0 ? errno : EOVERFLOW;
  } else {
    for (size_t index = count; index; --index) {
      out->buffer[out->length + index - 1] = (unsigned char)buffer[index - 1];
    }
    out->length += count;
  }
  return error;
}

static int string_conversion(struct wide_output *out, const struct conversion *spec,
  wchar_t kind, va_list *args)
{
  if (spec->length != LENGTH_INT && spec->length != LENGTH_L) {
    return EINVAL;
  }
  wchar_t character;
  const wchar_t *wide = NULL;
  const char *narrow = NULL;
  size_t count = 0;
  if (kind == L'c') {
    if (spec->length == LENGTH_L) {
      character = (wchar_t)va_arg(*args, wint_t);
    } else {
      wint_t converted = btowc(va_arg(*args, int));
      if (converted == WEOF) {
        return EILSEQ;
      }
      character = (wchar_t)converted;
    }
    wide = &character;
    count = 1;
  } else {
    if (spec->length == LENGTH_L) {
      wide = va_arg(*args, const wchar_t *);
    } else {
      narrow = va_arg(*args, const char *);
    }
    const char *cursor = narrow;
    while (spec->precision < 0 || count < (size_t)spec->precision) {
      if (wide ? !wide[count] : !*cursor) {
        break;
      }
      if (count >= out->capacity - out->length - 1 ||
          count >= (size_t)INT_MAX - out->length) {
        return EOVERFLOW;
      }
      if (narrow) {
        int bytes = mbtowc(NULL, cursor, strnlen(cursor, MB_CUR_MAX));
        if (bytes < 0) {
          return EILSEQ;
        }
        cursor += bytes;
      }
      ++count;
    }
  }
  size_t total = (size_t)spec->width > count ? (size_t)spec->width : count;
  if (total >= out->capacity - out->length ||
      total > (size_t)INT_MAX - out->length) {
    return EOVERFLOW;
  }
  size_t padding = total - count;
  if (!spec->left) {
    for (size_t index = 0; index < padding; ++index) {
      out->buffer[out->length++] = L' ';
    }
  }
  for (size_t index = 0; index < count; ++index) {
    wchar_t value;
    if (wide) {
      value = wide[index];
    } else {
      int bytes = mbtowc(&value, narrow, strnlen(narrow, MB_CUR_MAX));
      if (bytes < 0) {
        return EILSEQ;
      }
      narrow += bytes;
    }
    out->buffer[out->length++] = value;
  }
  if (spec->left) {
    for (size_t index = 0; index < padding; ++index) {
      out->buffer[out->length++] = L' ';
    }
  }
  return 0;
}

int vswprintf(wchar_t *restrict buffer, size_t capacity,
  const wchar_t *restrict format, va_list args)
{
  if (!capacity) {
    errno = EOVERFLOW;
    return -1;
  }
  struct wide_output out = {buffer, capacity, 0};
  const wchar_t *cursor = format;
  va_list copy;
  va_copy(copy, args);
  int error = 0;
  while (*cursor && !error) {
    if (*cursor != L'%') {
      error = put_character(&out, *cursor++);
      continue;
    }
    ++cursor;
    if (*cursor == L'%') {
      error = put_character(&out, *cursor++);
      continue;
    }
    struct conversion spec = {.precision = -1};
    error = parse_conversion(&cursor, &copy, &spec);
    if (!error) {
      if (*cursor == L's' || *cursor == L'c') {
        error = string_conversion(&out, &spec, *cursor, &copy);
      } else {
        error = numeric_conversion(&out, &spec, *cursor, &copy);
      }
      if (*cursor) {
        ++cursor;
      }
    }
  }
  va_end(copy);
  buffer[out.length] = L'\0';
  if (error) {
    errno = error;
    return -1;
  }
  return (int)out.length;
}

int swprintf(wchar_t *restrict buffer, size_t capacity,
  const wchar_t *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vswprintf(buffer, capacity, format, args);
  va_end(args);
  return result;
}
