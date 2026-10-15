#include <clock.h>
#include <environment.h>
#include <file.h>
#include <handle.h>
#include <http_prepare.h>
#include <path.h>
#include <startup.h>
#include <syscall.h>
#include <pyxis/stdio.h>
#include <pyxis/working_path.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECOND_NS UINT64_C(1000000000)
#define DEFAULT_TIMEOUT_NS (UINT64_C(30) * SECOND_NS)
#define HEADERS_CAPACITY (ENDPOINT_DATA_MAX - sizeof(struct http_endpoint_request))

static void erase(void *bytes, size_t size)
{
  volatile unsigned char *cursor = bytes;
  while (size--) {
    *cursor++ = 0;
  }
}

static bool parse_timeout(const char *text, uint64_t *timeout_ns)
{
  uint64_t seconds = 0;
  if (!*text) {
    return false;
  }
  while (*text) {
    unsigned char digit = *text++;
    if (digit < '0' || digit > '9' ||
        seconds > (UINT64_MAX / SECOND_NS - (digit - '0')) / 10) {
      return false;
    }
    seconds = seconds * 10 + digit - '0';
  }
  if (!seconds) {
    return false;
  }
  *timeout_ns = seconds * SECOND_NS;
  return true;
}

static enum call_status capture_root(const char *uri, handle_t *root, const char **target)
{
  const char *separator = strstr(uri, "://");
  if (!separator || separator == uri || separator - uri > ENVIRONMENT_NAME_MAX) {
    return CALL_BAD_REQUEST;
  }
  char name[ENVIRONMENT_NAME_MAX + 1];
  size_t size = separator - uri;
  memcpy(name, uri, size);
  name[size] = 0;
  *target = separator + 3;
  return environment_capability(startup_environment_handle(), name, root);
}

static enum call_status resolve_headers(const struct path_context *context, const char *path,
    struct path_workspace *workspace, handle_t *file)
{
  if (!strstr(path, "://")) {
    return path_resolve_native(context, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        workspace, file);
  }
  handle_t root = HANDLE_INVALID;
  const char *tail;
  enum call_status status = capture_root(path, &root, &tail);
  if (status != CALL_OK) {
    return status;
  }
  struct handle_info info;
  status = handle_query(root, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE || info.transport)) {
    status = CALL_WRONG_TYPE;
  }
  if (status == CALL_OK && info.protocol == PROTOCOL_FILE) {
    status = *tail ? CALL_BAD_REQUEST : handle_copy_restricted(root, FILE_RIGHT_READ, 0, file);
  } else if (status == CALL_OK && info.protocol == PROTOCOL_DIRECTORY) {
    size_t length = strlen(tail);
    char *native_path = length <= SIZE_MAX - 3 ? malloc(length + 3) : NULL;
    if (!native_path) {
      status = CALL_NO_MEMORY;
    } else {
      /* Force native traversal of the captured subtree, even if its first
       * component contains another apparent name:// prefix. */
      memcpy(native_path, "./", 2);
      memcpy(native_path + 2, tail, length + 1);
      struct path_context subtree = {.directories = &root, .count = 1, .capacity = 1};
      status = path_resolve_native(&subtree, native_path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
          workspace, file);
      free(native_path);
    }
  } else if (status == CALL_OK) {
    status = CALL_WRONG_TYPE;
  }
  if (handle_close(root) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (status != CALL_OK && *file != HANDLE_INVALID) {
    handle_close(*file);
    *file = HANDLE_INVALID;
  }
  return status;
}

/* The caller already attested a native FILE. Keep the temporary reply here so
 * both this copy and the final private header buffer can be erased. */
static enum call_status read_private(handle_t file, uint64_t offset, void *bytes,
    size_t capacity, size_t *count)
{
  *count = 0;
  if (capacity > FILE_READ_MAX_BYTES) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct file_read_request body;
  } message = {
    .header = {PROTOCOL_FILE, FILE_READ}, .body = {offset, capacity},
  };
  struct {
    struct file_read_reply body;
    unsigned char bytes[FILE_READ_MAX_BYTES];
  } reply = {0};
  struct syscall_result result = syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply.body) + capacity);
  enum call_status status = CALL_BAD_REQUEST;
  if (result.status < CALL_STATUS_COUNT) {
    if (result.status != CALL_OK) {
      status = result.reply_size ? CALL_BAD_REQUEST : (enum call_status)result.status;
    } else if (result.reply_size >= sizeof(reply.body) &&
        result.reply_size <= sizeof(reply.body) + capacity &&
        reply.body.read == result.reply_size - sizeof(reply.body) &&
        reply.body.read <= UINT64_MAX - offset) {
      *count = reply.body.read;
      memcpy(bytes, reply.bytes, *count);
      status = CALL_OK;
    }
  }
  erase(&reply, sizeof(reply));
  return status;
}

