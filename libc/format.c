#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "format.h"

enum length_modifier { LENGTH_INT, LENGTH_HH, LENGTH_H, LENGTH_L, LENGTH_LL,
                       LENGTH_J, LENGTH_Z, LENGTH_T, LENGTH_BIG_L };

struct conversion {
  bool left, plus, space, alternate, zero;
  int width, precision;
  enum length_modifier length;
};

/* Count discarded bytes too, but saturate once the return value cannot fit.
 * Padding takes time proportional to the destination, not a huge field width. */
void format_append(struct output *out, const char *text, size_t count)
{
  size_t available = out->capacity && out->length < out->capacity - 1 ?
                     out->capacity - 1 - out->length : 0;
  size_t copied = count < available ? count : available;
  if (copied) {
    memcpy(out->buffer + out->length, text, copied);
  }
  out->length = out->length > INT_MAX || count > (size_t)INT_MAX - out->length ?
                (size_t)INT_MAX + 1 : out->length + count;
}

void format_pad(struct output *out, char character, size_t count)
{
  size_t available = out->capacity && out->length < out->capacity - 1 ?
                     out->capacity - 1 - out->length : 0;
  size_t copied = count < available ? count : available;
  if (copied) {
    memset(out->buffer + out->length, character, copied);
  }
  out->length = out->length > INT_MAX || count > (size_t)INT_MAX - out->length ?
                (size_t)INT_MAX + 1 : out->length + count;
}

static int decimal(const char **format)
{
  int value = 0;
  while (**format >= '0' && **format <= '9') {
    int digit = *(*format)++ - '0';
    if (value > (INT_MAX - digit) / 10) {
      return -1;
    }
    value = value * 10 + digit;
  }
  return value;
}

