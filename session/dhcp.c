#include "dhcp.h"
#include <clock.h>
#include <handle.h>
#include <random.h>
#include <string.h>
#include <udp.h>

#define SECOND_NS UINT64_C(1000000000)
#define ACQUIRE_WAIT_NS (10 * SECOND_NS)
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
    struct dhcp_lease *lease)
{
  uint32_t address = read_u32(packet + BOOTP_YIADDR);
  if (!is_unicast(address) || !is_unicast(reply->server.value) ||
      !reply->mask.present || !reply->duration.present || !reply->duration.value) {
    return false;
  }
  uint32_t mask = reply->mask.value, prefix = 0;
  while (mask & UINT32_C(0x80000000)) {
    ++prefix;
    mask <<= 1;
  }
  if (!prefix || mask) {
    return false;
  }
  mask = reply->mask.value;
  uint32_t host = address & ~mask;
  if (prefix < 31 && (!host || host == ~mask)) {
    return false;
  }
  uint32_t gateway = reply->gateway.value;
  if (reply->gateway.present && (!is_unicast(gateway) || gateway == address ||
      (gateway & mask) != (address & mask) ||
      (prefix < 31 && (!(gateway & ~mask) || (gateway & ~mask) == ~mask)))) {
    return false;
  }
  uint32_t dns = reply->dns.value;
  if (reply->dns.present && (!(dns >> 24) || dns >= IPV4_MULTICAST_BASE)) {
    return false;
  }
  uint32_t duration = reply->duration.value;
  /* Retain integral-second defaults here; this task does not run lease timers. */
  uint32_t t1 = reply->t1.present ? reply->t1.value : duration / 2;
  uint32_t t2 = reply->t2.present ? reply->t2.value : (uint64_t)duration * 7 / 8;
  /* Compare before rounding defaults, including for one- and two-second leases. */
  uint64_t t1_eighths = reply->t1.present ? (uint64_t)t1 * 8 : (uint64_t)duration * 4;
  uint64_t t2_eighths = reply->t2.present ? (uint64_t)t2 * 8 : (uint64_t)duration * 7;
  if (t1_eighths >= t2_eighths || t2_eighths >= (uint64_t)duration * 8) {
    return false;
  }
  *lease = (struct dhcp_lease){
    .address = address, .prefix = prefix, .gateway = gateway, .dns = dns,
    .server = reply->server.value, .lease_seconds = duration,
    .t1_seconds = t1, .t2_seconds = t2,
  };
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

static void make_packet(uint8_t packet[DHCP_PACKET_LENGTH], bool requesting,
    uint32_t xid, uint64_t elapsed_ns, const uint8_t mac[6],
    const struct dhcp_lease *offer)
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
  packet[BOOTP_FLAGS] = BOOTP_BROADCAST >> 8;
  packet[BOOTP_FLAGS + 1] = BOOTP_BROADCAST & 255;
  memcpy(packet + BOOTP_CHADDR, mac, 6);
  memcpy(packet + BOOTP_OPTIONS, cookie, sizeof(cookie));
  size_t offset = BOOTP_OPTIONS + sizeof(cookie);
  packet[offset++] = OPTION_TYPE;
  packet[offset++] = 1;
  packet[offset++] = requesting ? DHCP_REQUEST : DHCP_DISCOVER;
  if (requesting) {
    write_address_option(packet, &offset, OPTION_ADDRESS, offer->address);
    write_address_option(packet, &offset, OPTION_SERVER, offer->server);
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

bool dhcp_acquire(handle_t udp, handle_t clock, handle_t random,
    const uint8_t mac[6], struct dhcp_lease *lease)
{
  if (!lease) {
    return false;
  }
  *lease = (struct dhcp_lease){0};
  uint64_t start;
  if (!mac || clock_now(clock, &start) != CALL_OK || start > UINT64_MAX - ACQUIRE_WAIT_NS) {
    return false;
  }
  uint64_t end = start + ACQUIRE_WAIT_NS;
  struct udp_open_reply opened;
  if (udp_open_broadcast(udp, DHCP_CLIENT_PORT, &opened) != CALL_OK) {
    return false;
  }
  bool success = false, requesting = false, transmit = true, new_transaction = true;
  struct dhcp_lease offer = {0};
  uint32_t xid = 0, retry_seconds = 4;
  uint64_t retry_at = start, request_at = 0;
  uint8_t packet[UDP_MAX_PAYLOAD];

  /* Send immediately instead of RFC 2131's suggested startup jitter: the
   * session gives first acquisition only ten seconds before starting programs.
   * Retransmissions still use randomized exponential backoff. */
  for (;;) {
    uint64_t now;
    if (clock_now(clock, &now) != CALL_OK || now >= end) {
      break;
    }
    if (transmit) {
      uint32_t random_words[2];
      if (random_read(random, random_words, sizeof(random_words),
          clipped_deadline(now, RANDOM_MAX_WAIT_NS, end)) != CALL_OK ||
          clock_now(clock, &now) != CALL_OK || now >= end) {
        break;
      }
      if (new_transaction) {
        xid = random_words[0];
        new_transaction = false;
      }
      make_packet(packet, requesting, xid, now - start, mac, &offer);
      uint64_t delay = (retry_seconds - 1) * SECOND_NS +
          (random_words[1] % 2001) * UINT64_C(1000000);
      retry_at = clipped_deadline(now, delay, end);
      enum call_status sent = udp_send(opened.handle, UINT32_MAX, DHCP_SERVER_PORT, packet,
          DHCP_PACKET_LENGTH, clipped_deadline(now, UDP_SEND_MAX_WAIT_NS, end));
      /* Initial PHY negotiation can leave carrier down. Keep the same bounded
       * retry schedule for transient local transmission failures. */
      if (sent != CALL_OK && sent != CALL_UNAVAILABLE && sent != CALL_QUEUE_FULL &&
          sent != CALL_NO_MEMORY && sent != CALL_TIMED_OUT) {
        break;
      }
      if (sent == CALL_OK && retry_seconds < 64) {
        retry_seconds *= 2;
      }
      if (sent == CALL_OK && requesting && !request_at) {
        request_at = now;
      }
      transmit = false;
    }
    struct udp_receive_reply received;
    enum call_status status = udp_receive(opened.handle, packet, sizeof(packet), retry_at, &received);
    if (status == CALL_TIMED_OUT) {
      transmit = true;
      continue;
    }
    if (status != CALL_OK) {
      break;
    }
    struct dhcp_reply reply;
    if (received.port != DHCP_SERVER_PORT || received.length > sizeof(packet) ||
        !decode_reply(packet, received.length, xid, mac, &reply)) {
      continue;
    }
    if (!requesting) {
      if (reply.type == DHCP_OFFER && decode_lease(packet, &reply, &offer)) {
        requesting = true;
        transmit = true;
        retry_seconds = 4;
      }
      continue;
    }
    if (reply.server.value != offer.server) {
      continue;
    }
    if (reply.type == DHCP_NAK) {
      requesting = false;
      transmit = true;
      new_transaction = true;
      retry_seconds = 4;
      request_at = 0;
    } else if (reply.type == DHCP_ACK && request_at &&
        read_u32(packet + BOOTP_YIADDR) == offer.address) {
      struct dhcp_lease acknowledged;
      if (decode_lease(packet, &reply, &acknowledged)) {
        acknowledged.acquired_ns = request_at;
        *lease = acknowledged;
        success = true;
        break;
      }
    }
  }
  enum call_status shutdown = udp_shutdown(opened.handle);
  int closed = handle_close(opened.handle);
  if (shutdown != CALL_OK || closed != 0) {
    *lease = (struct dhcp_lease){0};
    return false;
  }
  return success;
}
