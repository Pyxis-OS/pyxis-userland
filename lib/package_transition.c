#include <package_transition.h>
#include <package.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_NAME ".boot-selection"
#define PENDING_NAME ".upgrade-pending"
#define RECORD_BYTES 512u

static bool hex(const char *text, size_t count)
{
  if (strnlen(text, count + 1) != count) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') ||
          (text[i] >= 'a' && text[i] <= 'f'))) return false;
  }
  return true;
}

bool package_boot_identity_valid(const struct package_boot_identity *identity)
{
  return identity && identity->pool && hex(identity->attempt, 32) &&
      identity->kernel_size && identity->archive_size &&
      hex(identity->kernel_sha256, 64) && hex(identity->archive_sha256, 64);
}

bool package_boot_identity_equal(const struct package_boot_identity *a,
    const struct package_boot_identity *b)
{
  return package_boot_identity_valid(a) && package_boot_identity_valid(b) &&
      a->pool == b->pool && !strcmp(a->attempt, b->attempt) &&
      a->kernel_size == b->kernel_size && a->archive_size == b->archive_size &&
      !strcmp(a->kernel_sha256, b->kernel_sha256) &&
      !strcmp(a->archive_sha256, b->archive_sha256);
}

static enum call_status read_record(handle_t packages, const char *name,
    bool *present, char text[RECORD_BYTES + 1])
{
  *present = false;
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_lookup(packages, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status == CALL_NOT_FOUND) return CALL_OK;
  if (status != CALL_OK) return status;
  uint64_t size = 0;
  status = file_size(file, &size);
  if (status == CALL_OK && (!size || size > RECORD_BYTES)) status = CALL_BAD_REQUEST;
  for (uint64_t offset = 0; status == CALL_OK && offset < size;) {
    size_t read = 0;
    status = file_read(file, offset, text + offset, size - offset, &read);
    if (status == CALL_OK && !read) status = CALL_IO;
    offset += read;
  }
  if (status == CALL_OK) {
    text[size] = 0;
    if (memchr(text, 0, size)) status = CALL_BAD_REQUEST;
  }
  if (handle_close(file) != CALL_OK && status == CALL_OK) status = CALL_IO;
  *present = status == CALL_OK;
  return status;
}

