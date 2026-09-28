#include "dns.h"
#include "udp.h"
#include <clock.h>
#include <handle.h>
#include <random.h>
#include <stdlib.h>

#define DNS_DEFAULT_SERVER "1.1.1.1"
#define IPV4_MULTICAST_BASE UINT32_C(0xe0000000)
#define DNS_PORT 53
#define DNS_ATTEMPTS 2
#define DNS_BIND_ATTEMPTS 16
#define DNS_ATTEMPT_NS UINT64_C(3000000000)
#define DNS_EPHEMERAL_FIRST 49152
#define DNS_EPHEMERAL_COUNT 16384

bool dns_select_server(const char *override, uint32_t *server)
{
  const char *text = override ? override : getenv("DNS_SERVER");
  if (!text) {
    text = DNS_DEFAULT_SERVER;
  }
  return udp_parse_address(text, server) && (*server >> 24) && *server < IPV4_MULTICAST_BASE;
}

static enum call_status check_deadline(handle_t clock, uint64_t deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  return status != CALL_OK ? status : now >= deadline ? CALL_TIMED_OUT : CALL_OK;
}

static void query_attempt(handle_t udp, handle_t clock, handle_t random, uint32_t server,
    const struct dns_name *name, uint64_t deadline_ns, struct dns_exchange *exchange)
{
  uint64_t deadline;
  exchange->operation = "clock";
  exchange->status = udp_deadline(clock, DNS_ATTEMPT_NS, &deadline);
  if (exchange->status != CALL_OK) {
    return;
  }
  if (deadline_ns && deadline_ns < deadline) {
    deadline = deadline_ns;
  }

  struct udp_open_reply endpoint;
  uint16_t identity[2];
  for (unsigned i = 0; i < DNS_BIND_ATTEMPTS; ++i) {
    exchange->operation = "random read";
    exchange->status = random_read(random, identity, sizeof(identity), deadline);
    if (exchange->status != CALL_OK) {
      return;
    }
    /* A power-of-two port range divides the uniform 16-bit input exactly. */
    uint16_t port = DNS_EPHEMERAL_FIRST + identity[1] % DNS_EPHEMERAL_COUNT;
    exchange->operation = "UDP open";
    exchange->status = udp_open_route(udp, server, port, &endpoint);
    if (exchange->status == CALL_OK) {
      break;
    }
    if (exchange->status != CALL_ALREADY_EXISTS) {
      return;
    }
  }
  if (exchange->status != CALL_OK) {
    return;
  }

  uint8_t query[DNS_MESSAGE_BYTES];
  size_t query_length = dns_make_query(name, identity[0], query);
  exchange->operation = "clock";
  exchange->status = check_deadline(clock, deadline);
  if (exchange->status == CALL_OK) {
    exchange->operation = "UDP send";
    exchange->status = udp_send(endpoint.handle, server, DNS_PORT, query, query_length, deadline);
  }
  while (exchange->status == CALL_OK) {
    /* Drain whole supported UDP datagrams, including oversized DNS replies. */
    uint8_t bytes[UDP_MAX_PAYLOAD];
    struct udp_receive_reply peer;
    exchange->operation = "UDP receive";
    exchange->status = udp_receive(endpoint.handle, bytes, sizeof(bytes), deadline, &peer);
    if (exchange->status != CALL_OK) {
      break;
    }
    if (peer.address != server || peer.port != DNS_PORT) {
      continue;
    }
    exchange->response = dns_parse_reply(bytes, peer.length, name, identity[0], &exchange->reply);
    if (exchange->response != DNS_IGNORE) {
      break;
    }
    /* Reuse the original deadline even under a stream of unwanted replies. */
  }

  /* CLOSE retires asynchronously. SHUTDOWN releases the port before a retry. */
  enum call_status stopped = udp_shutdown(endpoint.handle);
  int closed = handle_close(endpoint.handle);
  if (exchange->status == CALL_OK && (stopped != CALL_OK || closed)) {
    exchange->operation = "UDP cleanup";
    exchange->status = stopped != CALL_OK ? stopped : CALL_BAD_HANDLE;
  }
}

void dns_query(handle_t udp, handle_t clock, handle_t random, uint32_t server,
    const struct dns_name *name, uint64_t deadline_ns, struct dns_exchange *exchange)
{
  for (unsigned i = 0; i < DNS_ATTEMPTS; ++i) {
    *exchange = (struct dns_exchange){0};
    if (deadline_ns) {
      exchange->operation = "clock";
      exchange->status = check_deadline(clock, deadline_ns);
      if (exchange->status != CALL_OK) {
        return;
      }
    }
    query_attempt(udp, clock, random, server, name, deadline_ns, exchange);
    if (exchange->status != CALL_TIMED_OUT) {
      return;
    }
  }
}
