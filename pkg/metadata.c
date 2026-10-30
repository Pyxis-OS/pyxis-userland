#include "metadata.h"

#include <ctype.h>
#include <literal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool alphanumeric(unsigned char byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
      (byte >= '0' && byte <= '9');
}

bool pkg_version_valid(const char *version)
{
  size_t length = strnlen(version, PACKAGE_VERSION_MAX + 1);
  if (!length || length > PACKAGE_VERSION_MAX || !alphanumeric(version[0])) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!alphanumeric(version[i]) && version[i] != '.' && version[i] != '+' &&
        version[i] != '-') {
      return false;
    }
  }
  return true;
}

static bool fields(const struct literal_value *value, const char *const *names,
    size_t count)
{
  if (!value || value->kind != LITERAL_TABLE || value->as.table.count != count) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!literal_field(value, names[i])) {
      return false;
    }
  }
  return true;
}

static bool text(const struct literal_value *table, const char *key,
    char *output, size_t capacity)
{
  const struct literal_value *value = literal_field(table, key);
  if (!value || value->kind != LITERAL_STRING || value->as.string.size >= capacity ||
      memchr(value->as.string.bytes, 0, value->as.string.size)) {
    return false;
  }
  memcpy(output, value->as.string.bytes, value->as.string.size);
  output[value->as.string.size] = 0;
  return true;
}

static bool integer(const struct literal_value *table, const char *key,
    uint64_t minimum, uint64_t maximum, uint64_t *output)
{
  const struct literal_value *value = literal_field(table, key);
  if (!value || value->kind != LITERAL_INTEGER || value->as.integer < 0 ||
      (uint64_t)value->as.integer < minimum || (uint64_t)value->as.integer > maximum) {
    return false;
  }
  *output = (uint64_t)value->as.integer;
  return true;
}

bool pkg_kind_bound(const char *kind)
{
  return !strcmp(kind, "program") || !strcmp(kind, "toolchain") || !strcmp(kind, "dev");
}

bool pkg_kind_commands(const char *kind)
{
  return !strcmp(kind, "program") || !strcmp(kind, "toolchain");
}

bool pkg_kind_release(const char *kind)
{
  return !strcmp(kind, "release");
}

static bool identity(const struct literal_value *table, struct pkg_identity *output)
{
  return text(table, "id", output->id, sizeof(output->id)) &&
      package_id_valid(output->id) &&
      text(table, "version", output->version, sizeof(output->version)) &&
      pkg_version_valid(output->version);
}

static bool kind(const struct literal_value *table, char output[10])
{
  return text(table, "kind", output, 10) &&
      (!strcmp(output, "data") || !strcmp(output, "program") || !strcmp(output, "toolchain") ||
       !strcmp(output, "dev") || !strcmp(output, "release"));
}

static bool array(const struct literal_value *value);

/* provides is required on release entries and refused on others: a non-empty
 * list of distinct ABI names ordered from fallback to preferred. */
static bool provides(const struct literal_value *table, struct pkg_entry *entry)
{
  const struct literal_value *list = literal_field(table, "provides");
  entry->provides_count = 0;
  if (!pkg_kind_release(entry->kind)) {
    return !list;
  }
  if (!list || !array(list) || !list->as.table.count ||
      list->as.table.count > PKG_PROVIDES_MAX) {
    return false;
  }
  for (const struct literal_entry *item = list->as.table.entries; item; item = item->next) {
    const struct literal_value *name = item->value;
    char *output = entry->provides[entry->provides_count];
    if (name->kind != LITERAL_STRING || name->as.string.size > PACKAGE_ABI_MAX ||
        strlen(name->as.string.bytes) != name->as.string.size) {
      return false;
    }
    memcpy(output, name->as.string.bytes, name->as.string.size + 1);
    if (!package_abi_valid(output)) {
      return false;
    }
    for (size_t i = 0; i < entry->provides_count; ++i) {
      if (!strcmp(entry->provides[i], output)) {
        return false;
      }
    }
    ++entry->provides_count;
  }
  return true;
}

/* abi is required on program, toolchain and dev packages and refused on data. */
static bool abi(const struct literal_value *table, const char *kind,
    char output[PACKAGE_ABI_MAX + 1])
{
  output[0] = 0;
  bool present = literal_field(table, "abi") != NULL;
  if (!pkg_kind_bound(kind)) {
    return !present;
  }
  return present && text(table, "abi", output, PACKAGE_ABI_MAX + 1) &&
      package_abi_valid(output);
}

