#include "upgrade.h"
#include "../installer/boot_publisher.h"
#include "../installer/esp_view.h"
#include "../installer/publication_snapshot.h"
#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <package_transition.h>
#include <random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVIDENCE_MAX_BYTES 512
#define CLEAR_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_REMOVE)
#define RETIRE_WAIT_NS UINT64_C(30000000000)

static bool digest_line(const char **cursor, const char *name, uint64_t *size, char sha256[65])
{
  size_t length = strlen(name);
  if (strncmp(*cursor, name, length) || (*cursor)[length] != ' ') {
    return false;
  }
  char *end;
  unsigned long long value = strtoull(*cursor + length + 1, &end, 10);
  if (end == *cursor + length + 1 || *end != ' ' || strspn(end + 1, "0123456789abcdef") != 64 ||
      end[65] != '\n') {
    return false;
  }
  *size = value;
  memcpy(sha256, end + 1, 64);
  sha256[64] = 0;
  *cursor = end + 66;
  return true;
}

bool pkg_boot_evidence_read(handle_t file, struct pkg_boot_evidence *evidence,
    const char **reason)
{
  *evidence = (struct pkg_boot_evidence){0};
  *reason = "unreadable boot evidence";
  char text[EVIDENCE_MAX_BYTES + 1];
  size_t used = 0;
  while (used < EVIDENCE_MAX_BYTES) {
    size_t count = 0;
    if (file_read(file, used, text + used, EVIDENCE_MAX_BYTES - used, &count) != CALL_OK) {
      return false;
    }
    if (!count) {
      break;
    }
    used += count;
  }
  text[used] = 0;
  static const char header[] = "PYXIS-BOOT-EVIDENCE 1\n";
  const char *cursor = text;
  if (strlen(text) != used || strncmp(cursor, header, sizeof(header) - 1)) {
    *reason = "malformed boot evidence";
    return false;
  }
  cursor += sizeof(header) - 1;
  char *end;
  bool ok = digest_line(&cursor, "kernel", &evidence->kernel_size, evidence->kernel_sha256) &&
      digest_line(&cursor, "archive", &evidence->archive_size, evidence->archive_sha256) &&
      digest_line(&cursor, "record", &evidence->record_size, evidence->record_sha256) &&
      !strncmp(cursor, "pool ", 5);
  unsigned long pool = ok ? strtoul(cursor + 5, &end, 10) : 0;
  if (!ok || !pool || pool > UINT32_MAX || *end != '\n' || end[1]) {
    *reason = "malformed boot evidence, or evidence from a boot-init without the pool line";
    return false;
  }
  evidence->pool = (uint32_t)pool;
  *reason = NULL;
  return true;
}

/* Directories nest only as deep as one staged release; each pass restarts
 * enumeration because removal changes the listing. */
static bool clear_children(handle_t directory, unsigned depth)
{
  char name[256];
  for (;;) {
    struct directory_cursor cursor = {0};
    struct directory_enumerate_reply entry;
    if (depth > 64 ||
        directory_enumerate(directory, &cursor, name, sizeof(name), &entry) != CALL_OK ||
        (entry.outcome != DIRECTORY_ENTRY && entry.outcome != DIRECTORY_END)) {
      return false;
    }
    if (entry.outcome == DIRECTORY_END) {
      return true;
    }
    if (entry.kind == DIRECTORY_KIND_DIRECTORY) {
      handle_t child;
      if (directory_lookup(directory, name, DIRECTORY_KIND_DIRECTORY, CLEAR_RIGHTS,
            &child) != CALL_OK) {
        return false;
      }
      bool cleared = clear_children(child, depth + 1);
      handle_close(child);
      if (!cleared) {
        return false;
      }
    }
    if (directory_remove(directory, name, entry.kind) != CALL_OK) {
      return false;
    }
  }
}

bool pkg_upgrade_clear(handle_t parent, const char *name)
{
  handle_t directory;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
      CLEAR_RIGHTS, &directory);
  if (status == CALL_NOT_FOUND) {
    return true;
  }
  if (status != CALL_OK) {
    return false;
  }
  bool ok = clear_children(directory, 0);
  handle_close(directory);
  return ok && directory_remove(parent, name, DIRECTORY_KIND_DIRECTORY) == CALL_OK &&
      directory_sync(parent) == CALL_OK;
}

