#include "plan_record.h"

#include <pyxis_fs/npfs.h>
#include <string.h>

#define PLAN_LINE_BYTES 160
#define PLAN_TEXT_BYTES 2048
#define PLAN_JOURNAL_MAX_MIB 1024
#define PLAN_HEADER "pyxis-install-plan 1"
#define PLAN_END "end"

struct plan_reader {
  FILE *input;
  uint64_t bytes;
};

static bool nonzero_id(const uint8_t id[16])
{
  for (size_t index = 0; index < 16; ++index) {
    if (id[index]) {
      return true;
    }
  }
  return false;
}

static bool revision_valid(const char revision[64], bool empty)
{
  size_t length = 0;
  while (length < 64 && revision[length]) {
    unsigned char byte = (unsigned char)revision[length];
    if (byte < 33 || byte > 126 || byte == '/') {
      return false;
    }
    ++length;
  }
  return length < 64 && (length || empty) && strcmp(revision, ".") &&
      strcmp(revision, "..");
}

static int hex_digit(unsigned char byte)
{
  if (byte >= '0' && byte <= '9') {
    return byte - '0';
  }
  if (byte >= 'a' && byte <= 'f') {
    return byte - 'a' + 10;
  }
  return -1;
}

static bool hash_valid(const char hash[65])
{
  for (size_t index = 0; index < 64; ++index) {
    if (hex_digit((unsigned char)hash[index]) < 0) {
      return false;
    }
  }
  return hash[64] == '\0';
}

bool install_plan_record_valid(const struct install_plan_record *record)
{
  if (!record || record->block_size < 512 || record->block_size > NPFS_BLOCK_SIZE ||
      (record->block_size & (record->block_size - 1)) ||
      record->block_count > UINT64_MAX / record->block_size ||
      record->portable != nonzero_id(record->existing_guid) ||
      !revision_valid(record->revision, false) ||
      !revision_valid(record->previous, !record->updating) ||
      !hash_valid(record->source_hash) || !hash_valid(record->target_hash) ||
      !nonzero_id(record->layout.disk_guid) || !nonzero_id(record->layout.esp_guid) ||
      !nonzero_id(record->layout.pool_guid) ||
      !memcmp(record->layout.disk_guid, record->layout.esp_guid, 16) ||
      !memcmp(record->layout.disk_guid, record->layout.pool_guid, 16) ||
      !memcmp(record->layout.esp_guid, record->layout.pool_guid, 16)) {
    return false;
  }
  struct install_disk disk = {
    .info = {.block_count = record->block_count, .block_size = record->block_size},
    .bytes = record->block_count * record->block_size,
  };
  struct install_layout expected = {0};
  if (!install_layout_plan(&disk, &expected) ||
      record->layout.esp_start != expected.esp_start ||
      record->layout.esp_bytes != expected.esp_bytes ||
      record->layout.pool_start != expected.pool_start ||
      record->layout.pool_bytes != expected.pool_bytes) {
    return false;
  }
  if (record->updating) {
    return record->portable && !record->read_the_room && !record->journal_mib &&
        !nonzero_id(record->pool_id) && !nonzero_id(record->volume_id) &&
        !memcmp(record->existing_guid, record->layout.disk_guid, 16);
  }
  if (record->legacy || record->previous[0] || !record->journal_mib ||
      record->journal_mib > PLAN_JOURNAL_MAX_MIB || !nonzero_id(record->pool_id) ||
      !nonzero_id(record->volume_id)) {
    return false;
  }
  struct npfs_header header;
  return npfs_header_layout(record->layout.pool_bytes / NPFS_BLOCK_SIZE,
      record->journal_mib * INSTALL_MIB / NPFS_BLOCK_SIZE, record->pool_id,
      &header) == NPFS_OK;
}

