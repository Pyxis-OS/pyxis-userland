#ifndef USERSPACE_CONFIG_EDITOR_RECORD_H
#define USERSPACE_CONFIG_EDITOR_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_FILE_MAX_BYTES (1024u * 1024u)
#define CONFIG_RECORD_MAX_BYTES 4096u
#define CONFIG_SET_FILES 4u
#define CONFIG_ALL_FILES 5u
#define CONFIG_ARTIFACT_NAME_BYTES 96u
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

struct config_record {
  bool committed;
  char transaction[33];
  char revision[65];
  struct config_file_digest selected[CONFIG_SET_FILES];
  struct config_recovery_file previous[CONFIG_ALL_FILES];
  /* Recovery from the preceding committed record. Deletion requires a fresh
   * content match after this record has committed; names alone grant nothing. */
  struct config_recovery_file obsolete[CONFIG_ALL_FILES];
};

const char *config_storage_file_name(enum config_storage_file file);
bool config_transaction_valid(const char *transaction);
bool config_digest_valid(const struct config_file_digest *digest);
bool config_record_artifact_name(const char *transaction, enum config_storage_file file,
    const char *suffix, char name[CONFIG_ARTIFACT_NAME_BYTES]);
bool config_record_parse(const void *bytes, size_t size, struct config_record *record);
bool config_record_render(const struct config_record *record, char **bytes, size_t *size);

#endif
