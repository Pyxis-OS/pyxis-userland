#include "../common/bootstrap_environment.h"
#include <disk.h>
#include <mount.h>
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failure(const char *operation, enum call_status status)
{
  fprintf(stderr, "fat32ctl: %s failed (status %u)\n", operation, status);
  return EXIT_FAILURE;
}

static int list(handle_t inventory)
{
  if (inventory == HANDLE_INVALID) {
    return failure("inventory unavailable", CALL_UNAVAILABLE);
  }
  for (uint64_t i = 0;; ++i) {
    struct disk_info info;
    enum call_status status = disks_enumerate(inventory, i, &info);
    if (status == CALL_NOT_FOUND) {
      return EXIT_SUCCESS;
    }
    if (status != CALL_OK) {
      return failure("list", status);
    }
    if (printf("disk %llu: %llu blocks of %llu bytes, preparation %llu, GPT %llu, %s\n",
        (unsigned long long)info.id, (unsigned long long)info.block_count,
        (unsigned long long)info.block_size, (unsigned long long)info.preparation,
        (unsigned long long)info.gpt_status,
        info.flags & DISK_FLAG_WRITABLE ? "writable" : "read-only") < 0) {
      return EXIT_FAILURE;
    }
  }
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    fputs("usage: fat32ctl list | state | sync | retire\n", stderr);
    return EXIT_FAILURE;
  }
  const char *operation = argv[1];
  if (!strcmp(operation, "list")) {
    return list(bootstrap_capability("inventory"));
  }
  bool state = !strcmp(operation, "state");
  bool sync = !strcmp(operation, "sync");
  bool retire = !strcmp(operation, "retire");
  if (!state && !sync && !retire) {
    fputs("usage: fat32ctl list | state | sync | retire\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t control = bootstrap_capability(sync ? "sync_control" : "control");
  if (control == HANDLE_INVALID) {
    return failure(operation, CALL_DENIED);
  }
  struct handle_info authority;
  enum call_status status = handle_query(control, &authority);
  if (status != CALL_OK) {
    return failure(operation, status);
  }
  if (printf("received %s: rights 0x%llx\n", sync ? "sync_control" : "control",
      (unsigned long long)authority.rights) < 0) {
    return EXIT_FAILURE;
  }
  if (state) {
    struct mount_state_reply observed;
    status = mount_observe(control, &observed);
    if (status == CALL_OK && printf("domain %llu, disk %llu, GPT entry %llu, "
        "blocks [%llu, %llu), %s%s%s%s%s\n"
        "failure status %llu, phase %llu, sector %llu; detached objects %llu\n",
        (unsigned long long)observed.domain, (unsigned long long)observed.device,
        (unsigned long long)observed.partition, (unsigned long long)observed.first_block,
        (unsigned long long)(observed.first_block + observed.block_count),
        observed.flags & MOUNT_STATE_READ_WRITE ? "read-write" : "read-only",
        observed.flags & MOUNT_STATE_RETIRED ? ", retired" : ", active",
        observed.flags & MOUNT_STATE_DIRTY_OWNED ? ", dirty owned" : "",
        observed.flags & MOUNT_STATE_FAILED ? ", failed" : "",
        (observed.flags & MOUNT_STATE_READ_WRITE) &&
        (observed.flags & MOUNT_STATE_ALLOCATION_INVALID) ? ", allocation proof invalid" : "",
        (unsigned long long)observed.failure_status,
        (unsigned long long)observed.failure_phase,
        (unsigned long long)observed.failed_sector,
        (unsigned long long)observed.detached_objects) < 0) {
      return EXIT_FAILURE;
    }
  } else {
    status = sync ? mount_sync(control) : mount_retire(control);
  }
  return status == CALL_OK ? EXIT_SUCCESS : failure(operation, status);
}
