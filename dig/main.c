#include "../common/dns.h"
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_address(uint32_t address)
{
  printf("%u.%u.%u.%u", address >> 24, (address >> 16) & 255, (address >> 8) & 255, address & 255);
}

static void print_name(const struct dns_name *name)
{
  size_t offset = 0;
  if (name->bytes[0] == 0) {
    putchar('.');
  }
  while (name->bytes[offset]) {
    unsigned count = name->bytes[offset++];
    for (unsigned i = 0; i < count; ++i) {
      unsigned byte = name->bytes[offset++];
      if (byte > ' ' && byte < 127 && byte != '\\' && byte != '.') {
        putchar(byte);
      } else {
        printf("\\%03u", byte);
      }
    }
    putchar('.');
  }
}

static bool print_answers(const struct dns_reply *reply)
{
  printf("status: %s (%u), answers: %u\n", dns_response_status(reply->rcode), reply->rcode,
      (unsigned)reply->answer_count);
  size_t offset = reply->answers_offset;
  for (unsigned i = 0; i < reply->answer_count; ++i) {
    struct dns_record record;
    if (!dns_read_record(reply->bytes, reply->length, &offset, &record)) {
      return false; /* The retained, immutable message was already validated. */
    }
    print_name(&record.owner);
    printf("\t%u\t", record.ttl);
    if (record.class == DNS_CLASS_IN) {
      fputs("IN\t", stdout);
    } else {
      printf("CLASS%u\t", (unsigned)record.class);
    }
    if (record.type == DNS_TYPE_A && record.class == DNS_CLASS_IN) {
      fputs("A\t", stdout);
      print_address(record.address);
    } else if (record.type == DNS_TYPE_CNAME) {
      fputs("CNAME\t", stdout);
      print_name(&record.target);
    } else {
      printf("TYPE%u\t%u bytes", (unsigned)record.type, (unsigned)record.data_length);
    }
    putchar('\n');
  }
  return !ferror(stdout);
}

int main(int argc, char **argv)
{
  int argument = 1;
  const char *server_text = NULL;
  if (argument < argc && argv[argument][0] == '@') {
    server_text = argv[argument++] + 1;
  }
  if (argc - argument < 1 || argc - argument > 2 ||
      (argc - argument == 2 && strcmp(argv[argument + 1], "A"))) {
    fputs("Usage: dig [@SERVER_IP] NAME [A]\n", stderr);
    return EXIT_FAILURE;
  }
  struct dns_name question;
  if (!dns_name_from_text(argv[argument], &question)) {
    fputs("dig: invalid ASCII hostname or DNS name length\n", stderr);
    return EXIT_FAILURE;
  }
  uint32_t server;
  if (!dns_select_server(server_text, &server)) {
    fputs("dig: server must be a numeric unicast IPv4 address\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t udp = startup_resource("udp"), clock = startup_resource("clock");
  handle_t random = startup_resource("random");
  if (udp == HANDLE_INVALID || clock == HANDLE_INVALID || random == HANDLE_INVALID) {
    fputs("dig: missing udp, clock or random capability\n", stderr);
    return EXIT_FAILURE;
  }

  fputs("server: ", stdout);
  print_address(server);
  fputs("\nquestion: ", stdout);
  print_name(&question);
  fputs(" IN A\n", stdout);
  fflush(stdout);

  struct dns_exchange exchange;
  dns_query(udp, clock, random, server, &question, &exchange);
  if (exchange.status != CALL_OK) {
    fprintf(stderr, "dig: %s failed (status %u)%s\n", exchange.operation, (unsigned)exchange.status,
        exchange.status == CALL_TIMED_OUT ? ": timed out after two attempts" :
        exchange.status == CALL_NO_ROUTE ? ": no route to server" :
        exchange.status == CALL_UNAVAILABLE ? ": unavailable" : "");
    return EXIT_FAILURE;
  }
  if (exchange.response != DNS_COMPLETE) {
    fputs(exchange.response == DNS_TRUNCATED ?
        "dig: truncated DNS response; TCP fallback is unavailable\n" :
        "dig: response exceeds the supported 512-byte DNS limit\n", stderr);
    return EXIT_FAILURE;
  }
  return print_answers(&exchange.reply) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
