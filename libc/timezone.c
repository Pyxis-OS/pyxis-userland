#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pyxis/environment.h>
#include "errors.h"
#include <string.h>
#include "timezone.h"

#define TZIF_HEADER_SIZE 44
#define TZIF_TYPE_SIZE 6
#define TZIF_MAX_TYPES 256

enum tzif_count_offset {
  TZIF_UT_COUNT = 20, TZIF_STANDARD_COUNT = 24, TZIF_LEAP_COUNT = 28,
  TZIF_TRANSITION_COUNT = 32, TZIF_TYPE_COUNT = 36, TZIF_NAME_SIZE = 40,
};

struct tz_footer {
  bool present, daylight, unspecified;
  long standard_offset, daylight_offset;
  const char *standard_name, *daylight_name;
  struct tz_rule start, end;
};

struct timezone_data {
  char *name;
  unsigned char *bytes;
  const unsigned char *transitions, *indices, *types, *names;
  uint32_t transition_count;
  struct tz_footer future;
};

/* One userspace thread per process. Publish only a fully validated replacement;
 * failed selections must never use the previously cached zone as a fallback. */
static struct timezone_data cached_zone;

static uint32_t read_u32(const unsigned char *bytes)
{
  return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
         (uint32_t)bytes[2] << 8 | bytes[3];
}

static int64_t read_time(const unsigned char *bytes)
{
  return (int64_t)((uint64_t)read_u32(bytes) << 32 | read_u32(bytes + 4));
}

static bool take_bytes(const unsigned char **cursor, size_t *remaining,
    uint32_t count, size_t width)
{
  if (count > *remaining / width) {
    return false;
  }
  size_t size = (size_t)count * width;
  *cursor += size;
  *remaining -= size;
  return true;
}

static bool read_number(const char **cursor, int maximum, int *value)
{
  const char *p = *cursor;
  if (*p < '0' || *p > '9') {
    return false;
  }
  int number = 0;
  do {
    int digit = *p++ - '0';
    if (number > maximum / 10 ||
        (number == maximum / 10 && digit > maximum % 10)) {
      return false;
    }
    number = number * 10 + digit;
  } while (*p >= '0' && *p <= '9');
  *cursor = p;
  *value = number;
  return true;
}

static bool consume(const char **cursor, char character)
{
  if (**cursor != character) {
    return false;
  }
  ++*cursor;
  return true;
}

static bool read_offset(const char **cursor, int maximum_hour, int *seconds)
{
  bool negative = consume(cursor, '-');
  if (!negative) {
    consume(cursor, '+');
  }
  int hour, minute = 0, second = 0;
  if (!read_number(cursor, maximum_hour, &hour)) {
    return false;
  }
  if (consume(cursor, ':')) {
    if (!read_number(cursor, 59, &minute)) {
      return false;
    }
    if (consume(cursor, ':') && !read_number(cursor, 59, &second)) {
      return false;
    }
  }
  *seconds = (hour * 3600 + minute * 60 + second) * (negative ? -1 : 1);
  return true;
}

