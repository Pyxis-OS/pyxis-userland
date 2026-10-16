#include <http_navigation.h>
#include <http_prepare.h>
#include <http_url.h>
#include <clock.h>
#include <environment.h>
#include <handle.h>
#include <provider.h>
#include <startup.h>
#include <pyxis/descriptor.h>
#include <pyxis/resource.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static bool utf8(const char *text, size_t size)
{
  size_t offset = 0;
  while (offset < size) {
    unsigned char first = text[offset++];
    if (first < 0x80) {
      if (first < 0x20 || first == 0x7f) {
        return false;
      }
      continue;
    }
    size_t count;
    unsigned char low = 0x80, high = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2;
      if (first == 0xe0) low = 0xa0;
      if (first == 0xed) high = 0x9f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3;
      if (first == 0xf0) low = 0x90;
      if (first == 0xf4) high = 0x8f;
    } else {
      return false;
    }
    if (count > size - offset || (unsigned char)text[offset] < low ||
        (unsigned char)text[offset] > high) {
      return false;
    }
    ++offset;
    while (--count) {
      unsigned char next = text[offset++];
      if (next < 0x80 || next > 0xbf) return false;
    }
  }
  return true;
}

static bool valid_metadata(const struct http_navigation *metadata)
{
  size_t url = strnlen(metadata->effective_url, sizeof(metadata->effective_url));
  size_t media = strnlen(metadata->media_type, sizeof(metadata->media_type));
  struct http_uri parsed;
  return url < sizeof(metadata->effective_url) && media < sizeof(metadata->media_type) &&
      metadata->status >= 100 && metadata->status <= 599 &&
      utf8(metadata->effective_url, url) && utf8(metadata->media_type, media) &&
      http_url_parse(metadata->effective_url, &parsed) == CALL_OK;
}

static bool append(char *bytes, size_t capacity, size_t *size, const char *text)
{
  size_t count = strlen(text);
  if (count > capacity - *size) return false;
  memcpy(bytes + *size, text, count);
  *size += count;
  return true;
}

static bool quoted(char *bytes, size_t capacity, size_t *size, const char *text)
{
  if (!append(bytes, capacity, size, "\"")) return false;
  for (; *text; ++text) {
    if ((*text == '"' || *text == '\\') && !append(bytes, capacity, size, "\\")) return false;
    if (*size == capacity) return false;
    bytes[(*size)++] = *text;
  }
  return append(bytes, capacity, size, "\"");
}

enum call_status http_navigation_encode(const struct http_navigation *metadata,
    void *bytes, size_t capacity, size_t *size)
{
  if (size) *size = 0;
  if (!metadata || !bytes || !size || !valid_metadata(metadata)) return CALL_BAD_REQUEST;
  if (capacity < 4) return CALL_BUFFER_TOO_SMALL;
  size_t limit = capacity - 4;
  if (limit > HTTP_NAVIGATION_METADATA_MAX) limit = HTTP_NAVIGATION_METADATA_MAX;
  char *json = (char *)bytes + 4;
  size_t count = 0;
  char status[] = {(char)('0' + metadata->status / 100),
    (char)('0' + metadata->status / 10 % 10), (char)('0' + metadata->status % 10), 0};
  if (!append(json, limit, &count, "{\"effective_url\":") ||
      !quoted(json, limit, &count, metadata->effective_url) ||
      !append(json, limit, &count, ",\"media_type\":") ||
      !quoted(json, limit, &count, metadata->media_type) ||
      !append(json, limit, &count, ",\"status\":") ||
      !append(json, limit, &count, status) || !append(json, limit, &count, "}")) {
    return CALL_BUFFER_TOO_SMALL;
  }
  unsigned char *prefix = bytes;
  for (unsigned i = 0; i < 4; ++i) prefix[i] = (unsigned char)(count >> (i * 8));
  *size = count + 4;
  return CALL_OK;
}

struct parser {
  const unsigned char *bytes;
  size_t size, offset;
};

static void whitespace(struct parser *parser)
{
  while (parser->offset < parser->size) {
    unsigned char byte = parser->bytes[parser->offset];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') break;
    ++parser->offset;
  }
}

