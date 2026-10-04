#include "dhcp.h"
#include <clock.h>
#include <handle.h>
#include <random.h>
#include <string.h>
#include <udp.h>

#define SECOND_NS UINT64_C(1000000000)
#define IPV4_MULTICAST_BASE UINT32_C(0xe0000000)

enum {
  DHCP_CLIENT_PORT = 68,
  DHCP_SERVER_PORT = 67,
  BOOTP_REQUEST = 1,
  BOOTP_REPLY = 2,
  BOOTP_ETHERNET = 1,
  BOOTP_XID = 4,
  BOOTP_SECS = 8,
  BOOTP_FLAGS = 10,
  BOOTP_CIADDR = 12,
  BOOTP_YIADDR = 16,
  BOOTP_CHADDR = 28,
  BOOTP_SNAME = 44,
  BOOTP_SNAME_LENGTH = 64,
  BOOTP_FILE = 108,
  BOOTP_FILE_LENGTH = 128,
  BOOTP_OPTIONS = 236,
  BOOTP_BROADCAST = 0x8000,
  DHCP_PACKET_LENGTH = 300,
  DHCP_DISCOVER = 1,
  DHCP_OFFER = 2,
  DHCP_REQUEST = 3,
  DHCP_ACK = 5,
  DHCP_NAK = 6,
  OPTION_PAD = 0,
  OPTION_MASK = 1,
  OPTION_ROUTER = 3,
  OPTION_DNS = 6,
  OPTION_ADDRESS = 50,
  OPTION_LEASE = 51,
  OPTION_OVERLOAD = 52,
  OPTION_TYPE = 53,
  OPTION_SERVER = 54,
  OPTION_PARAMETERS = 55,
  OPTION_T1 = 58,
  OPTION_T2 = 59,
  OPTION_END = 255,
  OVERLOAD_FILE = 1,
  OVERLOAD_SNAME = 2,
};

static const uint8_t cookie[] = {99, 130, 83, 99};
static const uint8_t parameters[] = {
  OPTION_MASK, OPTION_ROUTER, OPTION_DNS, OPTION_LEASE, OPTION_T1, OPTION_T2,
};

struct option_value {
  bool present;
  uint32_t value;
};

struct dhcp_reply {
  uint8_t type, overload;
  struct option_value mask, server, duration, t1, t2;
  struct option_value gateway, dns;
};

static uint32_t read_u32(const uint8_t *bytes)
{
  return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
      (uint32_t)bytes[2] << 8 | bytes[3];
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
  bytes[0] = value >> 24;
  bytes[1] = value >> 16;
  bytes[2] = value >> 8;
  bytes[3] = value;
}

static bool read_scalar(struct option_value *value, const uint8_t *data, size_t length)
{
  if (length != 4) {
    return false;
  }
  uint32_t decoded = read_u32(data);
  if (value->present && value->value != decoded) {
    return false;
  }
  *value = (struct option_value){true, decoded};
  return true;
}

static bool read_options(const uint8_t *data, size_t length, bool main,
    struct dhcp_reply *reply)
{
  size_t offset = 0;
  while (offset < length) {
    uint8_t code = data[offset++];
    if (code == OPTION_PAD) {
      continue;
    }
    if (code == OPTION_END) {
      return true;
    }
    if (offset == length) {
      return false;
    }
    size_t size = data[offset++];
    if (size > length - offset) {
      return false;
    }
    const uint8_t *value = data + offset;
    switch (code) {
    case OPTION_TYPE:
      if (size != 1 || !value[0] || (reply->type && reply->type != value[0])) {
        return false;
      }
      reply->type = value[0];
      break;
    case OPTION_OVERLOAD:
      if (!main || size != 1 || !value[0] || value[0] > 3 ||
          (reply->overload && reply->overload != value[0])) {
        return false;
      }
      reply->overload = value[0];
      break;
    case OPTION_MASK:
      if (!read_scalar(&reply->mask, value, size)) {
        return false;
      }
      break;
    case OPTION_SERVER:
      if (!read_scalar(&reply->server, value, size)) {
        return false;
      }
      break;
    case OPTION_LEASE:
      if (!read_scalar(&reply->duration, value, size)) {
        return false;
      }
      break;
    case OPTION_T1:
      if (!read_scalar(&reply->t1, value, size)) {
        return false;
      }
      break;
    case OPTION_T2:
      if (!read_scalar(&reply->t2, value, size)) {
        return false;
      }
      break;
    case OPTION_ROUTER:
    case OPTION_DNS:
      if (!size || size % 4) {
        return false;
      }
      struct option_value *first = code == OPTION_ROUTER ? &reply->gateway : &reply->dns;
      if (!first->present) {
        *first = (struct option_value){true, read_u32(value)};
      }
      break;
    default:
      break;
    }
    offset += size;
  }
  return false;
}

