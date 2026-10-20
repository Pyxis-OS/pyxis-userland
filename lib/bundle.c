#include <bundle.h>
#include <startup.h>
#include <bundle_launch.h>
#include <package.h>
#include <package_commands.h>
#include <abi/disk.h>
#include <abi/mount.h>
#include <abi/audio.h>
#include <abi/clipboard.h>
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/echo.h>
#include <abi/launcher.h>
#include <abi/memory.h>
#include <abi/pipe.h>
#include <abi/thread.h>
#include <abi/keyboard.h>
#include <abi/pointer.h>
#include <abi/profile.h>
#include <abi/provider.h>
#include <abi/random.h>
#include <abi/retention.h>
#include <abi/screen_capture.h>
#include <abi/wifi.h>
#include <abi/net_config.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/endpoint.h>
#include <abi/udp.h>
#include <file.h>
#include <handle.h>
#include <pxe/p1f.h>
#include <stdlib.h>
#include <string.h>
#include "bundle_json.h"

/* Development lookup budgets, not disk-format or ABI limits. */
#define BUNDLE_JSON_MAX_SIZE (64 * 1024)
#define BUNDLE_COMMAND_LIMIT 32
#define BUNDLE_RESOURCE_LIMIT 8
/* Separate resolver authority preserves existing UDP delivery with one more request. */
#define BUNDLE_GRANT_LIMIT 20
#define BUNDLE_COMPONENT_LIMIT 255
#define BUNDLE_PATH_LIMIT 1024
#define BUNDLE_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)

struct bundle_command {
  const char *name;
  const char *path;
};

struct bundle_program {
  struct bundle_json manifest;
  const char *id;
  const char *entry;
  uint64_t stack_bytes;
  handle_t revision;
  handle_t image;
  struct environment_value entries[BUNDLE_RESOURCE_LIMIT + 1];
  const char *resource_paths[BUNDLE_RESOURCE_LIMIT];
  size_t entry_count;
  struct bundle_command commands[BUNDLE_COMMAND_LIMIT];
  size_t command_count;
  struct bundle_grant_request grants[BUNDLE_GRANT_LIMIT];
  size_t grant_count;
  handle_t package_view;
};

struct grant_right {
  const char *resource;
  const char *right;
  uint64_t protocol;
  uint64_t rights;
};