static bool ascii_letter(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool read_name(const char **cursor, bool *unspecified,
    const char **name, size_t *name_size)
{
  bool quoted = consume(cursor, '<');
  const char *start = *cursor;
  while (ascii_letter(**cursor) || (quoted &&
      ((**cursor >= '0' && **cursor <= '9') || **cursor == '+' || **cursor == '-'))) {
    ++*cursor;
  }
  size_t length = (size_t)(*cursor - start);
  *name = start;
  *name_size = length;
  *unspecified = length == 3 && !memcmp(start, "-00", 3);
  return length >= 3 && (!quoted || consume(cursor, '>'));
}

static bool read_rule(const char **cursor, char version, struct tz_rule *rule)
{
  if (consume(cursor, 'M')) {
    rule->kind = TZ_MONTH_WEEK_DAY;
    if (!read_number(cursor, 12, &rule->month) || !rule->month ||
        !consume(cursor, '.') || !read_number(cursor, 5, &rule->week) ||
        !rule->week || !consume(cursor, '.') ||
        !read_number(cursor, 6, &rule->weekday)) {
      return false;
    }
  } else {
    rule->kind = consume(cursor, 'J') ? TZ_JULIAN_DAY : TZ_DAY_OF_YEAR;
    if (!read_number(cursor, 365, &rule->day) ||
        (rule->kind == TZ_JULIAN_DAY && !rule->day)) {
      return false;
    }
  }
  rule->seconds = 2 * 3600;
  if (consume(cursor, '/')) {
    if (version == '2' && (**cursor == '-' || **cursor == '+')) {
      return false;
    }
    return read_offset(cursor, version == '2' ? 24 : 167, &rule->seconds);
  }
  return true;
}

static bool read_footer(char *text, char version, struct tz_footer *future)
{
  if (!*text) {
    return true;
  }
  future->present = true;
  const char *cursor = text;
  size_t standard_size, daylight_size = 0;
  int offset;
  if (!read_name(&cursor, &future->unspecified, &future->standard_name,
      &standard_size) || !read_offset(&cursor, 24, &offset)) {
    return false;
  }
  /* POSIX footer offsets have the opposite sign from TZif and tm_gmtoff. */
  future->standard_offset = -offset;
  if (!*cursor) {
    text[future->standard_name - text + standard_size] = 0;
    return true;
  }
  bool unspecified;
  if (!read_name(&cursor, &unspecified, &future->daylight_name,
      &daylight_size) || unspecified) {
    return false;
  }
  future->daylight = true;
  future->daylight_offset = future->standard_offset + 3600;
  if (*cursor != ',') {
    if (!read_offset(&cursor, 24, &offset)) {
      return false;
    }
    future->daylight_offset = -offset;
  }
  if (!consume(&cursor, ',') || !read_rule(&cursor, version, &future->start) ||
      !consume(&cursor, ',') || !read_rule(&cursor, version, &future->end) || *cursor) {
    return false;
  }
  /* Parsing is complete before separators become designation terminators. */
  text[future->standard_name - text + standard_size] = 0;
  text[future->daylight_name - text + daylight_size] = 0;
  return true;
}

static bool read_block(const unsigned char **cursor, size_t *remaining,
    unsigned width, char version, struct timezone_data *zone)
{
  const unsigned char *header = *cursor;
  if (!take_bytes(cursor, remaining, TZIF_HEADER_SIZE, 1) ||
      memcmp(header, "TZif", 4) || header[4] != version) {
    return false;
  }
  uint32_t ut_count = read_u32(header + TZIF_UT_COUNT);
  uint32_t standard_count = read_u32(header + TZIF_STANDARD_COUNT);
  uint32_t leap_count = read_u32(header + TZIF_LEAP_COUNT);
  uint32_t time_count = read_u32(header + TZIF_TRANSITION_COUNT);
  uint32_t type_count = read_u32(header + TZIF_TYPE_COUNT);
  uint32_t name_size = read_u32(header + TZIF_NAME_SIZE);
  if (!type_count || type_count > TZIF_MAX_TYPES || !name_size || leap_count ||
      (ut_count && ut_count != type_count) ||
      (standard_count && standard_count != type_count)) {
    return false;
  }

  const unsigned char *transitions = *cursor;
  if (!take_bytes(cursor, remaining, time_count, width)) {
    return false;
  }
  const unsigned char *indices = *cursor;
  if (!take_bytes(cursor, remaining, time_count, 1)) {
    return false;
  }
  const unsigned char *types = *cursor;
  if (!take_bytes(cursor, remaining, type_count, TZIF_TYPE_SIZE)) {
    return false;
  }
  const unsigned char *names = *cursor;
  if (!take_bytes(cursor, remaining, name_size, 1) || names[name_size - 1]) {
    return false;
  }
  const unsigned char *standard = *cursor;
  if (!take_bytes(cursor, remaining, standard_count, 1)) {
    return false;
  }
  const unsigned char *ut = *cursor;
  if (!take_bytes(cursor, remaining, ut_count, 1)) {
    return false;
  }

  for (uint32_t i = 0; i < time_count; ++i) {
    if (indices[i] >= type_count) {
      return false;
    }
    if (i) {
      const unsigned char *current = transitions + (size_t)i * width;
      int64_t before = width == 8 ? read_time(current - width) :
          (int32_t)read_u32(current - width);
      int64_t after = width == 8 ? read_time(current) : (int32_t)read_u32(current);
      if (before >= after) {
        return false;
      }
    }
  }
  for (uint32_t i = 0; i < type_count; ++i) {
    const unsigned char *type = types + i * TZIF_TYPE_SIZE;
    if ((int32_t)read_u32(type) == INT32_MIN || type[4] > 1 ||
        type[5] >= name_size || (standard_count && standard[i] > 1) ||
        (ut_count && (ut[i] > 1 || (ut[i] && (!standard_count || !standard[i]))))) {
      return false;
    }
  }
  if (width == 8) {
    zone->transitions = transitions;
    zone->indices = indices;
    zone->types = types;
    zone->names = names;
    zone->transition_count = time_count;
  }
  return true;
}

static bool read_tzif(struct timezone_data *zone, size_t size)
{
  if (size < TZIF_HEADER_SIZE) {
    return false;
  }
  char version = zone->bytes[4];
  if (version != '2' && version != '3' && version != '4') {
    return false;
  }
  const unsigned char *cursor = zone->bytes;
  size_t remaining = size;
  if (!read_block(&cursor, &remaining, 4, version, zone) ||
      !read_block(&cursor, &remaining, 8, version, zone) ||
      remaining < 2 || cursor[0] != '\n' || cursor[remaining - 1] != '\n') {
    return false;
  }
  for (size_t i = 1; i < remaining - 1; ++i) {
    if (cursor[i] < ' ' || cursor[i] > '~') {
      return false;
    }
  }
  /* The validated final newline becomes the parser's bounded terminator. */
  zone->bytes[size - 1] = 0;
  return read_footer((char *)cursor + 1, version, &zone->future);
}

static bool valid_zone_name(const char *name)
{
  bool component = false;
  for (; *name; ++name) {
    if (*name == '/') {
      if (!component) {
        return false;
      }
      component = false;
    } else if (ascii_letter(*name) || (*name >= '0' && *name <= '9') ||
        *name == '_' || *name == '-' || *name == '+') {
      component = true;
    } else {
      return false;
    }
  }
  return component;
}

static int load_zone(const char *name)
{
  if (!valid_zone_name(name)) {
    errno = EINVAL;
    return -1;
  }
  static const char prefix[] = "boot://share/zoneinfo/";
  size_t length = strlen(name);
  if (length > SIZE_MAX - sizeof(prefix)) {
    errno = EOVERFLOW;
    return -1;
  }
  char *path = malloc(sizeof(prefix) + length);
  if (!path) {
    return -1;
  }
  memcpy(path, prefix, sizeof(prefix) - 1);
  memcpy(path + sizeof(prefix) - 1, name, length + 1);
  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) {
    return -1;
  }

  struct timezone_data zone = {0};
  if (fseek(file, 0, SEEK_END)) {
    goto fail;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET)) {
    goto fail;
  }
  if (size < TZIF_HEADER_SIZE) {
    errno = EINVAL;
    goto fail;
  }
  zone.bytes = malloc((size_t)size);
  if (!zone.bytes) {
    goto fail;
  }
  if (fread(zone.bytes, 1, (size_t)size, file) != (size_t)size) {
    if (!ferror(file)) {
      errno = EIO;
    }
    goto fail;
  }
  if (!read_tzif(&zone, (size_t)size)) {
    errno = EINVAL;
    goto fail;
  }
  zone.name = strdup(name);
  if (!zone.name) {
    goto fail;
  }
  if (fclose(file)) {
    free(zone.name);
    free(zone.bytes);
    return -1;
  }
  free(cached_zone.name);
  free(cached_zone.bytes);
  cached_zone = zone;
  return 0;