static bool hash(const struct literal_value *table, const char *key, char output[65])
{
  if (!text(table, key, output, 65) || strlen(output) != 64) {
    return false;
  }
  for (size_t i = 0; i < 64; ++i) {
    if (!((output[i] >= '0' && output[i] <= '9') ||
        (output[i] >= 'a' && output[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool array(const struct literal_value *value)
{
  if (!value || value->kind != LITERAL_TABLE) {
    return false;
  }
  int64_t index = 1;
  for (const struct literal_entry *entry = value->as.table.entries; entry;
      entry = entry->next, ++index) {
    if (entry->key->kind != LITERAL_INTEGER || entry->key->as.integer != index) {
      return false;
    }
  }
  return true;
}

static bool utf8(const char *bytes)
{
  size_t remaining = strlen(bytes);
  while (remaining) {
    int count = mbtowc(NULL, bytes, remaining);
    if (count <= 0) {
      return false;
    }
    bytes += count;
    remaining -= (size_t)count;
  }
  return true;
}

static bool command_name(const char *name)
{
  size_t size = strlen(name);
  if (!size || size > 255 || !strcmp(name, ".") || !strcmp(name, "..") || !utf8(name)) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = name[i];
    if (byte <= 0x20 || byte == 0x7f || byte == '/' || byte == ':' || byte == '\\') {
      return false;
    }
  }
  return true;
}

static bool command_ustar_path(const char *path, size_t size, bool directory)
{
  char full[PKG_PATH_MAX + 7] = "tree/";
  memcpy(full + 5, path, size);
  size += 5;
  if (directory) {
    full[size++] = '/';
  }
  full[size] = 0;
  if (size <= 100) {
    return true;
  }
  for (size_t i = 0; i < size && i <= 155; ++i) {
    if (full[i] == '/' && size - i - 1 >= 1 && size - i - 1 <= 100 &&
        strcmp(full + i + 1, "/")) {
      return true;
    }
  }
  return false;
}

static bool command_path(const char *path, bool bundle)
{
  size_t size = strlen(path);
  if (size < 4 || size > PKG_PATH_MAX || strchr(path, '\\') || !utf8(path) ||
      strcmp(path + size - 4, bundle ? ".pxb" : ".pxe")) {
    return false;
  }
  size_t start = 0;
  for (size_t i = 0; i <= size; ++i) {
    if (i != size && path[i] != '/') {
      continue;
    }
    size_t count = i - start;
    if (!count || (count == 1 && path[start] == '.') ||
        (count == 2 && path[start] == '.' && path[start + 1] == '.') ||
        (i != size && count >= 4 && !memcmp(path + i - 4, ".pxb", 4)) ||
        !command_ustar_path(path, i, i != size || bundle)) {
      return false;
    }
    start = i + 1;
  }
  return true;
}

static bool commands_parse(const struct literal_value *value,
    struct pkg_manifest *manifest, const char **reason)
{
  if (!value) {
    return true;
  }
  if (value->kind != LITERAL_TABLE) {
    return false;
  }
  manifest->command_count = value->as.table.count;
  if (manifest->command_count) {
    manifest->commands = calloc(manifest->command_count, sizeof(*manifest->commands));
    if (!manifest->commands) {
      *reason = "out of memory reading commands";
      return false;
    }
  }
  static const char *const plain_fields[] = { "path" };
  static const char *const bundle_fields[] = { "path", "command" };
  size_t index = 0;
  for (const struct literal_entry *item = value->as.table.entries; item;
      item = item->next, ++index) {
    struct pkg_command *command = &manifest->commands[index];
    if (item->key->kind != LITERAL_STRING ||
        item->key->as.string.size >= sizeof(command->name) ||
        memchr(item->key->as.string.bytes, 0, item->key->as.string.size)) {
      return false;
    }
    memcpy(command->name, item->key->as.string.bytes, item->key->as.string.size);
    bool bundle = literal_field(item->value, "command") != NULL;
    if (!command_name(command->name) ||
        !fields(item->value, bundle ? bundle_fields : plain_fields, bundle ? 2 : 1) ||
        !text(item->value, "path", command->path, sizeof(command->path)) ||
        !command_path(command->path, bundle) ||
        (bundle && (!text(item->value, "command", command->command, sizeof(command->command)) ||
            !command_name(command->command)))) {
      return false;
    }
    if (bundle && strcmp(command->name, command->command)) {
      *reason = "bundle export name must match its declared command";
      return false;
    }
  }
  return true;
}

bool pkg_time_valid(const char *text)
{
  static const char shape[] = "dddd-dd-ddTdd:dd:ddZ";
  if (strlen(text) != PKG_TIME_LENGTH) {
    return false;
  }
  for (size_t i = 0; i < PKG_TIME_LENGTH; ++i) {
    if (shape[i] == 'd' ? text[i] < '0' || text[i] > '9' : text[i] != shape[i]) {
      return false;
    }
  }
  unsigned month = (text[5] - '0') * 10 + (text[6] - '0');
  unsigned day = (text[8] - '0') * 10 + (text[9] - '0');
  unsigned hour = (text[11] - '0') * 10 + (text[12] - '0');
  unsigned minute = (text[14] - '0') * 10 + (text[15] - '0');
  unsigned second = (text[17] - '0') * 10 + (text[18] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour < 24 &&
      minute < 60 && second < 60;
}

/* The certificate file a signed index names, beside it in the repository. */
static bool signer_name(const char *name)
{
  size_t length = strlen(name);
  if (length < 5 || strcmp(name + length - 4, ".pem") || !isalnum((unsigned char)name[0])) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = (unsigned char)name[i];
    if (!isalnum(byte) && byte != '.' && byte != '_' && byte != '-') {
      return false;
    }
  }
  return true;
}

bool pkg_index_parse(const char *bytes, size_t size, struct pkg_index_header *header,
    struct pkg_entry **entries, size_t *count, const char **reason)
{
  *header = (struct pkg_index_header){0};
  *entries = NULL;
  *count = 0;
  *reason = "invalid repository index";
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  /* The index grows with every release and package; only memory bounds it. */
  if (!literal_parse_sized(bytes, size, &document, &diagnostic)) {
    static char text[256];
    char described[160];
    snprintf(text, sizeof(text), "index: %s",
        literal_describe(&diagnostic, described, sizeof(described)));
    *reason = text;
    return false;
  }
  static const char *const root_fields[] = { "repository", "serial", "signed", "packages" };
  static const char *const entry_fields[] = {
    "id", "version", "kind", "archive", "size", "sha256",
  };
  const struct literal_value *packages = literal_field(document->root, "packages");
  bool named = literal_field(document->root, "signer") != NULL;
  bool ok = document->root->kind == LITERAL_TABLE &&
      document->root->as.table.count == 4 + named;
  for (size_t f = 0; ok && f < 4; ++f) {
    ok = literal_field(document->root, root_fields[f]) != NULL;
  }
  ok = ok && array(packages) &&
      text(document->root, "repository", header->repository, sizeof(header->repository)) &&
      package_id_valid(header->repository) &&
      integer(document->root, "serial", 1, PKG_SERIAL_MAX, &header->serial) &&
      text(document->root, "signed", header->signed_at, sizeof(header->signed_at)) &&
      pkg_time_valid(header->signed_at) &&
      (!named || (text(document->root, "signer", header->signer, sizeof(header->signer)) &&
       signer_name(header->signer)));
  size_t length = ok ? packages->as.table.count : 0;
  struct pkg_entry *result = length ? calloc(length, sizeof(*result)) : NULL;
  if (length && !result) {
    *reason = "out of memory reading repository index";
    ok = false;
  }
  size_t index = 0;
  for (const struct literal_entry *item = ok ? packages->as.table.entries : NULL;
      ok && item; item = item->next, ++index) {
    struct pkg_entry *entry = &result[index];
    const struct literal_value *value = item->value;
    bool optional_fields = value && value->kind == LITERAL_TABLE &&
        (literal_field(value, "abi") || literal_field(value, "provides"));
    ok = value && value->kind == LITERAL_TABLE &&
        value->as.table.count == (optional_fields ? 7 : 6);
    for (size_t f = 0; ok && f < 6; ++f) {
      ok = literal_field(value, entry_fields[f]) != NULL;
    }
    ok = ok && identity(value, &entry->identity) &&
        kind(value, entry->kind) && abi(value, entry->kind, entry->abi) &&
        provides(value, entry) &&
        text(value, "archive", entry->archive, sizeof(entry->archive)) &&
        integer(value, "size", 1, INT64_MAX, &entry->size) && hash(value, "sha256", entry->sha256);
    if (!ok) {
      break;
    }
    char expected[PKG_ARCHIVE_PATH_MAX + 1];
    snprintf(expected, sizeof(expected), "packages/%s/%s.tar", entry->identity.id, entry->identity.version);
    ok = !strcmp(expected, entry->archive);
    for (size_t previous = 0; ok && previous < index; ++previous) {
      ok = strcmp(result[previous].identity.id, entry->identity.id) ||
          strcmp(result[previous].identity.version, entry->identity.version);
    }
  }
  literal_free(document);
  if (!ok) {
    free(result);
    return false;
  }
  *entries = result;
  *count = length;
  *reason = NULL;
  return true;
}

void pkg_manifest_free(struct pkg_manifest *manifest)
{
  free(manifest->dependencies);
  free(manifest->commands);
  memset(manifest, 0, sizeof(*manifest));
}

bool pkg_manifest_parse(const char *bytes, size_t size,
    struct pkg_manifest *manifest, const char **reason)
{
  memset(manifest, 0, sizeof(*manifest));
  *reason = "invalid package manifest";
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  if (!literal_parse(bytes, size, &document, &diagnostic)) {
    return false;
  }
  static const char *const manifest_fields[] = {
    "id", "version", "kind", "depends", "files", "bytes", "inventory_sha256",
  };
  static const char *const dependency_fields[] = { "id", "version" };
  const struct literal_value *root = document->root;
  const struct literal_value *dependencies = literal_field(root, "depends");
  const struct literal_value *commands = literal_field(root, "commands");
  bool bound = root && root->kind == LITERAL_TABLE && literal_field(root, "abi");
  bool ok = root && root->kind == LITERAL_TABLE &&
      root->as.table.count == 7 + (commands ? 1 : 0) + (bound ? 1 : 0);
  for (size_t i = 0; ok && i < 7; ++i) {
    ok = literal_field(root, manifest_fields[i]) != NULL;
  }
  ok = ok && identity(root, &manifest->identity) && kind(root, manifest->kind) &&
      abi(root, manifest->kind, manifest->abi) &&
      integer(root, "files", 0, PKG_MAX_FILES, &manifest->files) &&
      integer(root, "bytes", 0, INT64_MAX, &manifest->bytes) &&
      hash(root, "inventory_sha256", manifest->inventory_sha256) && array(dependencies);
  manifest->dependency_count = ok ? dependencies->as.table.count : 0;
  if (manifest->dependency_count) {
    manifest->dependencies = calloc(manifest->dependency_count, sizeof(*manifest->dependencies));
    if (!manifest->dependencies) {
      *reason = "out of memory reading dependencies";
      ok = false;
    }
  }
  size_t index = 0;
  for (const struct literal_entry *item = ok ? dependencies->as.table.entries : NULL;
      ok && item; item = item->next, ++index) {
    struct pkg_identity *dependency = &manifest->dependencies[index];
    /* A dependency names an exact version, or only an ID, which any version
     * satisfies. */
    bool any_version = fields(item->value, dependency_fields, 1);
    ok = (any_version ? text(item->value, "id", dependency->id, sizeof(dependency->id)) &&
        package_id_valid(dependency->id) :
        fields(item->value, dependency_fields, 2) && identity(item->value, dependency)) &&
        strcmp(dependency->id, manifest->identity.id);
    for (size_t previous = 0; ok && previous < index; ++previous) {
      ok = strcmp(manifest->dependencies[previous].id, dependency->id) ||
          strcmp(manifest->dependencies[previous].version, dependency->version);
    }
  }
  /* Missing or empty means no exports, which every kind may have. */
  if (ok && commands && commands->kind == LITERAL_TABLE && commands->as.table.count &&
      !pkg_kind_commands(manifest->kind)) {
    *reason = "only program and toolchain packages export commands";
    ok = false;
  }
  if (ok) {
    ok = commands_parse(commands, manifest, reason);
  }
  literal_free(document);
  if (!ok) {
    pkg_manifest_free(manifest);
    return false;
  }
  *reason = NULL;
  return true;
}