/* The manifest requests named ordinary interfaces, never numeric authority. */
static const struct grant_right grant_rights[] = {
  {"service", "create", PROTOCOL_ENDPOINT_SERVICE, ENDPOINT_SERVICE_RIGHT_CREATE},
  {"publication", "call", PROTOCOL_ENDPOINT, 0},
  {"memory", "manage", PROTOCOL_MEMORY, MEMORY_RIGHT_MANAGE},
  {"threads", "manage", PROTOCOL_THREAD_SERVICE, THREAD_RIGHT_MANAGE},
  {"keyboard", "input", PROTOCOL_KEYBOARD, KEYBOARD_RIGHT_INPUT},
  {"pointer", "input", PROTOCOL_POINTER, POINTER_RIGHT_INPUT},
  {"job_provisioner", "create", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_CREATE},
  {"job_provisioner", "remove", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_REMOVE},
  {"job_provisioner", "read-files", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_READ_FILES},
  {"job_provisioner", "write-files", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_WRITE_FILES},
  {"job_provisioner", "lookup", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_LOOKUP},
  {"job_provisioner", "enumerate", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_ENUMERATE},
  {"job_work", "create", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_CREATE},
  {"job_work", "remove", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_REMOVE},
  {"job_work", "read-files", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_READ_FILES},
  {"job_work", "write-files", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_WRITE_FILES},
  {"directory", "lookup", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_LOOKUP},
  {"directory", "enumerate", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_ENUMERATE},
  {"directory", "read-files", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_READ_FILES},
  {"directory", "write-files", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_WRITE_FILES},
  {"directory", "create", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_CREATE},
  {"directory", "remove", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_REMOVE},
  {"directory", "filesystem-info", PROTOCOL_DIRECTORY, DIRECTORY_RIGHT_FILESYSTEM_INFO},
  {"provider", "open-read", PROTOCOL_PROVIDER, PROVIDER_RIGHT_OPEN_READ},
  {"mount", "open-root", PROTOCOL_MOUNT, MOUNT_RIGHT_OPEN_ROOT},
  {"mount", "observe", PROTOCOL_MOUNT, MOUNT_RIGHT_OBSERVE},
  {"mount", "write", PROTOCOL_MOUNT, MOUNT_RIGHT_WRITE},
  {"mount", "retire", PROTOCOL_MOUNT, MOUNT_RIGHT_RETIRE},
  {"mount", "interval", PROTOCOL_MOUNT, MOUNT_RIGHT_INTERVAL},
  {"disks", "enumerate", PROTOCOL_DISKS, DISKS_RIGHT_ENUMERATE},
  {"disks", "mount-read-only", PROTOCOL_DISKS, DISKS_RIGHT_MOUNT_READ_ONLY},
  {"disks", "mount-read-write", PROTOCOL_DISKS, DISKS_RIGHT_MOUNT_READ_WRITE},
  {"clock", "read", PROTOCOL_CLOCK, CLOCK_RIGHT_READ},
  {"clock", "sleep", PROTOCOL_CLOCK, CLOCK_RIGHT_SLEEP},
  {"launcher", "launch", PROTOCOL_LAUNCHER, LAUNCHER_RIGHT_LAUNCH},
  {"random", "read", PROTOCOL_RANDOM, RANDOM_RIGHT_READ},
  {"system_info", "read", PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_RIGHT_READ},
  {"net_registry", "read", PROTOCOL_NET_REGISTRY, NET_REGISTRY_RIGHT_READ},
  {"wifi", "read", PROTOCOL_WIFI, WIFI_RIGHT_READ},
  {"wifi", "scan", PROTOCOL_WIFI, WIFI_RIGHT_SCAN},
  {"echo", "send", PROTOCOL_ECHO, ECHO_RIGHT_SEND},
  {"tcp", "connect", PROTOCOL_TCP_SERVICE, TCP_SERVICE_RIGHT_CONNECT},
  {"tcp", "listen", PROTOCOL_TCP_SERVICE, TCP_SERVICE_RIGHT_LISTEN},
  {"pipe", "create", PROTOCOL_PIPE_SERVICE, PIPE_SERVICE_RIGHT_CREATE},
  {"retention_account", "retain", PROTOCOL_RETENTION_ACCOUNT, RETENTION_RIGHT_CAPTURE},
  {"udp", "open", PROTOCOL_UDP_SERVICE, UDP_SERVICE_RIGHT_OPEN},
  {"dns_udp", "open", PROTOCOL_UDP_SERVICE, UDP_SERVICE_RIGHT_OPEN},
  {"display", "draw", PROTOCOL_DISPLAY, DISPLAY_RIGHT_DRAW},
  {"audio", "create", PROTOCOL_AUDIO, AUDIO_RIGHT_PLAYBACK},
  {"screen_capture", "capture", PROTOCOL_SCREEN_CAPTURE, SCREEN_CAPTURE_RIGHT_CAPTURE},
  {"clipboard_local", "publish", PROTOCOL_CLIPBOARD, CLIPBOARD_RIGHT_PUBLISH},
  {"clipboard_local", "paste", PROTOCOL_CLIPBOARD, CLIPBOARD_RIGHT_PASTE},
  {"clipboard_shared", "publish", PROTOCOL_CLIPBOARD, CLIPBOARD_RIGHT_PUBLISH},
  {"clipboard_shared", "paste", PROTOCOL_CLIPBOARD, CLIPBOARD_RIGHT_PASTE},
  {"input", "read", PROTOCOL_CONSOLE, CONSOLE_RIGHT_READ},
  {"output", "write", PROTOCOL_CONSOLE, CONSOLE_RIGHT_WRITE},
  {"profile", "memory", PROTOCOL_PROFILE, PROFILE_RIGHT_MEMORY},
  {"profile", "host", PROTOCOL_PROFILE, PROFILE_RIGHT_HOST},
};

static bool valid_name(const char *name)
{
  size_t size = strlen(name);
  if (!size || size > BUNDLE_COMPONENT_LIMIT || !strcmp(name, ".") || !strcmp(name, "..")) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = name[i];
    if (byte <= 0x20 || byte == 0x7f || byte == '/' || byte == ':' || byte == '\\') {
      return false;
    }
  }
  return true;
}