static enum call_status read_headers(const char *path, unsigned char *bytes,
    size_t capacity, size_t *size)
{
  *size = 0;
  const struct path_context *context = NULL;
  enum call_status status = CALL_OK;
  if (!strstr(path, "://")) {
    status = pyxis_working_context(&context);
    if (status != CALL_OK) {
      return status;
    }
  }
  size_t depth = context ? context->count : 0;
  size_t length = strlen(path);
  if (length == SIZE_MAX || depth >= SIZE_MAX / sizeof(handle_t) ||
      length >= SIZE_MAX / sizeof(handle_t) - depth) {
    return CALL_LIMIT;
  }
  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 3);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 3,
  };
  handle_t file = HANDLE_INVALID;
  status = resolve_headers(context, path, &workspace, &file);
  free(component);
  free(directories);
  if (status != CALL_OK) {
    return status;
  }
  struct handle_info info;
  status = handle_query(file, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != PROTOCOL_FILE || info.rights != FILE_RIGHT_READ || info.transport)) {
    status = CALL_DENIED;
  }
  uint64_t length_bytes = 0;
  if (status == CALL_OK) {
    status = file_size(file, &length_bytes);
  }
  if (status == CALL_OK && length_bytes > capacity) {
    status = CALL_FILE_TOO_LARGE;
  }
  if (status == CALL_OK && length_bytes) {
    size_t count;
    status = read_private(file, 0, bytes, (size_t)length_bytes, &count);
    if (status == CALL_OK && count != length_bytes) {
      status = CALL_IO;
    }
  }
  unsigned char extra = 0;
  if (status == CALL_OK) {
    size_t count;
    status = read_private(file, length_bytes, &extra, 1, &count);
    if (status == CALL_OK && count) {
      status = CALL_IO;
    }
  }
  erase(&extra, sizeof(extra));
  if (handle_close(file) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (status == CALL_OK) {
    *size = length_bytes;
  }
  return status;
}

static int usage(void)
{
  fputs("usage: http prepare POST|PUT|PATCH URI [--headers-file PATH] [--timeout SECONDS]\n",
      stderr);
  return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  if (argc < 4 || strcmp(argv[1], "prepare")) {
    return usage();
  }
  uint64_t method;
  if (!strcmp(argv[2], "POST")) {
    method = HTTP_ENDPOINT_POST;
  } else if (!strcmp(argv[2], "PUT")) {
    method = HTTP_ENDPOINT_PUT;
  } else if (!strcmp(argv[2], "PATCH")) {
    method = HTTP_ENDPOINT_PATCH;
  } else {
    return usage();
  }
  const char *headers_path = NULL;
  uint64_t timeout_ns = DEFAULT_TIMEOUT_NS;
  bool timeout_given = false;
  for (int i = 4; i < argc; i += 2) {
    if (i + 1 == argc) {
      return usage();
    }
    if (!strcmp(argv[i], "--headers-file") && !headers_path) {
      headers_path = argv[i + 1];
    } else if (!strcmp(argv[i], "--timeout") && !timeout_given &&
        parse_timeout(argv[i + 1], &timeout_ns)) {
      timeout_given = true;
    } else {
      return usage();
    }
  }
  handle_t root = HANDLE_INVALID;
  const char *target = NULL;
  enum call_status status = capture_root(argv[3], &root, &target);
  size_t target_size = target ? strlen(target) : 0;
  if (status == CALL_OK && target_size > HEADERS_CAPACITY) {
    status = CALL_FILE_TOO_LARGE;
  }
  unsigned char headers[HEADERS_CAPACITY] = {0};
  size_t headers_size = 0;
  if (status == CALL_OK && headers_path) {
    status = read_headers(headers_path, headers, HEADERS_CAPACITY - target_size, &headers_size);
  }
  handle_t clock = HANDLE_INVALID;
  uint64_t now = 0;
  if (status == CALL_OK) {
    status = environment_capability(startup_environment_handle(), "clock", &clock);
  }
  if (status == CALL_OK) {
    status = clock_now(clock, &now);
  }
  if (status == CALL_OK && now > UINT64_MAX - DEFAULT_TIMEOUT_NS) {
    status = CALL_LIMIT;
  }
  struct resource_result result = {0};
  if (status == CALL_OK) {
    status = http_prepare_endpoint(root, method, target, target_size, headers, headers_size,
        timeout_ns, now + DEFAULT_TIMEOUT_NS, &result);
  }
  erase(headers, sizeof(headers));
  if (clock != HANDLE_INVALID && handle_close(clock) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (root != HANDLE_INVALID && handle_close(root) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  enum call_status outcome = status == CALL_OK ?
      resource_outcome_status(result.info.description.outcome) : status;
  if (status == CALL_OK && result.resource != HANDLE_INVALID) {
    struct output_handoff_request owned = {result.resource, HANDLE_INVALID};
    status = pyxis_stdio_handoff_owned(stdout, &owned);
    result.resource = owned.value;
  }
  enum call_status cleanup = resource_result_close(&result);
  if (status == CALL_OK) {
    status = outcome != CALL_OK ? outcome : cleanup;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "http: prepare failed (status %u)\n", status);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
