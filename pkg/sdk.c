#include "sdk.h"
#include "archive.h"
#include <abi/package_sdk.h>
#include <handle.h>
#include <literal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sdk_file {
  char *path;
  uint32_t package;
};

struct sdk_build {
  struct pkg_identity *packages;
  size_t package_count;
  struct sdk_file *files;
  size_t file_count, file_capacity;
};

/* Conflicts name two packages and a path, so their reason is formatted here. */
static char conflict[2 * PACKAGE_COMMAND_ID_MAX + PACKAGE_SDK_PATH_MAX + 64];

static bool fail(const char **reason, const char *message)
{
  *reason = message;
  return false;
}

static int compare_identities(const void *left, const void *right)
{
  return strcmp(((const struct pkg_identity *)left)->id, ((const struct pkg_identity *)right)->id);
}

static int compare_files(const void *left, const void *right)
{
  return strcmp(((const struct sdk_file *)left)->path, ((const struct sdk_file *)right)->path);
}

/* include/..., lib/NAME or lib/pkgconfig/NAME. */
static bool view_path(const char *path)
{
  if (!strncmp(path, "include/", 8)) {
    return true;
  }
  if (strncmp(path, "lib/", 4)) {
    return false;
  }
  const char *rest = path + 4;
  if (!strchr(rest, '/')) {
    return true;
  }
  return !strncmp(rest, "pkgconfig/", 10) && !strchr(rest + 10, '/');
}

static bool add_file(struct sdk_build *build, const char *path, uint32_t package,
    const char **reason)
{
  if (build->file_count == build->file_capacity) {
    size_t capacity = build->file_capacity ? build->file_capacity * 2 : 256;
    struct sdk_file *files = realloc(build->files, capacity * sizeof(*files));
    if (!files) {
      return fail(reason, "cannot allocate the sdk index");
    }
    build->files = files;
    build->file_capacity = capacity;
  }
  char *copy = strdup(path);
  if (!copy) {
    return fail(reason, "cannot allocate the sdk index");
  }
  build->files[build->file_count++] = (struct sdk_file){copy, package};
  return true;
}

/* The selected dev packages, sorted by ID. */
static bool gather_packages(handle_t packages, const struct pkg_selection *selection,
    struct sdk_build *build, const char **reason)
{
  build->packages = calloc(selection->count ? selection->count : 1, sizeof(*build->packages));
  if (!build->packages) {
    return fail(reason, "cannot allocate the sdk index");
  }
  for (size_t i = 0; i < selection->count; ++i) {
    handle_t revision = HANDLE_INVALID;
    struct pkg_manifest manifest = {0};
    bool ok = pkg_tree_open(packages, &selection->entries[i], PKG_READ_RIGHTS, &revision, reason) &&
        pkg_tree_manifest(revision, &manifest, reason);
    if (revision != HANDLE_INVALID) {
      handle_close(revision);
    }
    if (ok && !strcmp(manifest.kind, "dev")) {
      build->packages[build->package_count++] = selection->entries[i];
    }
    pkg_manifest_free(&manifest);
    if (!ok) {
      return false;
    }
  }
  qsort(build->packages, build->package_count, sizeof(*build->packages), compare_identities);
  return true;
}

static bool gather_files(handle_t packages, struct sdk_build *build, const char **reason)
{
  for (size_t i = 0; i < build->package_count; ++i) {
    handle_t revision = HANDLE_INVALID;
    if (!pkg_tree_open(packages, &build->packages[i], PKG_READ_RIGHTS, &revision, reason)) {
      return false;
    }
    struct pkg_inventory inventory;
    bool ok = pkg_inventory_open(revision, &inventory, reason);
    handle_close(revision);
    char path[PKG_PATH_MAX + 1];
    while (ok && pkg_inventory_next(&inventory, path, reason)) {
      ok = !view_path(path) || add_file(build, path, (uint32_t)i, reason);
    }
    if (ok && *reason) {
      ok = false;
    }
    pkg_inventory_close(&inventory);
    if (!ok) {
      return false;
    }
  }
  return true;
}

