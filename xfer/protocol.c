#include "protocol.h"
#include <stdio.h>
#include <string.h>

static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool wire_fail(struct wire *wire, const char *message)
{
  if (!wire->error) {
    wire->error = message;
  }
  return false;
}

bool base64_encode(const void *input, size_t size, char *output, size_t capacity)
{
  if (size > (SIZE_MAX - 2) / 4 || (size + 2) / 3 * 4 >= capacity) {
    return false;
  }
  const unsigned char *bytes = input;
  size_t cursor = 0;
  for (size_t offset = 0; offset < size; offset += 3) {
    size_t remaining = size - offset;
    unsigned value = (unsigned)bytes[offset] << 16;
    if (remaining > 1) {
      value |= (unsigned)bytes[offset + 1] << 8;
    }
    if (remaining > 2) {
      value |= bytes[offset + 2];
    }
    output[cursor++] = alphabet[value >> 18];
    output[cursor++] = alphabet[(value >> 12) & 63];
    output[cursor++] = remaining > 1 ? alphabet[(value >> 6) & 63] : '=';
    output[cursor++] = remaining > 2 ? alphabet[value & 63] : '=';
  }
  output[cursor] = 0;
  return true;
}

static int base64_digit(unsigned char byte)
{
  if (byte >= 'A' && byte <= 'Z') {
    return byte - 'A';
  }
  if (byte >= 'a' && byte <= 'z') {
    return byte - 'a' + 26;
  }
  if (byte >= '0' && byte <= '9') {
    return byte - '0' + 52;
  }
  return byte == '+' ? 62 : byte == '/' ? 63 : -1;
}

bool base64_decode(const char *input, void *output, size_t capacity, size_t *size)
{
  *size = 0;
  if (!input) {
    return false;
  }
  size_t length = strlen(input);
  if (length % 4) {
    return false;
  }
  unsigned char *bytes = output;
  for (size_t offset = 0; offset < length; offset += 4) {
    int a = base64_digit(input[offset]);
    int b = base64_digit(input[offset + 1]);
    int c = input[offset + 2] == '=' ? 0 : base64_digit(input[offset + 2]);
    int d = input[offset + 3] == '=' ? 0 : base64_digit(input[offset + 3]);
    bool last = offset + 4 == length;
    size_t count = input[offset + 2] == '=' ? 1 : input[offset + 3] == '=' ? 2 : 3;
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (count != 3 && !last) || (count == 1 && input[offset + 3] != '=') ||
        (count == 1 && (b & 15)) || (count == 2 && (c & 3)) ||
        count > capacity - *size) {
      return false;
    }
    unsigned value = (unsigned)a << 18 | (unsigned)b << 12 | (unsigned)c << 6 | (unsigned)d;
    bytes[(*size)++] = value >> 16;
    if (count > 1) {
      bytes[(*size)++] = value >> 8;
    }
    if (count > 2) {
      bytes[(*size)++] = value;
    }
  }
  return true;
}

bool decimal_size(const char *input, size_t *size)
{
  *size = 0;
  if (!input || !*input) {
    return false;
  }
  for (; *input; input++) {
    if (*input < '0' || *input > '9' || *size > (SIZE_MAX - (*input - '0')) / 10) {
      return false;
    }
    *size = *size * 10 + (*input - '0');
  }
  return true;
}

bool protocol_id(const char *input)
{
  if (!input || !*input || strlen(input) > 63) {
    return false;
  }
  for (; *input; input++) {
    if (!((*input >= 'a' && *input <= 'z') || (*input >= 'A' && *input <= 'Z') ||
          (*input >= '0' && *input <= '9') || *input == '_' || *input == '-')) {
      return false;
    }
  }
  return true;
}