static bool number_line(char **cursor, const char *label, uint64_t *number)
{
  size_t length = strlen(label);
  if (strncmp(*cursor, label, length) || (*cursor)[length] != ' ') return false;
  char *first = *cursor + length + 1;
  if (*first < '0' || *first > '9' || (*first == '0' && first[1] != '\n')) return false;
  uint64_t value = 0;
  char *end = first;
  while (*end >= '0' && *end <= '9') {
    unsigned digit = (unsigned)(*end - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
    ++end;
  }
  if (*end != '\n') return false;
  *number = value;
  *cursor = end + 1;
  return true;
}

static bool parse_digest(char **cursor, const char *label, uint64_t *size,
    char sha256[65])
{
  size_t length = strlen(label);
  if (strncmp(*cursor, label, length) || (*cursor)[length] != ' ') return false;
  char *first = *cursor + length + 1;
  if (*first < '1' || *first > '9') return false;
  uint64_t value = 0;
  char *end = first;
  while (*end >= '0' && *end <= '9') {
    unsigned digit = (unsigned)(*end - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
    ++end;
  }
  if (*end++ != ' ' || strlen(end) < 65 || end[64] != '\n') return false;
  char saved = end[64];
  end[64] = 0;
  bool valid = hex(end, 64);
  end[64] = saved;
  if (!valid) return false;
  *size = value;
  memcpy(sha256, end, 64);
  sha256[64] = 0;
  *cursor = end + 65;
  return true;
}

static bool parse_identity(char **cursor, struct package_boot_identity *identity)
{
  uint64_t pool;
  if (!number_line(cursor, "pool", &pool) || !pool || pool > UINT32_MAX ||
      strncmp(*cursor, "attempt ", 8)) return false;
  identity->pool = (uint32_t)pool;
  char *attempt = *cursor + 8;
  if (strlen(attempt) < 33 || attempt[32] != '\n') return false;
  char saved = attempt[32];
  attempt[32] = 0;
  bool valid = hex(attempt, 32);
  attempt[32] = saved;
  if (!valid) return false;
  memcpy(identity->attempt, attempt, 32);
  identity->attempt[32] = 0;
  *cursor = attempt + 33;
  return parse_digest(cursor, "kernel", &identity->kernel_size,
        identity->kernel_sha256) &&
      parse_digest(cursor, "archive", &identity->archive_size,
        identity->archive_sha256);
}

enum call_status package_boot_load(handle_t packages, bool *present,
    uint64_t *set, struct package_boot_identity *identity)
{
  *set = 0;
  *identity = (struct package_boot_identity){0};
  char text[RECORD_BYTES + 1];
  enum call_status status = read_record(packages, BOOT_NAME, present, text);
  if (status != CALL_OK || !*present) return status;
  static const char header[] = "PYXIS-PACKAGE-BOOT 1\n";
  if (strlen(text) < sizeof(header) - 1) return CALL_BAD_REQUEST;
  char *cursor = text + sizeof(header) - 1;
  if (strncmp(text, header, sizeof(header) - 1) ||
      !number_line(&cursor, "set", set) || !*set || *set > INT64_MAX ||
      !parse_identity(&cursor, identity) || *cursor ||
      !package_boot_identity_valid(identity)) return CALL_BAD_REQUEST;
  return CALL_OK;
}

enum call_status package_transition_load(handle_t packages, bool *present,
    struct package_transition *transition)
{
  *transition = (struct package_transition){0};
  char text[RECORD_BYTES + 1];
  enum call_status status = read_record(packages, PENDING_NAME, present, text);
  if (status != CALL_OK || !*present) return status;
  static const char header[] = "PYXIS-PACKAGE-PENDING 1\n";
  if (strlen(text) < sizeof(header) - 1) return CALL_BAD_REQUEST;
  char *cursor = text + sizeof(header) - 1;
  if (strncmp(text, header, sizeof(header) - 1) ||
      !number_line(&cursor, "from", &transition->from_set) ||
      !number_line(&cursor, "to", &transition->to_set) ||
      !transition->to_set || transition->to_set <= transition->from_set ||
      transition->to_set > INT64_MAX || !parse_identity(&cursor, &transition->target) ||
      *cursor || !package_boot_identity_valid(&transition->target)) return CALL_BAD_REQUEST;
  return CALL_OK;
}

static enum call_status write_record(handle_t packages, const char *name,
    const char *temporary, const char *bytes, size_t size, bool replace)
{
  handle_t existing = HANDLE_INVALID;
  enum call_status status = directory_lookup(packages, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &existing);
  if (existing != HANDLE_INVALID) handle_close(existing);
  if (status != CALL_OK && status != CALL_NOT_FOUND) return status;
  if (!replace && status == CALL_OK) return CALL_ALREADY_EXISTS;
  status = directory_remove(packages, temporary, DIRECTORY_KIND_FILE);
  if (status != CALL_OK && status != CALL_NOT_FOUND) return status;
  handle_t file = HANDLE_INVALID;
  status = directory_create(packages, temporary, DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, &file);
  for (size_t offset = 0; status == CALL_OK && offset < size;) {
    size_t written = 0;
    status = file_write(file, offset, bytes + offset, size - offset, &written);
    if (status == CALL_OK && !written) status = CALL_IO;
    offset += written;
  }
  if (status == CALL_OK) status = file_sync(file);
  if (file != HANDLE_INVALID && handle_close(file) != CALL_OK && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status == CALL_OK) status = directory_sync(packages);
  if (status == CALL_OK) {
    status = directory_rename(packages, temporary, packages, name,
        replace ? DIRECTORY_RENAME_REPLACE : 0, DIRECTORY_KIND_FILE);
  }
  if (status == CALL_OK) status = directory_sync(packages);
  return status;
}

static bool render_identity(char *text, size_t size, size_t *used,
    const struct package_boot_identity *identity)
{
  int length = snprintf(text + *used, size - *used,
      "pool %u\nattempt %s\nkernel %llu %s\narchive %llu %s\n",
      identity->pool, identity->attempt,
      (unsigned long long)identity->kernel_size, identity->kernel_sha256,
      (unsigned long long)identity->archive_size, identity->archive_sha256);
  if (length < 0 || (size_t)length >= size - *used) return false;
  *used += (size_t)length;
  return true;
}

enum call_status package_boot_publish(handle_t packages, uint64_t set,
    const struct package_boot_identity *identity)
{
  if (!set || set > INT64_MAX || !package_boot_identity_valid(identity)) return CALL_BAD_REQUEST;
  char text[RECORD_BYTES + 1];
  int length = snprintf(text, sizeof(text), "PYXIS-PACKAGE-BOOT 1\nset %llu\n",
      (unsigned long long)set);
  size_t used = length > 0 ? (size_t)length : sizeof(text);
  if (used >= sizeof(text) || !render_identity(text, sizeof(text), &used, identity)) {
    return CALL_LIMIT;
  }
  return write_record(packages, BOOT_NAME, BOOT_NAME ".new", text, used, true);
}

enum call_status package_transition_publish(handle_t packages,
    const struct package_transition *transition)
{
  if (!transition || transition->to_set <= transition->from_set ||
      transition->to_set > INT64_MAX ||
      !package_boot_identity_valid(&transition->target)) return CALL_BAD_REQUEST;
  char text[RECORD_BYTES + 1];
  int length = snprintf(text, sizeof(text),
      "PYXIS-PACKAGE-PENDING 1\nfrom %llu\nto %llu\n",
      (unsigned long long)transition->from_set,
      (unsigned long long)transition->to_set);
  size_t used = length > 0 ? (size_t)length : sizeof(text);
  if (used >= sizeof(text) || !render_identity(text, sizeof(text), &used,
        &transition->target)) return CALL_LIMIT;
  return write_record(packages, PENDING_NAME, PENDING_NAME ".new", text, used, false);
}

enum call_status package_transition_remove(handle_t packages)
{
  enum call_status status = directory_remove(packages, PENDING_NAME, DIRECTORY_KIND_FILE);
  if (status == CALL_OK) status = directory_sync(packages);
  return status;
}

enum call_status package_set_activate(handle_t packages, uint64_t set)
{
  if (!set || set > INT64_MAX) return CALL_BAD_REQUEST;
  char text[96];
  int length = snprintf(text, sizeof(text), "return { set = %llu }\n",
      (unsigned long long)set);
  if (length < 0 || length >= (int)sizeof(text)) return CALL_LIMIT;
  return write_record(packages, "current", "current.new", text, (size_t)length, true);
}
