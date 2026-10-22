#ifndef LIBCONFIG_BOOT_H
#define LIBCONFIG_BOOT_H

#include <abi/mount.h>
#include <abi/space.h>
#include <machine_settings.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct config_failure;

/* Names of volumes and spaces: 1..SPACE_NAME_MAX bytes of a-z, 0-9 and '-'. */
#define BOOT_NAME_MAX SPACE_NAME_MAX
/* Inits are boot archive entries or files on the pool's system volume, such as
 * the installed spaces' scripts in system://config/, named by URI. */
#define BOOT_INIT_PREFIX "boot://"
#define BOOT_POOL_INIT_PREFIX "system://"
#define BOOT_INIT_MAX 255
#define BOOT_POOL_PARTITION 0
#define BOOT_SOURCE_PATH_MAX 255
#define BOOT_SOURCE_PATH_COMPONENTS 16
/* The highest CPU index a configuration may name; presence is checked at boot. */
#define BOOT_CPU_INDEX_MAX 4095
/* The pool's system volume, which holds the installed configuration. */
#define BOOT_SYSTEM_VOLUME "system"
/* Spaces start in this root unless they name another with start. */
#define BOOT_HOME_ROOT "home"
#define BOOT_PACKAGES_VOLUME "packages"
/* Appended for acpi_tables, so no configured root may take the name. */
#define BOOT_ACPI_ROOT "acpi"
#define BOOT_HTTP_STAGING_SOURCE "http_staging"
#define BOOT_HTTP_WORK_VOLUME "http-work"

enum boot_volume_kind {
  BOOT_VOLUME_NPFS,
  BOOT_VOLUME_RAM_NPFS,
  BOOT_VOLUME_VIRTIO_FS,
  BOOT_VOLUME_RAM,
  BOOT_VOLUME_BOOT_ESP,
  BOOT_VOLUME_FAT32,
};

struct boot_volume {
  char *name;
  enum boot_volume_kind kind;
  /* npfs/FAT32 only, one-based. An npfs volume that names none is on the pool
   * (BOOT_POOL_PARTITION), which boot init resolves from mount.pool. */
  uint64_t partition;
  char *volume; /* npfs and ram-npfs only. */
  uint8_t disk_guid[16]; /* FAT32 only, exact persistent GPT disk selection. */
  bool read_write; /* FAT32 admission mode, independent of delegated views. */
};

/* A root names a volume of the same name, or a package of the current set
 * resolved through BOOT_PACKAGES_VOLUME when the space starts. */
struct boot_root {
  char *name;
  char *volume;
  char *package; /* NULL for a volume root; package roots are read-only. */
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
  bool package_management; /* Selected package writer and repository configuration. */
  bool package_upgrade; /* Booted-ESP upgrade writer and boot evidence. */
  bool multiplexer; /* Local terminal startup may create pane sessions. */
  bool remote_multiplexer; /* Explicit remote pane-broker opt-in. */
  bool clipboard_local; /* Explicit space-local graphics/store delegation. */
  bool clipboard_shared; /* Independent cross-space graphics/store delegation. */
  bool screenshot; /* Whole-screen capture plus the initial private Print Screen saver. */
  bool acpi_tables; /* Receives the read-only ACPI table root as acpi://. */
  bool power; /* Receives power-off and restart authority. */
  uint64_t wifi_rights; /* Explicit controller READ/SCAN/CONNECT; default none. */
  bool remote_power; /* Opts remote root shells into power authority. */
};

/* One parsed file. Owns every string and array. */
struct boot_config {
  /* Machine defaults and RAM pool capacity remain archive-only. */
  char hostname[MACHINE_HOSTNAME_MAX + 1];
  char wifi_country[2]; /* Archive-only explicit policy; zero means no TX policy. */
  uint32_t wifi_channels;
  bool mouse_scroll_natural, touchpad_scroll_natural;
  bool input_diagnostics;
  bool touchpad_speed_present;
  int32_t touchpad_speed; /* Normalized permille; absent means zero in defaults. */
  bool num_lock_present;
  bool num_lock; /* Initial Num Lock state; absent means off in defaults. */
  uint64_t ram_npfs_bytes; /* Archive-only logical capacity; zero disables creation. */
  struct boot_volume *volumes;
  size_t volume_count;
  struct boot_space *spaces;
  size_t space_count;
};

/* Borrowed view of the configuration that boots. Entries point into the
 * boot_config, which must outlive the plan. The plan owns its pointer arrays. */
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

/* Reads a trusted archive configuration through libc, including hostname and
 * input policy. Schema errors go to stderr. CONFIG is cleared first and must
 * be freed. */
enum boot_config_result boot_config_read(const char *path, struct boot_config *config);
/* The pool's system://config/boot.lua, the whole installed configuration:
 * archive schema, literal data only. A non-NULL FAILURE receives the reason
 * instead of stderr. */
enum boot_config_result boot_config_read_pool_bytes(const char *name,
    const char *bytes, size_t size, struct boot_config *config, struct config_failure *failure);
void boot_config_free(struct boot_config *config);

/* Builds the plan for CONFIG and checks what spans entries: roots must name
 * defined volumes, start must name one of its space's roots and at most one
 * space may own the network or request boot focus. False reports the reason
 * on stderr, or in a non-NULL FAILURE, and leaves PLAN empty. */
bool boot_plan_build(const struct boot_config *config, struct boot_plan *plan,
    struct config_failure *failure);
void boot_plan_free(struct boot_plan *plan);
const struct boot_volume *boot_plan_volume(const struct boot_plan *plan, const char *name);
/* True for the boot, tmp and bin roots and the space's configured roots. */
bool boot_space_has_root(const struct boot_space *space, const char *name);

#endif