static bool read_line(struct plan_reader *reader, char line[PLAN_LINE_BYTES])
{
  size_t length = 0;
  for (;;) {
    if (reader->bytes == INSTALL_PLAN_RECORD_MAX_BYTES) {
      return false;
    }
    int byte = fgetc(reader->input);
    if (byte == EOF) {
      return false;
    }
    ++reader->bytes;
    if (byte == '\n') {
      line[length] = '\0';
      return !ferror(reader->input);
    }
    if (byte < 32 || byte > 126 || length + 1 >= PLAN_LINE_BYTES) {
      return false;
    }
    line[length++] = (char)byte;
  }
}

static bool read_value(struct plan_reader *reader, const char *key,
    char value[PLAN_LINE_BYTES])
{
  char line[PLAN_LINE_BYTES];
  size_t key_bytes = strlen(key);
  if (!read_line(reader, line) || strncmp(line, key, key_bytes) ||
      line[key_bytes] != ' ') {
    return false;
  }
  strcpy(value, line + key_bytes + 1);
  return true;
}

static bool read_boolean(struct plan_reader *reader, const char *key, bool *output)
{
  char value[PLAN_LINE_BYTES];
  if (!read_value(reader, key, value) ||
      (strcmp(value, "0") && strcmp(value, "1"))) {
    return false;
  }
  *output = value[0] == '1';
  return true;
}

static bool read_decimal(struct plan_reader *reader, const char *key, uint64_t *output)
{
  char value[PLAN_LINE_BYTES];
  if (!read_value(reader, key, value) || !value[0] ||
      (value[0] == '0' && value[1])) {
    return false;
  }
  uint64_t number = 0;
  for (size_t index = 0; value[index]; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte < '0' || byte > '9' || number > (UINT64_MAX - (byte - '0')) / 10) {
      return false;
    }
    number = number * 10 + (byte - '0');
  }
  *output = number;
  return true;
}

static bool decode_hex(const char *text, uint8_t *output, size_t bytes)
{
  if (strlen(text) != bytes * 2) {
    return false;
  }
  for (size_t index = 0; index < bytes; ++index) {
    int high = hex_digit((unsigned char)text[index * 2]);
    int low = hex_digit((unsigned char)text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    output[index] = (uint8_t)((high << 4) | low);
  }
  return true;
}

static bool read_id(struct plan_reader *reader, const char *key, uint8_t output[16])
{
  char value[PLAN_LINE_BYTES];
  return read_value(reader, key, value) && decode_hex(value, output, 16);
}

static bool read_revision(struct plan_reader *reader, const char *key, char output[64])
{
  char value[PLAN_LINE_BYTES];
  if (!read_value(reader, key, value)) {
    return false;
  }
  size_t length = strlen(value);
  if (length % 2 || length / 2 >= 64 ||
      !decode_hex(value, (uint8_t *)output, length / 2)) {
    return false;
  }
  /* Embedded NULs must not hide a suffix from component validation. */
  for (size_t index = 0; index < length / 2; ++index) {
    if (!output[index]) {
      return false;
    }
  }
  output[length / 2] = '\0';
  return true;
}

static bool read_hash(struct plan_reader *reader, const char *key, char output[65])
{
  char value[PLAN_LINE_BYTES];
  if (!read_value(reader, key, value) || strlen(value) != 64 || !hash_valid(value)) {
    return false;
  }
  memcpy(output, value, 65);
  return true;
}

bool install_plan_record_parse(FILE *input, struct install_plan_record *output)
{
  if (!input || !output || ferror(input)) {
    return false;
  }
  struct plan_reader reader = {.input = input};
  struct install_plan_record record = {0};
  char line[PLAN_LINE_BYTES];
  if (!read_line(&reader, line) || strcmp(line, PLAN_HEADER) ||
      !read_boolean(&reader, "updating", &record.updating) ||
      !read_boolean(&reader, "read_the_room", &record.read_the_room) ||
      !read_boolean(&reader, "legacy", &record.legacy) ||
      !read_boolean(&reader, "portable", &record.portable) ||
      !read_decimal(&reader, "block_count", &record.block_count) ||
      !read_decimal(&reader, "block_size", &record.block_size) ||
      !read_decimal(&reader, "journal_mib", &record.journal_mib) ||
      !read_id(&reader, "existing_guid", record.existing_guid) ||
      !read_id(&reader, "pool_id", record.pool_id) ||
      !read_id(&reader, "volume_id", record.volume_id) ||
      !read_decimal(&reader, "esp_start", &record.layout.esp_start) ||
      !read_decimal(&reader, "esp_bytes", &record.layout.esp_bytes) ||
      !read_decimal(&reader, "pool_start", &record.layout.pool_start) ||
      !read_decimal(&reader, "pool_bytes", &record.layout.pool_bytes) ||
      !read_id(&reader, "disk_guid", record.layout.disk_guid) ||
      !read_id(&reader, "esp_guid", record.layout.esp_guid) ||
      !read_id(&reader, "pool_guid", record.layout.pool_guid) ||
      !read_revision(&reader, "revision", record.revision) ||
      !read_revision(&reader, "previous", record.previous) ||
      !read_hash(&reader, "source_hash", record.source_hash) ||
      !read_hash(&reader, "target_hash", record.target_hash) ||
      !read_line(&reader, line) || strcmp(line, PLAN_END) ||
      !install_plan_record_valid(&record)) {
    return false;
  }
  *output = record;
  return true;
}

static void encode_hex(const uint8_t *bytes, size_t length, char *output)
{
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < length; ++index) {
    output[index * 2] = digits[bytes[index] >> 4];
    output[index * 2 + 1] = digits[bytes[index] & 15];
  }
  output[length * 2] = '\0';
}

