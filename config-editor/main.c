#include "../common/bootstrap_environment.h"
#include "../libconfig/boot.h"
#include "../libconfig/network.h"
#include "../libconfig/session.h"

#include <literal.h>
#include <startup.h>
#include <system_info.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_BOOT_PATH "system://config/boot.lua"
#define POOL_SESSION_PATH "system://config/session.lua"
#define POOL_NETWORK_PATH "system://config/network.lua"

/* Reads PATH whole, up to the literal source limit. False prints why. */
static bool read_pool_file(const char *path, char **bytes, size_t *size)
{
  *bytes = NULL;
  *size = 0;
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (errno == ENOENT) {
      fprintf(stderr, "config: %s is missing\n", path);
    } else {
      fprintf(stderr, "config: %s: not checked (no readable system:// root)\n", path);
    }
    return false;
  }
  char *buffer = malloc(LITERAL_SOURCE_LIMIT + 1);
  size_t count = buffer ? fread(buffer, 1, LITERAL_SOURCE_LIMIT + 1, file) : 0;
  bool ok = buffer && !ferror(file);
  fclose(file);
  if (!ok) {
    fprintf(stderr, "config: %s: cannot read\n", path);
  } else if (count > LITERAL_SOURCE_LIMIT) {
    fprintf(stderr, "config: %s exceeds %d bytes\n", path, LITERAL_SOURCE_LIMIT);
    ok = false;
  }
  if (!ok) {
    free(buffer);
    return false;
  }
  *bytes = buffer;
  *size = count;
  return true;
}

/* The spaces and volumes boot would start. A space naming an absent CPU is
 * only a warning: boot leaves that space unstarted and starts the others. */
static bool validate_boot(void)
{
  char *bytes;
  size_t size;
  if (!read_pool_file(POOL_BOOT_PATH, &bytes, &size)) {
    return false;
  }
  struct boot_config config = {0};
  struct boot_plan plan = {0};
  struct config_failure failure = {0};
  bool ok = boot_config_read_pool_bytes(POOL_BOOT_PATH, bytes, size, &config,
      &failure) == BOOT_CONFIG_OK && boot_plan_build(&config, &plan, &failure);
  free(bytes);
  if (!ok) {
    fprintf(stderr, "config: %s is invalid: %s\n", POOL_BOOT_PATH,
        failure.text[0] ? failure.text : "invalid");
    boot_config_free(&config);
    return false;
  }
  fputs("config: " POOL_BOOT_PATH " is valid\n", stderr);
  for (size_t i = 0; i < plan.space_count; ++i) {
    fprintf(stderr, "config: space %s, init %s\n", plan.spaces[i]->name, plan.spaces[i]->init);
  }
  for (size_t i = 0; i < plan.volume_count; ++i) {
    fprintf(stderr, "config: volume %s\n", plan.volumes[i]->name);
  }
  struct system_info_cpu cpu;
  if (system_info_get_cpu(bootstrap_capability("system_info"), &cpu) == CALL_OK && cpu.online_count) {
    for (size_t i = 0; i < plan.space_count; ++i) {
      const struct boot_space *space = plan.spaces[i];
      for (size_t j = 0; j < space->cpu_count; ++j) {
        if (space->cpus[j] >= cpu.online_count) {
          fprintf(stderr, "config: warning: space %s names absent CPU %llu; boot leaves it "
              "unstarted\n", space->name, (unsigned long long)space->cpus[j]);
        }
      }
    }
  } else {
    fputs("config: CPU presence: not checked (read authority/inventory unavailable)\n", stderr);
  }
  boot_plan_free(&plan);
  boot_config_free(&config);
  return ok;
}

static bool validate_session(void)
{
  char *bytes;
  size_t size;
  if (!read_pool_file(POOL_SESSION_PATH, &bytes, &size)) {
    return false;
  }
  struct session_config config;
  bool ok = session_config_read_literal_bytes(POOL_SESSION_PATH, bytes, size,
      &config) == CONFIG_OK;
  free(bytes);
  if (ok) {
    session_config_free(&config);
  }
  fprintf(stderr, "config: %s is %s\n", POOL_SESSION_PATH, ok ? "valid" : "invalid");
  return ok;
}

static bool validate_network(void)
{
  char *bytes;
  size_t size;
  if (!read_pool_file(POOL_NETWORK_PATH, &bytes, &size)) {
    return false;
  }
  struct network_config config;
  bool ok = network_config_read_literal_bytes(POOL_NETWORK_PATH, bytes, size,
      &config) == CONFIG_OK;
  free(bytes);
  if (ok) {
    network_config_free(&config);
  }
  fprintf(stderr, "config: %s is %s\n", POOL_NETWORK_PATH, ok ? "valid" : "invalid");
  return ok;
}

int main(int argc, char **argv)
{
  if (argc != 2 || strcmp(argv[1], "validate")) {
    fputs("usage: config validate\n", stderr);
    return EXIT_FAILURE;
  }
  /* Boot applies none of the three unless all are valid. */
  bool ok = validate_boot();
  ok = validate_session() && ok;
  ok = validate_network() && ok;
  fputs("config: mounted volumes, packaged timezone data and live network availability: not checked\n",
      stderr);
  fputs(ok ? "config: boot applies this configuration\n" :
      "config: boot rejects this configuration and starts the rescue space\n", stderr);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
