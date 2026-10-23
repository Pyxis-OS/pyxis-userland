#include "commands.h"
#include <bundle.h>
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <pxe/p1f.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool fail(const char **reason, const char *message)
{
  *reason = message;
  return false;
}

static bool native(handle_t handle, uint64_t protocol)
{
  struct handle_info info;
  return handle_query(handle, &info) == CALL_OK && info.kind == HANDLE_KIND_NATIVE &&
      info.protocol == protocol && !info.transport;
}

static bool suffix(const char *name, const char *ending)
{
  size_t length = strlen(name), ending_length = strlen(ending);
  return length >= ending_length && !strcmp(name + length - ending_length, ending);
}

static bool verify_target(handle_t tree, const struct pkg_command *command,
    uint64_t *kind, const char **reason)
{
  bool bundle = command->command[0] != 0;
  *kind = bundle ? DIRECTORY_KIND_DIRECTORY : DIRECTORY_KIND_FILE;
  char path[PKG_PATH_MAX + 1];
  strcpy(path, command->path);
  handle_t parent;
  if (handle_copy(tree, &parent) != CALL_OK) {
    return fail(reason, "cannot retain command tree");
  }
  char *part = path;
  enum call_status status = CALL_OK;
  handle_t target = HANDLE_INVALID;
  for (;;) {
    char *slash = strchr(part, '/');
    if (!slash) {
      status = directory_lookup(parent, part, *kind,
          bundle ? PKG_READ_RIGHTS : FILE_RIGHT_READ, &target);
      break;
    }
    *slash = 0;
    /* A plain image cannot bypass a containing bundle's launch policy. */
    if (suffix(part, ".pxb")) {
      status = CALL_BAD_REQUEST;
      break;
    }
    handle_t child = HANDLE_INVALID;
    status = directory_lookup(parent, part, DIRECTORY_KIND_DIRECTORY,
        PKG_READ_RIGHTS, &child);
    if (status != CALL_OK) {
      break;
    }
    if (!native(child, PROTOCOL_DIRECTORY)) {
      handle_close(child);
      status = CALL_BAD_REQUEST;
      break;
    }
    handle_close(parent);
    parent = child;
    part = slash + 1;
  }
  handle_close(parent);
  if (status == CALL_OK && !native(target, bundle ? PROTOCOL_DIRECTORY : PROTOCOL_FILE)) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK && bundle) {
    struct bundle_program *program = NULL;
    status = bundle_open_directory(NULL, target, command->command, &program);
    bundle_program_close(program);
  } else if (status == CALL_OK) {
    uint64_t magic = 0;
    size_t offset = 0;
    while (status == CALL_OK && offset < sizeof(magic)) {
      size_t count = 0;
      status = file_read(target, offset, (char *)&magic + offset,
          sizeof(magic) - offset, &count);
      if (status == CALL_OK && (!count || count > sizeof(magic) - offset)) {
        status = CALL_BAD_REQUEST;
      }
      offset += count;
    }
    if (status == CALL_OK && magic != P1F_MAGIC) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (target != HANDLE_INVALID) {
    handle_close(target);
  }
  return status == CALL_OK || fail(reason, "declared command target or bundle policy is invalid");
}

static int compare_records(const void *left, const void *right)
{
  const struct package_command_record *a = left, *b = right;
  return strcmp(a->name, b->name);
}

bool pkg_commands_build(handle_t packages, const struct pkg_selection *selection,
    struct pkg_projection *projection, const char **reason)
{
  *projection = (struct pkg_projection){0};
  /* Commands fill the front; root records, at most one per selected package,
   * are gathered after the command limit and moved down behind the commands
   * once both are sorted. */
  if (selection->count > SIZE_MAX / sizeof(struct package_command_record) - PACKAGE_COMMANDS_LIMIT) {
    return fail(reason, "selected set is too large to index");
  }
  struct package_command_record *output =
      calloc(PACKAGE_COMMANDS_LIMIT + selection->count, sizeof(*output));
  if (!output) {
    return fail(reason, "cannot allocate command projection");
  }
  struct package_command_record *roots = output + PACKAGE_COMMANDS_LIMIT;
  bool ok = true;
  size_t used = 0, root_count = 0;
  for (size_t i = 0; ok && i < selection->count; ++i) {
    handle_t revision = HANDLE_INVALID, tree = HANDLE_INVALID;
    char *bytes = NULL;
    size_t size = 0;
    struct pkg_manifest manifest = {0};
    const struct pkg_identity *identity = &selection->entries[i];
    ok = pkg_tree_open(packages, identity, PKG_READ_RIGHTS, &revision, reason) &&
        pkg_text(revision, "package.lua", LITERAL_SOURCE_LIMIT, &bytes, &size, reason) &&
        pkg_manifest_parse(bytes, size, &manifest, reason);
    free(bytes);
    if (ok && (strcmp(manifest.identity.id, identity->id) ||
        strcmp(manifest.identity.version, identity->version))) {
      ok = fail(reason, "command manifest differs from its selected package");
    }
    if (ok && manifest.command_count > PACKAGE_COMMANDS_LIMIT - used) {
      ok = fail(reason, "selected commands exceed projection limit");
    }
    if (ok && manifest.command_count) {
      ok = pkg_directory(revision, "tree", false, PKG_READ_RIGHTS, &tree, reason);
    }
    /* Every package with files is read as pkg://ID, whatever its kind. One
     * without files, such as base, has no tree. */
    if (ok && manifest.files) {
      struct package_command_record *root = &roots[root_count++];
      root->kind = DIRECTORY_KIND_DIRECTORY;
      strcpy(root->name, identity->id);
      strcpy(root->id, identity->id);
      strcpy(root->version, identity->version);
    }
    for (size_t c = 0; ok && c < manifest.command_count; ++c) {
      const struct pkg_command *command = &manifest.commands[c];
      struct package_command_record *record = &output[used];
      ok = verify_target(tree, command, &record->kind, reason);
      if (ok) {
        strcpy(record->name, command->name);
        strcpy(record->id, identity->id);
        strcpy(record->version, identity->version);
        strcpy(record->path, command->path);
        strcpy(record->command, command->command);
        strcpy(record->abi, manifest.abi);
        ++used;
      }
    }
    pkg_manifest_free(&manifest);
    if (tree != HANDLE_INVALID) handle_close(tree);
    if (revision != HANDLE_INVALID) handle_close(revision);
  }
  if (ok) {
    qsort(output, used, sizeof(*output), compare_records);
    for (size_t i = 1; i < used; ++i) {
      if (!strcmp(output[i - 1].name, output[i].name)) {
        ok = fail(reason, "selected packages export the same command name");
        break;
      }
    }
  }
  if (!ok) {
    free(output);
    return false;
  }
  qsort(roots, root_count, sizeof(*roots), compare_records);
  memmove(output + used, roots, root_count * sizeof(*roots));
  *projection = (struct pkg_projection){output, used, root_count};
  return true;
}
