#include <startup.h>
#include <process.h>
#include <launcher.h>
#include <abi/console.h>
#include <abi/memory.h>
#include <stdio.h>
#include <console.h>
#include <endpoint.h>
#include <handle.h>
#include "../common/content_service.h"
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

static int launch_server(handle_t output, handle_t *server)
{
  handle_t launcher = startup_resource("launcher");
  handle_t image = startup_resource("server_image");
  handle_t endpoint = startup_resource("server_endpoint");
  handle_t memory = startup_resource("memory");
  enum { SERVER_OUTPUT, SERVER_ENDPOINT, SERVER_MEMORY, SERVER_GRANTS };
  struct launch_grant grants[SERVER_GRANTS + STARTUP_STREAM_COUNT] = {
    {output, CONSOLE_RIGHT_WRITE},
    {endpoint, ENDPOINT_RIGHT_RECEIVE | ENDPOINT_RIGHT_REPLY},
    {memory, MEMORY_RIGHT_MANAGE},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"output", SERVER_OUTPUT},
    {(uintptr_t)"endpoint", SERVER_ENDPOINT},
    {(uintptr_t)"memory", SERVER_MEMORY},
  };
  const char *arguments[] = {"server.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = SERVER_GRANTS,
    .resources = (uintptr_t)resources,
    .resource_count = sizeof(resources) / sizeof(resources[0]),
    .argv = (uintptr_t)arguments,
    .argc = 1,
  };
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    struct startup_stream stream = startup_stream(i);
    if (stream.protocol == STARTUP_STREAM_NONE || i == STARTUP_STDIN) {
      continue;
    }
    bool input = i == STARTUP_STDIN;
    uint64_t rights = stream.protocol == PROTOCOL_FILE ?
        (input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE) :
        (input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE);
    request.streams[i] = (struct launch_stream){stream.protocol, request.grant_count};
    grants[request.grant_count++] = (struct launch_grant){stream.handle, rights};
  }
  int result = launcher_launch(launcher, &request, server) == CALL_OK ? 0 : -1;
  /* Launch copies grants. Drop our server end so endpoint closure reflects
   * the two communicating programs, and release preparation-only authority. */
  handle_t preparation[] = {endpoint, image, launcher};
  for (size_t i = 0; i < sizeof(preparation) / sizeof(preparation[0]); ++i) {
    if (handle_close(preparation[i]) != 0) {
      result = -1;
    }
  }
  return result;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  handle_t output = startup_resource("output");
  handle_t endpoint = startup_resource("endpoint");
  handle_t content = startup_resource("content");
  handle_t server = HANDLE_INVALID;
  if (output == HANDLE_INVALID || endpoint == HANDLE_INVALID || content == HANDLE_INVALID) {
    return 1;
  }

  int result = launch_server(output, &server);
  if (result == 0) {
    result = run_client(output, content, endpoint);
  }
  if (handle_close(content) != 0) {
    result = 1;
  }
  /* The server exits on endpoint closure; waiting before close would deadlock. */
  if (handle_close(endpoint) != 0) {
    result = 1;
  } else if (server != HANDLE_INVALID && wait_for_server(output, server) != 0) {
    result = 1;
  }
  if (server != HANDLE_INVALID && handle_close(server) != 0) {
    result = 1;
  }
  if (handle_close(output) != 0) {
    result = 1;
  }
  return result != 0;
}
