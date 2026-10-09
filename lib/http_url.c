#include <http_url.h>
#include <string.h>

#define DNS_LABEL_MAX 63
#define DNS_TEXT_MAX 253
#define HTTP_MERGED_PATH_MAX (HTTP_URI_MAX * 2)

struct url_part {
  const char *text;
  size_t length;
};

struct url_reference {
  struct url_part scheme, authority, path, query, fragment;
};

struct url_authority {
  enum http_scheme scheme;
  struct url_part host;
  uint16_t port;
};

static bool ascii_alpha(unsigned byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

static bool ascii_digit(unsigned byte)
{
  return byte >= '0' && byte <= '9';
}

static unsigned ascii_lower(unsigned byte)
{
  return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

static int hex_value(unsigned byte)
{
  if (ascii_digit(byte)) {
    return byte - '0';
  }
  byte = ascii_lower(byte);
  return byte >= 'a' && byte <= 'f' ? (int)(byte - 'a' + 10) : -1;
}

static bool part_equal(struct url_part part, const char *text)
{
  if (part.length != strlen(text)) {
    return false;
  }
  for (size_t i = 0; i < part.length; ++i) {
    if (ascii_lower((unsigned char)part.text[i]) != (unsigned char)text[i]) {
      return false;
    }
  }
  return true;
}

static enum call_status reference_parse(const char *text,
    struct url_reference *reference)
{
  *reference = (struct url_reference){0};
  if (!text) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strnlen(text, HTTP_URI_MAX + 1);
  if (length > HTTP_URI_MAX) {
    return CALL_FILE_TOO_LARGE;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned byte = (unsigned char)text[i];
    if (!ascii_alpha(byte) && !ascii_digit(byte) &&
        !strchr("-._~:/?#@!$&'()*+,;=%", byte)) {
      return CALL_BAD_REQUEST;
    }
    if (byte == '%' && (length - i < 3 || hex_value(text[i + 1]) < 0 ||
        hex_value(text[i + 2]) < 0)) {
      return CALL_BAD_REQUEST;
    }
  }
  const char *end = text + length;
  const char *fragment = memchr(text, '#', length);
  if (fragment) {
    if (memchr(fragment + 1, '#', end - fragment - 1)) {
      return CALL_BAD_REQUEST;
    }
    reference->fragment = (struct url_part){fragment + 1, end - fragment - 1};
    end = fragment;
  }
  const char *query = memchr(text, '?', end - text);
  if (query) {
    reference->query = (struct url_part){query + 1, end - query - 1};
    end = query;
  }
  const char *cursor = text;
  while (cursor < end && *cursor != '/' && *cursor != ':') {
    ++cursor;
  }
  if (cursor < end && *cursor == ':') {
    if (cursor == text || !ascii_alpha((unsigned char)*text)) {
      return CALL_BAD_REQUEST;
    }
    for (const char *byte = text + 1; byte < cursor; ++byte) {
      if (!ascii_alpha((unsigned char)*byte) && !ascii_digit((unsigned char)*byte) &&
          !strchr("+.-", *byte)) {
        return CALL_BAD_REQUEST;
      }
    }
    reference->scheme = (struct url_part){text, cursor - text};
    text = cursor + 1;
  }
  if (end - text >= 2 && text[0] == '/' && text[1] == '/') {
    text += 2;
    cursor = text;
    while (cursor < end && *cursor != '/') {
      ++cursor;
    }
    reference->authority = (struct url_part){text, cursor - text};
    text = cursor;
  }
  reference->path = (struct url_part){text, end - text};
  return CALL_OK;
}

static bool ipv4_address(struct url_part host)
{
  size_t position = 0;
  for (unsigned i = 0; i < 4; ++i) {
    unsigned value = 0, digits = 0;
    while (position < host.length && ascii_digit(host.text[position])) {
      if (++digits > 3) {
        return false;
      }
      value = value * 10 + (unsigned)(host.text[position++] - '0');
    }
    if (!digits || value > 255) {
      return false;
    }
    if (i != 3 && (position == host.length || host.text[position++] != '.')) {
      return false;
    }
  }
  return position == host.length;
}

static bool dns_host(struct url_part host)
{
  if (host.length && host.text[host.length - 1] == '.') {
    --host.length;
  }
  if (!host.length || host.length > DNS_TEXT_MAX) {
    return false;
  }
  size_t label = 0;
  for (size_t i = 0; i < host.length; ++i) {
    unsigned byte = (unsigned char)host.text[i];
    if (byte == '.') {
      if (!label || host.text[i - 1] == '-') {
        return false;
      }
      label = 0;
    } else {
      if ((!ascii_alpha(byte) && !ascii_digit(byte) && byte != '-') ||
          (!label && byte == '-') || ++label > DNS_LABEL_MAX) {
        return false;
      }
    }
  }
  return label && host.text[host.length - 1] != '-';
}

static enum call_status authority_parse(const struct url_reference *reference,
    struct url_authority *authority)
{
  if (part_equal(reference->scheme, "http")) {
    authority->scheme = HTTP_SCHEME_HTTP;
    authority->port = 80;
  } else if (part_equal(reference->scheme, "https")) {
    authority->scheme = HTTP_SCHEME_HTTPS;
    authority->port = 443;
  } else {
    return reference->scheme.text ? CALL_BAD_OPERATION : CALL_BAD_REQUEST;
  }
  struct url_part part = reference->authority;
  if (!part.text || !part.length || part.length >= sizeof(((struct http_uri *)0)->authority) ||
      memchr(part.text, '@', part.length)) {
    return CALL_BAD_REQUEST;
  }
  const char *colon = memchr(part.text, ':', part.length);
  authority->host = (struct url_part){part.text, colon ? (size_t)(colon - part.text) : part.length};
  if (!authority->host.length || authority->host.length >= sizeof(((struct http_uri *)0)->host)) {
    return CALL_BAD_REQUEST;
  }
  if (colon) {
    const char *end = part.text + part.length;
    unsigned port = 0;
    if (++colon == end) {
      return CALL_BAD_REQUEST;
    }
    while (colon < end) {
      unsigned byte = (unsigned char)*colon++;
      if (!ascii_digit(byte) || port > (UINT16_MAX - (byte - '0')) / 10) {
        return CALL_BAD_REQUEST;
      }
      port = port * 10 + byte - '0';
    }
    if (!port) {
      return CALL_BAD_REQUEST;
    }
    authority->port = port;
  }
  if (!ipv4_address(authority->host) && !dns_host(authority->host)) {
    return CALL_BAD_REQUEST;
  }
  if (authority->scheme == HTTP_SCHEME_HTTPS) {
    if (authority->host.text[authority->host.length - 1] == '.') {
      --authority->host.length;
    }
    if (ipv4_address(authority->host)) {
      return CALL_BAD_OPERATION;
    }
  }
  return CALL_OK;
}

enum call_status http_url_parse(const char *text, struct http_uri *uri)
{
  if (!uri) {
    return CALL_BAD_REQUEST;
  }
  *uri = (struct http_uri){0};
  struct url_reference reference;
  enum call_status status = reference_parse(text, &reference);
  if (status != CALL_OK) {
    return status;
  }
  struct url_authority authority;
  status = authority_parse(&reference, &authority);
  if (status != CALL_OK) {
    return status;
  }
  uri->scheme = authority.scheme;
  uri->port = authority.port;
  size_t host_length = authority.host.length;
  if (host_length && authority.host.text[host_length - 1] == '.') {
    --host_length;
  }
  for (size_t i = 0; i < host_length; ++i) {
    unsigned char byte = authority.host.text[i];
    uri->host[i] = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
  }
  memcpy(uri->authority, reference.authority.text, reference.authority.length);
  size_t offset = 0;
  if (!reference.path.length) {
    uri->target[offset++] = '/';
  }
  memcpy(uri->target + offset, reference.path.text, reference.path.length);
  offset += reference.path.length;
  if (reference.query.text) {
    uri->target[offset++] = '?';
    memcpy(uri->target + offset, reference.query.text, reference.query.length);
  }
  return CALL_OK;
}

static bool append_bytes(char *output, size_t *length, size_t maximum,
    const char *text, size_t count)
{
  if (count > maximum - *length) {
    return false;
  }
  if (count) {
    memcpy(output + *length, text, count);
  }
  *length += count;
  output[*length] = 0;
  return true;
}

static void remove_last_segment(char *path, size_t *length)
{
  while (*length && path[*length - 1] != '/') {
    --*length;
  }
  if (*length) {
    --*length;
  }
}

/* RFC 3986 section 5.2.4; output never overtakes unread input. Only literal
 * segments are removed here, so encoded request bytes retain their meaning. */
static size_t remove_dot_segments(char *path, size_t length)
{
  size_t read = 0, written = 0;
  while (read < length) {
    size_t remaining = length - read;
    if (remaining >= 3 && !memcmp(path + read, "../", 3)) {
      read += 3;
    } else if (remaining >= 2 && !memcmp(path + read, "./", 2)) {
      read += 2;
    } else if (remaining >= 3 && !memcmp(path + read, "/./", 3)) {
      read += 2;
    } else if (remaining == 2 && !memcmp(path + read, "/.", 2)) {
      read += 2;
      path[written++] = '/';
    } else if (remaining >= 4 && !memcmp(path + read, "/../", 4)) {
      read += 3;
      remove_last_segment(path, &written);
    } else if (remaining == 3 && !memcmp(path + read, "/..", 3)) {
      read += 3;
      remove_last_segment(path, &written);
      path[written++] = '/';
    } else if ((remaining == 1 && path[read] == '.') ||
        (remaining == 2 && !memcmp(path + read, "..", 2))) {
      read = length;
    } else {
      size_t end = read + (path[read] == '/');
      while (end < length && path[end] != '/') {
        ++end;
      }
      memmove(path + written, path + read, end - read);
      written += end - read;
      read = end;
    }
  }
  path[written] = 0;
  return written;
}

static bool append_part(char *output, size_t *length, size_t maximum,
    char delimiter, struct url_part part)
{
  return !part.text || (append_bytes(output, length, maximum, &delimiter, 1) &&
      append_bytes(output, length, maximum, part.text, part.length));
}

enum call_status http_url_resolve(const char *base, const char *location,
    char output[HTTP_URI_MAX + 1])
{
  if (!output) {
    return CALL_BAD_REQUEST;
  }
  output[0] = 0;
  struct url_reference current, reference;
  enum call_status status = reference_parse(base, &current);
  struct url_authority authority;
  if (status == CALL_OK) {
    status = authority_parse(&current, &authority);
  }
  if (status != CALL_OK) {
    return status;
  }
  status = reference_parse(location, &reference);
  if (status != CALL_OK || !*location) {
    return CALL_BAD_REQUEST;
  }
  struct url_reference target = reference;
  char path[HTTP_MERGED_PATH_MAX + 1];
  size_t path_length = 0;
  bool normalize_path = true;
  if (!reference.scheme.text) {
    target.scheme = current.scheme;
    if (!reference.authority.text) {
      target.authority = current.authority;
      if (!reference.path.length) {
        target.path = current.path;
        normalize_path = false;
        if (!reference.query.text) {
          target.query = current.query;
        }
      } else if (reference.path.text[0] != '/') {
        size_t prefix = current.path.length;
        while (prefix && current.path.text[prefix - 1] != '/') {
          --prefix;
        }
        if (!current.path.length) {
          append_bytes(path, &path_length, HTTP_MERGED_PATH_MAX, "/", 1);
        } else {
          append_bytes(path, &path_length, HTTP_MERGED_PATH_MAX, current.path.text, prefix);
        }
      }
    }
  }
  status = authority_parse(&target, &authority);
  if (status != CALL_OK) {
    return CALL_BAD_REQUEST;
  }
  if (!target.fragment.text) {
    target.fragment = current.fragment;
  }
  if (!append_bytes(path, &path_length, HTTP_MERGED_PATH_MAX, target.path.text, target.path.length)) {
    return CALL_FILE_TOO_LARGE;
  }
  if (normalize_path) {
    path_length = remove_dot_segments(path, path_length);
  }
  size_t length = 0;
  if (!append_bytes(output, &length, HTTP_URI_MAX, target.scheme.text, target.scheme.length) ||
      !append_bytes(output, &length, HTTP_URI_MAX, "://", 3) ||
      !append_bytes(output, &length, HTTP_URI_MAX, target.authority.text, target.authority.length) ||
      !append_bytes(output, &length, HTTP_URI_MAX, path, path_length) ||
      !append_part(output, &length, HTTP_URI_MAX, '?', target.query) ||
      !append_part(output, &length, HTTP_URI_MAX, '#', target.fragment)) {
    output[0] = 0;
    return CALL_FILE_TOO_LARGE;
  }
  return CALL_OK;
}

static bool unreserved(unsigned byte)
{
  return ascii_alpha(byte) || ascii_digit(byte) || (byte && strchr("-._~", byte));
}

static size_t normalize_escapes(struct url_part part, char *output)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t length = 0;
  for (size_t i = 0; i < part.length; ++i) {
    unsigned byte = (unsigned char)part.text[i];
    if (byte == '%') {
      byte = (unsigned)hex_value(part.text[i + 1]) * 16 + hex_value(part.text[i + 2]);
      i += 2;
      if (!unreserved(byte)) {
        output[length++] = '%';
        output[length++] = hex[byte >> 4];
        output[length++] = hex[byte & 15];
        continue;
      }
    }
    output[length++] = byte;
  }
  output[length] = 0;
  return length;
}

enum call_status http_url_key(const char *text, char output[HTTP_URL_KEY_BYTES])
{
  if (!output) {
    return CALL_BAD_REQUEST;
  }
  output[0] = 0;
  struct url_reference reference;
  enum call_status status = reference_parse(text, &reference);
  struct url_authority authority;
  if (status == CALL_OK) {
    status = authority_parse(&reference, &authority);
  }
  if (status != CALL_OK) {
    return status;
  }
  size_t length = 0;
  const size_t maximum = HTTP_URL_KEY_BYTES - 1;
  const char *scheme = authority.scheme == HTTP_SCHEME_HTTP ? "http://" : "https://";
  append_bytes(output, &length, maximum, scheme, strlen(scheme));
  size_t host_length = authority.host.length;
  if (authority.host.text[host_length - 1] == '.') {
    --host_length;
  }
  for (size_t i = 0; i < host_length; ++i) {
    output[length++] = ascii_lower((unsigned char)authority.host.text[i]);
  }
  output[length] = 0;
  unsigned default_port = authority.scheme == HTTP_SCHEME_HTTP ? 80 : 443;
  if (authority.port != default_port) {
    char port[6];
    size_t count = 0;
    unsigned value = authority.port;
    do {
      port[count++] = '0' + value % 10;
      value /= 10;
    } while (value);
    output[length++] = ':';
    while (count) {
      output[length++] = port[--count];
    }
    output[length] = 0;
  }
  char normalized[HTTP_URI_MAX + 1];
  size_t count = normalize_escapes(reference.path, normalized);
  count = remove_dot_segments(normalized, count);
  if (!count) {
    normalized[count++] = '/';
  }
  if (!append_bytes(output, &length, maximum, normalized, count)) {
    output[0] = 0;
    return CALL_FILE_TOO_LARGE;
  }
  if (reference.query.text) {
    count = normalize_escapes(reference.query, normalized);
    if (!append_bytes(output, &length, maximum, "?", 1) ||
        !append_bytes(output, &length, maximum, normalized, count)) {
      output[0] = 0;
      return CALL_FILE_TOO_LARGE;
    }
  }
  return CALL_OK;
}