static bool decode_reply(const uint8_t *packet, size_t length, uint32_t xid,
    const uint8_t mac[6], struct dhcp_reply *reply)
{
  *reply = (struct dhcp_reply){0};
  if (length < BOOTP_OPTIONS + sizeof(cookie) || packet[0] != BOOTP_REPLY ||
      packet[1] != BOOTP_ETHERNET || packet[2] != 6 ||
      read_u32(packet + BOOTP_XID) != xid || memcmp(packet + BOOTP_CHADDR, mac, 6) ||
      memcmp(packet + BOOTP_OPTIONS, cookie, sizeof(cookie))) {
    return false;
  }
  if (!read_options(packet + BOOTP_OPTIONS + sizeof(cookie),
      length - BOOTP_OPTIONS - sizeof(cookie), true, reply)) {
    return false;
  }
  if ((reply->overload & OVERLOAD_FILE) &&
      !read_options(packet + BOOTP_FILE, BOOTP_FILE_LENGTH, false, reply)) {
    return false;
  }
  if ((reply->overload & OVERLOAD_SNAME) &&
      !read_options(packet + BOOTP_SNAME, BOOTP_SNAME_LENGTH, false, reply)) {
    return false;
  }
  return reply->type && reply->server.present;
}

static bool is_unicast(uint32_t address)
{
  return address >> 24 && address >> 24 != 127 && address < IPV4_MULTICAST_BASE;
}

static bool decode_lease(const uint8_t *packet, const struct dhcp_reply *reply,
    uint64_t acquired_ns, const struct dhcp_lease *current, struct dhcp_lease *lease)
{
  uint32_t address = read_u32(packet + BOOTP_YIADDR);
  if (!is_unicast(address) || !is_unicast(reply->server.value) ||
      (!reply->mask.present && !current) || !reply->duration.present ||
      !reply->duration.value || (current && address != current->address)) {
    return false;
  }
  uint32_t prefix = 0;
  uint32_t mask = reply->mask.present ? reply->mask.value :
      UINT32_MAX << (32 - current->prefix);
  uint32_t remaining_mask = mask;
  while (remaining_mask & UINT32_C(0x80000000)) {
    ++prefix;
    remaining_mask <<= 1;
  }
  if (!prefix || remaining_mask) {
    return false;
  }
  uint32_t host = address & ~mask;
  if (prefix < 31 && (!host || host == ~mask)) {
    return false;
  }
  uint32_t gateway = reply->gateway.present ? reply->gateway.value :
      current ? current->gateway : 0;
  if ((gateway || reply->gateway.present) && (!is_unicast(gateway) || gateway == address ||
      (gateway & mask) != (address & mask) ||
      (prefix < 31 && (!(gateway & ~mask) || (gateway & ~mask) == ~mask)))) {
    return false;
  }
  uint32_t dns = reply->dns.present ? reply->dns.value : current ? current->dns : 0;
  if ((dns || reply->dns.present) && (!(dns >> 24) || dns >= IPV4_MULTICAST_BASE)) {
    return false;
  }
  *lease = (struct dhcp_lease){
    .address = address, .prefix = prefix, .gateway = gateway, .dns = dns,
    .server = reply->server.value, .acquired_ns = acquired_ns,
    .renewal_ns = UINT64_MAX, .rebind_ns = UINT64_MAX, .expires_ns = UINT64_MAX,
  };
  if (reply->duration.value == UINT32_MAX) {
    return true;
  }
  uint64_t duration = (uint64_t)reply->duration.value * SECOND_NS;
  uint64_t t1 = reply->t1.present ? (uint64_t)reply->t1.value * SECOND_NS : duration / 2;
  uint64_t t2 = reply->t2.present ? (uint64_t)reply->t2.value * SECOND_NS : duration / 8 * 7;
  if (!t1 || t1 >= t2 || t2 >= duration) {
    /* Invalid server timers do not invalidate an otherwise usable lease. */
    t1 = duration / 2;
    t2 = duration / 8 * 7;
  }
  if (acquired_ns > UINT64_MAX - duration) {
    return false;
  }
  lease->renewal_ns = acquired_ns + t1;
  lease->rebind_ns = acquired_ns + t2;
  lease->expires_ns = acquired_ns + duration;
  return true;
}

