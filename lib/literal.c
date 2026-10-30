#include "literal.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH_LIMIT 32
/* Work is proportional to the source: a legitimate document costs a small
 * multiple of its size, so this only stops a bug or a hostile input that a
 * linear parser should never need. */
#define WORK_PER_BYTE 64
#define WORK_BASE (1024 * 1024)
/* literal_equal compares two parsed documents under a fixed bound. */
#define EQUAL_WORK_LIMIT (16 * 1024 * 1024)
/* A table with at most this many entries checks duplicate keys by scanning. */
#define SCAN_ENTRIES 8

struct parser {
  struct literal *document;
  size_t position, work, work_limit, comment_capacity;
  struct literal_diagnostic *diagnostic;
  bool failed;
};

/* FOUND and LIMIT name the number that overflowed and the limit it met. */
static bool fail_limit(struct parser *parser, const char *reason, size_t found, size_t limit)
{
  if (!parser->failed && parser->diagnostic) {
    parser->diagnostic->offset = parser->position;
    parser->diagnostic->reason = reason;
    parser->diagnostic->found = found;
    parser->diagnostic->limit = limit;
  }
  parser->failed = true;
  return false;
}

static bool fail(struct parser *parser, const char *reason)
{
  return fail_limit(parser, reason, 0, 0);
}

/* Memory ran out: the source size says how large the document was. */
static bool fail_memory(struct parser *parser, const char *reason)
{
  return fail_limit(parser, reason, parser->document->size, 0);
}

static bool spend(struct parser *parser, size_t count)
{
  if (count > parser->work_limit - parser->work) {
    return fail_limit(parser, "literal parsing work exceeds its bound for this source",
        parser->work + count, parser->work_limit);
  }
  parser->work += count;
  return true;
}

static char peek(const struct parser *parser)
{
  return parser->position < parser->document->size ?
      parser->document->source[parser->position] : '\0';
}

static bool space(char byte)
{
  return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
      byte == '\v' || byte == '\f';
}

static bool digit(char byte)
{
  return byte >= '0' && byte <= '9';
}

