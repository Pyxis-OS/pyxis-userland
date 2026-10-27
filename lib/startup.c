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

static bool valid_startup(const struct startup_info *info)
{
  if (!info || (uintptr_t)info % _Alignof(struct startup_info) ||
      info->version != STARTUP_VERSION || info->size > STARTUP_MAX_SIZE ||
      info->read_only_size < sizeof(*info) || info->read_only_size >= info->size ||
      (uintptr_t)info > UINTPTR_MAX - info->size || info->thread_fs == HANDLE_INVALID ||
      info->environment_handle == HANDLE_INVALID ||
      info->thread_fs == info->environment_handle || !info->usable_cpu_count) {
    return false;
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
    if (directories[i] == HANDLE_INVALID || directories[i] == info->thread_fs ||
        directories[i] == info->environment_handle) {
      return false;
    }
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

handle_t startup_environment_handle(void)
{
  return startup->environment_handle;
}

size_t startup_usable_cpu_count(void)
{
  return startup->usable_cpu_count;
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
