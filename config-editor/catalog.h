#ifndef CONFIG_EDITOR_CATALOG_H
#define CONFIG_EDITOR_CATALOG_H

#include "../libconfig/literal.h"

#define CONFIG_CATALOG_ENTRY_LIMIT 128
#define CONFIG_ENTRY_ID_LIMIT 64

enum config_file {
  CONFIG_FILE_BOOT, CONFIG_FILE_SESSION, CONFIG_FILE_NETWORK, CONFIG_FILE_LIMINE,
  CONFIG_FILE_COUNT,
};
enum config_entry_kind {
  CONFIG_ENTRY_VOLUME, CONFIG_ENTRY_SPACE, CONFIG_ENTRY_SETTING, CONFIG_ENTRY_LIMINE,
};
enum config_migration_kind {
  CONFIG_MIGRATION_NONE, CONFIG_MIGRATION_REPLACE, CONFIG_MIGRATION_MOVE,
  CONFIG_MIGRATION_RENAME_FIELD,
};
struct config_migration {
  enum config_migration_kind kind;
  const struct config_literal_value *value;
  enum config_file from_file;
  enum config_entry_kind from_kind;
  char *from_name, *from, *to;
};
struct config_catalog_entry {
  char *id, *name;
  enum config_file file;
  enum config_entry_kind kind;
  bool required;
  const struct config_literal_value *default_value;
  struct config_migration migration;
};
struct config_catalog {
  struct config_literal *document;
  char *catalog_id, *revision;
  struct config_catalog_entry *entries;
  size_t count;
};

/* Revision is the installer's printable non-slash component (1..63 bytes),
 * rather than assuming every logical revision is a Git hash.
 * Catalog/default nodes remain owned by the catalog until free. Decoder checks
 * declaration shape and uniqueness; the planner validates target schemas and
 * migration applicability before offering any rewrite. Errors go to stderr. */
bool config_catalog_read(const char *name, const char *bytes, size_t size,
    struct config_catalog **output);
void config_catalog_free(struct config_catalog *catalog);
const struct config_catalog_entry *config_catalog_find(
    const struct config_catalog *catalog, const char *id);
const char *config_file_name(enum config_file file);
bool config_entry_id_valid(const char *bytes, size_t size);

#endif
