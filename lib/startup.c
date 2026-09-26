#include <abi/startup.h>
#include <startup.h>

static const struct startup_info *startup;

static bool contains(uint64_t base, uint64_t size, uint64_t address, uint64_t bytes)
{
  return address >= base && address - base <= size && bytes <= size - (address - base);
}

static bool terminated(uint64_t base, uint64_t size, uint64_t address)
{
  if (!contains(base, size, address, 1)) {
    return false;
  }
  const char *text = (const char *)(uintptr_t)address;
  size_t remaining = size - (address - base);
  for (size_t i = 0; i < remaining; ++i) {
    if (!text[i]) {
      return true;
    }
  }
  return false;
}

static bool valid_bindings(const struct startup_info *info, uint64_t address,
                            uint64_t count)
{
  if (!count) {
    return !address;
  }
  if (count > info->read_only_size / sizeof(struct startup_binding) ||
      address % _Alignof(struct startup_binding) ||
      !contains((uintptr_t)info, info->read_only_size, address,
        count * sizeof(struct startup_binding))) {
    return false;
  }
  const struct startup_binding *bindings = (const void *)(uintptr_t)address;
  for (size_t i = 0; i < count; ++i) {
    if (bindings[i].handle == HANDLE_INVALID ||
        !terminated((uintptr_t)info, info->read_only_size, bindings[i].name)) {
      return false;
    }
  }
  return true;
}

static bool valid_streams(const struct startup_info *info)
{
  const struct startup_binding *resources = (const void *)(uintptr_t)info->resources;
  const struct startup_binding *roots = (const void *)(uintptr_t)info->roots;
  const handle_t *directories = (const void *)(uintptr_t)info->working_directories;
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    const struct startup_stream *stream = &info->streams[i];
    if (stream->protocol == STARTUP_STREAM_NONE) {
      if (stream->handle != HANDLE_INVALID) {
        return false;
      }
      continue;
    }
    if ((stream->protocol != PROTOCOL_CONSOLE &&
         stream->protocol != PROTOCOL_FILE &&
         stream->protocol != PROTOCOL_PIPE) || stream->handle == HANDLE_INVALID) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (stream->handle == info->streams[j].handle) {
        return false;
      }
    }
    for (size_t j = 0; j < info->resource_count; ++j) {
      if (stream->handle == resources[j].handle) {
        return false;
      }
    }
    for (size_t j = 0; j < info->root_count; ++j) {
      if (stream->handle == roots[j].handle) {
        return false;
      }
    }
    for (size_t j = 0; j < info->working_directory_count; ++j) {
      if (stream->handle == directories[j]) {
        return false;
      }
    }
  }
  return true;
}

