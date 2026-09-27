#include <abi/console.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <console.h>
#include <directory.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdio.h>
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

static enum call_status open_content(handle_t app, handle_t *content)
{
  handle_t share;
  enum call_status status = directory_lookup(app, "share", DIRECTORY_KIND_DIRECTORY,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &share);
  if (status != CALL_OK) {
    return status;
  }
  status = directory_lookup(share, "hello.txt", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, content);
  if (handle_close(share) != 0 && status == CALL_OK) {
    handle_close(*content);
    *content = HANDLE_INVALID;
    return CALL_BAD_HANDLE;
  }
  return status;
}

static enum call_status launch_client(handle_t launcher, handle_t image,
    handle_t output, handle_t memory, handle_t caller, handle_t content,
    const char *mode, const char *id, handle_t *child)
{
  enum { OUTPUT, MEMORY, CALLER, CONTENT, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [OUTPUT] = {output, CONSOLE_RIGHT_WRITE},
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE},
    [CALLER] = {caller, ENDPOINT_RIGHT_CALL},
    [CONTENT] = {content, FILE_RIGHT_READ},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"memory", MEMORY},
    {(uintptr_t)"endpoint", CALLER},
    {(uintptr_t)"content", CONTENT},
  };
  const char *arguments[] = {"client.pxe", mode, id};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources,
    .resource_count = sizeof(resources) / sizeof(resources[0]),
    .argv = (uintptr_t)arguments,
    .argc = sizeof(arguments) / sizeof(arguments[0]),
  };
  return launcher_launch(launcher, &request, child);
}

static uint64_t print_content(const struct endpoint_packet *packet, handle_t output)
{
  if (packet->grant_count == 0) {
    return CONTENT_INVALID;
  }
  char buffer[128];
  uint64_t offset = 0;
  for (;;) {
    size_t count;
    if (file_read(packet->grants[0].handle, offset, buffer, sizeof(buffer), &count) != CALL_OK) {
      return CONTENT_IO_ERROR;
    }
    if (count == 0) {
      return CONTENT_OK;
    }
    if (count > UINT64_MAX - offset ||
        console_write_all(output, buffer, count) != CALL_OK) {
      return CONTENT_IO_ERROR;
    }
    offset += count;
  }
}

static bool finish_request(struct endpoint_packet *packet, handle_t output, bool abandon)
{
  bool valid = packet->size == sizeof(struct content_request) ||
      packet->size == ENDPOINT_DATA_MAX;
  struct content_request request;
  if (valid) {
    memcpy(&request, packet->data, sizeof(request));
    valid = request.operation == CONTENT_PRINT && request.client >= 1 &&
        request.client <= 2 && packet->grant_count ==
        (packet->size == ENDPOINT_DATA_MAX ? ENDPOINT_GRANTS_MAX : 1);
  }
  if (valid && packet->size == ENDPOINT_DATA_MAX) {
    for (size_t i = sizeof(request); i < packet->size; ++i) {
      if (packet->data[i] != (uint8_t)i) {
        valid = false;
        break;
      }
    }
  }
  uint64_t application_result = valid ? print_content(packet, output) : CONTENT_INVALID;
  enum call_status status;
  if (abandon) {
    status = handle_close(packet->receipt) == 0 ? CALL_OK : CALL_BAD_HANDLE;
  } else {
    status = endpoint_reply(packet->receipt, application_result,
        packet->data, packet->size, packet->grants, packet->grant_count);
    if (status != CALL_OK) {
      handle_close(packet->receipt);
    }
  }
  return close_grants(packet) && status == CALL_OK;
}

static bool wait_client(handle_t child)
{
  struct process_result result;
  return process_wait(child, &result) == CALL_OK &&
      result.kind != PROCESS_FAULTED && result.exit_status == 0;
}

static bool from_client_one(const struct endpoint_packet *packet)
{
  if (packet->size < sizeof(struct content_request)) {
    return false;
  }
  struct content_request request;
  memcpy(&request, packet->data, sizeof(request));
  return request.client == 1;
}

int main(int argc, char **argv)
{
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "--wide") &&
      strcmp(argv[1], "--abandon"))) {
    return 1;
  }
  const char *mode = argc == 1 ? "normal" : argv[1] + 2;
  handle_t service = startup_resource("service");
  handle_t launcher = startup_resource("launcher");
  handle_t memory = startup_resource("memory");
  handle_t output = startup_resource("output");
  handle_t app = startup_root("app");
  if (service == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      memory == HANDLE_INVALID || output == HANDLE_INVALID || app == HANDLE_INVALID) {
    return 1;
  }

  struct endpoint_create_reply endpoint;
  enum call_status status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    return 1;
  }
  handle_t image = HANDLE_INVALID, content = HANDLE_INVALID;
  handle_t children[2] = {HANDLE_INVALID, HANDLE_INVALID};
  bool ok = false;
  status = directory_lookup(app, "client.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK || open_content(app, &content) != CALL_OK) {
    goto done;
  }
  for (size_t i = 0; i < 2; ++i) {
    status = launch_client(launcher, image, output, memory, endpoint.caller,
        content, mode, i == 0 ? "1" : "2", &children[i]);
    if (status != CALL_OK) {
      goto done;
    }
  }
  if (handle_close(endpoint.caller) != 0) {
    goto done;
  }
  endpoint.caller = HANDLE_INVALID;
  if (handle_close(content) != 0) {
    goto done;
  }
  content = HANDLE_INVALID;
  if (handle_close(image) != 0) {
    goto done;
  }
  image = HANDLE_INVALID;

  struct endpoint_packet first, second;
  if (endpoint_receive(endpoint.receiver, &first) != CALL_OK) {
    goto done;
  }
  if (endpoint_receive(endpoint.receiver, &second) != CALL_OK) {
    handle_close(first.receipt);
    close_grants(&first);
    goto done;
  }
  /* Both calls remain admitted while we complete the second before the first. */
  bool abandon = !strcmp(mode, "abandon");
  bool reverse_ok = finish_request(&second, output, abandon && from_client_one(&second));
  bool first_ok = finish_request(&first, output, abandon && from_client_one(&first));
  ok = reverse_ok && first_ok;
  for (size_t i = 0; i < 2; ++i) {
    if (!wait_client(children[i])) {
      ok = false;
    }
  }
  if (console_print(output, ok ? "server: complete\n" : "server: failed\n") != CALL_OK) {
    ok = false;
  }

done:
  if (endpoint.caller != HANDLE_INVALID && handle_close(endpoint.caller) != 0) {
    ok = false;
  }
  if (endpoint.receiver != HANDLE_INVALID && handle_close(endpoint.receiver) != 0) {
    ok = false;
  }
  if (content != HANDLE_INVALID && handle_close(content) != 0) {
    ok = false;
  }
  if (image != HANDLE_INVALID && handle_close(image) != 0) {
    ok = false;
  }
  for (size_t i = 0; i < 2; ++i) {
    if (children[i] != HANDLE_INVALID && handle_close(children[i]) != 0) {
      ok = false;
    }
  }
  return ok ? 0 : 1;
}