static void write_address_option(uint8_t *packet, size_t *offset, uint8_t code,
    uint32_t address)
{
  packet[(*offset)++] = code;
  packet[(*offset)++] = 4;
  write_u32(packet + *offset, address);
  *offset += 4;
}

enum request_state {
  DISCOVERING,
  REQUESTING,
  REFRESHING,
};

static void make_packet(uint8_t packet[DHCP_PACKET_LENGTH], enum request_state state,
    uint32_t xid, uint64_t elapsed_ns, const uint8_t mac[6],
    const struct dhcp_lease *lease)
{
  memset(packet, 0, DHCP_PACKET_LENGTH);
  packet[0] = BOOTP_REQUEST;
  packet[1] = BOOTP_ETHERNET;
  packet[2] = 6;
  write_u32(packet + BOOTP_XID, xid);
  uint64_t secs = elapsed_ns / SECOND_NS;
  if (secs > UINT16_MAX) {
    secs = UINT16_MAX;
  }
  packet[BOOTP_SECS] = secs >> 8;
  packet[BOOTP_SECS + 1] = secs;
  if (state == REFRESHING) {
    write_u32(packet + BOOTP_CIADDR, lease->address);
  } else {
    packet[BOOTP_FLAGS] = BOOTP_BROADCAST >> 8;
    packet[BOOTP_FLAGS + 1] = BOOTP_BROADCAST & 255;
  }
  memcpy(packet + BOOTP_CHADDR, mac, 6);
  memcpy(packet + BOOTP_OPTIONS, cookie, sizeof(cookie));
  size_t offset = BOOTP_OPTIONS + sizeof(cookie);
  packet[offset++] = OPTION_TYPE;
  packet[offset++] = 1;
  packet[offset++] = state == DISCOVERING ? DHCP_DISCOVER : DHCP_REQUEST;
  if (state == REQUESTING) {
    write_address_option(packet, &offset, OPTION_ADDRESS, lease->address);
    write_address_option(packet, &offset, OPTION_SERVER, lease->server);
  }
  packet[offset++] = OPTION_PARAMETERS;
  packet[offset++] = sizeof(parameters);
  memcpy(packet + offset, parameters, sizeof(parameters));
  packet[offset + sizeof(parameters)] = OPTION_END;
}

static uint64_t clipped_deadline(uint64_t now, uint64_t duration, uint64_t end)
{
  return duration < end - now ? now + duration : end;
}

static bool transient_send_failure(enum call_status status)
{
  return status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL ||
      status == CALL_NO_MEMORY || status == CALL_TIMED_OUT;
}

void dhcp_discover(struct dhcp_client *client)
{
  client->requesting = false;
  client->xid_ready = false;
  client->request_sent = false;
  client->xid = 0;
  client->retry_seconds = 4;
  client->started_ns = 0;
  client->retry_ns = 0;
  client->request_ns = 0;
  client->offer = (struct dhcp_lease){0};
}

bool dhcp_open(struct dhcp_client *client, handle_t udp, handle_t clock,
    handle_t random, const uint8_t mac[6])
{
  if (!client) {
    return false;
  }
  *client = (struct dhcp_client){.endpoint = HANDLE_INVALID};
  if (!mac || clock == HANDLE_INVALID || random == HANDLE_INVALID) {
    return false;
  }
  struct udp_open_reply opened;
  if (udp_open_broadcast(udp, DHCP_CLIENT_PORT, &opened) != CALL_OK) {
    return false;
  }
  client->endpoint = opened.handle;
  client->clock = clock;
  client->random = random;
  memcpy(client->mac, mac, sizeof(client->mac));
  dhcp_discover(client);
  return true;
}

bool dhcp_close(struct dhcp_client *client)
{
  if (!client || client->endpoint == HANDLE_INVALID) {
    return true;
  }
  enum call_status shutdown = udp_shutdown(client->endpoint);
  int closed = handle_close(client->endpoint);
  client->endpoint = HANDLE_INVALID;
  return shutdown == CALL_OK && closed == 0;
}

static enum dhcp_result acknowledge(struct dhcp_client *client, const uint8_t *packet,
    const struct dhcp_reply *reply, uint64_t request_ns,
    const struct dhcp_lease *current, struct dhcp_lease *lease, bool *expired)
{
  if (expired) {
    *expired = false;
  }
  struct dhcp_lease candidate;
  uint64_t now;
  if (!decode_lease(packet, reply, request_ns, current, &candidate)) {
    return DHCP_TIMEOUT;
  }
  if (clock_now(client->clock, &now) != CALL_OK) {
    return DHCP_FAILED;
  }
  if (current && now >= current->expires_ns) {
    return DHCP_TIMEOUT;
  }
  if (now >= candidate.expires_ns) {
    if (expired) {
      *expired = true;
    }
    return DHCP_TIMEOUT;
  }
  *lease = candidate;
  return DHCP_ACKNOWLEDGED;
}