static bool identifier_start(char byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static bool identifier_part(char byte)
{
  return identifier_start(byte) || digit(byte);
}

static int hex(char byte)
{
  if (digit(byte)) return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

/* Return the end of an opening/closing long bracket, or its original offset. */
static size_t bracket(struct parser *parser, size_t start, char delimiter)
{
  const char *source = parser->document->source;
  size_t size = parser->document->size;
  if (start >= size || source[start] != delimiter) return start;
  size_t end = start + 1;
  while (end < size && source[end] == '=') ++end;
  if (!spend(parser, end - start)) return start;
  return end < size && source[end] == delimiter ? end + 1 : start;
}

static bool long_body(struct parser *parser, struct literal_span *body)
{
  size_t start = parser->position;
  size_t open_end = bracket(parser, start, '[');
  if (open_end == start) return fail(parser, "invalid long bracket");
  body->start = open_end;
  parser->position = open_end;
  while (parser->position < parser->document->size) {
    size_t close_end = bracket(parser, parser->position, ']');
    if (parser->failed) return false;
    if (close_end - parser->position == open_end - start) {
      body->end = parser->position;
      parser->position = close_end;
      return true;
    }
    ++parser->position;
  }
  return fail(parser, "unterminated long string/comment");
}

static bool comment(struct parser *parser)
{
  struct literal *document = parser->document;
  if (document->comment_count == parser->comment_capacity) {
    size_t capacity = parser->comment_capacity ? parser->comment_capacity * 2 : 16;
    void *comments = realloc(document->comments, capacity * sizeof(*document->comments));
    if (!comments) return fail_memory(parser, "cannot allocate comments");
    document->comments = comments;
    parser->comment_capacity = capacity;
  }
  struct literal_comment *item = &document->comments[document->comment_count++];
  *item = (struct literal_comment){.span.start = parser->position};
  parser->position += 2;
  if (bracket(parser, parser->position, '[') != parser->position) {
    item->long_comment = true;
    if (!long_body(parser, &item->content)) return false;
  } else {
    item->content.start = parser->position;
    while (parser->position < document->size && peek(parser) != '\n' && peek(parser) != '\r') {
      ++parser->position;
    }
    item->content.end = parser->position;
  }
  item->span.end = parser->position;
  return !parser->failed;
}

static bool skip(struct parser *parser)
{
  while (parser->position < parser->document->size) {
    if (space(peek(parser))) {
      ++parser->position;
    } else if (peek(parser) == '-' && parser->position + 1 < parser->document->size &&
        parser->document->source[parser->position + 1] == '-') {
      if (!comment(parser)) return false;
    } else {
      break;
    }
  }
  return !parser->failed;
}

static struct literal_value *new_value(struct parser *parser,
    enum literal_kind kind)
{
  struct literal_value *value = calloc(1, sizeof(*value));
  if (!value) {
    fail_memory(parser, "cannot allocate literal value");
    return NULL;
  }
  value->kind = kind;
  value->span.start = parser->position;
  return value;
}

static void free_value(struct literal_value *value)
{
  if (!value) return;
  if (value->kind == LITERAL_STRING) {
    free(value->as.string.bytes);
  } else if (value->kind == LITERAL_TABLE) {
    struct literal_entry *entry = value->as.table.entries;
    while (entry) {
      struct literal_entry *next = entry->next;
      free_value(entry->key);
      free_value(entry->value);
      free(entry);
      entry = next;
    }
  }
  free(value);
}

void literal_free(struct literal *document)
{
  if (!document) return;
  free_value(document->root);
  free(document->comments);
  free(document->source);
  free(document);
}

static bool text_value(struct parser *parser, struct literal_value *value,
    size_t start, size_t end)
{
  value->as.string.bytes = malloc(end - start + 1);
  if (!value->as.string.bytes) return fail_memory(parser, "cannot allocate string");
  memcpy(value->as.string.bytes, parser->document->source + start, end - start);
  value->as.string.bytes[end - start] = '\0';
  value->as.string.size = end - start;
  return true;
}

static void newline(struct parser *parser)
{
  char first = peek(parser);
  ++parser->position;
  if ((first == '\r' && peek(parser) == '\n') || (first == '\n' && peek(parser) == '\r')) {
    ++parser->position;
  }
}

static bool quoted(struct parser *parser, struct literal_value *value)
{
  char quote = peek(parser);
  ++parser->position;
  /* Reserve this string's raw extent, not the rest of the document per value.
   * Escaped quotes cannot terminate the string; full escape checks follow. */
  size_t end = parser->position;
  while (end < parser->document->size && parser->document->source[end] != quote) {
    if (parser->document->source[end] == '\\' && end + 1 < parser->document->size) ++end;
    ++end;
  }
  char *bytes = malloc(end - parser->position + 1);
  if (!bytes) return fail(parser, "cannot allocate string");
  value->as.string.bytes = bytes;
  size_t used = 0;
  while (parser->position < parser->document->size) {
    char byte = peek(parser);
    ++parser->position;
    if (byte == quote) {
      bytes[used] = '\0';
      value->as.string.size = used;
      return true;
    }
    if (byte == '\r' || byte == '\n') return fail(parser, "newline in quoted string");
    if (byte != '\\') {
      bytes[used++] = byte;
      continue;
    }
    if (parser->position == parser->document->size) break;
    byte = peek(parser);
    ++parser->position;
    switch (byte) {
      case 'a': bytes[used++] = '\a'; break;
      case 'b': bytes[used++] = '\b'; break;
      case 'f': bytes[used++] = '\f'; break;
      case 'n': bytes[used++] = '\n'; break;
      case 'r': bytes[used++] = '\r'; break;
      case 't': bytes[used++] = '\t'; break;
      case 'v': bytes[used++] = '\v'; break;
      case '\\': case '\'': case '"': bytes[used++] = byte; break;
      case '\r': case '\n':
        --parser->position;
        newline(parser);
        bytes[used++] = '\n';
        break;
      case 'z':
        while (space(peek(parser))) ++parser->position;
        break;
      case 'x': {
        int high = hex(peek(parser));
        if (high < 0) return fail(parser, "hex escape needs two digits");
        ++parser->position;
        int low = hex(peek(parser));
        if (low < 0) return fail(parser, "hex escape needs two digits");
        ++parser->position;
        bytes[used++] = (char)(high * 16 + low);
        break;
      }
      case 'u': {
        if (peek(parser) != '{') return fail(parser, "Unicode escape needs braces");
        ++parser->position;
        unsigned codepoint = 0;
        size_t digits = 0;
        while (hex(peek(parser)) >= 0) {
          if (codepoint > (0x7fffffffU - (unsigned)hex(peek(parser))) / 16) {
            return fail(parser, "Unicode escape is out of range");
          }
          codepoint = codepoint * 16 + (unsigned)hex(peek(parser));
          ++parser->position;
          ++digits;
        }
        if (!digits || peek(parser) != '}') return fail(parser, "invalid Unicode escape");
        ++parser->position;
        if (codepoint < 0x80) {
          bytes[used++] = (char)codepoint;
        } else {
          unsigned count = 1, remaining = codepoint >> 6;
          while (remaining >= (1U << (6 - count))) {
            ++count;
            remaining >>= 6;
          }
          bytes[used++] = (char)((0xffU << (7 - count)) | remaining);
          for (unsigned i = count; i; --i) {
            bytes[used++] = (char)(0x80 | ((codepoint >> ((i - 1) * 6)) & 0x3f));
          }
        }
        break;
      }
      default:
        if (!digit(byte)) return fail(parser, "unsupported string escape");
        unsigned number = (unsigned)(byte - '0');
        for (unsigned i = 1; i < 3 && digit(peek(parser)); ++i) {
          number = number * 10 + (unsigned)(peek(parser) - '0');
          ++parser->position;
        }
        if (number > 255) return fail(parser, "decimal escape is out of range");
        bytes[used++] = (char)number;
        break;
    }
  }
  return fail(parser, "unterminated quoted string");
}

static bool string_value(struct parser *parser, struct literal_value *value)
{
  if (peek(parser) != '[') return quoted(parser, value);
  struct literal_span body;
  if (!long_body(parser, &body) || !text_value(parser, value, body.start, body.end)) return false;
  size_t position = body.start, used = 0;
  if (position < body.end && (parser->document->source[position] == '\r' ||
      parser->document->source[position] == '\n')) {
    char first = parser->document->source[position++];
    if (position < body.end && ((first == '\r' && parser->document->source[position] == '\n') ||
        (first == '\n' && parser->document->source[position] == '\r'))) ++position;
  }
  while (position < body.end) {
    char byte = parser->document->source[position++];
    if (byte == '\r' || byte == '\n') {
      if (position < body.end && ((byte == '\r' && parser->document->source[position] == '\n') ||
          (byte == '\n' && parser->document->source[position] == '\r'))) ++position;
      byte = '\n';
    }
    value->as.string.bytes[used++] = byte;
  }
  value->as.string.bytes[used] = '\0';
  value->as.string.size = used;
  return true;
}

static bool numeric(struct parser *parser, struct literal_value *value)
{
  size_t start = parser->position;
  bool negative = peek(parser) == '-';
  if (negative) ++parser->position;
  bool hexadecimal = peek(parser) == '0' && parser->position + 1 < parser->document->size &&
      (parser->document->source[parser->position + 1] == 'x' ||
      parser->document->source[parser->position + 1] == 'X');
  if (hexadecimal) parser->position += 2;
  size_t digits = 0;
  while (hexadecimal ? hex(peek(parser)) >= 0 : digit(peek(parser))) {
    ++parser->position;
    ++digits;
  }
  bool integer = true;
  if (peek(parser) == '.') {
    integer = false;
    ++parser->position;
    while (hexadecimal ? hex(peek(parser)) >= 0 : digit(peek(parser))) {
      ++parser->position;
      ++digits;
    }
  }
  if (!digits) return fail(parser, "invalid literal number");
  if (peek(parser) == (hexadecimal ? 'p' : 'e') || peek(parser) == (hexadecimal ? 'P' : 'E')) {
    integer = false;
    ++parser->position;
    if (peek(parser) == '+' || peek(parser) == '-') ++parser->position;
    size_t exponent = parser->position;
    while (digit(peek(parser))) ++parser->position;
    if (parser->position == exponent) return fail(parser, "number exponent needs digits");
  }
  size_t length = parser->position - start;
  if (length > 128 || identifier_part(peek(parser)) || peek(parser) == '.') {
    return fail(parser, "unsupported literal number");
  }
  char token[129];
  memcpy(token, parser->document->source + start, length);
  token[length] = '\0';
  char *end;
  if (integer) {
    errno = 0;
    if (hexadecimal) {
      unsigned long long number = strtoull(token + (negative ? 1 : 0), &end, 16);
      if (!errno && !*end) {
        value->kind = LITERAL_INTEGER;
        value->as.integer = (int64_t)(negative ? 0ULL - number : number);
        return true;
      }
      return fail(parser, "hex integer is out of range");
    }
    long long number = strtoll(token, &end, 10);
    if (!errno && !*end) {
      value->kind = LITERAL_INTEGER;
      value->as.integer = number;
      return true;
    }
  }
  errno = 0;
  double number = strtod(token, &end);
  if (*end || !__builtin_isfinite(number)) return fail(parser, "number is not finite or representable");
  value->kind = LITERAL_NUMBER;
  value->as.number = number;
  return true;
}

static bool number_equal(const struct literal_value *a,
    const struct literal_value *b)
{
  if (a->kind == LITERAL_INTEGER && b->kind == LITERAL_INTEGER) {
    return a->as.integer == b->as.integer;
  }
  if (a->kind == LITERAL_NUMBER && b->kind == LITERAL_NUMBER) {
    return a->as.number == b->as.number;
  }
  const struct literal_value *integer = a->kind == LITERAL_INTEGER ? a : b;
  double number = a->kind == LITERAL_NUMBER ? a->as.number : b->as.number;
  /* Upper bound is exclusive: converting LLONG_MAX to double rounds upward. */
  return number >= (double)LLONG_MIN && number < -(double)LLONG_MIN &&
      (double)(int64_t)number == number && (int64_t)number == integer->as.integer;
}

static bool same_key(const struct literal_value *a,
    const struct literal_value *b, size_t *work)
{
  bool a_number = a->kind == LITERAL_INTEGER || a->kind == LITERAL_NUMBER;
  bool b_number = b->kind == LITERAL_INTEGER || b->kind == LITERAL_NUMBER;
  if (!*work) return false;
  --*work;
  if (a_number && b_number) return number_equal(a, b);
  if (a->kind != b->kind) return false;
  if (a->kind == LITERAL_BOOLEAN) return a->as.boolean == b->as.boolean;
  if (a->kind != LITERAL_STRING || a->as.string.size != b->as.string.size) return false;
  if (a->as.string.size > *work) {
    *work = 0;
    return false;
  }
  *work -= a->as.string.size;
  return !memcmp(a->as.string.bytes, b->as.string.bytes, a->as.string.size);
}

static bool keyword(struct parser *parser, const char *word)
{
  size_t length = strlen(word);
  if (length > parser->document->size - parser->position ||
      memcmp(parser->document->source + parser->position, word, length)) return false;
  if (parser->position + length < parser->document->size &&
      identifier_part(parser->document->source[parser->position + length])) return false;
  parser->position += length;
  return true;
}

static bool at_keyword(struct parser *parser, const char *word)
{
  size_t start = parser->position;
  bool matched = keyword(parser, word);
  parser->position = start;
  return matched;
}

static bool reserved(const char *bytes, size_t size)
{
  const char *words[] = {"and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
    "then", "true", "until", "while"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
    if (strlen(words[i]) == size && !memcmp(bytes, words[i], size)) return true;
  }
  return false;
}

static struct literal_value *parse_value(struct parser *parser, size_t depth);

/* The keys of one large table, so a duplicate is found without comparing a new
 * key with every earlier one. Open addressing; transient, freed with the table. */
struct key_set {
  struct literal_entry **slots;
  size_t capacity, count;
};

static uint64_t finish_hash(uint64_t hash)
{
  hash ^= hash >> 30;
  hash *= UINT64_C(0xbf58476d1ce4e5b9);
  hash ^= hash >> 27;
  hash *= UINT64_C(0x94d049bb133111eb);
  return hash ^ (hash >> 31);
}

/* Keys that same_key calls equal hash equal: an integer-valued number hashes
 * as the integer. */
static uint64_t key_hash(const struct literal_value *key)
{
  switch (key->kind) {
  case LITERAL_STRING: {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < key->as.string.size; ++i) {
      hash = (hash ^ (unsigned char)key->as.string.bytes[i]) * UINT64_C(1099511628211);
    }
    return finish_hash(hash);
  }
  case LITERAL_INTEGER:
    return finish_hash((uint64_t)key->as.integer);
  case LITERAL_NUMBER: {
    double number = key->as.number;
    if (number >= (double)LLONG_MIN && number < -(double)LLONG_MIN &&
        (double)(int64_t)number == number) {
      return finish_hash((uint64_t)(int64_t)number);
    }
    uint64_t bits;
    memcpy(&bits, &number, sizeof(bits));
    return finish_hash(bits);
  }
  default:
    return finish_hash(key->as.boolean ? 2 : 1);
  }
}

static bool keys_equal(const struct literal_value *a, const struct literal_value *b)
{
  size_t work = SIZE_MAX;
  return same_key(a, b, &work);
}

static size_t key_slot(const struct key_set *set, const struct literal_value *key)
{
  size_t mask = set->capacity - 1;
  size_t slot = (size_t)key_hash(key) & mask;
  while (set->slots[slot] && !keys_equal(set->slots[slot]->key, key)) {
    slot = (slot + 1) & mask;
  }
  return slot;
}

/* Adds ENTRY, whose key is not in the set. */
static bool key_set_add(struct parser *parser, struct key_set *set, struct literal_entry *entry)
{
  if ((set->count + 1) * 2 > set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 4 * SCAN_ENTRIES;
    struct literal_entry **slots = calloc(capacity, sizeof(*slots));
    if (!slots) return fail_memory(parser, "cannot allocate table key set");
    struct literal_entry **old = set->slots;
    size_t old_capacity = set->capacity;
    set->slots = slots;
    set->capacity = capacity;
    if (!spend(parser, old_capacity)) {
      free(old);
      return false;
    }
    for (size_t i = 0; i < old_capacity; ++i) {
      if (old[i]) set->slots[key_slot(set, old[i]->key)] = old[i];
    }
    free(old);
  }
  set->slots[key_slot(set, entry->key)] = entry;
  ++set->count;
  return true;
}

/* Rejects ENTRY, the newest of VALUE's entries, when an earlier one has its key. */
static bool check_duplicate(struct parser *parser, struct literal_value *value,
    struct literal_entry *entry, struct key_set *set)
{
  if (value->as.table.count <= SCAN_ENTRIES) {
    size_t remaining = parser->work_limit - parser->work;
    for (struct literal_entry *prior = value->as.table.entries; prior != entry; prior = prior->next) {
      bool duplicate = same_key(prior->key, entry->key, &remaining);
      if (!remaining) {
        return fail_limit(parser, "literal parsing work exceeds its bound for this source",
            parser->work_limit, parser->work_limit);
      }
      if (duplicate) return fail(parser, "duplicate literal table key");
    }
    parser->work = parser->work_limit - remaining;
    return true;
  }
  if (!set->slots) {
    for (struct literal_entry *prior = value->as.table.entries; prior != entry; prior = prior->next) {
      if (!key_set_add(parser, set, prior)) return false;
    }
  }
  size_t cost = 1 + (entry->key->kind == LITERAL_STRING ? entry->key->as.string.size : 0);
  if (!spend(parser, cost)) return false;
  if (set->slots[key_slot(set, entry->key)]) return fail(parser, "duplicate literal table key");
  return key_set_add(parser, set, entry);
}

static bool table_entries(struct parser *parser, struct literal_value *value, size_t depth,
    struct key_set *set)
{
  if (depth == DEPTH_LIMIT) {
    return fail_limit(parser, "literal table depth exceeds its limit", depth + 1, DEPTH_LIMIT);
  }
  ++parser->position;
  struct literal_entry **tail = &value->as.table.entries;
  int64_t array_index = 0;
  if (!skip(parser)) return false;
  while (peek(parser) != '}') {
    struct literal_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) return fail_memory(parser, "cannot allocate table entry");
    *tail = entry;
    tail = &entry->next;
    ++value->as.table.count;
    entry->span.start = parser->position;
    if (peek(parser) == '[' && bracket(parser, parser->position, '[') == parser->position) {
      ++parser->position;
      if (!skip(parser)) return false;
      entry->key = parse_value(parser, depth + 1);
      if (!entry->key) return false;
      if (entry->key->kind == LITERAL_TABLE) return fail(parser, "table keys are unsupported");
      if (!skip(parser) || peek(parser) != ']') return fail(parser, "explicit key needs closing bracket");
      ++parser->position;
      if (!skip(parser) || peek(parser) != '=') return fail(parser, "explicit key needs equals");
      ++parser->position;
    } else if (identifier_start(peek(parser)) &&
        !at_keyword(parser, "true") && !at_keyword(parser, "false")) {
      size_t start = parser->position;
      while (identifier_part(peek(parser))) ++parser->position;
      if (reserved(parser->document->source + start, parser->position - start)) {
        return fail(parser, "reserved word cannot be a field key");
      }
      entry->key = new_value(parser, LITERAL_STRING);
      if (!entry->key) return false;
      entry->key->span = (struct literal_span){start, parser->position};
      if (!text_value(parser, entry->key, start, parser->position)) return false;
      if (!skip(parser) || peek(parser) != '=') return fail(parser, "literal field needs equals");
      ++parser->position;
    } else {
      entry->implicit = true;
      entry->key = new_value(parser, LITERAL_INTEGER);
      if (!entry->key) return false;
      entry->key->span.end = entry->key->span.start;
      entry->key->as.integer = ++array_index;
    }
    if (!skip(parser)) return false;
    entry->value = parse_value(parser, depth + 1);
    if (!entry->value) return false;
    if (!check_duplicate(parser, value, entry, set)) return false;
    size_t value_end = parser->position;
    if (!skip(parser)) return false;
    if (peek(parser) == ',' || peek(parser) == ';') {
      ++parser->position;
      entry->span.end = parser->position;
      if (!skip(parser)) return false;
    } else {
      entry->span.end = value_end;
      if (peek(parser) != '}') return fail(parser, "table entry needs separator");
    }
  }
  ++parser->position;
  return true;
}