fail:
  int error = errno;
  fclose(file);
  free(zone.name);
  free(zone.bytes);
  errno = error;
  return -1;
}

static int future_offset(const struct tz_footer *future, int64_t seconds,
    long *offset, int *daylight)
{
  if (future->unspecified) {
    errno = ENOTSUP;
    return -1;
  }
  *daylight = 0;
  *offset = future->standard_offset;
  if (!future->daylight) {
    return 0;
  }
  long long utc_year;
  if (__tz_year_for_secs(seconds, &utc_year) < 0) {
    errno = EOVERFLOW;
    return -1;
  }
  int64_t latest = INT64_MIN;
  /* Signed rule times and UTC offsets can cross New Year. Consider neighboring
   * rule years, including southern-hemisphere and all-year DST. At coincident
   * end/start transitions, the new daylight interval wins. */
  for (long long year = utc_year - 1; year <= utc_year + 1; ++year) {
    int64_t start = __tz_rule_to_secs(&future->start, year) - future->standard_offset;
    int64_t end = __tz_rule_to_secs(&future->end, year) - future->daylight_offset;
    if (end <= seconds && end > latest) {
      latest = end;
      *daylight = 0;
    }
    if (start <= seconds && start >= latest) {
      latest = start;
      *daylight = 1;
    }
  }
  *offset = *daylight ? future->daylight_offset : future->standard_offset;
  return 0;
}

int timezone_offset(int64_t seconds, long *offset, int *daylight,
    const char **designation)
{
  const char *name;
  enum call_status status = pyxis_environment_get("TZ", &name);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    errno = libc_call_errno(status);
    return -1;
  }
  if (!name || !*name) {
    *offset = 0;
    *daylight = 0;
    *designation = "UTC";
    return 0;
  }
  if ((!cached_zone.name || strcmp(name, cached_zone.name)) && load_zone(name)) {
    return -1;
  }
  const struct timezone_data *zone = &cached_zone;
  size_t count = zone->transition_count;
  if (!count || seconds >= read_time(zone->transitions + (count - 1) * 8)) {
    if (zone->future.present) {
      if (future_offset(&zone->future, seconds, offset, daylight)) {
        return -1;
      }
      *designation = *daylight ? zone->future.daylight_name :
          zone->future.standard_name;
      return 0;
    }
    if (count) {
      errno = ENOTSUP;
      return -1;
    }
  }

  /* TZif type zero applies before the first transition (also with no table).
   * Search for the first transition strictly later than the requested time. */
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (seconds < read_time(zone->transitions + middle * 8)) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }
  unsigned index = low ? zone->indices[low - 1] : 0;
  const unsigned char *type = zone->types + index * TZIF_TYPE_SIZE;
  if (!strcmp((const char *)zone->names + type[5], "-00")) {
    errno = ENOTSUP;
    return -1;
  }
  *offset = (int32_t)read_u32(type);
  *daylight = type[4];
  *designation = (const char *)zone->names + type[5];
  return 0;
}
