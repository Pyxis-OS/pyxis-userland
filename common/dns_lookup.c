#include "dns.h"
#include <startup.h>
#include <stdio.h>

bool dns_resolve_address(const char *program, const char *target,
    handle_t clock, uint32_t *address)
{
  struct dns_name question;
  if (!dns_name_from_text(target, &question)) {
    fprintf(stderr, "%s: invalid ASCII hostname or DNS name length\n", program);
    return false;
  }
  uint32_t server;
  if (!dns_select_server(NULL, &server)) {
    fprintf(stderr, "%s: DNS server must be a numeric unicast IPv4 address\n", program);
    return false;
  }
  handle_t udp = startup_resource("udp"), random = startup_resource("random");
  if (udp == HANDLE_INVALID || random == HANDLE_INVALID) {
    fprintf(stderr, "%s: missing udp or random capability for DNS\n", program);
    return false;
  }

  struct dns_exchange exchange;
  dns_query(udp, clock, random, server, &question, 0, &exchange);
  if (exchange.status != CALL_OK) {
    fprintf(stderr, "%s: DNS %s failed (status %u)%s\n",
        program, exchange.operation, (unsigned)exchange.status,
        exchange.status == CALL_TIMED_OUT ? ": timed out after two attempts" :
        exchange.status == CALL_NO_ROUTE ? ": no route to server" :
        exchange.status == CALL_UNAVAILABLE ? ": unavailable" : "");
    return false;
  }
  if (exchange.response != DNS_COMPLETE) {
    fprintf(stderr, exchange.response == DNS_TRUNCATED ?
        "%s: truncated DNS response; TCP fallback is unavailable\n" :
        "%s: response exceeds the supported 512-byte DNS limit\n", program);
    return false;
  }
  if (exchange.reply.rcode) {
    fprintf(stderr, "%s: DNS status %s (%u)\n",
        program, dns_response_status(exchange.reply.rcode), exchange.reply.rcode);
    return false;
  }
  enum dns_address_result result = dns_select_address(&exchange.reply, &question, address);
  if (result != DNS_ADDRESS_FOUND) {
    fprintf(stderr, result == DNS_ADDRESS_MISSING ?
        "%s: DNS reply has no IPv4 address for the requested name\n" :
        "%s: DNS reply has a looping or conflicting CNAME chain\n", program);
    return false;
  }
  return true;
}