static int parse_conversion(const char **format, va_list *args, struct conversion *spec)
{
  for (;;) {
    switch (**format) {
      case '-': spec->left = true; break;
      case '+': spec->plus = true; break;
      case ' ': spec->space = true; break;
      case '#': spec->alternate = true; break;
      case '0': spec->zero = true; break;
      default: goto width;
    }
    ++*format;
  }

width:
  if (**format == '*') {
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

  if (**format == '.') {
    ++*format;
    if (**format == '*') {
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
    case 'h':
      ++*format;
      spec->length = LENGTH_H;
      if (**format == 'h') {
        ++*format;
        spec->length = LENGTH_HH;
      }
      break;
    case 'l':
      ++*format;
      spec->length = LENGTH_L;
      if (**format == 'l') {
        ++*format;
        spec->length = LENGTH_LL;
      }
      break;
    case 'L': ++*format; spec->length = LENGTH_BIG_L; break;
    case 'j': ++*format; spec->length = LENGTH_J; break;
    case 'z': ++*format; spec->length = LENGTH_Z; break;
    case 't': ++*format; spec->length = LENGTH_T; break;
  }
  return 0;
}

static uintmax_t unsigned_argument(va_list *args, enum length_modifier length)
{
  switch (length) {
    case LENGTH_HH: return (unsigned char)va_arg(*args, unsigned int);
    case LENGTH_H: return (unsigned short)va_arg(*args, unsigned int);
    case LENGTH_L: return va_arg(*args, unsigned long);
    case LENGTH_LL: return va_arg(*args, unsigned long long);
    case LENGTH_J: return va_arg(*args, uintmax_t);
    case LENGTH_Z: return va_arg(*args, size_t);
    /* On this target size_t is the unsigned counterpart of ptrdiff_t. */
    case LENGTH_T: return va_arg(*args, size_t);
    default: return va_arg(*args, unsigned int);
  }
}

static intmax_t signed_argument(va_list *args, enum length_modifier length)
{
  switch (length) {
    case LENGTH_HH: return (signed char)va_arg(*args, int);
    case LENGTH_H: return (short)va_arg(*args, int);
    case LENGTH_L: return va_arg(*args, long);
    case LENGTH_LL: return va_arg(*args, long long);
    case LENGTH_J: return va_arg(*args, intmax_t);
    /* On this target ptrdiff_t is the signed counterpart of size_t. */
    case LENGTH_Z: return va_arg(*args, ptrdiff_t);
    case LENGTH_T: return va_arg(*args, ptrdiff_t);
    default: return va_arg(*args, int);
  }
}

static void number(struct output *out, struct conversion spec, uintmax_t value,
                     bool negative, bool is_signed, char kind)
{
  unsigned base = kind == 'o' ? 8 : (kind == 'x' || kind == 'X' || kind == 'p') ? 16 : 10;
  const char *alphabet = kind == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
  char digits[sizeof(uintmax_t) * CHAR_BIT];
  size_t start = sizeof(digits);
  uintmax_t original = value;
  if (value || spec.precision != 0 || kind == 'p') {
    do {
      digits[--start] = alphabet[value % base];
      value /= base;
    } while (value);
  }
  size_t count = sizeof(digits) - start;
  size_t zeroes = spec.precision > 0 && (size_t)spec.precision > count ?
                  (size_t)spec.precision - count : 0;
  char prefix[3];
  size_t prefix_length = 0;
  if (is_signed) {
    if (negative) {
      prefix[prefix_length++] = '-';
    } else if (spec.plus || spec.space) {
      prefix[prefix_length++] = spec.plus ? '+' : ' ';
    }
  }
  if (kind == 'p' || (spec.alternate && base == 16 && original)) {
    prefix[prefix_length++] = '0';
    prefix[prefix_length++] = kind == 'X' ? 'X' : 'x';
  }
  if (spec.alternate && base == 8 && !zeroes && (!count || digits[start] != '0')) {
    zeroes = 1;
  }
  size_t total = prefix_length + zeroes + count;
  size_t padding = (size_t)spec.width > total ? (size_t)spec.width - total : 0;
  if (spec.zero && !spec.left && spec.precision < 0) {
    zeroes += padding;
    padding = 0;
  }
  if (!spec.left) {
    format_pad(out, ' ', padding);
  }
  format_append(out, prefix, prefix_length);
  format_pad(out, '0', zeroes);
  format_append(out, digits + start, count);
  if (spec.left) {
    format_pad(out, ' ', padding);
  }
}

static int format_conversion(struct output *out, const struct conversion *spec,
                                char kind, va_list *args)
{
  if (kind == 'f' || kind == 'F' || kind == 'e' || kind == 'E' ||
      kind == 'g' || kind == 'G' || kind == 'a' || kind == 'A') {
    if (spec->length != LENGTH_INT && spec->length != LENGTH_L &&
        spec->length != LENGTH_BIG_L) {
      return EINVAL;
    }
    long double value = spec->length == LENGTH_BIG_L ?
                        va_arg(*args, long double) : va_arg(*args, double);
    unsigned flags = (spec->left ? FLOAT_LEFT : 0) |
                     (spec->plus ? FLOAT_PLUS : 0) |
                     (spec->space ? FLOAT_SPACE : 0) |
                     (spec->alternate ? FLOAT_ALTERNATE : 0) |
                     (spec->zero ? FLOAT_ZERO : 0);
    return format_float(out, value, spec->width, spec->precision, flags, kind) < 0 ?
           EOVERFLOW : 0;
  }
  if (spec->length == LENGTH_BIG_L) {
    return EINVAL;
  }
  if (kind == 'd' || kind == 'i') {
    intmax_t value = signed_argument(args, spec->length);
    /* Unsigned subtraction handles INTMAX_MIN without signed overflow. */
    uintmax_t magnitude = value < 0 ? 0 - (uintmax_t)value : (uintmax_t)value;
    number(out, *spec, magnitude, value < 0, true, kind);
  } else if (kind == 'u' || kind == 'o' || kind == 'x' || kind == 'X') {
    number(out, *spec, unsigned_argument(args, spec->length), false, false, kind);
  } else if (spec->length != LENGTH_INT) {
    return EINVAL;
  } else if (kind == 'p') {
    number(out, *spec, (uintptr_t)va_arg(*args, void *), false, false, kind);
  } else if (kind == 's' || kind == 'c' || kind == '%') {
    char character;
    const char *text;
    size_t length;
    if (kind == 's') {
      text = va_arg(*args, const char *);
      length = spec->precision < 0 ? strlen(text) : strnlen(text, spec->precision);
    } else {
      character = kind == '%' ? '%' : (char)va_arg(*args, int);
      text = &character;
      length = 1;
    }
    size_t padding = (size_t)spec->width > length ? (size_t)spec->width - length : 0;
    if (!spec->left) {
      format_pad(out, ' ', padding);
    }
    format_append(out, text, length);
    if (spec->left) {
      format_pad(out, ' ', padding);
    }
  } else {
    return EINVAL;
  }
  return 0;
}

int vsnprintf(char *restrict buffer, size_t capacity, const char *format, va_list args)
{
  struct output out = {buffer, capacity, 0};
  va_list copy;
  va_copy(copy, args);
  int error = 0;
  while (*format && !error && out.length <= INT_MAX) {
    if (*format != '%') {
      const char *start = format;
      while (*format && *format != '%') {
        ++format;
      }
      format_append(&out, start, format - start);
      continue;
    }
    ++format;
    struct conversion spec = {.precision = -1};
    error = parse_conversion(&format, &copy, &spec);
    if (!error) {
      error = format_conversion(&out, &spec, *format, &copy);
      if (*format) {
        ++format;
      }
    }
  }
  va_end(copy);
  if (capacity) {
    buffer[out.length < capacity ? out.length : capacity - 1] = '\0';
  }
  if (out.length > INT_MAX) {
    error = EOVERFLOW;
  }
  if (error) {
    errno = error;
    return -1;
  }
  return (int)out.length;
}

int snprintf(char *restrict buffer, size_t capacity, const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vsnprintf(buffer, capacity, format, args);
  va_end(args);
  return result;
}
