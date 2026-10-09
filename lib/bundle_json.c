#include "bundle_json.h"
#include <stdlib.h>
#include <string.h>

/* Parser depth is an implementation budget, independent of path depth. */
#define JSON_DEPTH_LIMIT 16

struct parser {
  struct bundle_json *json;
  size_t position;
};

static void whitespace(struct parser *parser)
{
  while (parser->position < parser->json->size) {
    char byte = parser->json->text[parser->position];
    if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
      break;
    }
    ++parser->position;
  }
}

static bool take(struct parser *parser, char byte)
{
  if (parser->position == parser->json->size ||
      parser->json->text[parser->position] != byte) {
    return false;
  }
  ++parser->position;
  return true;
}

static bool hex_word(struct parser *parser, uint32_t *word)
{
  *word = 0;
  for (size_t i = 0; i < 4; ++i) {
    if (parser->position == parser->json->size) {
      return false;
    }
    unsigned char byte = parser->json->text[parser->position++];
    uint32_t digit;
    if (byte >= '0' && byte <= '9') {
      digit = byte - '0';
    } else if (byte >= 'a' && byte <= 'f') {
      digit = byte - 'a' + 10;
    } else if (byte >= 'A' && byte <= 'F') {
      digit = byte - 'A' + 10;
    } else {
      return false;
    }
    *word = *word * 16 + digit;
  }
  return true;
}

static void encode_utf8(char **destination, uint32_t value)
{
  if (value < 0x80) {
    *(*destination)++ = value;
  } else if (value < 0x800) {
    *(*destination)++ = 0xc0 | (value >> 6);
    *(*destination)++ = 0x80 | (value & 0x3f);
  } else if (value < 0x10000) {
    *(*destination)++ = 0xe0 | (value >> 12);
    *(*destination)++ = 0x80 | ((value >> 6) & 0x3f);
    *(*destination)++ = 0x80 | (value & 0x3f);
  } else {
    *(*destination)++ = 0xf0 | (value >> 18);
    *(*destination)++ = 0x80 | ((value >> 12) & 0x3f);
    *(*destination)++ = 0x80 | ((value >> 6) & 0x3f);
    *(*destination)++ = 0x80 | (value & 0x3f);
  }
}

static bool string(struct parser *parser, char **value)
{
  if (!take(parser, '"')) {
    return false;
  }
  char *destination = parser->json->text + parser->position;
  *value = destination;
  while (parser->position < parser->json->size) {
    unsigned char byte = parser->json->text[parser->position++];
    if (byte == '"') {
      *destination = 0;
      return true;
    }
    if (byte < 0x20) {
      return false;
    }
    if (byte == '\\') {
      if (parser->position == parser->json->size) {
        return false;
      }
      byte = parser->json->text[parser->position++];
      switch (byte) {
      case '"': case '\\': case '/':
        *destination++ = byte;
        break;
      case 'b': *destination++ = '\b'; break;
      case 'f': *destination++ = '\f'; break;
      case 'n': *destination++ = '\n'; break;
      case 'r': *destination++ = '\r'; break;
      case 't': *destination++ = '\t'; break;
      case 'u': {
        uint32_t word;
        if (!hex_word(parser, &word) || !word) {
          return false;
        }
        if (word >= 0xd800 && word <= 0xdbff) {
          uint32_t low;
          if (!take(parser, '\\') || !take(parser, 'u') ||
              !hex_word(parser, &low) || low < 0xdc00 || low > 0xdfff) {
            return false;
          }
          word = 0x10000 + ((word - 0xd800) << 10) + low - 0xdc00;
        } else if (word >= 0xdc00 && word <= 0xdfff) {
          return false;
        }
        encode_utf8(&destination, word);
        break;
      }
      default: return false;
      }
    } else if (byte < 0x80) {
      *destination++ = byte;
    } else {
      size_t tail;
      uint32_t code, minimum;
      if (byte >= 0xc2 && byte <= 0xdf) {
        tail = 1; code = byte & 0x1f; minimum = 0x80;
      } else if (byte >= 0xe0 && byte <= 0xef) {
        tail = 2; code = byte & 0x0f; minimum = 0x800;
      } else if (byte >= 0xf0 && byte <= 0xf4) {
        tail = 3; code = byte & 7; minimum = 0x10000;
      } else {
        return false;
      }
      if (tail > parser->json->size - parser->position) {
        return false;
      }
      *destination++ = byte;
      for (size_t i = 0; i < tail; ++i) {
        byte = parser->json->text[parser->position++];
        if ((byte & 0xc0) != 0x80) {
          return false;
        }
        code = (code << 6) | (byte & 0x3f);
        *destination++ = byte;
      }
      if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        return false;
      }
    }
  }
  return false;
}