/* Receive calls have a thirty-second ABI limit even for long leases. */
static enum call_status receive(struct dhcp_client *client, uint8_t packet[UDP_MAX_PAYLOAD],
    uint64_t now, uint64_t end, struct udp_receive_reply *reply)
{
  return udp_receive(client->endpoint, packet, UDP_MAX_PAYLOAD,
      end <= now ? end : clipped_deadline(now, UDP_RECEIVE_MAX_WAIT_NS, end), reply);
}

enum dhcp_result dhcp_acquire(struct dhcp_client *client, uint64_t deadline_ns,
    struct dhcp_lease *lease)
{
  if (!lease) {
    return DHCP_FAILED;
  }
  *lease = (struct dhcp_lease){0};
  if (!client || client->endpoint == HANDLE_INVALID) {
    return DHCP_FAILED;
  }
  uint8_t packet[UDP_MAX_PAYLOAD];
  /* Preserve immediate first discovery within the session's startup budget;
   * later calls resume the same randomized exponential retry schedule. */
  for (;;) {
    uint64_t now;
    if (clock_now(client->clock, &now) != CALL_OK) {
      return DHCP_FAILED;
    }
    if (now >= deadline_ns) {
      return DHCP_TIMEOUT;
    }
    if (now >= client->retry_ns) {
      uint32_t random_words[2];
      enum call_status entropy = random_read(client->random, random_words,
          sizeof(random_words), clipped_deadline(now, RANDOM_MAX_WAIT_NS, deadline_ns));
      if (entropy != CALL_OK) {
        return entropy == CALL_TIMED_OUT ? DHCP_TIMEOUT : DHCP_FAILED;
      }
      if (clock_now(client->clock, &now) != CALL_OK) {
        return DHCP_FAILED;
      }
      if (now >= deadline_ns) {
        return DHCP_TIMEOUT;
      }
      if (!client->xid_ready) {
        client->xid = random_words[0];
        client->started_ns = now;
        client->xid_ready = true;
      }
      make_packet(packet, client->requesting ? REQUESTING : DISCOVERING,
          client->xid, now - client->started_ns, client->mac, &client->offer);
      uint64_t delay = (client->retry_seconds - 1) * SECOND_NS +
          (random_words[1] % 2001) * UINT64_C(1000000);
      client->retry_ns = clipped_deadline(now, delay, UINT64_MAX);
      enum call_status sent = udp_send(client->endpoint, UINT32_MAX, DHCP_SERVER_PORT,
          packet, DHCP_PACKET_LENGTH, clipped_deadline(now, UDP_SEND_MAX_WAIT_NS, deadline_ns));
      if (sent != CALL_OK && !transient_send_failure(sent)) {
        return DHCP_FAILED;
      }
      if (sent == CALL_OK && client->retry_seconds < 64) {
        client->retry_seconds *= 2;
      }
      if (sent == CALL_OK && client->requesting && !client->request_sent) {
        client->request_ns = now;
        client->request_sent = true;
      }
    }
    if (clock_now(client->clock, &now) != CALL_OK) {
      return DHCP_FAILED;
    }
    if (now >= deadline_ns) {
      return DHCP_TIMEOUT;
    }
    uint64_t until = client->retry_ns < deadline_ns ? client->retry_ns : deadline_ns;
    struct udp_receive_reply received;
    enum call_status status = receive(client, packet, now, until, &received);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return DHCP_FAILED;
    }
    if (clock_now(client->clock, &now) != CALL_OK) {
      return DHCP_FAILED;
    }
    if (now >= deadline_ns) {
      return DHCP_TIMEOUT;
    }
    struct dhcp_reply reply;
    if (received.port != DHCP_SERVER_PORT || received.length > sizeof(packet) ||
        !decode_reply(packet, received.length, client->xid, client->mac, &reply)) {
      continue;
    }
    if (!client->requesting) {
      if (reply.type == DHCP_OFFER && decode_lease(packet, &reply, 0, NULL, &client->offer)) {
        client->requesting = true;
        client->retry_ns = now;
        client->retry_seconds = 4;
      }
      continue;
    }
    if (!client->request_sent || reply.server.value != client->offer.server) {
      continue;
    }
    if (reply.type == DHCP_NAK) {
      return DHCP_NAK_RECEIVED;
    }
    if (reply.type == DHCP_ACK && read_u32(packet + BOOTP_YIADDR) == client->offer.address) {
      bool expired;
      enum dhcp_result result = acknowledge(client, packet, &reply,
          client->request_ns, NULL, lease, &expired);
      if (result != DHCP_TIMEOUT) {
        return result;
      }
      if (expired) {
        dhcp_discover(client);
      }
    }
  }
}