static bool table(struct parser *parser, struct literal_value *value, size_t depth)
{
  struct key_set set = {0};
  bool valid = table_entries(parser, value, depth, &set);
  free(set.slots);
  return valid;
}

static struct literal_value *parse_value(struct parser *parser, size_t depth)
{
  struct literal_value *value = new_value(parser, LITERAL_TABLE);
  if (!value) return NULL;
  bool valid = false;
  if (peek(parser) == '{') {
    valid = table(parser, value, depth);
  } else if (peek(parser) == '\'' || peek(parser) == '"' || peek(parser) == '[') {
    value->kind = LITERAL_STRING;
    valid = string_value(parser, value);
  } else if (digit(peek(parser)) || peek(parser) == '.' || peek(parser) == '-') {
    value->kind = LITERAL_NUMBER;
    valid = numeric(parser, value);
  } else if (keyword(parser, "true")) {
    value->kind = LITERAL_BOOLEAN;
    value->as.boolean = true;
    valid = true;
  } else if (keyword(parser, "false")) {
    value->kind = LITERAL_BOOLEAN;
    valid = true;
  } else {
    fail(parser, "unsupported Lua expression; expected literal value");
  }
  if (!valid) {
    free_value(value);
    return NULL;
  }
  value->span.end = parser->position;
  return value;
}