static bool take(struct parser *parser, unsigned char byte)
{
  whitespace(parser);
  if (parser->offset == parser->size || parser->bytes[parser->offset] != byte) return false;
  ++parser->offset;
  return true;
}

static bool hex_word(struct parser *parser, uint32_t *word)
{
  *word = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (parser->offset == parser->size) return false;
    unsigned byte = parser->bytes[parser->offset++], digit;
    if (byte >= '0' && byte <= '9') digit = byte - '0';
    else if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10;
    else if (byte >= 'A' && byte <= 'F') digit = byte - 'A' + 10;
    else return false;
    *word = *word * 16 + digit;
  }
  return true;
}

static bool string(struct parser *parser, char *text, size_t capacity)
{
  if (!take(parser, '"')) return false;
  size_t count = 0;
  while (parser->offset < parser->size) {
    unsigned byte = parser->bytes[parser->offset++];
    if (byte == '"') {
      text[count] = 0;
      return utf8(text, count);
    }
    if (byte < 0x20) return false;
    if (byte == '\\') {
      if (parser->offset == parser->size) return false;
      byte = parser->bytes[parser->offset++];
      if (byte == 'u') {
        uint32_t value;
        if (!hex_word(parser, &value)) return false;
        if (value >= 0xd800 && value <= 0xdbff) {
          uint32_t low;
          if (parser->size - parser->offset < 2 || parser->bytes[parser->offset++] != '\\' ||
              parser->bytes[parser->offset++] != 'u' || !hex_word(parser, &low) ||
              low < 0xdc00 || low > 0xdfff) return false;
          value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
        } else if (value >= 0xdc00 && value <= 0xdfff) return false;
        if (value < 0x20 || value == 0x7f) return false;
        unsigned char encoded[4];
        size_t length;
        if (value < 0x80) {
          encoded[0] = value;
          length = 1;
        } else if (value < 0x800) {
          encoded[0] = 0xc0 | (value >> 6); encoded[1] = 0x80 | (value & 63); length = 2;
        } else if (value < 0x10000) {
          encoded[0] = 0xe0 | (value >> 12); encoded[1] = 0x80 | ((value >> 6) & 63);
          encoded[2] = 0x80 | (value & 63); length = 3;
        } else {
          encoded[0] = 0xf0 | (value >> 18); encoded[1] = 0x80 | ((value >> 12) & 63);
          encoded[2] = 0x80 | ((value >> 6) & 63); encoded[3] = 0x80 | (value & 63); length = 4;
        }
        if (length >= capacity - count) return false;
        memcpy(text + count, encoded, length);
        count += length;
        continue;
      }
      if (byte != '"' && byte != '\\' && byte != '/') return false;
    }
    if (count + 1 >= capacity) return false;
    text[count++] = byte;
  }
  return false;
}

static bool decode(const unsigned char *bytes, size_t size, struct http_navigation *metadata)
{
  struct parser parser = {bytes, size, 0};
  if (!take(&parser, '{')) return false;
  unsigned seen = 0;
  do {
    char key[14];
    if (!string(&parser, key, sizeof(key)) || !take(&parser, ':')) return false;
    unsigned field;
    if (!strcmp(key, "effective_url")) {
      field = 1;
      if (!string(&parser, metadata->effective_url, sizeof(metadata->effective_url))) return false;
    } else if (!strcmp(key, "media_type")) {
      field = 2;
      if (!string(&parser, metadata->media_type, sizeof(metadata->media_type))) return false;
    } else if (!strcmp(key, "status")) {
      field = 4;
      whitespace(&parser);
      if (parser.size - parser.offset < 3) return false;
      unsigned status = 0;
      for (unsigned i = 0; i < 3; ++i) {
        unsigned byte = parser.bytes[parser.offset++];
        if (byte < '0' || byte > '9') return false;
        status = status * 10 + byte - '0';
      }
      metadata->status = status;
    } else return false;
    if (seen & field) return false;
    seen |= field;
  } while (take(&parser, ','));
  if (!take(&parser, '}')) return false;
  whitespace(&parser);
  return seen == 7 && parser.offset == parser.size && valid_metadata(metadata);
}

