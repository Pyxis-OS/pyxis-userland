#include <startup.h>
#include <console.h>
#include <endpoint.h>
#include <handle.h>
#include "../common/content_service.h"
#include <file.h>

static int print_content(handle_t content, handle_t output, uint64_t *size)
{
  char buffer[128];
  uint64_t offset = 0;
  for (;;) {
    size_t count;
    if (file_read(content, offset, buffer, sizeof(buffer), &count) != 0) {
      return -1;
    }
    if (!count) {
      *size = offset;
      return 0;
    }
    if (count > UINT64_MAX - offset ||
        console_write_all(output, buffer, count) != 0) {
      return -1;
    }
    offset += count;
  }
}

static int serve(handle_t output, handle_t endpoint)
{
  for (;;) {
    struct endpoint_packet packet;
    enum call_status status = endpoint_receive(endpoint, &packet);
    if (status == CALL_ENDPOINT_CLOSED) {
      return console_print(output, "server: client endpoint closed\n");
    }
    if (status != CALL_OK) {
      return -1;
    }

    struct content_reply reply = {.status = CONTENT_INVALID};
    if (packet.size == sizeof(struct content_request)) {
      struct content_request request;
      for (size_t i = 0; i < sizeof(request); ++i) {
        ((uint8_t *)&request)[i] = packet.data[i];
      }
      if (request.operation == CONTENT_PRINT && packet.grant.handle != HANDLE_INVALID) {
        reply.status = print_content(packet.grant.handle, output, &reply.size) == 0 ?
                       CONTENT_OK : CONTENT_IO_ERROR;
      }
    }
    if (packet.grant.handle != HANDLE_INVALID && handle_close(packet.grant.handle) != 0) {
      return -1;
    }
    status = endpoint_reply(endpoint, packet.id, &reply, sizeof(reply));
    if (status == CALL_ENDPOINT_CLOSED) {
      return 0;
    }
    if (status != CALL_OK) {
      return -1;
    }
  }
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  handle_t output = startup_resource("output");
  handle_t endpoint = startup_resource("endpoint");
  if (output == HANDLE_INVALID || endpoint == HANDLE_INVALID) {
    return 1;
  }

  int result = serve(output, endpoint);
  if (handle_close(endpoint) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