static bool parse_source(const char *bytes, size_t size, bool small,
    struct literal **output, struct literal_diagnostic *diagnostic)
{
  *output = NULL;
  if (diagnostic) *diagnostic = (struct literal_diagnostic){.line = 1, .column = 1};
  struct parser parser = {.diagnostic = diagnostic};
  if (small && size > LITERAL_SOURCE_LIMIT) {
    return fail_limit(&parser, "literal source exceeds the small-document limit", size,
        LITERAL_SOURCE_LIMIT);
  }
  if ((!bytes && size) || size == SIZE_MAX) {
    return fail(&parser, "literal source is unavailable");
  }
  parser.work_limit = size > (SIZE_MAX - WORK_BASE) / WORK_PER_BYTE ? SIZE_MAX :
      size * WORK_PER_BYTE + WORK_BASE;
  parser.document = calloc(1, sizeof(*parser.document));
  if (!parser.document) {
    parser.document = &(struct literal){.size = size};
    return fail_memory(&parser, "cannot allocate literal document");
  }
  parser.document->source = malloc(size + 1);
  if (!parser.document->source) {
    literal_free(parser.document);
    parser.document = &(struct literal){.size = size};
    return fail_memory(&parser, "cannot allocate literal source");
  }
  if (size) memcpy(parser.document->source, bytes, size);
  parser.document->source[size] = '\0';
  parser.document->size = size;
  parser.work = size * 2;
  const char *nul = memchr(parser.document->source, '\0', size);
  if (nul) {
    parser.position = (size_t)(nul - parser.document->source);
    fail(&parser, "literal source contains NUL");
  } else if (skip(&parser) && keyword(&parser, "return") && skip(&parser) && peek(&parser) == '{') {
    parser.document->root = parse_value(&parser, 0);
    if (parser.document->root && skip(&parser)) {
      if (peek(&parser) == ';') {
        ++parser.position;
        skip(&parser);
      }
      if (parser.position != size) fail(&parser, "unexpected bytes after return table");
    }
  } else {
    fail(&parser, "literal configuration must return one table");
  }
  if (parser.failed || !parser.document->root) {
    if (diagnostic) {
      for (size_t i = 0; i < diagnostic->offset && i < size; ++i) {
        if (bytes[i] == '\r' || bytes[i] == '\n') {
          char first = bytes[i];
          if (i + 1 < diagnostic->offset && ((first == '\r' && bytes[i + 1] == '\n') ||
              (first == '\n' && bytes[i + 1] == '\r'))) ++i;
          ++diagnostic->line;
          diagnostic->column = 1;
        } else {
          ++diagnostic->column;
        }
      }
    }
    literal_free(parser.document);
    return false;
  }
  *output = parser.document;
  return true;
}

