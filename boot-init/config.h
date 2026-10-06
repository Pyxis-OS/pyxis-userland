#ifndef BOOT_INIT_CONFIG_H
#define BOOT_INIT_CONFIG_H

#include <abi/mount.h>
#include <abi/space.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Names of volumes and spaces: 1..SPACE_NAME_MAX bytes of a-z, 0-9 and '-'. */
#define BOOT_NAME_MAX SPACE_NAME_MAX
/* Inits are boot archive entries named by URI. */
#define BOOT_INIT_PREFIX "boot://"
#define BOOT_INIT_MAX 255
/* The highest CPU index a configuration may name; presence is checked at boot. */
#define BOOT_CPU_INDEX_MAX 4095
/* The volume the pool override lives on; an override cannot redefine it. */
#define BOOT_SYSTEM_VOLUME "system"

enum boot_volume_kind {
  BOOT_VOLUME_NPFS,
  BOOT_VOLUME_VIRTIO_FS,
};

struct boot_volume {
  char *name;
  enum boot_volume_kind kind;
  uint64_t partition; /* npfs only, one-based. */
  char *volume; /* npfs only. */
};

struct boot_root {
  char *volume;
  bool read_write;
  bool optional;
};

struct boot_space {
  char *name;
  char *title; /* Defaults to the name. */
  char *init;
  uint64_t *cpus; /* Distinct CPU indices; none means every CPU. */
  size_t cpu_count;
  struct boot_root *roots;
  size_t root_count;
  bool network;
};

/* One parsed file. Owns every string and array. */
struct boot_config {
  struct boot_volume *volumes;
  size_t volume_count;
  struct boot_space *spaces;
  size_t space_count;
};

/* Borrowed view of the configuration that boots: the default with an
 * override merged by name. Entries point into one or two boot_configs. */
struct boot_plan {
  const struct boot_volume **volumes;
  size_t volume_count;
  const struct boot_space **spaces;
  size_t space_count;
};

enum boot_config_result {
  BOOT_CONFIG_OK,
  BOOT_CONFIG_MISSING,
  BOOT_CONFIG_INVALID,
};

/* Reads one file through libc, or SIZE bytes read through a capability. Schema
 * errors are reported on stderr. CONFIG is cleared first and must be freed. */
enum boot_config_result boot_config_read(const char *path, struct boot_config *config);
enum boot_config_result boot_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config);
void boot_config_free(struct boot_config *config);

/* Builds the plan for DEFAULTS and an optional OVERRIDE. An override entry
 * replaces a default of the same name whole; new names follow the defaults.
 * Then checks what spans entries: the override cannot redefine the system
 * volume, roots must name defined volumes and at most one space may own the
 * network. False reports the reason on stderr and leaves PLAN empty. */
bool boot_plan_build(const struct boot_config *defaults, const struct boot_config *override,
    struct boot_plan *plan);
void boot_plan_free(struct boot_plan *plan);
const struct boot_volume *boot_plan_volume(const struct boot_plan *plan, const char *name);

#endif
