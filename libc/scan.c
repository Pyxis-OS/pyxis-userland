#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Numeric fields end at this many characters, as if limited by a width. */
#define SCAN_TOKEN_CAPACITY 512
#define SCAN_SET_SIZE 256

/* String input or a stream read through fgetc with one ungetc of lookahead. */
struct scan_input {
  FILE *stream;
  const char *text;
  size_t consumed; /* Characters taken, reported by %n. */
};

enum scan_length {
  LENGTH_NONE,
  LENGTH_HH,
  LENGTH_H,
  LENGTH_L,
  LENGTH_LL,
  LENGTH_J,
  LENGTH_Z,
  LENGTH_T,
  LENGTH_LONG_DOUBLE,
};

enum scan_outcome {
  SCAN_CONTINUE,
  SCAN_MATCH_FAILURE, /* Input did not match; stop and report assignments. */
  SCAN_INPUT_FAILURE, /* EOF or a read error before the directive completed. */
  SCAN_FORMAT_ERROR,  /* Malformed or unsupported conversion: EINVAL. */
};

static int next_character(struct scan_input *input)
{
  int character;
  if (input->stream) {
    character = fgetc(input->stream);
  } else {
    character = *input->text ? (unsigned char)*input->text++ : EOF;
  }
  if (character != EOF) {
    ++input->consumed;
  }
  return character;
}

/* Only the character just read is returned, so one pushback always suffices. */
static void unread_character(struct scan_input *input, int character)
{
  if (character == EOF) {
    return;
  }
  --input->consumed;
  if (input->stream) {
    ungetc(character, input->stream);
  } else {
    --input->text;
  }
}

/* Returns the first non-space character without consuming it, or EOF. */
static int skip_space(struct scan_input *input)
{
  int character;
  do {
    character = next_character(input);
  } while (character != EOF && isspace(character));
  unread_character(input, character);
  return character;
}

static bool digit_in_base(int character, int base)
{
  if (base == 8) {
    return character >= '0' && character <= '7';
  }
  return base == 16 ? isxdigit(character) : isdigit(character);
}

/* Collect the longest prefix of an integer in base (0 detects 0x and 0).
 * Conversion later rejects a token that is only a prefix, such as "0x". */
static size_t integer_token(struct scan_input *input, size_t width, int base, char *token)
{
  size_t length = 0;
  int character = next_character(input);
  if ((character == '+' || character == '-') && length < width) {
    token[length++] = character;
    character = next_character(input);
  }
  if ((base == 0 || base == 16) && character == '0' && length < width) {
    token[length++] = character;
    character = next_character(input);
    if ((character == 'x' || character == 'X') && length < width) {
      token[length++] = character;
      character = next_character(input);
      base = 16;
    } else if (base == 0) {
      base = 8;
    }
  } else if (base == 0) {
    base = 10;
  }
  while (length < width && digit_in_base(character, base)) {
    token[length++] = character;
    character = next_character(input);
  }
  unread_character(input, character);
  token[length] = '\0';
  return length;
}

/* Append characters while they match word case-insensitively. */
static bool take_word(struct scan_input *input, int *character, const char *word,
    size_t width, char *token, size_t *length)
{
  for (; *word; ++word) {
    if (*length >= width || tolower(*character) != *word) {
      return false;
    }
    token[(*length)++] = *character;
    *character = next_character(input);
  }
  return true;
}

static void take_digits(struct scan_input *input, int *character, int base,
    size_t width, char *token, size_t *length)
{
  while (*length < width && digit_in_base(*character, base)) {
    token[(*length)++] = *character;
    *character = next_character(input);
  }
}

/* Collect the longest prefix of a decimal or hexadecimal float, infinity or
 * NaN. strtod then decides whether the whole token is a number. */