int http_navigation_read(FILE *stream, struct http_navigation *metadata)
{
  if (!stream || !metadata) { errno = EINVAL; return -1; }
  *metadata = (struct http_navigation){0};
  unsigned char prefix[4], bytes[HTTP_NAVIGATION_METADATA_MAX];
  if (fread(prefix, 1, sizeof(prefix), stream) != sizeof(prefix)) {
    if (!ferror(stream)) errno = EIO;
    return -1;
  }
  uint32_t size = (uint32_t)prefix[0] | (uint32_t)prefix[1] << 8 |
      (uint32_t)prefix[2] << 16 | (uint32_t)prefix[3] << 24;
  if (!size || size > sizeof(bytes) || fread(bytes, 1, size, stream) != size ||
      !decode(bytes, size, metadata)) {
    *metadata = (struct http_navigation){0};
    errno = EIO;
    return -1;
  }
  return 0;
}

FILE *http_navigation_fopen(const char *path, struct http_navigation *metadata)
{
  if (!path || !metadata) { errno = EINVAL; return NULL; }
  *metadata = (struct http_navigation){0};
  const char *separator = strstr(path, "://");
  if (!separator) return fopen(path, "rb");
  size_t length = separator - path;
  if (!length || length > ENVIRONMENT_NAME_MAX) { errno = EINVAL; return NULL; }
  char name[ENVIRONMENT_NAME_MAX + 1];
  memcpy(name, path, length); name[length] = 0;
  handle_t root = HANDLE_INVALID, clock = HANDLE_INVALID;
  struct resource_result result = {0}, configured = {0};
  enum call_status status = environment_lookup_file(startup_environment_handle(), name, &root);
  struct handle_info info;
  if (status == CALL_OK) status = handle_query(root, &info);
  bool framed = false;
  if (status == CALL_OK && info.protocol == PROTOCOL_PROVIDER) {
    uint64_t now = 0;
    status = environment_capability(startup_environment_handle(), "clock", &clock);
    if (status == CALL_OK) status = clock_now(clock, &now);
    if (status == CALL_OK && now > UINT64_MAX - HTTP_FETCH_NS) status = CALL_LIMIT;
    if (status == CALL_OK) {
      const char *tail = separator + 3;
      bool unsupported = false;
      status = http_prepare_endpoint(root, HTTP_ENDPOINT_GET, HTTP_ENDPOINT_NAVIGATION,
          tail, strlen(tail), NULL, 0, HTTP_FETCH_NS, now + HTTP_FETCH_NS, &configured, &unsupported);
      if (status == CALL_BAD_OPERATION && unsupported) {
        status = resource_open_held(root, tail, PROVIDER_INTENT_READ, 0,
            now + HTTP_FETCH_NS, &result);
      } else if (status == CALL_OK) {
        status = resource_outcome_status(configured.info.description.outcome);
        if (status == CALL_OK) {
          status = resource_open_held(configured.resource, "", PROVIDER_INTENT_READ, 0,
              now + HTTP_FETCH_NS, &result);
          framed = true;
        }
      }
    }
  } else if (status == CALL_OK) {
    status = resource_open_held(root, separator + 3, PROVIDER_INTENT_READ, 0, 0, &result);
  }
  if (status == CALL_OK) status = resource_outcome_status(result.info.description.outcome);
  FILE *stream = NULL;
  if (status == CALL_OK) {
    int descriptor = pyxis_descriptor_adopt(&result.resource, PYXIS_DESCRIPTOR_READ);
    if (descriptor >= 0) {
      stream = fdopen(descriptor, "rb");
      if (!stream) close(descriptor);
    }
  } else errno = pyxis_resource_errno(status);
  int error = errno;
  if (root) handle_close(root);
  if (clock) handle_close(clock);
  resource_result_close(&configured);
  resource_result_close(&result);
  errno = error;
  if (stream && framed && http_navigation_read(stream, metadata)) {
    error = errno;
    fclose(stream);
    errno = error;
    return NULL;
  }
  return stream;
}
