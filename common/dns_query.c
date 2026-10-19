#include "dns.h"
#include "udp.h"
#include <completion.h>
#include <string.h>
#include <clock.h>
#include <handle.h>
#include <random.h>
#include <stdlib.h>
#include <pyxis/environment.h>

#define IPV4_MULTICAST_BASE UINT32_C(0xe0000000)
#define DNS_PORT 53
#define DNS_ATTEMPTS 2
#define DNS_BIND_ATTEMPTS 16
#define DNS_ATTEMPT_NS UINT64_C(3000000000)
#define DNS_EPHEMERAL_FIRST 49152
#define DNS_EPHEMERAL_COUNT 16384

bool dns_select_server(const char *override, uint32_t *server)
{
  const char *text = override;
  if (!text) {
    enum call_status status = pyxis_environment_get("DNS_SERVER", &text);
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      return false;
    }
  }
  if (!text) {
    return false;
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
  enum call_status cleanup = stopped != CALL_OK ? stopped : closed ? CALL_BAD_HANDLE : CALL_OK;
  if (exchange->cleanup_status == CALL_OK) {
    exchange->cleanup_status = cleanup;
  }
  if (exchange->status == CALL_OK && cleanup != CALL_OK) {
    exchange->operation = "UDP cleanup";
    exchange->status = cleanup;
  }
}

void dns_query(handle_t udp, handle_t clock, handle_t random, uint32_t server,
    const struct dns_name *name, uint64_t deadline_ns, struct dns_exchange *exchange)
{
  enum call_status cleanup = CALL_OK;
  for (unsigned i = 0; i < DNS_ATTEMPTS; ++i) {
    *exchange = (struct dns_exchange){.cleanup_status = cleanup};
    if (deadline_ns) {
      exchange->operation = "clock";
      exchange->status = check_deadline(clock, deadline_ns);
      if (exchange->status != CALL_OK) {
        return;
      }
    }
    query_attempt(udp, clock, random, server, name, deadline_ns, exchange);
    cleanup = exchange->cleanup_status;
    if (exchange->status != CALL_TIMED_OUT) {
      return;
    }
  }
}

#define DNS_STAGE_OPEN 0
#define DNS_STAGE_SEND 1
#define DNS_STAGE_RECEIVE 2
#define DNS_STAGE_DONE 3

static void query_close(struct dns_query_state *query)
{
  if (query->endpoint == HANDLE_INVALID) {
    return;
  }
  enum call_status stopped = udp_shutdown(query->endpoint);
  int closed = handle_close(query->endpoint);
  query->endpoint = HANDLE_INVALID;
  enum call_status cleanup = stopped != CALL_OK ? stopped : closed ? CALL_BAD_HANDLE : CALL_OK;
  if (query->exchange.cleanup_status == CALL_OK) {
    query->exchange.cleanup_status = cleanup;
  }
  if (query->exchange.status == CALL_OK && cleanup != CALL_OK) {
    query->exchange.operation = "UDP cleanup";
    query->exchange.status = cleanup;
  }
}

void dns_query_begin(struct dns_query_state *query, handle_t udp, handle_t clock,
    handle_t random, uint32_t server, const struct dns_name *name,
    uint64_t deadline, handle_t receiver, uint64_t *next_id)
{
  *query = (struct dns_query_state){
    .udp = udp, .clock = clock, .random = random, .server = server,
    .name = *name, .deadline = deadline, .receiver = receiver,
    .next_id = next_id, .endpoint = HANDLE_INVALID,
  };
}

static enum dns_query_step query_done(struct dns_query_state *query)
{
  query_close(query);
  if (!query->aborted && query->exchange.status == CALL_TIMED_OUT &&
      ++query->attempt < DNS_ATTEMPTS) {
    query->stage = DNS_STAGE_OPEN;
    return DNS_QUERY_PROGRESS;
  }
  query->stage = DNS_STAGE_DONE;
  return DNS_QUERY_DONE;
}