static size_t float_token(struct scan_input *input, size_t width, char *token)
{
  size_t length = 0;
  int character = next_character(input);
  if ((character == '+' || character == '-') && length < width) {
    token[length++] = character;
    character = next_character(input);
  }
  if (tolower(character) == 'i') {
    if (take_word(input, &character, "inf", width, token, &length)) {
      take_word(input, &character, "inity", width, token, &length);
    }
  } else if (tolower(character) == 'n') {
    if (take_word(input, &character, "nan", width, token, &length) &&
        character == '(' && length < width) {
      token[length++] = character;
      character = next_character(input);
      while (length < width && (isalnum(character) || character == '_')) {
        token[length++] = character;
        character = next_character(input);
      }
      if (character == ')' && length < width) {
        token[length++] = character;
        character = next_character(input);
      }
    }
  } else {
    int base = 10;
    if (character == '0' && length < width) {
      token[length++] = character;
      character = next_character(input);
      if ((character == 'x' || character == 'X') && length < width) {
        token[length++] = character;
        character = next_character(input);
        base = 16;
      }
    }
    take_digits(input, &character, base, width, token, &length);
    if (character == '.' && length < width) {
      token[length++] = character;
      character = next_character(input);
      take_digits(input, &character, base, width, token, &length);
    }
    if (tolower(character) == (base == 16 ? 'p' : 'e') && length < width) {
      token[length++] = character;
      character = next_character(input);
      if ((character == '+' || character == '-') && length < width) {
        token[length++] = character;
        character = next_character(input);
      }
      take_digits(input, &character, 10, width, token, &length);
    }
  }
  unread_character(input, character);
  token[length] = '\0';
  return length;
}

static void store_signed(va_list *args, enum scan_length length, long long value)
{
  switch (length) {
  case LENGTH_HH: *va_arg(*args, signed char *) = (signed char)value; break;
  case LENGTH_H: *va_arg(*args, short *) = (short)value; break;
  case LENGTH_L: *va_arg(*args, long *) = (long)value; break;
  case LENGTH_LL: *va_arg(*args, long long *) = value; break;
  case LENGTH_J: *va_arg(*args, intmax_t *) = (intmax_t)value; break;
  case LENGTH_Z: *va_arg(*args, ptrdiff_t *) = (ptrdiff_t)value; break;
  case LENGTH_T: *va_arg(*args, ptrdiff_t *) = (ptrdiff_t)value; break;
  default: *va_arg(*args, int *) = (int)value; break;
  }
}

static void store_unsigned(va_list *args, enum scan_length length, unsigned long long value)
{
  switch (length) {
  case LENGTH_HH: *va_arg(*args, unsigned char *) = (unsigned char)value; break;
  case LENGTH_H: *va_arg(*args, unsigned short *) = (unsigned short)value; break;
  case LENGTH_L: *va_arg(*args, unsigned long *) = (unsigned long)value; break;
  case LENGTH_LL: *va_arg(*args, unsigned long long *) = value; break;
  case LENGTH_J: *va_arg(*args, uintmax_t *) = (uintmax_t)value; break;
  case LENGTH_Z: *va_arg(*args, size_t *) = (size_t)value; break;
  case LENGTH_T: *va_arg(*args, size_t *) = (size_t)value; break;
  default: *va_arg(*args, unsigned *) = (unsigned)value; break;
  }
}

static enum scan_outcome scan_integer(struct scan_input *input, char conversion, size_t width,
    enum scan_length length, bool suppress, va_list *args)
{
  int base = conversion == 'd' || conversion == 'u' ? 10 : conversion == 'o' ? 8 :
      conversion == 'i' ? 0 : 16;
  char token[SCAN_TOKEN_CAPACITY];
  if (skip_space(input) == EOF) {
    return SCAN_INPUT_FAILURE;
  }
  size_t count = integer_token(input, width < sizeof(token) ? width : sizeof(token) - 1,
      base, token);
  char *end;
  if (conversion == 'd' || conversion == 'i') {
    long long value = strtoll(token, &end, base);
    if (!count || end != token + count) {
      return SCAN_MATCH_FAILURE;
    }
    if (!suppress) {
      store_signed(args, length, value);
    }
  } else {
    unsigned long long value = strtoull(token, &end, base);
    if (!count || end != token + count) {
      return SCAN_MATCH_FAILURE;
    }
    if (!suppress && conversion == 'p') {
      *va_arg(*args, void **) = (void *)(uintptr_t)value;
    } else if (!suppress) {
      store_unsigned(args, length, value);
    }
  }
  return SCAN_CONTINUE;
}