static struct sdk_file *find(const struct sdk_build *build, const char *path)
{
  struct sdk_file key = {(char *)path, 0};
  return bsearch(&key, build->files, build->file_count, sizeof(*build->files), compare_files);
}

/* Each path has one owner, and no file is also another's directory. */
static bool conflicts_check(const struct sdk_build *build, const char **reason)
{
  for (size_t i = 1; i < build->file_count; ++i) {
    const struct sdk_file *a = &build->files[i - 1], *b = &build->files[i];
    if (!strcmp(a->path, b->path)) {
      snprintf(conflict, sizeof(conflict), "%s and %s both provide sdk://%s",
          build->packages[a->package].id, build->packages[b->package].id, a->path);
      return fail(reason, conflict);
    }
  }
  for (size_t i = 0; i < build->file_count; ++i) {
    char prefix[PKG_PATH_MAX + 1];
    strcpy(prefix, build->files[i].path);
    for (char *slash = strchr(prefix, '/'); slash; slash = strchr(slash + 1, '/')) {
      *slash = 0;
      const struct sdk_file *file = find(build, prefix);
      if (file) {
        snprintf(conflict, sizeof(conflict), "%s provides sdk://%s as a file, %s as a directory",
            build->packages[file->package].id, prefix, build->packages[build->files[i].package].id);
        return fail(reason, conflict);
      }
      *slash = '/';
    }
  }
  return true;
}

static bool serialize(const struct sdk_build *build, struct pkg_sdk *sdk, const char **reason)
{
  uint64_t strings = 0;
  for (size_t i = 0; i < build->file_count; ++i) {
    strings += strlen(build->files[i].path) + 1;
  }
  uint64_t size = sizeof(struct package_sdk_index_header) +
      (uint64_t)build->package_count * sizeof(struct package_sdk_package) +
      (uint64_t)build->file_count * sizeof(struct package_sdk_file) + strings;
  /* The kernel holds a captured index in memory and refuses one past this
   * bound, so a set that large is refused here, before it is published. */
  if (size > PACKAGE_SDK_INDEX_MAX_BYTES) {
    return fail(reason, "the set's dev packages have too many files for the sdk index bound");
  }
  uint8_t *bytes = calloc(1, (size_t)size);
  if (!bytes) {
    return fail(reason, "cannot allocate the sdk index");
  }
  struct package_sdk_index_header *header = (void *)bytes;
  memcpy(header->magic, PACKAGE_SDK_INDEX_MAGIC, sizeof(header->magic));
  header->packages = (uint32_t)build->package_count;
  header->files = (uint32_t)build->file_count;
  header->strings = (uint32_t)strings;
  struct package_sdk_package *packages = (void *)(header + 1);
  for (size_t i = 0; i < build->package_count; ++i) {
    strcpy(packages[i].id, build->packages[i].id);
    strcpy(packages[i].version, build->packages[i].version);
  }
  struct package_sdk_file *files = (void *)(packages + build->package_count);
  char *text = (char *)(files + build->file_count);
  uint32_t offset = 0;
  for (size_t i = 0; i < build->file_count; ++i) {
    size_t length = strlen(build->files[i].path);
    files[i] = (struct package_sdk_file){.path = offset, .path_length = (uint32_t)length,
      .package = build->files[i].package};
    memcpy(text + offset, build->files[i].path, length + 1);
    offset += (uint32_t)length + 1;
  }
  *sdk = (struct pkg_sdk){bytes, (size_t)size};
  return true;
}

bool pkg_sdk_build(handle_t packages, const struct pkg_selection *selection,
    struct pkg_sdk *sdk, const char **reason)
{
  *sdk = (struct pkg_sdk){0};
  struct sdk_build build = {0};
  bool ok = gather_packages(packages, selection, &build, reason) &&
      gather_files(packages, &build, reason);
  if (ok) {
    qsort(build.files, build.file_count, sizeof(*build.files), compare_files);
    ok = conflicts_check(&build, reason) && serialize(&build, sdk, reason);
  }
  for (size_t i = 0; i < build.file_count; ++i) {
    free(build.files[i].path);
  }
  free(build.files);
  free(build.packages);
  return ok;
}