static bool new_attempt(handle_t clock, handle_t random, char attempt[33])
{
  uint64_t now;
  uint8_t bytes[16];
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - RANDOM_MAX_WAIT_NS ||
      random_read(random, bytes, sizeof(bytes), now + RANDOM_MAX_WAIT_NS) != CALL_OK) {
    return false;
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    attempt[i * 2] = digits[bytes[i] >> 4];
    attempt[i * 2 + 1] = digits[bytes[i] & 15];
  }
  attempt[32] = 0;
  return true;
}

bool pkg_upgrade_publish(const struct pkg_upgrade_publish *request, const char **reason)
{
  struct mount_boot_reply boot;
  if (mount_boot_observe(request->issuer, &boot) != CALL_OK) {
    *reason = "cannot observe the boot ESP";
    return false;
  }
  if (!(boot.flags & MOUNT_BOOT_POOL_DISK) || memcmp(boot.pool_disk_guid, boot.disk_guid, 16)) {
    *reason = "the native pool is not on the disk that booted";
    return false;
  }
  char attempt[33], revision_record[80];
  if (!new_attempt(request->clock, request->random, attempt)) {
    *reason = "cannot name a fresh boot attempt";
    return false;
  }
  snprintf(revision_record, sizeof(revision_record), "%s\n", request->revision);

  /* The one admission of this boot: from here a failure needs a reboot to retry. */
  struct install_esp_view view = {0};
  enum call_status status = install_esp_view_admit_boot(&view, request->issuer, &boot,
      INSTALL_ESP_VIEW_RIGHTS);
  struct config_target_facts facts = {0};
  bool ok = status == CALL_OK && install_snapshot_facts(view.control, &facts);
  if (!ok) {
    fprintf(stderr, "pkg: boot ESP admission failed (status %u)\n", status);
    *reason = "the boot ESP was not admitted; reboot before trying again";
  }
  struct boot_publisher_input input = {.name = "pkg", .workspace = request->workspace,
    .clock = request->clock, .boot = HANDLE_INVALID, .loaded_kernel = HANDLE_INVALID,
    .kernel = {.handle = request->kernel, .bytes = request->kernel_size},
    .archive = {.handle = request->archive, .bytes = request->archive_size},
    .revision = request->revision, .attempt = attempt, .revision_record = revision_record,
    .disk_guid = boot.disk_guid, .pool_partition = request->evidence->pool, .target = facts};
  struct boot_publication publication = {0};
  if (ok && !boot_publication_prepare(&publication, &input, view.root, view.control)) {
    ok = false;
    *reason = "the boot ESP cannot take this release (see above); nothing was written";
  }
  /* The ESP must still hold the boot record this boot read. */
  if (ok && (!publication.record.digest.present ||
      publication.record.digest.size != request->evidence->record_size ||
      strcmp(publication.record.digest.sha256, request->evidence->record_sha256))) {
    ok = false;
    *reason = "the boot record changed since this boot; reboot before upgrading";
  }
  if (ok) {
    const struct config_boot_binding *binding = &publication.binding;
    struct package_transition transition = {.from_set = request->from_set,
      .to_set = request->to_set,
      .target = {.pool = request->evidence->pool,
        .kernel_size = binding->kernel.digest.size,
        .archive_size = binding->archive.digest.size}};
    memcpy(transition.target.attempt, attempt, sizeof(transition.target.attempt));
    memcpy(transition.target.kernel_sha256, binding->kernel.digest.sha256,
        sizeof(transition.target.kernel_sha256));
    memcpy(transition.target.archive_sha256, binding->archive.digest.sha256,
        sizeof(transition.target.archive_sha256));
    status = package_transition_publish(request->packages, &transition);
    if (status != CALL_OK) {
      ok = false;
      *reason = "cannot durably record the pending package set; no ESP payload was written";
    }
  }
  if (ok) {
    publication.prepared = true;
    boot_publication_report(&publication, &input, stdout, false);
  }
  if (ok && !boot_publication_admit(&publication, &input, &view)) {
    ok = false;
    *reason = "the boot ESP changed during review; nothing was written";
  }
  struct config_storage_report report = {0};
  if (ok && !boot_publication_publish(&publication, &input, &view, &report)) {
    ok = false;
    *reason = "publishing the release failed (see above); inspect boot selection before reboot";
  }
  boot_publication_destroy(&publication);
  uint64_t now = 0;
  if (view.attempted && (clock_now(request->clock, &now) != CALL_OK ||
      now > UINT64_MAX - RETIRE_WAIT_NS ||
      install_esp_view_close_and_retire(&view, request->clock, now + RETIRE_WAIT_NS) != CALL_OK)) {
    fputs("pkg: boot ESP retirement remains pending\n", stderr);
  }
  return ok;
}