bool literal_parse(const char *bytes, size_t size,
    struct literal **output, struct literal_diagnostic *diagnostic)
{
  return parse_source(bytes, size, true, output, diagnostic);
}

bool literal_parse_sized(const char *bytes, size_t size,
    struct literal **output, struct literal_diagnostic *diagnostic)
{
  return parse_source(bytes, size, false, output, diagnostic);
}

const char *literal_describe(const struct literal_diagnostic *diagnostic,
    char *text, size_t capacity)
{
  const char *reason = diagnostic->reason ? diagnostic->reason : "invalid literal data";
  if (diagnostic->limit) {
    snprintf(text, capacity, "line %zu column %zu: %s (%zu exceeds the limit of %zu)",
        diagnostic->line, diagnostic->column, reason, diagnostic->found, diagnostic->limit);
  } else if (diagnostic->found) {
    snprintf(text, capacity, "%s (source of %zu bytes)", reason, diagnostic->found);
  } else {
    snprintf(text, capacity, "line %zu column %zu: %s", diagnostic->line, diagnostic->column,
        reason);
  }
  return text;
}

const struct literal_value *literal_field(
    const struct literal_value *table_value, const char *key)
{
  if (!table_value || table_value->kind != LITERAL_TABLE) return NULL;
  size_t size = strlen(key);
  for (const struct literal_entry *entry = table_value->as.table.entries; entry; entry = entry->next) {
    if (entry->key->kind == LITERAL_STRING && entry->key->as.string.size == size &&
        !memcmp(entry->key->as.string.bytes, key, size)) return entry->value;
  }
  return NULL;
}

static bool equal(const struct literal_value *a,
    const struct literal_value *b, size_t *work, size_t depth)
{
  if (!a || !b || depth > DEPTH_LIMIT) return false;
  if (a->kind != LITERAL_TABLE || b->kind != LITERAL_TABLE) {
    return same_key(a, b, work);
  }
  if (a->as.table.count != b->as.table.count || !*work) return false;
  --*work;
  for (const struct literal_entry *entry = a->as.table.entries; entry; entry = entry->next) {
    const struct literal_entry *other = b->as.table.entries;
    while (other && !same_key(entry->key, other->key, work)) {
      if (!*work) return false;
      other = other->next;
    }
    if (!other || !equal(entry->value, other->value, work, depth + 1)) return false;
  }
  return true;
}

bool literal_equal(const struct literal_value *a,
    const struct literal_value *b)
{
  size_t work = EQUAL_WORK_LIMIT;
  return equal(a, b, &work, 0);
}

