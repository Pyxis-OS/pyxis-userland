#include <bundle.h>
#include <abi/audio.h>
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/echo.h>
#include <abi/launcher.h>
#include <abi/memory.h>
#include <abi/profile.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <file.h>
#include <handle.h>
#include <pxe/p1f.h>
#include <stdlib.h>
#include <string.h>
#include "bundle_json.h"

/* Development lookup budgets, not disk-format or ABI limits. */
#define BUNDLE_JSON_MAX_SIZE (64 * 1024)
#define BUNDLE_COUNT_LIMIT 8
#define BUNDLE_COMMAND_LIMIT 32
#define BUNDLE_RESOURCE_LIMIT 8
#define BUNDLE_GRANT_LIMIT 16
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
  struct startup_binding roots[BUNDLE_RESOURCE_LIMIT + 1];
  const char *resource_paths[BUNDLE_RESOURCE_LIMIT];
  size_t root_count;
  struct bundle_command commands[BUNDLE_COMMAND_LIMIT];
  size_t command_count;
  struct bundle_grant_request grants[BUNDLE_GRANT_LIMIT];
  size_t grant_count;
};

struct grant_right {
  const char *resource;
  const char *right;
  uint64_t protocol;
  uint64_t rights;
};

/* The manifest requests named ordinary interfaces, never numeric authority. */
static const struct grant_right grant_rights[] = {
  {"memory", "manage", PROTOCOL_MEMORY, MEMORY_RIGHT_MANAGE},
  {"clock", "read", PROTOCOL_CLOCK, CLOCK_RIGHT_READ},
  {"clock", "sleep", PROTOCOL_CLOCK, CLOCK_RIGHT_SLEEP},
  {"launcher", "launch", PROTOCOL_LAUNCHER, LAUNCHER_RIGHT_LAUNCH},
  {"random", "read", PROTOCOL_RANDOM, RANDOM_RIGHT_READ},
  {"system_info", "read", PROTOCOL_SYSTEM_INFO, SYSTEM_INFO_RIGHT_READ},
  {"echo", "send", PROTOCOL_ECHO, ECHO_RIGHT_SEND},
  {"tcp", "connect", PROTOCOL_TCP_SERVICE, TCP_SERVICE_RIGHT_CONNECT},
  {"tcp", "listen", PROTOCOL_TCP_SERVICE, TCP_SERVICE_RIGHT_LISTEN},
  {"udp", "open", PROTOCOL_UDP_SERVICE, UDP_SERVICE_RIGHT_OPEN},
  {"display", "draw", PROTOCOL_DISPLAY, DISPLAY_RIGHT_DRAW},
  {"audio", "create", PROTOCOL_AUDIO, AUDIO_RIGHT_PLAYBACK},
  {"screen_capture", "capture", PROTOCOL_SCREEN_CAPTURE, SCREEN_CAPTURE_RIGHT_CAPTURE},
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
  return length > 4 && !strcmp(uri + length - 4, ".pxb");
}

