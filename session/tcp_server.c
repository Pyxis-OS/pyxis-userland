#include "tcp_server.h"
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/pipe.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <tcp.h>

int launch_tcp_server(uint32_t address, uint16_t port, const char *count)
{
  handle_t launcher = startup_resource("launcher"), memory = startup_resource("memory");
  handle_t clock = startup_resource("clock"), tcp = startup_resource("tcp");
  handle_t app = startup_root("app");
  if (launcher == HANDLE_INVALID || memory == HANDLE_INVALID ||
      clock == HANDLE_INVALID || tcp == HANDLE_INVALID || app == HANDLE_INVALID) {
    fputs("session: missing TCP server launch authority\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t image;
  enum call_status status = directory_lookup(app, "tcp.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open app://tcp.pxe (status %u)\n", status);
    return EXIT_FAILURE;
  }

  enum { MEMORY, CLOCK, LISTENER, OUTPUT, ERROR, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [CLOCK] = {clock, CLOCK_RIGHT_READ, 0},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"clock", CLOCK},
    {(uintptr_t)"tcp_listener", LISTENER},
  };
  const char *arguments[] = {"app://tcp.pxe", "--serve", count};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources, .resource_count = sizeof(resources) / sizeof(resources[0]),
    .argv = (uintptr_t)arguments, .argc = count ? 3 : 2,
  };
  int result = EXIT_FAILURE;
  handle_t listener = HANDLE_INVALID;
  for (unsigned i = STARTUP_STDOUT; i <= STARTUP_STDERR; ++i) {
    struct startup_stream stream = startup_stream(i);
    if (stream.protocol == STARTUP_STREAM_NONE) {
      stream = (struct startup_stream){PROTOCOL_CONSOLE, startup_resource("output")};
    }
    if (stream.handle == HANDLE_INVALID) {
      fputs("session: missing TCP server output stream\n", stderr);
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

  struct tcp_listen_reply reply;
  status = tcp_listen(tcp, address, port, &reply);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot listen on %u.%u.%u.%u:%u (status %u)\n",
        address >> 24, (address >> 16) & 255, (address >> 8) & 255, address & 255,
        port, status);
    goto done;
  }
  listener = reply.handle;
  grants[LISTENER] = (struct launch_grant){listener, TCP_LISTENER_RIGHTS, 0};
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch TCP server (status %u)\n", status);
    goto done;
  }
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  if (listener != HANDLE_INVALID && handle_close(listener) != 0) {
    result = EXIT_FAILURE;
  }
  if (handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}
