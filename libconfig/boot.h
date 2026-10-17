#ifndef LIBCONFIG_BOOT_H
#define LIBCONFIG_BOOT_H

#include <abi/mount.h>
#include <abi/space.h>
#include <machine_settings.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Names of volumes and spaces: 1..SPACE_NAME_MAX bytes of a-z, 0-9 and '-'. */
#define BOOT_NAME_MAX SPACE_NAME_MAX
/* Inits are boot archive entries named by URI. */
#define BOOT_INIT_PREFIX "boot://"
#define BOOT_INIT_MAX 255
#define BOOT_SOURCE_PATH_MAX 255
#define BOOT_SOURCE_PATH_COMPONENTS 16
/* The highest CPU index a configuration may name; presence is checked at boot. */
#define BOOT_CPU_INDEX_MAX 4095
/* The volume the pool override lives on; an override cannot redefine it. */
#define BOOT_SYSTEM_VOLUME "system"
/* Spaces start in this root unless they name another with start. */
#define BOOT_HOME_ROOT "home"
#define BOOT_HTTP_STAGING_SOURCE "http_staging"

enum boot_volume_kind {
  BOOT_VOLUME_NPFS,
  BOOT_VOLUME_VIRTIO_FS,
  BOOT_VOLUME_RAM,
  BOOT_VOLUME_BOOT_ESP,
  BOOT_VOLUME_FAT32,
};

struct boot_volume {
  char *name;
  enum boot_volume_kind kind;
  uint64_t partition; /* npfs/FAT32 only, one-based. */
  char *volume; /* npfs only. */
  uint8_t disk_guid[16]; /* FAT32 only, exact persistent GPT disk selection. */
  bool read_write; /* FAT32 admission mode, independent of delegated views. */
};

struct boot_root {
  char *volume;
  bool read_write;
  bool optional;
};

enum boot_grant_kind {
  BOOT_GRANT_DIRECTORY,
  BOOT_GRANT_MOUNT,
};

/* Selected launch sources are resources, never inherited URI roots. */
struct boot_grant {
  char *name, *volume;
  /* Directory-only relative subtree, opened by trusted boot init before launch.
   * NULL delegates the existing volume root; never a URI or traversal escape. */
  char *path;
  enum boot_grant_kind kind;
  bool read_write, optional;
};

struct boot_space {
  char *name;
  char *title; /* Defaults to the name. */
  char *init;
  uint64_t *cpus; /* Distinct CPU indices; none means every CPU. */
  size_t cpu_count;
  struct boot_root *roots;
  size_t root_count;
  struct boot_grant *grants;
  size_t grant_count;
  bool fat32_mount, fat32_mount_write; /* Explicit mount-only factory delegation. */
  char *start; /* A root name; NULL means BOOT_HOME_ROOT. */
  bool network;
  bool focus; /* One boot-only selection request after successful startup. */
  bool launch;
  bool jobs; /* Private contexts for clients that explicitly declare job_work. */
  bool multiplexer; /* Local terminal startup may create pane sessions. */
  bool remote_multiplexer; /* Explicit remote pane-broker opt-in. */
  bool clipboard_local; /* Explicit space-local graphics/store delegation. */
  bool clipboard_shared; /* Independent cross-space graphics/store delegation. */
  bool screenshot; /* Receives whole-screen capture authority. */
  bool acpi_tables; /* Receives the read-only ACPI table root as acpi://. */
  bool power; /* Receives power-off and restart authority. */
  bool remote_power; /* Opts remote root shells into power authority. */
};

/* One parsed file. Owns every string and array. */
struct boot_config {
  /* Hostname, directions and diagnostics remain archive-only machine defaults. */
  char hostname[MACHINE_HOSTNAME_MAX + 1];
  bool mouse_scroll_natural, touchpad_scroll_natural;
  bool input_diagnostics;
  bool touchpad_speed_present;
  int32_t touchpad_speed; /* Normalized permille; absent means zero in defaults. */
  bool num_lock_present;
  bool num_lock; /* Initial Num Lock state; absent means off in defaults. */
  struct boot_volume *volumes;
  size_t volume_count;
  struct boot_space *spaces;
  size_t space_count;
};

enum boot_entry_source { BOOT_ENTRY_DEFAULT, BOOT_ENTRY_OVERRIDE };

/* Borrowed view of the configuration that boots: the default with an
 * override merged by name. Entries point into one or two boot_configs that
 * must outlive the plan. The plan owns its pointer and source arrays. */
struct boot_plan {
  const struct boot_volume **volumes;
  enum boot_entry_source *volume_sources;
  size_t volume_count;
  const struct boot_space **spaces;
  enum boot_entry_source *space_sources;
  size_t space_count;
};

enum boot_config_result {
  BOOT_CONFIG_OK,
  BOOT_CONFIG_MISSING,
  BOOT_CONFIG_INVALID,
};

enum boot_plan_error {
  BOOT_PLAN_OK,
  BOOT_PLAN_INVALID,
  BOOT_PLAN_DUPLICATE_FOCUS,
};

/* Reads archive defaults through libc, including hostname/input policy, or a pool
 * boot override from SIZE bytes. The override accepts volumes, spaces and
 * input.touchpad_speed; no other machine policy may be overridden.
 * Schema errors go to stderr. CONFIG is cleared first and must be freed. */
enum boot_config_result boot_config_read(const char *path, struct boot_config *config);
enum boot_config_result boot_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config);
/* Trusted archive bytes include machine defaults. Literal ESP bytes use the
 * same override schema and never evaluate executable Lua. */
enum boot_config_result boot_config_read_archive_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config);
enum boot_config_result boot_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config);
/* Archive schema, including machine defaults, without executing input Lua. */
enum boot_config_result boot_config_read_archive_literal_bytes(const char *name, const char *bytes,
    size_t size, struct boot_config *config);
void boot_config_free(struct boot_config *config);

/* Builds the plan for DEFAULTS and an optional OVERRIDE. An override entry
 * replaces a default of the same name whole; new names follow the defaults.
 * Then checks what spans entries: the override cannot redefine the system
 * volume, roots must name defined volumes, start must name one of its space's
 * roots and at most one space may own the network or request boot focus.
 * False reports the reason on stderr and leaves PLAN empty. Optional ERROR
 * distinguishes duplicate focus so boot can suppress it even after fallback. */
bool boot_plan_build(const struct boot_config *defaults, const struct boot_config *override,
    struct boot_plan *plan, enum boot_plan_error *error);
void boot_plan_free(struct boot_plan *plan);
const struct boot_volume *boot_plan_volume(const struct boot_plan *plan, const char *name);
/* True for the boot, tmp and bin roots and the space's configured roots. */
bool boot_space_has_root(const struct boot_space *space, const char *name);

#endif