static bool valid_path(const char *path)
{
  size_t size = strlen(path);
  if (!size || size > BUNDLE_PATH_LIMIT) {
    return false;
  }
  const char *component = path;
  for (size_t i = 0; i <= size; ++i) {
    if (path[i] == ':' || path[i] == '\\') {
      return false;
    }
    if (!path[i] || path[i] == '/') {
      size_t length = path + i - component;
      if (!length || length > BUNDLE_COMPONENT_LIMIT ||
          (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.')) {
        return false;
      }
      component = path + i + 1;
    }
  }
  return true;
}

static bool bundle_suffix(const char *uri)
{
  size_t length = strlen(uri);
  while (length && uri[length - 1] == '/') {
    --length;
  }
  return length > 4 && !memcmp(uri + length - 4, ".pxb", 4);
}



static enum call_status resolve(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, handle_t *handle)
{
  *handle = HANDLE_INVALID;
  size_t length = strlen(path);
  size_t ancestors = context ? context->count : 0;
  if (ancestors == SIZE_MAX || length > SIZE_MAX - ancestors - 1 || length + ancestors + 1 >
      SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  size_t capacity = length + ancestors + 1;
  handle_t *directories = malloc(capacity * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = capacity,
    .component = component, .component_capacity = length + 1,
  };
  enum call_status status = path_resolve_native(context, path, kind, rights, &workspace, handle);
  free(component);
  free(directories);
  return status;
}

static enum call_status resolve_inside(handle_t root, const char *path,
    uint64_t kind, uint64_t rights, handle_t *handle)
{
  /* Confined paths have no scheme or parent traversal; the held root is borrowed. */
  struct path_context context = {.directories = &root, .count = 1, .capacity = 1};
  return resolve(&context, path, kind, rights, handle);
}

static enum call_status require_native(handle_t handle, uint64_t protocol)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE || info.protocol != protocol)) {
    return CALL_WRONG_TYPE;
  }
  return status;
}

static enum call_status read_json(handle_t file, struct bundle_json *json)
{
  uint64_t size;
  enum call_status status = require_native(file, PROTOCOL_FILE);
  if (status != CALL_OK) {
    return status;
  }
  status = file_size(file, &size);
  if (status != CALL_OK) {
    return status;
  }
  if (!size || size > BUNDLE_JSON_MAX_SIZE) {
    return size ? CALL_LIMIT : CALL_BAD_REQUEST;
  }
  char *text = malloc(size + 1);
  if (!text) {
    return CALL_NO_MEMORY;
  }
  size_t position = 0;
  while (position < size) {
    size_t count;
    status = file_read(file, position, text + position, size - position, &count);
    if (status != CALL_OK || !count) {
      free(text);
      return status == CALL_OK ? CALL_BAD_REQUEST : status;
    }
    position += count;
  }
  text[size] = 0;
  return bundle_json_parse(text, size, json);
}

static bool root_conflict(const struct path_context *context, const char *name)
{
  if (!strcmp(name, "app")) {
    return true;
  }
  /* Publication validates policy independently of the publisher's namespace.
   * Launch preparation still checks collisions against the actual child. */
  if (!context) {
    return false;
  }
  struct environment_entry_reply reply;
  enum call_status status = environment_lookup(startup_environment_handle(), name,
      &reply, NULL, 0);
  if (status == CALL_OK && reply.handle) {
    handle_close(reply.handle);
  }
  return status != CALL_NOT_FOUND;
}

static enum call_status parse_commands(struct bundle_program *program,
    const struct bundle_json_node *object)
{
  if (object->type != BUNDLE_JSON_OBJECT) {
    return CALL_BAD_REQUEST;
  }
  const struct bundle_json_node *nodes = program->manifest.nodes;
  for (size_t key = object->child; key; key = nodes[nodes[key].next].next) {
    const struct bundle_json_node *value = &nodes[nodes[key].next];
    if (program->command_count == BUNDLE_COMMAND_LIMIT) {
      return CALL_LIMIT;
    }
    if (!valid_name(nodes[key].string) || value->type != BUNDLE_JSON_STRING ||
        !valid_path(value->string)) {
      return CALL_BAD_REQUEST;
    }
    program->commands[program->command_count++] =
        (struct bundle_command){nodes[key].string, value->string};
  }
  return CALL_OK;
}

