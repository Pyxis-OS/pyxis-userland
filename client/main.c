#include <startup.h>
#include <console.h>
#include <endpoint.h>
#include <handle.h>
#include <content_service.h>
#include <abi/blob.h>

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

static int run_client(handle_t output, handle_t content, handle_t endpoint)
{
  struct content_request request = {CONTENT_PRINT};
  struct endpoint_grant grant = {content, BLOB_RIGHT_READ};
  struct endpoint_packet packet;
  enum call_status status = endpoint_request(endpoint, &request,
      sizeof(request), &grant, &packet);
  if (status != CALL_OK || packet.size != sizeof(struct content_reply)) {
    return -1;
  }
  struct content_reply reply;
  for (size_t i = 0; i < sizeof(reply); ++i) {
    ((uint8_t *)&reply)[i] = packet.data[i];
  }
  if (reply.status != CONTENT_OK) {
    return -1;
  }
  if (console_print(output, "client: server read ") != 0 ||
      print_number(output, reply.size) != 0 ||
      console_print(output, " bytes\n") != 0) {
    return -1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  handle_t output = startup_resource("output");
  handle_t endpoint = startup_resource("endpoint");
  handle_t content = startup_resource("content");
  if (output == HANDLE_INVALID || endpoint == HANDLE_INVALID || content == HANDLE_INVALID) {
    return 1;
  }

  int result = run_client(output, content, endpoint);
  if (handle_close(content) != 0) {
    result = 1;
  }
  if (handle_close(endpoint) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
