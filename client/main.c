#include <startup.h>
#include <process.h>
#include <stdio.h>
#include <console.h>
#include <endpoint.h>
#include <handle.h>
#include <content_service.h>
#include <abi/file.h>

static int print_number(handle_t output, uint64_t value)
{
  char digits[21];
  int length = snprintf(digits, sizeof(digits), "%ju", (uintmax_t)value);
  if (length < 0 || (size_t)length >= sizeof(digits)) {
    return -1;
  }
  return console_write_all(output, digits, length);
}

static int run_client(handle_t output, handle_t content, handle_t endpoint)
{
  struct content_request request = {CONTENT_PRINT};
  struct endpoint_grant grant = {content, FILE_RIGHT_READ};
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

static int wait_for_server(handle_t output, handle_t server)
{
  struct process_result completion;
  if (process_wait(server, &completion) != CALL_OK) {
    return -1;
  }
  if (completion.kind == PROCESS_FAULTED) {
    console_print(output, "client: server faulted\n");
    return -1;
  }
  char message[80];
  int length = snprintf(message, sizeof(message), "client: server exited with status %jd\n",
      (intmax_t)completion.exit_status);
  if (length < 0 || (size_t)length >= sizeof(message) ||
      console_write_all(output, message, length) != 0) {
    return -1;
  }
  return completion.exit_status == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  handle_t output = startup_resource("output");
  handle_t endpoint = startup_resource("endpoint");
  handle_t content = startup_resource("content");
  handle_t server = startup_resource("server_process");
  if (output == HANDLE_INVALID || endpoint == HANDLE_INVALID || content == HANDLE_INVALID ||
      server == HANDLE_INVALID) {
    return 1;
  }

  int result = run_client(output, content, endpoint);
  if (handle_close(content) != 0) {
    result = 1;
  }
  /* The server exits on endpoint closure; waiting before close would deadlock. */
  if (handle_close(endpoint) != 0) {
    result = 1;
  } else if (wait_for_server(output, server) != 0) {
    result = 1;
  }
  if (handle_close(server) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