static enum call_status parse_resources(struct bundle_program *program,
    const struct bundle_json_node *object)
{
  if (object->type != BUNDLE_JSON_OBJECT) {
    return CALL_BAD_REQUEST;
  }
  const struct bundle_json_node *nodes = program->manifest.nodes;
  for (size_t key = object->child; key; key = nodes[nodes[key].next].next) {
    const struct bundle_json_node *value = &nodes[nodes[key].next];
    if (program->entry_count == BUNDLE_RESOURCE_LIMIT + 1) {
      return CALL_LIMIT;
    }
    if (!valid_name(nodes[key].string) || !strcmp(nodes[key].string, "app") ||
        value->type != BUNDLE_JSON_STRING || !valid_path(value->string)) {
      return CALL_BAD_REQUEST;
    }
    size_t index = program->entry_count++;
    if (!environment_name(program->entries[index].name, nodes[key].string)) {
      return CALL_BAD_REQUEST;
    }
    program->entries[index].kind = ENVIRONMENT_CAP;
    program->entries[index].flags = ENVIRONMENT_EXPORTED;
    program->resource_paths[index - 1] = value->string;
  }
  return CALL_OK;
}

static enum call_status parse_rights(struct bundle_program *program,
    struct bundle_grant_request *request, const struct bundle_json_node *array)
{
  if (array->type != BUNDLE_JSON_ARRAY || !array->child) {
    return CALL_BAD_REQUEST;
  }
  const struct bundle_json_node *nodes = program->manifest.nodes;
  for (size_t index = array->child; index; index = nodes[index].next) {
    if (nodes[index].type != BUNDLE_JSON_STRING) {
      return CALL_BAD_REQUEST;
    }
    const struct grant_right *right = NULL;
    for (size_t i = 0; i < sizeof(grant_rights) / sizeof(*grant_rights); ++i) {
      if (!strcmp(request->resource, grant_rights[i].resource) &&
          !strcmp(nodes[index].string, grant_rights[i].right)) {
        right = &grant_rights[i];
        break;
      }
    }
    for (size_t previous = array->child; previous != index; previous = nodes[previous].next) {
      if (!strcmp(nodes[previous].string, nodes[index].string)) {
        return CALL_BAD_REQUEST;
      }
    }
    if (!right || (request->rights & right->rights)) {
      return CALL_BAD_REQUEST;
    }
    request->protocol = right->protocol;
    request->rights |= right->rights;
  }
  return CALL_OK;
}

