#include <abi/file.h>
#include <clock.h>
#include <console.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/content_service.h"

#define QUEUED_TIMEOUT_NS UINT64_C(3000000000)
#define DELIVERED_TIMEOUT_NS UINT64_C(10000000000)

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
      strcmp(argv[1], "close") && strcmp(argv[1], "exit") &&
      strcmp(argv[1], "send") && strcmp(argv[1], "mixed") &&
      strcmp(argv[1], "expired") && strcmp(argv[1], "queued-timeout") &&
      strcmp(argv[1], "delivered-timeout") && strcmp(argv[1], "cancel-full") &&
      strcmp(argv[1], "cancel-finish") && strcmp(argv[1], "deadline-reply"))) {
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
  handle_t clock = startup_resource("clock");
  if (output == HANDLE_INVALID || endpoint == HANDLE_INVALID || content == HANDLE_INVALID) {
    return 1;
  }

  bool wide = !strcmp(argv[1], "wide");
  bool abandoned = !strcmp(argv[1], "abandon") && id == 1;
  bool saturated = !strcmp(argv[1], "saturate") && id == ENDPOINT_DELIVERIES_MAX + 1;
  bool closed = !strcmp(argv[1], "close") || !strcmp(argv[1], "exit");
  bool sending = !strcmp(argv[1], "send") || (!strcmp(argv[1], "mixed") && id == 2);
  bool expired = !strcmp(argv[1], "expired");
  bool queued_timeout = !strcmp(argv[1], "queued-timeout");
  bool delivered_timeout = !strcmp(argv[1], "delivered-timeout") ||
      !strcmp(argv[1], "cancel-full") || !strcmp(argv[1], "cancel-finish");
  bool deadline_reply = !strcmp(argv[1], "deadline-reply");
  uint8_t payload[ENDPOINT_DATA_MAX] = {0};
  struct content_request request = {CONTENT_PRINT, id};
  memcpy(payload, &request, sizeof(request));
  size_t size = wide || sending ? sizeof(payload) : sizeof(request);
  for (size_t i = sizeof(request); i < size; ++i) {
    payload[i] = (uint8_t)i;
  }
  size_t grant_count = wide || sending ? ENDPOINT_GRANTS_MAX : 1;
  struct endpoint_grant grants[ENDPOINT_GRANTS_MAX] = {0};
  for (size_t i = 0; i < grant_count; ++i) {
    grants[i] = (struct endpoint_grant){content, FILE_RIGHT_READ, 0};
  }

  bool ok = false;
  enum call_status status;
  struct endpoint_packet reply = {0};
  uint64_t deadline_ns = 0;
  if (expired || queued_timeout || delivered_timeout || deadline_reply) {
    uint64_t now;
    if (clock == HANDLE_INVALID || clock_now(clock, &now) != CALL_OK ||
        (expired && now == 0) ||
        (!expired && now > UINT64_MAX - (queued_timeout ?
        QUEUED_TIMEOUT_NS : DELIVERED_TIMEOUT_NS))) {
      return 1;
    }
    deadline_ns = expired ? now - 1 : now +
        (queued_timeout ? QUEUED_TIMEOUT_NS : DELIVERED_TIMEOUT_NS);
  }
  if (queued_timeout && endpoint_send(endpoint, &deadline_ns,
      sizeof(deadline_ns), NULL, 0) != CALL_OK) {
    return 1;
  }
  if (sending) {
    size_t sends = !strcmp(argv[1], "mixed") ? ENDPOINT_DELIVERIES_MAX : 1;
    ok = true;
    for (size_t i = 0; i < sends; ++i) {
      request.client = id + i;
      memcpy(payload, &request, sizeof(request));
      status = endpoint_send(endpoint, payload, size, grants, grant_count);
      if (status != (i == ENDPOINT_DELIVERIES_MAX - 1 ? CALL_QUEUE_FULL : CALL_OK)) {
        ok = false;
        break;
      }
    }
  } else {
    status = endpoint_request(endpoint, payload, size, grants, grant_count,
        deadline_ns, &reply);
  }
  if (!sending && abandoned) {
    ok = status == CALL_ABANDONED && reply.delivery == ENDPOINT_DELIVERED;
  } else if (!sending && saturated) {
    ok = status == CALL_QUEUE_FULL && reply.delivery == ENDPOINT_NOT_DELIVERED;
  } else if (!sending && closed) {
    ok = status == CALL_ENDPOINT_CLOSED && reply.delivery ==
        ((!strcmp(argv[1], "exit") || id == 1) ?
        ENDPOINT_DELIVERED : ENDPOINT_NOT_DELIVERED);
  } else if (!sending && (expired || queued_timeout || delivered_timeout)) {
    ok = status == CALL_TIMED_OUT && reply.deadline_ns == deadline_ns &&
        reply.delivery == (delivered_timeout ? ENDPOINT_DELIVERED : ENDPOINT_NOT_DELIVERED);
  } else if (!sending && status == CALL_OK) {
    ok = reply.deadline_ns == deadline_ns && reply.result == CONTENT_OK && reply.size == size &&
        reply.grant_count == grant_count &&
        memcmp(reply.data, payload, size) == 0 && read_grants(&reply);
    if (!close_grants(&reply)) {
      ok = false;
    }
  }
  char line[100];
  int length = sending ?
      snprintf(line, sizeof(line), "client %s: sends %s\n", argv[2], ok ? "accepted" : "failed") :
      snprintf(line, sizeof(line), "client %s: transport %u delivery %ju result %ju\n",
          argv[2], status, (uintmax_t)reply.delivery,
          (uintmax_t)(status == CALL_OK ? reply.result : 0));
  if (length < 0 || (size_t)length >= sizeof(line) ||
      console_write_all(output, line, length) != CALL_OK) {
    ok = false;
  }
  if (handle_close(content) != 0 || handle_close(endpoint) != 0 ||
      (clock != HANDLE_INVALID && handle_close(clock) != 0) ||
      handle_close(output) != 0) {
    ok = false;
  }
  return ok ? 0 : 1;
}