enum dhcp_result dhcp_refresh(struct dhcp_client *client,
    const struct dhcp_lease *current, struct dhcp_lease *updated)
{
  if (!updated) {
    return DHCP_FAILED;
  }
  *updated = (struct dhcp_lease){0};
  if (!client || client->endpoint == HANDLE_INVALID || !current) {
    return DHCP_FAILED;
  }
  if (current->expires_ns == UINT64_MAX) {
    return DHCP_TIMEOUT;
  }
  uint8_t packet[UDP_MAX_PAYLOAD];
  bool xid_ready = false, request_sent = false, rebinding = false;
  uint32_t xid = 0;
  uint64_t request_ns = 0, started_ns = 0, retry_ns = current->renewal_ns;
  for (;;) {
    uint64_t now;
    if (clock_now(client->clock, &now) != CALL_OK) {
      return DHCP_FAILED;
    }
    if (now >= current->expires_ns) {
      return DHCP_TIMEOUT;
    }
    if (!rebinding && now >= current->rebind_ns) {
      rebinding = true;
      retry_ns = now;
    }
    uint64_t end = rebinding ? current->expires_ns : current->rebind_ns;
    if (now >= retry_ns) {
      if (!xid_ready) {
        enum call_status entropy = random_read(client->random, &xid, sizeof(xid),
            clipped_deadline(now, RANDOM_MAX_WAIT_NS, end));
        if (entropy != CALL_OK && entropy != CALL_TIMED_OUT) {
          return DHCP_FAILED;
        }
        if (entropy == CALL_TIMED_OUT) {
          continue;
        }
        if (clock_now(client->clock, &now) != CALL_OK) {
          return DHCP_FAILED;
        }
        if (now >= end) {
          continue;
        }
        xid_ready = true;
        started_ns = now;
      }
      make_packet(packet, REFRESHING, xid, now - started_ns, client->mac, current);
      uint64_t delay = (end - now) / 2;
      if (delay < 60 * SECOND_NS) {
        delay = 60 * SECOND_NS;
      }
      retry_ns = clipped_deadline(now, delay, end);
      enum call_status sent = udp_send(client->endpoint,
          rebinding ? UINT32_MAX : current->server, DHCP_SERVER_PORT,
          packet, DHCP_PACKET_LENGTH, clipped_deadline(now, UDP_SEND_MAX_WAIT_NS, end));
      if (sent != CALL_OK && !transient_send_failure(sent)) {
        return DHCP_FAILED;
      }
      if (sent == CALL_OK && !request_sent) {
        /* Same xid across retransmission/T2: an older ACK is indistinguishable.
         * Retain the earliest successful REQUEST origin conservatively. */
        request_sent = true;
        request_ns = now;
      }
    }
    if (clock_now(client->clock, &now) != CALL_OK) {
      return DHCP_FAILED;
    }
    if (now >= end) {
      continue;
    }
    uint64_t until = retry_ns < end ? retry_ns : end;
    struct udp_receive_reply received;
    enum call_status status = receive(client, packet, now, until, &received);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return DHCP_FAILED;
    }
    if (clock_now(client->clock, &now) != CALL_OK) {
      return DHCP_FAILED;
    }
    if (now >= current->expires_ns) {
      return DHCP_TIMEOUT;
    }
    if (!rebinding && now >= current->rebind_ns) {
      rebinding = true;
      retry_ns = now;
    }
    struct dhcp_reply reply;
    if (!request_sent || received.port != DHCP_SERVER_PORT ||
        received.length > sizeof(packet) ||
        !decode_reply(packet, received.length, xid, client->mac, &reply) ||
        !is_unicast(reply.server.value) || (!rebinding && reply.server.value != current->server)) {
      continue;
    }
    if (reply.type == DHCP_NAK) {
      return DHCP_NAK_RECEIVED;
    }
    if (reply.type == DHCP_ACK) {
      enum dhcp_result result = acknowledge(client, packet, &reply,
          request_ns, current, updated, NULL);
      if (result != DHCP_TIMEOUT) {
        return result;
      }
    }
  }
}
