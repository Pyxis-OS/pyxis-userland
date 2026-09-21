#include <abi/startup.h>
#include <console.h>
#include <endpoint.h>
#include <handle.h>
#include <number_service.h>

static int print_number(handle_t output, uint64_t value)
{
  char digits[20];
  size_t start = sizeof(digits);
  do {
    digits[--start] = '0' + value % 10;
    value /= 10;
  } while (value);
  return console_write_all(output, digits + start, sizeof(digits) - start);
}

static int run_client(const struct startup_info *startup)
{
  struct number_request request = {NUMBER_DOUBLE, 21};
  struct endpoint_packet packet;
  enum call_status status = endpoint_request(startup->endpoint, &request,
      sizeof(request), NULL, &packet);
  if (status != CALL_OK || packet.size != sizeof(struct number_reply)) {
    return -1;
  }
  struct number_reply reply;
  for (size_t i = 0; i < sizeof(reply); ++i) {
    ((uint8_t *)&reply)[i] = packet.data[i];
  }
  if (reply.status != NUMBER_OK) {
    return -1;
  }
  if (console_print(startup->output, "client: server returned ") != 0 ||
      print_number(startup->output, reply.value) != 0 ||
      console_print(startup->output, "\n") != 0) {
    return -1;
  }
  return 0;
}

int main(const struct startup_info *startup)
{
  if (!startup || startup->version != STARTUP_VERSION || startup->size < sizeof(*startup)) {
    return 1;
  }
  int result = run_client(startup);
  if (handle_close(startup->endpoint) != 0) {
    result = 1;
  }
  if (handle_close(startup->output) != 0) {
    result = 1;
  }
  return result != 0;
}
