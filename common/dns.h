#ifndef COMMON_DNS_H
#define COMMON_DNS_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DNS_MESSAGE_BYTES 512
#define DNS_NAME_BYTES 255
#define DNS_HEADER_BYTES 12
#define DNS_TYPE_A 1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN 1

/* Expanded wire labels, including the final zero. Keeping label lengths avoids
 * confusing a literal dot/NUL inside an untrusted label with a separator. */
struct dns_name {
  uint8_t bytes[DNS_NAME_BYTES];
  size_t length;
};

struct dns_record {
  struct dns_name owner, target;
  uint16_t type, class, data_length;
  uint32_t ttl, address;
};

struct dns_reply {
  uint8_t bytes[DNS_MESSAGE_BYTES];
  size_t length, answers_offset;
  uint16_t answer_count;
  unsigned rcode;
};

enum dns_address_result { DNS_ADDRESS_FOUND, DNS_ADDRESS_MISSING, DNS_ADDRESS_INVALID };

enum dns_response { DNS_IGNORE, DNS_COMPLETE, DNS_TRUNCATED, DNS_OVERSIZED };

bool dns_name_from_text(const char *text, struct dns_name *name);
/* name must be a successfully encoded hostname. */
size_t dns_make_query(const struct dns_name *name, uint16_t id, uint8_t bytes[DNS_MESSAGE_BYTES]);
/* IGNORE includes malformed/unrelated messages. Only COMPLETE fills reply;
 * all three record sections have then been checked, without printing data. */
enum dns_response dns_parse_reply(const uint8_t *bytes, size_t length,
    const struct dns_name *question, uint16_t id, struct dns_reply *reply);
bool dns_read_record(const uint8_t *bytes, size_t length, size_t *offset, struct dns_record *record);

/* A validated NOERROR reply; only answer-section IN records are candidates.
 * Conflicting aliases, alias/address coexistence and CNAME loops are invalid. */
enum dns_address_result dns_select_address(const struct dns_reply *reply,
    const struct dns_name *question, uint32_t *address);
const char *dns_response_status(unsigned rcode);
/* NULL override selects DNS_SERVER, or 1.1.1.1 only when it is absent. */
bool dns_select_server(const char *override, uint32_t *server);

struct dns_exchange {
  struct dns_reply reply;
  enum dns_response response;
  enum call_status status;
  const char *operation; /* Diagnostic stage on native failure. */
};

/* Borrowed authorities; owns and closes each attempt's endpoint. A nonzero
 * absolute deadline caps both attempts; zero allows three seconds per attempt. */
void dns_query(handle_t udp, handle_t clock, handle_t random, uint32_t server,
    const struct dns_name *name, uint64_t deadline_ns, struct dns_exchange *exchange);

/* Resolve one hostname, reporting failures to stderr with the program label.
 * Uses the startup UDP/random authorities and configured DNS server. */
bool dns_resolve_address(const char *program, const char *target,
    handle_t clock, uint32_t *address);

#endif