static bool valid_startup(const struct startup_info *info)
{
  if (!info || (uintptr_t)info % _Alignof(struct startup_info) ||
      info->version != STARTUP_VERSION || info->size > STARTUP_MAX_SIZE ||
      info->read_only_size < sizeof(*info) || info->read_only_size >= info->size ||
      (uintptr_t)info > UINTPTR_MAX - info->size) {
    return false;
  }
  if (!valid_bindings(info, info->resources, info->resource_count) ||
      !valid_bindings(info, info->roots, info->root_count)) {
    return false;
  }

  if (info->environment_count > info->read_only_size / sizeof(struct startup_variable) ||
      info->environment % _Alignof(struct startup_variable) ||
      (!info->environment_count && info->environment) ||
      (info->environment_count && !contains((uintptr_t)info, info->read_only_size,
        info->environment, info->environment_count * sizeof(struct startup_variable)))) {
    return false;
  }
  const struct startup_variable *environment = (const void *)(uintptr_t)info->environment;
  for (size_t i = 0; i < info->environment_count; ++i) {
    if (!terminated((uintptr_t)info, info->read_only_size, environment[i].name) ||
        !terminated((uintptr_t)info, info->read_only_size, environment[i].value)) {
      return false;
    }
  }

  if (info->working_directory_count > info->read_only_size / sizeof(handle_t) ||
      info->working_directories % _Alignof(handle_t) ||
      (!info->working_directory_count && info->working_directories) ||
      (info->working_directory_count && !contains((uintptr_t)info, info->read_only_size,
        info->working_directories, info->working_directory_count * sizeof(handle_t))) ||
      (info->working_path && (!info->working_directory_count ||
        !terminated((uintptr_t)info, info->read_only_size, info->working_path)))) {
    return false;
  }

  const handle_t *directories = (const void *)(uintptr_t)info->working_directories;
  for (size_t i = 0; i < info->working_directory_count; ++i) {
    if (directories[i] == HANDLE_INVALID) {
      return false;
    }
  }

  if (!valid_streams(info)) {
    return false;
  }

  uint64_t arguments = (uintptr_t)info + info->read_only_size;
  size_t argument_size = info->size - info->read_only_size;
  if (info->argc >= argument_size / sizeof(char *) ||
      info->argv % _Alignof(char *) ||
      !contains(arguments, argument_size, info->argv, (info->argc + 1) * sizeof(char *))) {
    return false;
  }
  char **argv = (char **)(uintptr_t)info->argv;
  if (argv[info->argc]) {
    return false;
  }
  for (size_t i = 0; i < info->argc; ++i) {
    if (!terminated(arguments, argument_size, (uintptr_t)argv[i])) {
      return false;
    }
  }
  return true;
}

/* Called by the runtime before application code can mutate argv.
 * The kernel supplies mapped storage; bounds checks do not probe arbitrary
 * user pointers or replace the kernel's startup preparation checks. */
bool startup_init(const struct startup_info *info)
{
  if (!valid_startup(info)) {
    return false;
  }
  startup = info;
  return true;
}

static bool same_name(const char *left, const char *right)
{
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return *left == *right;
}

static handle_t find_binding(uint64_t address, size_t count, const char *name)
{
  if (!name) {
    return HANDLE_INVALID;
  }
  const struct startup_binding *bindings = (const void *)(uintptr_t)address;
  for (size_t i = 0; i < count; ++i) {
    if (same_name((const char *)(uintptr_t)bindings[i].name, name)) {
      return bindings[i].handle;
    }
  }
  return HANDLE_INVALID;
}

handle_t startup_resource(const char *name)
{
  return find_binding(startup->resources, startup->resource_count, name);
}

handle_t startup_root(const char *scheme)
{
  return find_binding(startup->roots, startup->root_count, scheme);
}

const char *startup_environment(const char *name)
{
  if (!name) {
    return NULL;
  }
  const struct startup_variable *environment = (const void *)(uintptr_t)startup->environment;
  for (size_t i = 0; i < startup->environment_count; ++i) {
    if (same_name((const char *)(uintptr_t)environment[i].name, name)) {
      return (const char *)(uintptr_t)environment[i].value;
    }
  }
  return NULL;
}

const struct startup_variable *startup_environment_variables(void)
{
  return (const void *)(uintptr_t)startup->environment;
}

size_t startup_environment_count(void)
{
  return startup->environment_count;
}

const handle_t *startup_working_directories(void)
{
  return (const void *)(uintptr_t)startup->working_directories;
}

size_t startup_working_directory_count(void)
{
  return startup->working_directory_count;
}

handle_t startup_working_directory(size_t index)
{
  if (index >= startup->working_directory_count) {
    return HANDLE_INVALID;
  }
  const handle_t *directories = (const void *)(uintptr_t)startup->working_directories;
  return directories[index];
}

const char *startup_working_path(void)
{
  return (const char *)(uintptr_t)startup->working_path;
}

struct startup_stream startup_stream(enum startup_stream_index index)
{
  if ((unsigned)index >= STARTUP_STREAM_COUNT) {
    return (struct startup_stream){0};
  }
  return startup->streams[index];
}