static enum scan_outcome scan_float(struct scan_input *input, size_t width,
    enum scan_length length, bool suppress, va_list *args)
{
  char token[SCAN_TOKEN_CAPACITY];
  if (skip_space(input) == EOF) {
    return SCAN_INPUT_FAILURE;
  }
  size_t count = float_token(input, width < sizeof(token) ? width : sizeof(token) - 1, token);
  char *end;
  if (length == LENGTH_LONG_DOUBLE) {
    long double value = strtold(token, &end);
    if (count && end == token + count && !suppress) {
      *va_arg(*args, long double *) = value;
    }
  } else if (length == LENGTH_L) {
    double value = strtod(token, &end);
    if (count && end == token + count && !suppress) {
      *va_arg(*args, double *) = value;
    }
  } else {
    float value = strtof(token, &end);
    if (count && end == token + count && !suppress) {
      *va_arg(*args, float *) = value;
    }
  }
  return count && end == token + count ? SCAN_CONTINUE : SCAN_MATCH_FAILURE;
}

/* %s, %c and %[ share this loop; set selects accepted characters. */
static enum scan_outcome scan_characters(struct scan_input *input, size_t width,
    const bool set[SCAN_SET_SIZE], bool terminate, bool exact, bool suppress, va_list *args)
{
  char *output = suppress ? NULL : va_arg(*args, char *);
  size_t count = 0;
  int character = EOF;
  while (count < width) {
    character = next_character(input);
    if (character == EOF || !set[character]) {
      break;
    }
    if (output) {
      output[count] = (char)character;
    }
    ++count;
  }
  if (count < width) {
    unread_character(input, character);
  }
  if (!count || (exact && count < width)) {
    return character == EOF ? SCAN_INPUT_FAILURE : SCAN_MATCH_FAILURE;
  }
  if (output && terminate) {
    output[count] = '\0';
  }
  return SCAN_CONTINUE;
}

/* Parse "[...]" after the '[', leaving *format after the closing bracket.
 * A leading ']' (after any '^') is a member; a-z denotes a range. */
static bool parse_set(const char **format, bool set[SCAN_SET_SIZE])
{
  const unsigned char *cursor = (const unsigned char *)*format;
  bool invert = *cursor == '^';
  if (invert) {
    ++cursor;
  }
  bool members[SCAN_SET_SIZE] = {false};
  const unsigned char *first = cursor;
  while (*cursor && (*cursor != ']' || cursor == first)) {
    if (cursor[1] == '-' && cursor[2] && cursor[2] != ']' && cursor[0] <= cursor[2]) {
      for (unsigned value = cursor[0]; value <= cursor[2]; ++value) {
        members[value] = true;
      }
      cursor += 3;
    } else {
      members[*cursor++] = true;
    }
  }
  if (*cursor != ']') {
    return false;
  }
  for (unsigned value = 0; value < SCAN_SET_SIZE; ++value) {
    set[value] = members[value] != invert;
  }
  *format = (const char *)cursor + 1;
  return true;
}

