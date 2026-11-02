#ifndef COMMON_DNS_H
#define COMMON_DNS_H

#include <abi/handle.h>
#include <abi/endpoint.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Queries are well under a classic 512-byte datagram. They carry an EDNS(0)
 * OPT record advertising DNS_EDNS_PAYLOAD (RFC 6891, the 1232-byte guideline
 * that avoids IP fragmentation), so replies may be that large. */
#define DNS_QUERY_BYTES 512
#define DNS_EDNS_PAYLOAD 1232
#define DNS_REPLY_BYTES DNS_EDNS_PAYLOAD
/* A truncated UDP answer is fetched again over TCP, where it may be larger. */
#define DNS_TCP_REPLY_BYTES 4096
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
  uint8_t bytes[DNS_TCP_REPLY_BYTES];
  size_t length, answers_offset;
  uint16_t answer_count;
  unsigned rcode;
};

enum dns_address_result { DNS_ADDRESS_FOUND, DNS_ADDRESS_MISSING, DNS_ADDRESS_INVALID };

enum dns_response { DNS_IGNORE, DNS_COMPLETE, DNS_TRUNCATED, DNS_OVERSIZED };

bool dns_name_from_text(const char *text, struct dns_name *name);
/* name must be a successfully encoded hostname. With edns, an OPT record
 * advertising DNS_EDNS_PAYLOAD follows the question. */
size_t dns_make_query(const struct dns_name *name, uint16_t id, bool edns,
    uint8_t bytes[DNS_QUERY_BYTES]);
/* A server that does not understand EDNS answers FORMERR or NOTIMP; the query
 * is then repeated without the OPT record (RFC 6891 section 7). */
bool dns_edns_refused(unsigned rcode);
/* IGNORE includes malformed/unrelated messages. Only COMPLETE fills reply;
 * all three record sections have then been checked, without printing data.
 * A message longer than limit (DNS_REPLY_BYTES for UDP, DNS_TCP_REPLY_BYTES
 * for TCP) is OVERSIZED. */
enum dns_response dns_parse_reply(const uint8_t *bytes, size_t length,
    const struct dns_name *question, uint16_t id, size_t limit, struct dns_reply *reply);
bool dns_read_record(const uint8_t *bytes, size_t length, size_t *offset, struct dns_record *record);

/* A validated NOERROR reply; only answer-section IN records are candidates.
 * Conflicting aliases, alias/address coexistence and CNAME loops are invalid. */
enum dns_address_result dns_select_address(const struct dns_reply *reply,
    const struct dns_name *question, uint32_t *address);
const char *dns_response_status(unsigned rcode);
/* NULL override selects the declared startup DNS_SERVER; absent is unavailable. */
bool dns_select_server(const char *override, uint32_t *server);

struct dns_exchange {
  struct dns_reply reply;
  enum dns_response response;
  enum call_status status;
  enum call_status cleanup_status; /* First endpoint cleanup failure, independent of status. */
  const char *operation; /* Diagnostic stage on native failure. */
};

/* Borrowed authorities; owns and closes each attempt's endpoint. A nonzero
 * absolute deadline caps both attempts; zero allows three seconds per attempt.
 * tcp is the optional dns_tcp factory (HANDLE_INVALID when absent): a truncated
 * UDP answer is then asked again over one TCP connection to the same server. */
void dns_query(handle_t udp, handle_t tcp, handle_t clock, handle_t random, uint32_t server,
    const struct dns_name *name, uint64_t deadline_ns, struct dns_exchange *exchange);

enum dns_query_step { DNS_QUERY_PROGRESS, DNS_QUERY_WAIT, DNS_QUERY_DONE };
struct dns_query_state {
  handle_t udp, clock, random, receiver, endpoint;
  uint64_t *next_id, deadline, attempt_deadline, pending;
  uint32_t server;
  struct dns_name name;
  struct dns_exchange exchange;
  uint16_t identity;
  unsigned attempt, stage;
  bool aborted, plain; /* plain: repeat without EDNS after FORMERR or NOTIMP. */
};
void dns_query_begin(struct dns_query_state *query, handle_t udp, handle_t clock,
    handle_t random, uint32_t server, const struct dns_name *name,
    uint64_t deadline, handle_t receiver, uint64_t *next_id);
enum dns_query_step dns_query_step(struct dns_query_state *query);
bool dns_query_completion(struct dns_query_state *query, const struct endpoint_packet *packet);
enum call_status dns_query_abort(struct dns_query_state *query);

/* Resolve one hostname, reporting failures to stderr with the program label.
 * Uses the startup UDP/random authorities and configured DNS server. */
bool dns_resolve_address(const char *program, const char *target,
    handle_t clock, uint32_t *address);

#endif