enum dns_query_step dns_query_step(struct dns_query_state *query)
{
  if (query->pending) {
    return DNS_QUERY_WAIT;
  }
  if (query->stage == DNS_STAGE_DONE) {
    return DNS_QUERY_DONE;
  }
  if (query->aborted) {
    query->exchange.status = CALL_ABANDONED;
    return query_done(query);
  }
  if (query->stage == DNS_STAGE_OPEN) {
    query->exchange = (struct dns_exchange){.operation = "clock",
      .cleanup_status = query->exchange.cleanup_status};
    query->exchange.status = udp_deadline(query->clock, DNS_ATTEMPT_NS,
        &query->attempt_deadline);
    if (query->exchange.status != CALL_OK) {
      return query_done(query);
    }
    if (query->deadline < query->attempt_deadline) {
      query->attempt_deadline = query->deadline;
    }
    query->exchange.status = check_deadline(query->clock, query->attempt_deadline);
    if (query->exchange.status != CALL_OK) {
      return query_done(query);
    }
    for (unsigned i = 0; i < DNS_BIND_ATTEMPTS; ++i) {
      uint16_t identity[2];
      query->exchange.operation = "random read";
      query->exchange.status = random_read(query->random, identity, sizeof(identity),
          query->attempt_deadline);
      if (query->exchange.status != CALL_OK) {
        return query_done(query);
      }
      struct udp_open_reply endpoint;
      query->exchange.operation = "UDP open";
      query->exchange.status = udp_open_route(query->udp, query->server,
          DNS_EPHEMERAL_FIRST + identity[1] % DNS_EPHEMERAL_COUNT, &endpoint);
      if (query->exchange.status == CALL_OK) {
        query->identity = identity[0];
        query->endpoint = endpoint.handle;
        query->stage = DNS_STAGE_SEND;
        return DNS_QUERY_PROGRESS;
      }
      if (query->exchange.status != CALL_ALREADY_EXISTS) {
        return query_done(query);
      }
    }
    return query_done(query);
  }
  if (!query->next_id || !*query->next_id || *query->next_id == UINT64_MAX) {
    query->exchange.status = CALL_LIMIT;
    return query_done(query);
  }
  uint64_t operation;
  unsigned char payload[sizeof(struct completion_udp_send_request) + DNS_MESSAGE_BYTES];
  size_t size;
  if (query->stage == DNS_STAGE_SEND) {
    operation = UDP_SEND;
    struct completion_udp_send_request request = {
      .address = query->server, .port = DNS_PORT,
      .size = dns_make_query(&query->name, query->identity,
          payload + sizeof(struct completion_udp_send_request)),
    };
    memcpy(payload, &request, sizeof(request));
    size = sizeof(request) + request.size;
    query->exchange.operation = "UDP send";
  } else {
    operation = UDP_RECEIVE;
    struct completion_udp_receive_request request = {.capacity = UDP_MAX_PAYLOAD};
    memcpy(payload, &request, sizeof(request));
    size = sizeof(request);
    query->exchange.operation = "UDP receive";
  }
  query->exchange.status = completion_submit(query->endpoint, query->receiver,
      *query->next_id, PROTOCOL_UDP, operation, query->attempt_deadline, payload, size);
  if (query->exchange.status != CALL_OK) {
    return query_done(query);
  }
  query->pending = (*query->next_id)++;
  return DNS_QUERY_WAIT;
}

bool dns_query_completion(struct dns_query_state *query, const struct endpoint_packet *packet)
{
  if (!query->pending || packet->kind != ENDPOINT_MESSAGE_COMPLETION ||
      packet->correlation_id != query->pending) {
    return false;
  }
  query->pending = 0;
  uint64_t operation = query->stage == DNS_STAGE_SEND ? UDP_SEND : UDP_RECEIVE;
  query->exchange.status = packet->result;
  if (packet->protocol != PROTOCOL_UDP || packet->operation != operation || packet->grant_count) {
    query->exchange.status = CALL_BAD_REQUEST;
  }
  if (query->exchange.status == CALL_OK && !query->aborted) {
    if (operation == UDP_SEND) {
      if (packet->size) {
        query->exchange.status = CALL_BAD_REQUEST;
      } else {
        query->stage = DNS_STAGE_RECEIVE;
      }
    } else if (packet->size < sizeof(struct udp_receive_reply)) {
      query->exchange.status = CALL_BAD_REQUEST;
    } else {
      struct udp_receive_reply peer;
      memcpy(&peer, packet->data, sizeof(peer));
      if (peer.length > UDP_MAX_PAYLOAD || packet->size != sizeof(peer) + peer.length) {
        query->exchange.status = CALL_BAD_REQUEST;
      } else if (peer.address == query->server && peer.port == DNS_PORT) {
        query->exchange.response = dns_parse_reply(packet->data + sizeof(peer), peer.length,
            &query->name, query->identity, &query->exchange.reply);
        if (query->exchange.response != DNS_IGNORE) {
          query->stage = DNS_STAGE_DONE;
        }
      }
    }
  }
  for (size_t i = 0; i < packet->grant_count; ++i) {
    handle_close(packet->grants[i].handle);
  }
  if (handle_close(packet->receipt) && query->exchange.status == CALL_OK) {
    query->exchange.status = CALL_BAD_HANDLE;
  }
  if (query->aborted || query->exchange.status != CALL_OK || query->stage == DNS_STAGE_DONE) {
    query_done(query);
  }
  return true;
}

enum call_status dns_query_abort(struct dns_query_state *query)
{
  query->aborted = true;
  if (query->pending) {
    return completion_cancel(query->receiver, query->pending);
  }
  query->exchange.status = CALL_ABANDONED;
  query_done(query);
  return CALL_OK;
}