static enum call_status parse_grants(struct bundle_program *program,
    const struct bundle_json_node *array)
{
  if (array->type != BUNDLE_JSON_ARRAY) {
    return CALL_BAD_REQUEST;
  }
  const struct bundle_json_node *nodes = program->manifest.nodes;
  for (size_t index = array->child; index; index = nodes[index].next) {
    if (program->grant_count == BUNDLE_GRANT_LIMIT) {
      return CALL_LIMIT;
    }
    if (nodes[index].type != BUNDLE_JSON_OBJECT) {
      return CALL_BAD_REQUEST;
    }
    struct bundle_grant_request *request = &program->grants[program->grant_count];
    const struct bundle_json_node *rights = NULL;
    bool required = false;
    for (size_t key = nodes[index].child; key; key = nodes[nodes[key].next].next) {
      const struct bundle_json_node *value = &nodes[nodes[key].next];
      if (!strcmp(nodes[key].string, "name")) {
        if (value->type != BUNDLE_JSON_STRING || !valid_name(value->string)) {
          return CALL_BAD_REQUEST;
        }
        request->name = value->string;
      } else if (!strcmp(nodes[key].string, "resource")) {
        if (value->type != BUNDLE_JSON_STRING) {
          return CALL_BAD_REQUEST;
        }
        request->resource = value->string;
      } else if (!strcmp(nodes[key].string, "source")) {
        if (value->type != BUNDLE_JSON_STRING || !valid_name(value->string)) {
          return CALL_BAD_REQUEST;
        }
        /* The packages volume is reached per package ID, never as a whole. */
        if (!strcmp(value->string, PACKAGE_SOURCE)) {
          return CALL_BAD_REQUEST;
        }
        request->source = value->string;
      } else if (!strcmp(nodes[key].string, "package")) {
        if (value->type != BUNDLE_JSON_STRING || !package_id_valid(value->string)) {
          return CALL_BAD_REQUEST;
        }
        request->package = value->string;
      } else if (!strcmp(nodes[key].string, "root")) {
        if (value->type != BUNDLE_JSON_STRING || !valid_name(value->string)) {
          return CALL_BAD_REQUEST;
        }
        request->root = value->string;
      } else if (!strcmp(nodes[key].string, "rights")) {
        rights = value;
      } else if (!strcmp(nodes[key].string, "required")) {
        if (value->type != BUNDLE_JSON_BOOLEAN) {
          return CALL_BAD_REQUEST;
        }
        required = true;
        request->required = value->integer;
      } else {
        return CALL_BAD_REQUEST;
      }
    }
    if (!request->name || !request->resource || !rights || !required ||
        !strcmp(request->name, "app") ||
        !strncmp(request->name, BUNDLE_SOURCE_PREFIX, sizeof(BUNDLE_SOURCE_PREFIX) - 1)) {
      return CALL_BAD_REQUEST;
    }
    for (size_t i = 0; i < program->grant_count; ++i) {
      if (!strcmp(program->grants[i].name, request->name)) {
        return CALL_BAD_REQUEST;
      }
    }
    enum call_status status = parse_rights(program, request, rights);
    if (status != CALL_OK) {
      return status;
    }
    bool external = !strcmp(request->resource, "directory") ||
        !strcmp(request->resource, "mount") || !strcmp(request->resource, "disks") ||
        !strcmp(request->resource, "provider");
    bool directory = !strcmp(request->resource, "directory");
    bool selected = request->source || request->package;
    if (external != selected || (request->source && request->package) ||
        (request->package && (!directory || (request->rights & ~PACKAGE_TREE_RIGHTS))) ||
        (request->root && (!directory ||
         !(request->rights & DIRECTORY_RIGHT_LOOKUP)))) {
      return CALL_BAD_REQUEST;
    }
    ++program->grant_count;
  }
  return CALL_OK;
}

static enum call_status parse_manifest(const struct path_context *context,
    struct bundle_program *program)
{
  const struct bundle_json_node *nodes = program->manifest.nodes;
  if (nodes[1].type != BUNDLE_JSON_OBJECT) {
    return CALL_BAD_REQUEST;
  }
  bool format = false, commands = false, resources = false, grants = false;
  memcpy(program->entries[0].name, "app", sizeof("app"));
  program->entries[0].kind = ENVIRONMENT_CAP;
  program->entries[0].flags = ENVIRONMENT_EXPORTED;
  program->entry_count = 1;
  for (size_t key = nodes[1].child; key; key = nodes[nodes[key].next].next) {
    const struct bundle_json_node *value = &nodes[nodes[key].next];
    const char *name = nodes[key].string;
    enum call_status status = CALL_OK;
    if (!strcmp(name, "format")) {
      format = value->type == BUNDLE_JSON_INTEGER && value->integer == 1;
      if (!format) {
        return CALL_BAD_REQUEST;
      }
    } else if (!strcmp(name, "id")) {
      if (value->type != BUNDLE_JSON_STRING || !valid_name(value->string)) {
        return CALL_BAD_REQUEST;
      }
      program->id = value->string;
    } else if (!strcmp(name, "entry")) {
      if (value->type != BUNDLE_JSON_STRING || !valid_path(value->string)) {
        return CALL_BAD_REQUEST;
      }
      program->entry = value->string;
    } else if (!strcmp(name, "commands")) {
      commands = true;
      status = parse_commands(program, value);
    } else if (!strcmp(name, "stack_bytes")) {
      if (value->type != BUNDLE_JSON_INTEGER ||
          value->integer < LAUNCH_INITIAL_STACK_MIN_SIZE ||
          value->integer > LAUNCH_INITIAL_STACK_MAX_SIZE ||
          value->integer % MEMORY_PAGE_SIZE) {
        return CALL_BAD_REQUEST;
      }
      program->stack_bytes = value->integer;
    } else if (!strcmp(name, "resource_dirs")) {
      resources = true;
      status = parse_resources(program, value);
    } else if (!strcmp(name, "grants")) {
      grants = true;
      status = parse_grants(program, value);
    } else {
      return CALL_BAD_REQUEST;
    }
    if (status != CALL_OK) {
      return status;
    }
  }
  if (!format || !program->id || !program->entry || !commands || !resources || !grants) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 1; i < program->entry_count; ++i) {
    for (size_t j = 0; j < program->grant_count; ++j) {
      const char *name = program->entries[i].name;
      if (!strcmp(name, program->grants[j].name) ||
          (program->grants[j].root && !strcmp(name, program->grants[j].root))) {
        return CALL_BAD_REQUEST;
      }
    }
  }
  for (size_t i = 0; i < program->grant_count; ++i) {
    const struct bundle_grant_request *request = &program->grants[i];
    if (!request->root) {
      continue;
    }
    if (root_conflict(context, request->root)) {
      return CALL_BAD_REQUEST;
    }
    for (size_t j = 0; j < program->grant_count; ++j) {
      if (!strcmp(request->root, program->grants[j].name) ||
          (j != i && program->grants[j].root &&
           !strcmp(request->root, program->grants[j].root))) {
        return CALL_BAD_REQUEST;
      }
    }
  }
  return CALL_OK;
}

