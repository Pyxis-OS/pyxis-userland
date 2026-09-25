#include "dns.h"
#include <string.h>

#define DNS_LABEL_MAX 63
#define DNS_POINTER_TAG 0xc0
#define DNS_LABEL_TAG_MASK 0xc0
#define DNS_POINTER_HIGH_MASK 0x3f
#define DNS_FLAG_RESPONSE 0x8000
#define DNS_OPCODE_MASK 0x7800
#define DNS_FLAG_TRUNCATED 0x0200
#define DNS_FLAG_RECURSION_DESIRED 0x0100
#define DNS_FLAG_RESERVED 0x0040
#define DNS_RCODE_MASK 0x000f
#define DNS_QUESTION_TAIL_BYTES 4
#define DNS_RECORD_TAIL_BYTES 10

enum header_offset { HEADER_ID, HEADER_FLAGS = 2, HEADER_QUESTIONS = 4,
                     HEADER_ANSWERS = 6, HEADER_AUTHORITY = 8, HEADER_ADDITIONAL = 10 };

static uint16_t read_u16(const uint8_t *bytes)
{
  return (uint16_t)bytes[0] << 8 | bytes[1];
}

static uint32_t read_u32(const uint8_t *bytes)
{
  return (uint32_t)read_u16(bytes) << 16 | read_u16(bytes + 2);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
  bytes[0] = value >> 8;
  bytes[1] = value;
}

