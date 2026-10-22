#ifndef USERSPACE_INSTALLER_BOOT_RECORD_H
#define USERSPACE_INSTALLER_BOOT_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_FILE_MAX_BYTES (1024u * 1024u)
#define CONFIG_RECORD_MAX_BYTES 8192u
#define CONFIG_ARTIFACT_NAME_BYTES 96u
#define CONFIG_PAYLOAD_PATH_BYTES 192u
/* The record and the owned boot menu it binds, as named in artifacts. */
#define CONFIG_RECORD_NAME "config.record"
#define CONFIG_MENU_NAME "limine.conf"

struct config_file_digest {
  bool present;
  uint64_t size;
  char sha256[65];
};

struct config_recovery_file {
  char name[CONFIG_ARTIFACT_NAME_BYTES];
  struct config_file_digest digest;
};

struct config_boot_payload {
  char path[CONFIG_PAYLOAD_PATH_BYTES];
  struct config_file_digest digest;
};

/* One immutable boot attempt: its kernel, archive and revision payloads. */
struct config_boot_binding {
  bool present;
  char kernel_revision[65], program_revision[65], attempt[33];
  struct config_boot_payload kernel, archive, revision;
};

/* The ESP's boot record: the current attempt, the one before it, and the
 * owned menu's recovery copies. The transaction is the current attempt. */
struct config_record {
  bool committed;
  char transaction[33];
  struct config_boot_binding boot;
  struct config_boot_binding previous_boot;
  struct config_recovery_file previous_menu;
  /* Recovery from the preceding committed record. Deletion requires a fresh
   * content match after this record has committed; names alone grant nothing. */
  struct config_recovery_file obsolete_menu;
};

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
/* FILE is CONFIG_MENU_NAME or CONFIG_RECORD_NAME. */
bool config_record_artifact_name(const char *transaction, const char *file,
    const char *suffix, char name[CONFIG_ARTIFACT_NAME_BYTES]);
bool config_record_parse(const void *bytes, size_t size, struct config_record *record);
bool config_record_render(const struct config_record *record, char **bytes, size_t *size);

#endif