static bool value(struct parser *parser, size_t depth, size_t *index);

static bool container(struct parser *parser, size_t depth, size_t index, bool object)
{
  char end = object ? '}' : ']';
  whitespace(parser);
  if (take(parser, end)) {
    return true;
  }
  size_t *link = &parser->json->nodes[index].child;
  for (;;) {
    size_t item;
    if (object) {
      if (!value(parser, depth + 1, &item) ||
          parser->json->nodes[item].type != BUNDLE_JSON_STRING) {
        return false;
      }
      for (size_t key = parser->json->nodes[index].child; key;
          key = parser->json->nodes[parser->json->nodes[key].next].next) {
        if (!strcmp(parser->json->nodes[key].string, parser->json->nodes[item].string)) {
          return false;
        }
      }
      *link = item;
      link = &parser->json->nodes[item].next;
      whitespace(parser);
      if (!take(parser, ':')) {
        return false;
      }
    }
    if (!value(parser, depth + 1, &item)) {
      return false;
    }
    *link = item;
    link = &parser->json->nodes[item].next;
    whitespace(parser);
    if (take(parser, end)) {
      return true;
    }
    if (!take(parser, ',')) {
      return false;
    }
    whitespace(parser);
  }
}

static bool literal(struct parser *parser, const char *text)
{
  size_t size = strlen(text);
  if (size > parser->json->size - parser->position ||
      memcmp(parser->json->text + parser->position, text, size)) {
    return false;
  }
  parser->position += size;
  return true;
}

static bool value(struct parser *parser, size_t depth, size_t *index)
{
  whitespace(parser);
  if (depth > JSON_DEPTH_LIMIT || parser->position == parser->json->size ||
      parser->json->count == parser->json->capacity) {
    return false;
  }
  *index = parser->json->count++;
  struct bundle_json_node *node = &parser->json->nodes[*index];
  char byte = parser->json->text[parser->position];
  if (byte == '{' || byte == '[') {
    ++parser->position;
    node->type = byte == '{' ? BUNDLE_JSON_OBJECT : BUNDLE_JSON_ARRAY;
    return container(parser, depth, *index, byte == '{');
  }
  if (byte == '"') {
    node->type = BUNDLE_JSON_STRING;
    return string(parser, &node->string);
  }
  if (byte == 't' || byte == 'f') {
    node->type = BUNDLE_JSON_BOOLEAN;
    node->integer = byte == 't';
    return literal(parser, byte == 't' ? "true" : "false");
  }
  if (byte == 'n') {
    node->type = BUNDLE_JSON_NULL;
    return literal(parser, "null");
  }
  if (byte < '0' || byte > '9') {
    return false;
  }
  node->type = BUNDLE_JSON_INTEGER;
  bool zero = byte == '0';
  do {
    unsigned digit = byte - '0';
    if (node->integer > (UINT64_MAX - digit) / 10) {
      return false;
    }
    node->integer = node->integer * 10 + digit;
    ++parser->position;
    if (parser->position == parser->json->size) {
      break;
    }
    byte = parser->json->text[parser->position];
    if (zero && byte >= '0' && byte <= '9') {
      return false;
    }
  } while (byte >= '0' && byte <= '9');
  return true;
}

void bundle_json_close(struct bundle_json *json)
{
  free(json->nodes);
  free(json->text);
  *json = (struct bundle_json){0};
}

enum call_status bundle_json_parse(char *text, size_t size, struct bundle_json *json)
{
  *json = (struct bundle_json){.text = text, .size = size, .capacity = size + 1};
  json->nodes = calloc(json->capacity, sizeof(*json->nodes));
  if (!json->nodes) {
    bundle_json_close(json);
    return CALL_NO_MEMORY;
  }
  json->count = 1;
  struct parser parser = {.json = json};
  size_t root;
  bool valid = value(&parser, 0, &root);
  whitespace(&parser);
  if (!valid || parser.position != size) {
    bundle_json_close(json);
    return CALL_BAD_REQUEST;
  }
  return CALL_OK;
}