static enum call_status open_entry(struct bundle_program *program, const char *path,
    handle_t *image)
{
  enum call_status status = resolve_inside(program->entries[0].handle, path,
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, image);
  if (status != CALL_OK) {
    return status;
  }
  status = require_native(*image, PROTOCOL_FILE);
  uint64_t magic = 0;
  size_t position = 0;
  while (status == CALL_OK && position < sizeof(magic)) {
    size_t count;
    status = file_read(*image, position, (char *)&magic + position,
        sizeof(magic) - position, &count);
    if (status != CALL_OK || !count) {
      status = status == CALL_OK ? CALL_BAD_REQUEST : status;
      break;
    }
    position += count;
  }
  if (status == CALL_OK && magic != P1F_MAGIC) {
    status = CALL_BAD_REQUEST;
  }
  if (status != CALL_OK) {
    handle_close(*image);
    *image = HANDLE_INVALID;
  }
  return status;
}

void bundle_program_close(struct bundle_program *program)
{
  if (!program) {
    return;
  }
  if (program->image != HANDLE_INVALID) {
    handle_close(program->image);
  }
  for (size_t i = 0; i < program->entry_count; ++i) {
    if (program->entries[i].handle != HANDLE_INVALID) {
      handle_close(program->entries[i].handle);
    }
  }
  if (program->revision != HANDLE_INVALID) {
    handle_close(program->revision);
  }
  if (program->package_view != HANDLE_INVALID) {
    handle_close(program->package_view);
  }
  bundle_json_close(&program->manifest);
  free(program);
}

static enum call_status open_revision(const struct path_context *context,
    handle_t revision, const char *command, struct bundle_program **output)
{
  *output = NULL;
  struct bundle_program *program = calloc(1, sizeof(*program));
  if (!program) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = handle_copy_restricted(revision,
      BUNDLE_DIRECTORY_RIGHTS, 0, &program->revision);
  handle_t manifest = HANDLE_INVALID;
  if (status == CALL_OK) {
    status = require_native(program->revision, PROTOCOL_DIRECTORY);
  }
  if (status == CALL_OK) {
    status = directory_lookup(program->revision, "manifest.json", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &manifest);
  }
  if (status == CALL_OK) {
    status = read_json(manifest, &program->manifest);
  }
  if (manifest != HANDLE_INVALID) {
    handle_close(manifest);
  }
  if (status == CALL_OK) {
    status = parse_manifest(context, program);
    /* Names and paths borrow decoded text, not the parse tree. */
    free(program->manifest.nodes);
    program->manifest.nodes = NULL;
  }
  if (status == CALL_OK) {
    status = directory_lookup(program->revision, "app", DIRECTORY_KIND_DIRECTORY,
        BUNDLE_DIRECTORY_RIGHTS, &program->entries[0].handle);
  }
  if (status == CALL_OK) {
    status = require_native(program->entries[0].handle, PROTOCOL_DIRECTORY);
  }
  for (size_t i = 1; status == CALL_OK && i < program->entry_count; ++i) {
    status = resolve_inside(program->entries[0].handle, program->resource_paths[i - 1],
        DIRECTORY_KIND_DIRECTORY, BUNDLE_DIRECTORY_RIGHTS, &program->entries[i].handle);
    if (status == CALL_OK) {
      status = require_native(program->entries[i].handle, PROTOCOL_DIRECTORY);
    }
  }
  if (status == CALL_OK) {
    status = open_entry(program, program->entry, &program->image);
  }
  bool selected = command == NULL;
  for (size_t i = 0; status == CALL_OK && i < program->command_count; ++i) {
    handle_t image = HANDLE_INVALID;
    status = open_entry(program, program->commands[i].path, &image);
    if (status == CALL_OK && command && !strcmp(command, program->commands[i].name)) {
      handle_close(program->image);
      program->image = image;
      selected = true;
    } else if (image != HANDLE_INVALID) {
      handle_close(image);
    }
  }
  if (status == CALL_OK && !selected) {
    status = CALL_BAD_REQUEST;
  }
  if (status != CALL_OK) {
    /* Once selected, a missing member is a broken bundle, not a lookup miss. */
    if (status == CALL_NOT_FOUND && program->revision != HANDLE_INVALID) {
      status = CALL_BAD_REQUEST;
    }
    bundle_program_close(program);
    return status;
  }
  *output = program;
  return CALL_OK;
}

