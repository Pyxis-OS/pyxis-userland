#ifndef USERSPACE_CONFIG_EDITOR_RECORD_H
#define USERSPACE_CONFIG_EDITOR_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_FILE_MAX_BYTES (1024u * 1024u)
#define CONFIG_RECORD_MAX_BYTES 8192u
#define CONFIG_SET_FILES 4u
#define CONFIG_ALL_FILES 5u
#define CONFIG_ARTIFACT_NAME_BYTES 96u
#define CONFIG_PAYLOAD_PATH_BYTES 192u
#define CONFIG_RECORD_NAME "config.record"

enum config_storage_file {
  CONFIG_BOOT, CONFIG_SESSION, CONFIG_NETWORK, CONFIG_CATALOG, CONFIG_LIMINE
};

struct config_file_digest {
  bool present;
  uint64_t size;
  char sha256[65];
};

struct config_recovery_file {
  char name[CONFIG_ARTIFACT_NAME_BYTES];
  struct config_file_digest digest;
};

enum config_active_source { CONFIG_ACTIVE_ESP, CONFIG_ACTIVE_ARCHIVE };
enum config_legacy_disposition { CONFIG_LEGACY_NONE, CONFIG_LEGACY_INACTIVE_RECOVERY };

struct config_boot_payload {
  char path[CONFIG_PAYLOAD_PATH_BYTES];
  struct config_file_digest digest;
};

/* Optional only for standalone scratch editing. Installed readiness binds the
 * entire selected source to one immutable boot pair and its archive catalog. */
struct config_boot_binding {
  bool present;
  enum config_active_source source;
  char kernel_revision[65], program_revision[65], attempt[33];
  struct config_boot_payload kernel, archive, revision;
  struct config_file_digest archive_catalog;
  /* Legacy evidence refers only to the unchanged system://config/boot.lua;
   * its inactive recovery disposition never authorizes fallback or deletion. */
  struct config_file_digest legacy_override;
  enum config_legacy_disposition legacy_disposition;
};

struct config_record {
  bool committed;
  char transaction[33];
  char revision[65];
  struct config_boot_binding boot;
  struct config_boot_binding previous_boot;
  struct config_file_digest selected[CONFIG_SET_FILES];
  struct config_recovery_file previous[CONFIG_ALL_FILES];
  /* Recovery from the preceding committed record. Deletion requires a fresh
   * content match after this record has committed; names alone grant nothing. */
  struct config_recovery_file obsolete[CONFIG_ALL_FILES];
};

const char *config_storage_file_name(enum config_storage_file file);
bool config_transaction_valid(const char *transaction);
bool config_digest_valid(const struct config_file_digest *digest);
/* Boot archives and kernels use native streamed FILE hashing, without the
 * configuration input size limit. Absence still requires empty evidence. */
bool config_payload_digest_valid(const struct config_file_digest *digest);
bool config_revision_valid(const char *revision);
bool config_boot_payload_path(const char *revision, const char *attempt,
    const char *name, char path[CONFIG_PAYLOAD_PATH_BYTES]);
bool config_boot_binding_valid(const struct config_boot_binding *binding);
bool config_record_valid(const struct config_record *record);
bool config_record_artifact_name(const char *transaction, enum config_storage_file file,
    const char *suffix, char name[CONFIG_ARTIFACT_NAME_BYTES]);
bool config_record_parse(const void *bytes, size_t size, struct config_record *record);
bool config_record_render(const struct config_record *record, char **bytes, size_t *size);

#endif
