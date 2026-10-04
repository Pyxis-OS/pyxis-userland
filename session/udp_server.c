#include "udp_server.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/pipe.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <net_config.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <udp.h>

int launch_udp_broadcast(uint16_t port, const char *count, bool unassigned)
{
  handle_t launcher = startup_resource("launcher"), memory = startup_resource("memory");
  handle_t clock = startup_resource("clock"), udp = startup_resource("udp");
  handle_t app = startup_root("app");
  if (launcher == HANDLE_INVALID || memory == HANDLE_INVALID ||
      clock == HANDLE_INVALID || udp == HANDLE_INVALID || app == HANDLE_INVALID) {
    fputs("session: missing UDP broadcast echo launch authority\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t image;
  enum call_status status = directory_lookup(app, "udp-echo.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open app://udp-echo.pxe (status %u)\n", status);
    return EXIT_FAILURE;
  }

  enum { MEMORY, CLOCK, ENDPOINT, OUTPUT, ERROR, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [CLOCK] = {clock, CLOCK_RIGHT_READ, 0},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"clock", CLOCK},
    {(uintptr_t)"udp_endpoint", ENDPOINT},
  };
  const char *arguments[] = {"app://udp-echo.pxe", "--endpoint", "--count", count};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources, .resource_count = sizeof(resources) / sizeof(resources[0]),
    .argv = (uintptr_t)arguments, .argc = count ? 4 : 2,
  };
  int result = EXIT_FAILURE;
  handle_t endpoint = HANDLE_INVALID;
  for (unsigned i = STARTUP_STDOUT; i <= STARTUP_STDERR; ++i) {
    struct startup_stream stream = startup_stream(i);
    if (stream.protocol == STARTUP_STREAM_NONE) {
      stream = (struct startup_stream){PROTOCOL_CONSOLE, startup_resource("output")};
    }
    if (stream.handle == HANDLE_INVALID) {
      fputs("session: missing UDP broadcast echo output stream\n", stderr);
      goto done;
    }
    uint64_t rights = stream.protocol == PROTOCOL_FILE ? FILE_RIGHT_WRITE :
        stream.protocol == PROTOCOL_PIPE ? PIPE_RIGHT_WRITE : CONSOLE_RIGHT_WRITE;
    uint64_t transport = 0;
    if (stream.protocol == PROTOCOL_FILE) {
      status = handle_rights(stream.handle, NULL, &transport);
      if (status != CALL_OK) {
        fprintf(stderr, "session: cannot query stream rights (status %u)\n", status);
        goto done;
      }
      transport &= HANDLE_TRANSPORT_CALL;
    }
    size_t grant = i == STARTUP_STDOUT ? OUTPUT : ERROR;
    grants[grant] = (struct launch_grant){stream.handle, rights, transport};
    request.streams[i] = (struct launch_stream){stream.protocol, grant};
  }

  struct udp_open_reply reply;
  status = udp_open_broadcast(udp, port, &reply);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot bind broadcast port %u (status %u)\n", port, status);
    goto done;
  }
  endpoint = reply.handle;
  if (unassigned) {
    status = net_config_clear(startup_resource("net_config"));
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot clear IPv4 for broadcast handoff (status %u)\n", status);
      goto done;
    }
  }
  grants[ENDPOINT] = (struct launch_grant){endpoint, UDP_RIGHT_INSPECT | UDP_RIGHT_SEND | UDP_RIGHT_RECEIVE, 0};
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch UDP broadcast echo (status %u)\n", status);
    goto done;
  }
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  if (endpoint != HANDLE_INVALID && handle_close(endpoint) != 0) {
    result = EXIT_FAILURE;
  }
  if (handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}