static int scan_format(struct scan_input *input, const char *format, va_list arguments)
{
  va_list args;
  va_copy(args, arguments);
  int assigned = 0;
  bool converted = false;
  enum scan_outcome outcome = SCAN_CONTINUE;

  while (*format && outcome == SCAN_CONTINUE) {
    if (isspace((unsigned char)*format)) {
      while (isspace((unsigned char)*format)) {
        ++format;
      }
      skip_space(input);
      continue;
    }
    if (*format != '%' || format[1] == '%') {
      if (*format == '%') {
        ++format;
        skip_space(input);
      }
      int character = next_character(input);
      if (character != (unsigned char)*format) {
        unread_character(input, character);
        outcome = character == EOF ? SCAN_INPUT_FAILURE : SCAN_MATCH_FAILURE;
        break;
      }
      ++format;
      continue;
    }

    ++format;
    bool suppress = *format == '*';
    if (suppress) {
      ++format;
    }
    size_t width = 0;
    bool has_width = false;
    while (isdigit((unsigned char)*format)) {
      size_t digit = (size_t)(*format++ - '0');
      width = width > (SIZE_MAX - digit) / 10 ? SIZE_MAX : width * 10 + digit;
      has_width = true;
    }
    if (has_width && !width) {
      outcome = SCAN_FORMAT_ERROR;
      break;
    }
    enum scan_length length = LENGTH_NONE;
    switch (*format) {
    case 'h': length = format[1] == 'h' ? LENGTH_HH : LENGTH_H; break;
    case 'l': length = format[1] == 'l' ? LENGTH_LL : LENGTH_L; break;
    case 'j': length = LENGTH_J; break;
    case 'z': length = LENGTH_Z; break;
    case 't': length = LENGTH_T; break;
    case 'L': length = LENGTH_LONG_DOUBLE; break;
    default: break;
    }
    if (length != LENGTH_NONE) {
      format += length == LENGTH_HH || length == LENGTH_LL ? 2 : 1;
    }

    char conversion = *format++;
    bool set[SCAN_SET_SIZE];
    switch (conversion) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
      if (length == LENGTH_LONG_DOUBLE || (conversion == 'p' && length != LENGTH_NONE)) {
        outcome = SCAN_FORMAT_ERROR;
        break;
      }
      outcome = scan_integer(input, conversion == 'X' ? 'x' : conversion,
          has_width ? width : SIZE_MAX, length, suppress, &args);
      break;
    case 'a': case 'A': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
      if (length != LENGTH_NONE && length != LENGTH_L && length != LENGTH_LONG_DOUBLE) {
        outcome = SCAN_FORMAT_ERROR;
        break;
      }
      outcome = scan_float(input, has_width ? width : SIZE_MAX, length, suppress, &args);
      break;
    case 's':
      if (length != LENGTH_NONE) {
        outcome = SCAN_FORMAT_ERROR;
        break;
      }
      if (skip_space(input) == EOF) {
        outcome = SCAN_INPUT_FAILURE;
        break;
      }
      for (unsigned value = 0; value < SCAN_SET_SIZE; ++value) {
        set[value] = !isspace((int)value);
      }
      outcome = scan_characters(input, has_width ? width : SIZE_MAX, set, true, false,
          suppress, &args);
      break;
    case 'c':
      if (length != LENGTH_NONE) {
        outcome = SCAN_FORMAT_ERROR;
        break;
      }
      memset(set, true, sizeof(set));
      outcome = scan_characters(input, has_width ? width : 1, set, false, true,
          suppress, &args);
      break;
    case '[':
      if (length != LENGTH_NONE || !parse_set(&format, set)) {
        outcome = SCAN_FORMAT_ERROR;
        break;
      }
      outcome = scan_characters(input, has_width ? width : SIZE_MAX, set, true, false,
          suppress, &args);
      break;
    case 'n':
      if (!suppress) {
        if (length == LENGTH_NONE) {
          *va_arg(args, int *) = (int)input->consumed;
        } else {
          store_signed(&args, length, (long long)input->consumed);
        }
      }
      continue;
    default:
      outcome = SCAN_FORMAT_ERROR;
      break;
    }
    if (outcome == SCAN_CONTINUE) {
      converted = true;
      if (!suppress) {
        ++assigned;
      }
    }
  }
  va_end(args);

  if (outcome == SCAN_FORMAT_ERROR) {
    errno = EINVAL;
  }
  if ((outcome == SCAN_INPUT_FAILURE || outcome == SCAN_FORMAT_ERROR) && !converted) {
    return EOF;
  }
  return assigned;
}

int vfscanf(FILE *restrict stream, const char *restrict format, va_list args)
{
  if (!stream) {
    errno = EBADF;
    return EOF;
  }
  struct scan_input input = {.stream = stream};
  return scan_format(&input, format, args);
}

int fscanf(FILE *restrict stream, const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vfscanf(stream, format, args);
  va_end(args);
  return result;
}

int vscanf(const char *restrict format, va_list args)
{
  return vfscanf(stdin, format, args);
}

int scanf(const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vfscanf(stdin, format, args);
  va_end(args);
  return result;
}

int vsscanf(const char *restrict text, const char *restrict format, va_list args)
{
  struct scan_input input = {.text = text};
  return scan_format(&input, format, args);
}

int sscanf(const char *restrict text, const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vsscanf(text, format, args);
  va_end(args);
  return result;
}