static unsigned ascii_lower(unsigned byte)
{
  return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

bool dns_name_from_text(const char *text, struct dns_name *name)
{
  *name = (struct dns_name){0};
  if (!*text) {
    return false;
  }
  while (*text) {
    size_t start = name->length++;
    size_t count = 0;
    while (*text && *text != '.') {
      unsigned byte = (unsigned char)*text++;
      bool alnum = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9');
      if ((!alnum && byte != '-') || (!count && byte == '-') ||
          count == DNS_LABEL_MAX || name->length >= DNS_NAME_BYTES - 1) {
        return false;
      }
      name->bytes[name->length++] = ascii_lower(byte);
      ++count;
    }
    if (!count || name->bytes[name->length - 1] == '-') {
      return false;
    }
    name->bytes[start] = count;
    if (*text == '.') {
      ++text;
    }
  }
  name->bytes[name->length++] = 0;
  return true;
}

size_t dns_make_query(const struct dns_name *name, uint16_t id, uint8_t bytes[DNS_MESSAGE_BYTES])
{
  memset(bytes, 0, DNS_HEADER_BYTES);
  write_u16(bytes + HEADER_ID, id);
  write_u16(bytes + HEADER_FLAGS, DNS_FLAG_RECURSION_DESIRED);
  write_u16(bytes + HEADER_QUESTIONS, 1);
  memcpy(bytes + DNS_HEADER_BYTES, name->bytes, name->length);
  size_t offset = DNS_HEADER_BYTES + name->length;
  write_u16(bytes + offset, DNS_TYPE_A);
  write_u16(bytes + offset + 2, DNS_CLASS_IN);
  return offset + DNS_QUESTION_TAIL_BYTES;
}

static bool read_name(const uint8_t *bytes, size_t length, size_t *offset, struct dns_name *name)
{
  *name = (struct dns_name){0};
  size_t position = *offset;
  bool jumped = false;
  /* Backward pointers can still loop through literal labels. Bound traversal
   * independently of expanded length, including pointer-only chains. */
  for (size_t steps = 0; steps < DNS_MESSAGE_BYTES; ++steps) {
    if (position >= length) {
      return false;
    }
    unsigned count = bytes[position];
    if ((count & DNS_LABEL_TAG_MASK) == DNS_POINTER_TAG) {
      if (length - position < 2) {
        return false;
      }
      size_t target = ((count & DNS_POINTER_HIGH_MASK) << 8) | bytes[position + 1];
      if (target < DNS_HEADER_BYTES || target >= position) {
        return false;
      }
      if (!jumped) {
        *offset = position + 2;
        jumped = true;
      }
      position = target;
      continue;
    }
    if (count & DNS_LABEL_TAG_MASK) {
      return false;
    }
    ++position;
    if (!count) {
      name->bytes[name->length++] = 0;
      if (!jumped) {
        *offset = position;
      }
      return true;
    }
    if (count > length - position || name->length + count + 1 >= DNS_NAME_BYTES) {
      return false;
    }
    name->bytes[name->length++] = count;
    memcpy(name->bytes + name->length, bytes + position, count);
    name->length += count;
    position += count;
  }
  return false;
}

static bool same_name(const struct dns_name *a, const struct dns_name *b)
{
  if (a->length != b->length) {
    return false;
  }
  for (size_t i = 0; i < a->length; ++i) {
    /* Label lengths are <=63, so ASCII folding only changes label data. */
    if (ascii_lower(a->bytes[i]) != ascii_lower(b->bytes[i])) {
      return false;
    }
  }
  return true;
}

bool dns_read_record(const uint8_t *bytes, size_t length, size_t *offset, struct dns_record *record)
{
  *record = (struct dns_record){0};
  if (!read_name(bytes, length, offset, &record->owner) || length - *offset < DNS_RECORD_TAIL_BYTES) {
    return false;
  }
  const uint8_t *fields = bytes + *offset;
  record->type = read_u16(fields);
  record->class = read_u16(fields + 2);
  record->ttl = read_u32(fields + 4);
  record->data_length = read_u16(fields + 8);
  *offset += DNS_RECORD_TAIL_BYTES;
  if (record->data_length > length - *offset) {
    return false;
  }
  size_t end = *offset + record->data_length;
  if (record->type == DNS_TYPE_A && record->class == DNS_CLASS_IN) {
    if (record->data_length != 4) {
      return false;
    }
    record->address = read_u32(bytes + *offset);
  } else if (record->type == DNS_TYPE_CNAME) {
    size_t target_offset = *offset;
    if (!read_name(bytes, length, &target_offset, &record->target) || target_offset != end) {
      return false;
    }
  }
  *offset = end;
  return true;
}

enum dns_response dns_parse_reply(const uint8_t *bytes, size_t length,
    const struct dns_name *question, uint16_t id, struct dns_reply *reply)
{
  if (length < DNS_HEADER_BYTES || read_u16(bytes + HEADER_ID) != id) {
    return DNS_IGNORE;
  }
  unsigned flags = read_u16(bytes + HEADER_FLAGS);
  if (!(flags & DNS_FLAG_RESPONSE) || (flags & (DNS_OPCODE_MASK | DNS_FLAG_RESERVED)) ||
      read_u16(bytes + HEADER_QUESTIONS) != 1) {
    return DNS_IGNORE;
  }
  /* The question fits the classic limit even when an oversized datagram was
   * received. Check its matching fields before reporting a limitation. */
  size_t bounded = length < DNS_MESSAGE_BYTES ? length : DNS_MESSAGE_BYTES;
  size_t offset = DNS_HEADER_BYTES;
  struct dns_name echoed;
  if (!read_name(bytes, bounded, &offset, &echoed) || !same_name(question, &echoed) ||
      bounded - offset < DNS_QUESTION_TAIL_BYTES ||
      read_u16(bytes + offset) != DNS_TYPE_A || read_u16(bytes + offset + 2) != DNS_CLASS_IN) {
    return DNS_IGNORE;
  }
  offset += DNS_QUESTION_TAIL_BYTES;
  if (flags & DNS_FLAG_TRUNCATED) {
    return DNS_TRUNCATED;
  }
  if (length > DNS_MESSAGE_BYTES) {
    return DNS_OVERSIZED;
  }

  size_t answers_offset = offset;
  unsigned answers = read_u16(bytes + HEADER_ANSWERS);
  unsigned records = answers + read_u16(bytes + HEADER_AUTHORITY) + read_u16(bytes + HEADER_ADDITIONAL);
  /* Every RR needs at least a root name and ten fixed bytes. */
  if (records > (length - offset) / (1 + DNS_RECORD_TAIL_BYTES)) {
    return DNS_IGNORE;
  }
  for (unsigned i = 0; i < records; ++i) {
    struct dns_record record;
    if (!dns_read_record(bytes, length, &offset, &record)) {
      return DNS_IGNORE;
    }
  }
  if (offset != length) {
    return DNS_IGNORE;
  }
  *reply = (struct dns_reply){
    .length = length, .answers_offset = answers_offset, .answer_count = answers,
    .rcode = flags & DNS_RCODE_MASK,
  };
  memcpy(reply->bytes, bytes, length);
  return DNS_COMPLETE;
}

const char *dns_response_status(unsigned rcode)
{
  switch (rcode) {
  case 0: return "NOERROR";
  case 1: return "FORMERR";
  case 2: return "SERVFAIL";
  case 3: return "NXDOMAIN";
  case 4: return "NOTIMP";
  case 5: return "REFUSED";
  default: return "UNKNOWN";
  }
}

enum dns_address_result dns_select_address(const struct dns_reply *reply,
    const struct dns_name *question, uint32_t *address)
{
  struct dns_name current = *question;
  /* Every distinct hop needs an answer record. Exceeding this bound means a
   * cycle; rescanning keeps selection independent of answer ordering. */
  for (unsigned hops = 0; hops <= reply->answer_count; ++hops) {
    size_t offset = reply->answers_offset;
    struct dns_name next;
    bool found_alias = false, found_address = false;
    uint32_t candidate = 0;
    for (unsigned i = 0; i < reply->answer_count; ++i) {
      struct dns_record record;
      if (!dns_read_record(reply->bytes, reply->length, &offset, &record)) {
        return DNS_ADDRESS_INVALID;
      }
      if (record.class != DNS_CLASS_IN || !same_name(&current, &record.owner)) {
        continue;
      }
      if (record.type == DNS_TYPE_CNAME) {
        if (found_alias && !same_name(&next, &record.target)) {
          return DNS_ADDRESS_INVALID;
        }
        next = record.target;
        found_alias = true;
      } else if (record.type == DNS_TYPE_A && !found_address) {
        candidate = record.address;
        found_address = true;
      }
    }

    if (found_alias && found_address) {
      return DNS_ADDRESS_INVALID;
    }
    if (found_address) {
      *address = candidate;
      return DNS_ADDRESS_FOUND;
    }
    if (!found_alias) {
      return DNS_ADDRESS_MISSING;
    }
    current = next;
  }
  return DNS_ADDRESS_INVALID;
}