bool install_plan_record_format(FILE *output, const struct install_plan_record *record)
{
  if (!output || !install_plan_record_valid(record) || ferror(output)) {
    return false;
  }
  char existing_guid[33], pool_id[33], volume_id[33];
  char disk_guid[33], esp_guid[33], pool_guid[33];
  char revision[127], previous[127];
  encode_hex(record->existing_guid, 16, existing_guid);
  encode_hex(record->pool_id, 16, pool_id);
  encode_hex(record->volume_id, 16, volume_id);
  encode_hex(record->layout.disk_guid, 16, disk_guid);
  encode_hex(record->layout.esp_guid, 16, esp_guid);
  encode_hex(record->layout.pool_guid, 16, pool_guid);
  encode_hex((const uint8_t *)record->revision, strlen(record->revision), revision);
  encode_hex((const uint8_t *)record->previous, strlen(record->previous), previous);
  char text[PLAN_TEXT_BYTES];
  int length = snprintf(text, sizeof(text), PLAN_HEADER "\n"
      "updating %u\nread_the_room %u\nlegacy %u\nportable %u\n"
      "block_count %llu\nblock_size %llu\njournal_mib %llu\n"
      "existing_guid %s\npool_id %s\nvolume_id %s\n"
      "esp_start %llu\nesp_bytes %llu\npool_start %llu\npool_bytes %llu\n"
      "disk_guid %s\nesp_guid %s\npool_guid %s\n"
      "revision %s\nprevious %s\nsource_hash %s\ntarget_hash %s\n" PLAN_END "\n",
      (unsigned)record->updating, (unsigned)record->read_the_room,
      (unsigned)record->legacy, (unsigned)record->portable,
      (unsigned long long)record->block_count, (unsigned long long)record->block_size,
      (unsigned long long)record->journal_mib, existing_guid, pool_id, volume_id,
      (unsigned long long)record->layout.esp_start,
      (unsigned long long)record->layout.esp_bytes,
      (unsigned long long)record->layout.pool_start,
      (unsigned long long)record->layout.pool_bytes, disk_guid, esp_guid, pool_guid,
      revision, previous, record->source_hash, record->target_hash);
  if (length < 0 || (size_t)length >= sizeof(text) ||
      (uint64_t)length > INSTALL_PLAN_RECORD_MAX_BYTES) {
    return false;
  }
  return fwrite(text, 1, (size_t)length, output) == (size_t)length &&
      fflush(output) == 0 && !ferror(output);
}
