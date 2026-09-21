#include <abi/startup.h>
#include <console.h>
#include <endpoint.h>
#include <handle.h>
#include <number_service.h>

static int serve(const struct startup_info *startup)
{
  for (;;) {
    struct endpoint_packet packet;
    enum call_status status = endpoint_receive(startup->endpoint, &packet);
    if (status == CALL_ENDPOINT_CLOSED) {
      return console_print(startup->output, "server: client endpoint closed\n");
    }
    if (status != CALL_OK) {
      return -1;
    }

    struct number_reply reply = {.status = NUMBER_INVALID};
    if (packet.size == sizeof(struct number_request)) {
      struct number_request request;
      for (size_t i = 0; i < sizeof(request); ++i) {
        ((uint8_t *)&request)[i] = packet.data[i];
      }
      if (request.operation == NUMBER_DOUBLE && request.value <= UINT64_MAX / 2) {
        reply.status = NUMBER_OK;
        reply.value = request.value * 2;
      }
    }
    status = endpoint_reply(startup->endpoint, packet.id, &reply, sizeof(reply));
    if (status == CALL_ENDPOINT_CLOSED) {
      return 0;
    }
    if (status != CALL_OK) {
      return -1;
    }
  }
}

int main(const struct startup_info *startup)
{
  if (!startup || startup->version != STARTUP_VERSION || startup->size < sizeof(*startup)) {
    return 1;
  }
  int result = serve(startup);
  if (handle_close(startup->endpoint) != 0) {
    result = 1;
  }
  if (handle_close(startup->output) != 0) {
    result = 1;
  }
  return result != 0;
}
