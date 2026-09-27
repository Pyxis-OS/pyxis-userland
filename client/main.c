#include <abi/file.h>
#include <console.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/content_service.h"

static bool close_grants(const struct endpoint_packet *packet)
{
  bool closed = true;
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (handle_close(packet->grants[i].handle) != 0) {
      closed = false;
    }
  }
  return closed;
}

static bool read_grants(const struct endpoint_packet *packet)
{
  for (size_t i = 0; i < packet->grant_count; ++i) {
    char byte;
    size_t count;
    if (file_read(packet->grants[i].handle, 0, &byte, 1, &count) != CALL_OK || count != 1) {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv)
{
  if (argc != 3 || (strcmp(argv[1], "normal") && strcmp(argv[1], "wide") &&
      strcmp(argv[1], "abandon") && strcmp(argv[1], "saturate") &&
      strcmp(argv[1], "close") && strcmp(argv[1], "exit"))) {
    return 1;
  }
  char *end;
  unsigned long id = strtoul(argv[2], &end, 10);
  if (*end || id < 1 || id > ENDPOINT_DELIVERIES_MAX + 1) {
    return 1;
  }
  handle_t output = startup_resource("output");
  handle_t endpoint = startup_resource("endpoint");
  handle_t content = startup_resource("content");
  if (output == HANDLE_INVALID || endpoint == HANDLE_INVALID || content == HANDLE_INVALID) {
    return 1;
  }

  bool wide = !strcmp(argv[1], "wide");
  bool abandoned = !strcmp(argv[1], "abandon") && id == 1;
  bool saturated = !strcmp(argv[1], "saturate") && id == ENDPOINT_DELIVERIES_MAX + 1;
  bool closed = !strcmp(argv[1], "close") || !strcmp(argv[1], "exit");
  uint8_t payload[ENDPOINT_DATA_MAX] = {0};
  struct content_request request = {CONTENT_PRINT, id};
  memcpy(payload, &request, sizeof(request));
  size_t size = wide ? sizeof(payload) : sizeof(request);
  for (size_t i = sizeof(request); i < size; ++i) {
    payload[i] = (uint8_t)i;
  }
  size_t grant_count = wide ? ENDPOINT_GRANTS_MAX : 1;
  struct endpoint_grant grants[ENDPOINT_GRANTS_MAX] = {0};
  for (size_t i = 0; i < grant_count; ++i) {
    grants[i] = (struct endpoint_grant){content, FILE_RIGHT_READ};
  }

  struct endpoint_packet reply;
  enum call_status status = endpoint_request(endpoint, payload, size,
      grants, grant_count, &reply);
  bool ok = false;
  if (abandoned) {
    ok = status == CALL_ABANDONED && reply.delivery == ENDPOINT_DELIVERED;
  } else if (saturated) {
    ok = status == CALL_QUEUE_FULL && reply.delivery == ENDPOINT_NOT_DELIVERED;
  } else if (closed) {
    ok = status == CALL_ENDPOINT_CLOSED && reply.delivery ==
        ((!strcmp(argv[1], "exit") || id == 1) ?
        ENDPOINT_DELIVERED : ENDPOINT_NOT_DELIVERED);
  } else if (status == CALL_OK) {
    ok = reply.result == CONTENT_OK && reply.size == size &&
        reply.grant_count == grant_count &&
        memcmp(reply.data, payload, size) == 0 && read_grants(&reply);
    if (!close_grants(&reply)) {
      ok = false;
    }
  }
  char line[100];
  int length = snprintf(line, sizeof(line), "client %s: transport %u delivery %ju result %ju\n",
      argv[2], status, (uintmax_t)reply.delivery,
      (uintmax_t)(status == CALL_OK ? reply.result : 0));
  if (length < 0 || (size_t)length >= sizeof(line) ||
      console_write_all(output, line, length) != CALL_OK) {
    ok = false;
  }
  if (handle_close(content) != 0 || handle_close(endpoint) != 0 ||
      handle_close(output) != 0) {
    ok = false;
  }
  return ok ? 0 : 1;
}