bool utf8_text(const char *input)
{
  const unsigned char *bytes = (const unsigned char *)input;
  while (*bytes) {
    if (*bytes < 0x80) {
      bytes++;
      continue;
    }
    unsigned first = *bytes++;
    unsigned count, value, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1; value = first & 31; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2; value = first & 15; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3; value = first & 7; minimum = 0x10000;
    } else {
      return false;
    }
    while (count--) {
      unsigned next = *bytes++;
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      value = value << 6 | (next & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

bool wire_send(struct wire *wire, const char *action, const char *fields)
{
  char frame[XFER_FRAME_MAX + 1];
  int length = snprintf(frame, sizeof(frame), "\033]5113;ac=%s;id=%s%s%s\033\\",
      action, wire->id, *fields ? ";" : "", fields);
  if (length < 0 || (size_t)length > XFER_FRAME_MAX) {
    return wire_fail(wire, "Transfer frame exceeds the 4096-byte limit");
  }
  if (term_write_all(&wire->terminal, frame, length) != CALL_OK) {
    return wire_fail(wire, "Terminal output failed");
  }
  return true;
}

bool wire_status(struct wire *wire, const char *status, const char *fid, size_t size)
{
  char encoded[256], fields[384];
  if (!base64_encode(status, strlen(status), encoded, sizeof(encoded))) {
    return wire_fail(wire, "Status exceeds transfer limit");
  }
  snprintf(fields, sizeof(fields), "st=%s%s%s;sz=%zu", encoded,
      fid ? ";fid=" : "", fid ? fid : "", size);
  return wire_send(wire, "status", fields);
}

static bool input_byte(struct wire *wire, uint64_t deadline, unsigned char *byte, bool draining)
{
  if (wire->input_offset == wire->input_size) {
    uint64_t now;
    if (clock_now(wire->clock, &now) != CALL_OK) {
      return wire_fail(wire, "Monotonic clock unavailable");
    }
    if (now >= deadline) {
      return wire_fail(wire, "Timed out waiting for the transfer peer");
    }
    uint64_t milliseconds = (deadline - now + 999999) / 1000000;
    if (milliseconds > UINT32_MAX) {
      milliseconds = UINT32_MAX;
    }
    size_t count;
    enum call_status status = term_read_timeout(&wire->terminal, wire->input,
        wire->buffered ? sizeof(wire->input) : 1, milliseconds, &count);
    if (status != CALL_OK || !count) {
      return wire_fail(wire, status == CALL_TIMED_OUT ?
          "Timed out waiting for the transfer peer" : "Terminal input failed or closed");
    }
    wire->input_offset = 0;
    wire->input_size = count;
  }
  *byte = wire->input[wire->input_offset++];
  if (*byte == 3 && !draining) {
    return wire_fail(wire, "Canceled by Ctrl+C");
  }
  return true;
}

static bool parse_packet(struct wire *wire, struct packet *packet)
{
  char *field = packet->body;
  while (*field) {
    char *separator = strchr(field, ';');
    if (separator) {
      *separator = 0;
    }
    char *value = strchr(field, '=');
    if (!value || value == field) {
      return wire_fail(wire, "Malformed transfer field");
    }
    *value++ = 0;
    for (const char *key = field; *key; key++) {
      if (!((*key >= 'a' && *key <= 'z') || (*key >= '0' && *key <= '9') || *key == '_')) {
        return wire_fail(wire, "Malformed transfer key");
      }
    }
    const char **destination = NULL;
    if (!strcmp(field, "ac")) destination = &packet->action;
    else if (!strcmp(field, "id")) destination = &packet->id;
    else if (!strcmp(field, "fid")) destination = &packet->fid;
    else if (!strcmp(field, "n")) destination = &packet->name;
    else if (!strcmp(field, "st")) destination = &packet->status;
    else if (!strcmp(field, "sz")) destination = &packet->size;
    else if (!strcmp(field, "d")) destination = &packet->data;
    else if (!strcmp(field, "ft")) destination = &packet->file_type;
    else if (!strcmp(field, "tt")) destination = &packet->transfer_type;
    else if (!strcmp(field, "zip")) destination = &packet->compression;
    else if (!strcmp(field, "sha256")) destination = &packet->sha256;
    else if (!strcmp(field, "px_sha256")) destination = &packet->extension;
    if (destination) {
      if (*destination) {
        return wire_fail(wire, "Duplicate transfer field");
      }
      *destination = value;
    }
    if (!separator) {
      break;
    }
    field = separator + 1;
    if (!*field) {
      return wire_fail(wire, "Empty transfer field");
    }
  }
  if (!packet->action || !protocol_id(packet->id) ||
      (packet->fid && !protocol_id(packet->fid))) {
    return wire_fail(wire, "Missing or invalid transfer identifiers");
  }
  return true;
}

static bool next_until(struct wire *wire, struct packet *packet, uint64_t deadline, bool draining)
{
  static const unsigned char prefix[] = "\033]5113;";
  for (;;) {
    *packet = (struct packet){0};
    size_t matched = 0;
    unsigned char byte;
    while (matched < sizeof(prefix) - 1) {
      if (!input_byte(wire, deadline, &byte, draining)) {
        return false;
      }
      matched = byte == prefix[matched] ? matched + 1 : byte == 27 ? 1 : 0;
    }
    size_t length = 0;
    for (;;) {
      if (!input_byte(wire, deadline, &byte, draining)) {
        return false;
      }
      if (byte == 7) {
        break;
      }
      if (byte == 27) {
        if (!input_byte(wire, deadline, &byte, draining)) {
          return false;
        }
        if (byte != '\\') {
          return wire_fail(wire, "Malformed transfer terminator");
        }
        break;
      }
      /* Count the OSC prefix and two-byte ST in the encoded frame limit. */
      if (byte < 32 || byte > 126 || length >= XFER_FRAME_MAX - sizeof(prefix) - 1) {
        return wire_fail(wire, "Invalid or oversized transfer frame");
      }
      packet->body[length++] = byte;
    }
    packet->body[length] = 0;
    if (!parse_packet(wire, packet)) {
      return false;
    }
    if (strcmp(packet->id, wire->id)) {
      continue;
    }
    if (!draining && !strcmp(packet->action, "cancel")) {
      wire->peer_cancelled = true;
      wire_status(wire, "CANCELED", NULL, 0);
      return wire_fail(wire, "Canceled by the transfer peer");
    }
    return true;
  }
}

bool wire_next(struct wire *wire, struct packet *packet)
{
  uint64_t now;
  if (clock_now(wire->clock, &now) != CALL_OK || now > UINT64_MAX - XFER_IDLE_NS) {
    return wire_fail(wire, "Monotonic clock unavailable");
  }
  return next_until(wire, packet, now + XFER_IDLE_NS, false);
}

bool wire_poll_cancel(struct wire *wire)
{
  /* Publication performs synchronous native I/O. Poll between writes, retaining
   * incomplete OSC bytes so cancellation can be recognized at the next poll. */
  size_t pending = wire->input_size - wire->input_offset;
  memmove(wire->input, wire->input + wire->input_offset, pending);
  wire->input_offset = 0;
  wire->input_size = pending;
  size_t count = 0;
  if (pending < sizeof(wire->input)) {
    enum call_status status = term_read_timeout(&wire->terminal, wire->input + pending,
        sizeof(wire->input) - pending, 0, &count);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      return wire_fail(wire, "Terminal input failed while preparing publication");
    }
    if (status == CALL_OK && !count) {
      return wire_fail(wire, "Terminal input closed while preparing publication");
    }
    wire->input_size += count;
  }
  if (memchr(wire->input, 3, wire->input_size)) {
    return wire_fail(wire, "Canceled by Ctrl+C");
  }
  static const char prefix[] = "\033]5113;";
  size_t start = 0;
  for (; start + sizeof(prefix) - 1 <= wire->input_size; start++) {
    if (!memcmp(wire->input + start, prefix, sizeof(prefix) - 1)) {
      break;
    }
  }
  if (start + sizeof(prefix) - 1 > wire->input_size) {
    /* Preserve a possible partial prefix, discarding ordinary terminal bytes. */
    size_t keep = wire->input_size < sizeof(prefix) - 2 ? wire->input_size : sizeof(prefix) - 2;
    wire->input_offset = wire->input_size - keep;
    return true;
  }
  wire->input_offset = start;
  size_t body_start = start + sizeof(prefix) - 1;
  for (size_t end = body_start; end < wire->input_size; end++) {
    bool terminated = wire->input[end] == 7 || (wire->input[end] == 27 &&
        end + 1 < wire->input_size && wire->input[end + 1] == '\\');
    if (!terminated) {
      continue;
    }
    struct packet packet = {0};
    size_t length = end - body_start;
    memcpy(packet.body, wire->input + body_start, length);
    packet.body[length] = 0;
    if (memchr(packet.body, 0, length) || !parse_packet(wire, &packet)) {
      return wire_fail(wire, "Malformed transfer frame during publication");
    }
    wire->input_offset = end + (wire->input[end] == 7 ? 1 : 2);
    if (!strcmp(packet.id, wire->id) && !strcmp(packet.action, "cancel")) {
      wire->peer_cancelled = true;
      wire->buffered = false;
      wire_status(wire, "CANCELED", NULL, 0);
      return wire_fail(wire, "Canceled by the transfer peer");
    }
    return wire_fail(wire, "Unexpected transfer frame during publication");
  }
  if (wire->input_size == sizeof(wire->input)) {
    return wire_fail(wire, "Oversized transfer frame during publication");
  }
  return true;
}

static bool status_text(const struct packet *packet, char *text, size_t capacity)
{
  size_t length;
  if (!base64_decode(packet->status, text, capacity - 1, &length) ||
      memchr(text, 0, length)) {
    return false;
  }
  text[length] = 0;
  return true;
}

bool wire_expect(struct wire *wire, const char *status, const char *fid,
    size_t size, bool extension)
{
  struct packet packet;
  char text[192];
  if (!wire_next(wire, &packet)) {
    return false;
  }
  if (strcmp(packet.action, "status") || !status_text(&packet, text, sizeof(text))) {
    return wire_fail(wire, "Expected a transfer status reply");
  }
  if (strcmp(text, status)) {
    /* Status text is untrusted: display printable bytes only, outside OSC. */
    for (char *cursor = text; *cursor; cursor++) {
      if ((unsigned char)*cursor < 32 || (unsigned char)*cursor > 126) {
        *cursor = '?';
      }
    }
    char diagnostic[256];
    snprintf(diagnostic, sizeof(diagnostic), "xfer: peer: %s\n", text);
    term_print(&wire->terminal, diagnostic);
    return wire_fail(wire, "Transfer peer refused or failed the request");
  }
  if ((fid && (!packet.fid || strcmp(packet.fid, fid))) || (!fid && packet.fid)) {
    return wire_fail(wire, "Unexpected file identifier in status reply");
  }
  if (extension && (!packet.extension || strcmp(packet.extension, "1"))) {
    return wire_fail(wire, "Peer lacks the mandatory SHA-256 extension");
  }
  if (!strcmp(status, "PROGRESS")) {
    size_t progress;
    if (!decimal_size(packet.size, &progress) || progress != size) {
      return wire_fail(wire, "Unexpected transfer progress");
    }
  }
  return true;
}

void wire_cancel(struct wire *wire)
{
  if (!wire->active || wire->peer_cancelled) {
    return;
  }
  /* The peer waits for cancel before acknowledging. Exact reads preserve input
   * arriving after that final acknowledgement for the next shell reader. */
  wire->buffered = false;
  if (!wire_send(wire, "cancel", "")) {
    return;
  }
  uint64_t now;
  if (clock_now(wire->clock, &now) != CALL_OK || now > UINT64_MAX - XFER_CANCEL_NS) {
    return;
  }
  struct packet packet;
  while (next_until(wire, &packet, now + XFER_CANCEL_NS, true)) {
    char text[192];
    if (!strcmp(packet.action, "cancel")) {
      wire_status(wire, "CANCELED", NULL, 0);
    } else if (!strcmp(packet.action, "status") && !packet.fid &&
        status_text(&packet, text, sizeof(text)) && !strcmp(text, "CANCELED")) {
      wire->active = false;
      return;
    }
  }
  term_print(&wire->terminal, "xfer: cancellation acknowledgement unavailable\n");
}