static enum call_status open_bundle(const struct path_context *context, const char *uri,
    const char *command, struct bundle_program **output)
{
  *output = NULL;
  if (!bundle_suffix(uri)) {
    return CALL_BAD_REQUEST;
  }
  handle_t revision = HANDLE_INVALID;
  enum call_status status = resolve(context, uri, DIRECTORY_KIND_DIRECTORY,
      BUNDLE_DIRECTORY_RIGHTS, &revision);
  if (status == CALL_OK) {
    /* URI opening keeps its existing startup-namespace collision checks even
     * when it uses the implicit path context. Only the new held-directory
     * validation entry point accepts an absent publication namespace. */
    const struct path_context implicit = {0};
    status = open_revision(context ? context : &implicit, revision, command, output);
    handle_close(revision);
  }
  return status;
}

enum call_status bundle_open_directory(const struct path_context *context,
    handle_t revision, const char *command, struct bundle_program **program)
{
  if (!program) {
    return CALL_BAD_REQUEST;
  }
  *program = NULL;
  if (!revision || (command && !valid_name(command))) {
    return CALL_BAD_REQUEST;
  }
  return open_revision(context, revision, command, program);
}

enum call_status bundle_open_package_directory(const struct path_context *context,
    handle_t revision, const char *command, handle_t fixed_view,
    struct bundle_program **program)
{
  if (!program) {
    return CALL_BAD_REQUEST;
  }
  *program = NULL;
  struct package_command_info info;
  enum call_status status = package_commands_info(fixed_view, &info);
  if (status == CALL_OK && ((info.flags & PACKAGE_COMMAND_VIEW_LIVE) || !info.set ||
      !command || !valid_name(command))) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK) {
    status = open_revision(context, revision, command, program);
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(fixed_view, BUNDLE_DIRECTORY_RIGHTS, 0,
        &(*program)->package_view);
    if (status != CALL_OK) {
      bundle_program_close(*program);
      *program = NULL;
    }
  }
  return status;
}

handle_t bundle_program_package_view(const struct bundle_program *program)
{
  return program ? program->package_view : HANDLE_INVALID;
}

enum call_status bundle_open(const struct path_context *context,
    const char *bundle_uri, struct bundle_program **program)
{
  if (!program) {
    return CALL_BAD_REQUEST;
  }
  *program = NULL;
  if (!bundle_uri || !*bundle_uri) {
    return CALL_BAD_REQUEST;
  }
  return open_bundle(context, bundle_uri, NULL, program);
}


handle_t bundle_program_image(const struct bundle_program *program)
{
  return program ? program->image : HANDLE_INVALID;
}

uint64_t bundle_program_stack_bytes(const struct bundle_program *program)
{
  return program ? program->stack_bytes : 0;
}

const struct environment_value *bundle_program_entries(const struct bundle_program *program,
    size_t *count)
{
  if (count) {
    *count = program ? program->entry_count : 0;
  }
  return program ? program->entries : NULL;
}

const struct bundle_grant_request *bundle_program_grants(const struct bundle_program *program,
    size_t *count)
{
  if (count) {
    *count = program ? program->grant_count : 0;
  }
  return program ? program->grants : NULL;
}