static bool explicit_uri(const char *uri)
{
  const char *end = uri;
  while (*end && *end != ':' && *end != '/') {
    if ((unsigned char)*end <= 0x20) {
      return false;
    }
    ++end;
  }
  return end != uri && *end == ':' && end[1] == '/' && end[2] == '/';
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
  if (context && context->roots) {
    for (size_t i = 0; i < context->root_count; ++i) {
      if (!strcmp(context->roots[i].name, name)) {
        return true;
      }
    }
  } else {
    const struct startup_binding *roots = startup_roots();
    for (size_t i = 0; i < startup_root_count(); ++i) {
      if (!strcmp((const char *)(uintptr_t)roots[i].name, name)) {
        return true;
      }
    }
  }
  return startup_resource(name) != HANDLE_INVALID;
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

static enum call_status parse_resources(const struct path_context *context,
    struct bundle_program *program, const struct bundle_json_node *object)
{
  if (object->type != BUNDLE_JSON_OBJECT) {
    return CALL_BAD_REQUEST;
  }
  const struct bundle_json_node *nodes = program->manifest.nodes;
  for (size_t key = object->child; key; key = nodes[nodes[key].next].next) {
    const struct bundle_json_node *value = &nodes[nodes[key].next];
    if (program->root_count == BUNDLE_RESOURCE_LIMIT + 1) {
      return CALL_LIMIT;
    }
    if (!valid_name(nodes[key].string) || root_conflict(context, nodes[key].string) ||
        value->type != BUNDLE_JSON_STRING || !valid_path(value->string)) {
      return CALL_BAD_REQUEST;
    }
    size_t index = program->root_count++;
    program->roots[index].name = (uintptr_t)nodes[key].string;
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
        !strcmp(request->name, "app")) {
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
  program->roots[0].name = (uintptr_t)"app";
  program->root_count = 1;
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
      status = parse_resources(context, program, value);
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
  for (size_t i = 1; i < program->root_count; ++i) {
    for (size_t j = 0; j < program->grant_count; ++j) {
      if (!strcmp((const char *)(uintptr_t)program->roots[i].name, program->grants[j].name)) {
        return CALL_BAD_REQUEST;
      }
    }
  }
  return CALL_OK;
}

static enum call_status open_entry(struct bundle_program *program, const char *path,
    handle_t *image)
{
  enum call_status status = resolve_inside(program->roots[0].handle, path,
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
  for (size_t i = 0; i < program->root_count; ++i) {
    if (program->roots[i].handle != HANDLE_INVALID) {
      handle_close(program->roots[i].handle);
    }
  }
  if (program->revision != HANDLE_INVALID) {
    handle_close(program->revision);
  }
  bundle_json_close(&program->manifest);
  free(program);
}

static enum call_status open_bundle(const struct path_context *context, const char *uri,
    const char *command, struct bundle_program **output)
{
  *output = NULL;
  if (!bundle_suffix(uri)) {
    return CALL_BAD_REQUEST;
  }
  struct bundle_program *program = calloc(1, sizeof(*program));
  if (!program) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = resolve(context, uri, DIRECTORY_KIND_DIRECTORY,
      BUNDLE_DIRECTORY_RIGHTS, &program->revision);
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
        BUNDLE_DIRECTORY_RIGHTS, &program->roots[0].handle);
  }
  if (status == CALL_OK) {
    status = require_native(program->roots[0].handle, PROTOCOL_DIRECTORY);
  }
  for (size_t i = 1; status == CALL_OK && i < program->root_count; ++i) {
    status = resolve_inside(program->roots[0].handle, program->resource_paths[i - 1],
        DIRECTORY_KIND_DIRECTORY, BUNDLE_DIRECTORY_RIGHTS, &program->roots[i].handle);
    if (status == CALL_OK) {
      status = require_native(program->roots[i].handle, PROTOCOL_DIRECTORY);
    }
  }
  if (status == CALL_OK) {
    status = open_entry(program, program->entry, &program->image);
  }
  for (size_t i = 0; status == CALL_OK && i < program->command_count; ++i) {
    handle_t image = HANDLE_INVALID;
    status = open_entry(program, program->commands[i].path, &image);
    if (status == CALL_OK && command && !strcmp(command, program->commands[i].name)) {
      handle_close(program->image);
      program->image = image;
    } else if (image != HANDLE_INVALID) {
      handle_close(image);
    }
  }
  if (status != CALL_OK) {
    bundle_program_close(program);
    return status;
  }
  *output = program;
  return CALL_OK;
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

static enum call_status plain_collision(handle_t bin, const char *command)
{
  if (bin == HANDLE_INVALID) {
    return CALL_OK;
  }
  size_t size = strlen(command);
  if (size > BUNDLE_COMPONENT_LIMIT - 4) {
    return CALL_OK;
  }
  char name[BUNDLE_COMPONENT_LIMIT + 1];
  memcpy(name, command, size);
  memcpy(name + size, ".pxe", 5);
  handle_t image = HANDLE_INVALID;
  enum call_status status = directory_lookup(bin, name, DIRECTORY_KIND_FILE,
      0, &image);
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  if (status == CALL_OK) {
    return CALL_BAD_REQUEST;
  }
  return status == CALL_NOT_FOUND ? CALL_OK : status;
}

static enum call_status validate_registration(struct bundle_program *const *view,
    size_t count, handle_t bin)
{
  for (size_t i = 0; i < count; ++i) {
    for (size_t previous = 0; previous < i; ++previous) {
      if (!strcmp(view[i]->id, view[previous]->id)) {
        return CALL_BAD_REQUEST;
      }
    }
    for (size_t command = 0; command < view[i]->command_count; ++command) {
      const char *name = view[i]->commands[command].name;
      enum call_status status = plain_collision(bin, name);
      if (status != CALL_OK) {
        return status;
      }
      for (size_t previous = 0; previous < i; ++previous) {
        for (size_t other = 0; other < view[previous]->command_count; ++other) {
          if (!strcmp(name, view[previous]->commands[other].name)) {
            return CALL_BAD_REQUEST;
          }
        }
      }
    }
  }
  return CALL_OK;
}

enum call_status bundle_command_open(const struct path_context *context,
    const char *catalog_uri, const char *command, struct bundle_program **program)
{
  if (!program) {
    return CALL_BAD_REQUEST;
  }
  *program = NULL;
  if (!catalog_uri || !explicit_uri(catalog_uri) || !command || !valid_name(command)) {
    return CALL_BAD_REQUEST;
  }
  handle_t catalog = HANDLE_INVALID, bin = HANDLE_INVALID;
  struct bundle_json json = {0};
  struct bundle_program *view[BUNDLE_COUNT_LIMIT] = {0};
  size_t count = 0;
  enum call_status status = resolve(context, catalog_uri, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &catalog);
  if (status == CALL_OK) {
    status = read_json(catalog, &json);
  }
  if (catalog != HANDLE_INVALID) {
    handle_close(catalog);
  }
  const struct bundle_json_node *nodes = json.nodes;
  const struct bundle_json_node *bundles = NULL;
  bool format = false;
  if (status == CALL_OK) {
    if (nodes[1].type != BUNDLE_JSON_OBJECT) {
      status = CALL_BAD_REQUEST;
    } else {
      for (size_t key = nodes[1].child; key; key = nodes[nodes[key].next].next) {
        const struct bundle_json_node *value = &nodes[nodes[key].next];
        if (!strcmp(nodes[key].string, "format")) {
          format = value->type == BUNDLE_JSON_INTEGER && value->integer == 1;
        } else if (!strcmp(nodes[key].string, "bundles")) {
          bundles = value;
        } else {
          status = CALL_BAD_REQUEST;
        }
      }
      if (!format || !bundles || bundles->type != BUNDLE_JSON_ARRAY) {
        status = CALL_BAD_REQUEST;
      }
    }
  }
  if (status == CALL_OK) {
    status = resolve(context, "bin://", DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_LOOKUP, &bin);
    if (status == CALL_NOT_FOUND) {
      status = CALL_OK;
    } else if (status == CALL_OK) {
      status = require_native(bin, PROTOCOL_DIRECTORY);
    }
  }
  if (status == CALL_OK) {
    for (size_t index = bundles->child; index; index = nodes[index].next) {
      if (count == BUNDLE_COUNT_LIMIT) {
        status = CALL_LIMIT;
        break;
      }
      if (nodes[index].type != BUNDLE_JSON_STRING || !explicit_uri(nodes[index].string)) {
        status = CALL_BAD_REQUEST;
        break;
      }
      status = open_bundle(context, nodes[index].string, command, &view[count]);
      if (status != CALL_OK) {
        break;
      }
      ++count;
    }
  }
  if (status == CALL_OK) {
    status = validate_registration(view, count, bin);
  }
  /* NOT_FOUND identifies a missing registered command, not a broken source. */
  if (status == CALL_NOT_FOUND) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK) {
    status = CALL_NOT_FOUND;
    for (size_t i = 0; i < count; ++i) {
      for (size_t j = 0; j < view[i]->command_count; ++j) {
        if (!strcmp(command, view[i]->commands[j].name)) {
          *program = view[i];
          view[i] = NULL;
          status = CALL_OK;
          break;
        }
      }
      if (status == CALL_OK) {
        break;
      }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    bundle_program_close(view[i]);
  }
  if (bin != HANDLE_INVALID) {
    handle_close(bin);
  }
  bundle_json_close(&json);
  return status;
}

handle_t bundle_program_image(const struct bundle_program *program)
{
  return program ? program->image : HANDLE_INVALID;
}

uint64_t bundle_program_stack_bytes(const struct bundle_program *program)
{
  return program ? program->stack_bytes : 0;
}

const struct startup_binding *bundle_program_roots(const struct bundle_program *program,
    size_t *count)
{
  if (count) {
    *count = program ? program->root_count : 0;
  }
  return program ? program->roots : NULL;
}

const struct bundle_grant_request *bundle_program_grants(const struct bundle_program *program,
    size_t *count)
{
  if (count) {
    *count = program ? program->grant_count : 0;
  }
  return program ? program->grants : NULL;
}
