#include <directory.h>
#include <file.h>
#include <forwarder.h>
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdbool.h>

static bool close_owned(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool ok = handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return ok;
}

static bool close_pair(struct forwarder_reply *pair)
{
  bool holder = close_owned(&pair->holder);
  bool controller = close_owned(&pair->controller);
  return holder && controller;
}

static bool revoked(handle_t file)
{
  uint64_t size;
  return file_size(file, &size) == CALL_DENIED;
}

static bool size_matches(handle_t file, uint64_t expected)
{
  uint64_t size;
  return file_size(file, &size) == CALL_OK && size == expected;
}

/* Explicit manual qualification on two existing native files. It creates no
 * fixture and changes neither file; callers select the root and components. */
int counter_forwarders_main(const char *scheme, const char *name, const char *replacement)
{
  handle_t root = startup_root(scheme);
  handle_t first = HANDLE_INVALID, second = HANDLE_INVALID;
  handle_t child = HANDLE_INVALID, child_copy = HANDLE_INVALID;
  handle_t holder_copy = HANDLE_INVALID, supervisor = HANDLE_INVALID;
  handle_t weak = HANDLE_INVALID;
  struct forwarder_reply directory = {0}, nested = {0}, automatic = {0}, slot = {0};
  uint64_t first_size = 0, second_size = 0;
  const char *phase = "selected native files";
  bool ok = root != HANDLE_INVALID &&
      directory_lookup(root, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &first) == CALL_OK &&
      directory_lookup(root, replacement, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &second) == CALL_OK &&
      file_size(first, &first_size) == CALL_OK && file_size(second, &second_size) == CALL_OK;
  if (ok) {
    phase = "directory child and nested lineage";
    ok = forwarder_revocable(root, DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES,
        0, 0, &directory) == CALL_OK &&
        forwarder_revocable(directory.holder, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &nested) == CALL_OK &&
        directory_lookup(nested.holder, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &child) == CALL_OK && handle_copy(child, &child_copy) == CALL_OK &&
        size_matches(child_copy, first_size) && forwarder_cut(directory.controller) == CALL_OK &&
        revoked(child) && revoked(child_copy) && size_matches(first, first_size);
  }
  if (ok) {
    handle_t denied = HANDLE_INVALID;
    ok = directory_lookup(nested.holder, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &denied) == CALL_DENIED && denied == HANDLE_INVALID;
  }
  if (ok) {
    phase = "last logical controller release";
    ok = forwarder_revocable(first, 0, 0, FORWARDER_CREATE_SAME_RIGHTS,
        &automatic) == CALL_OK && handle_copy(automatic.controller, &supervisor) == CALL_OK &&
        handle_copy(automatic.holder, &holder_copy) == CALL_OK &&
        close_owned(&automatic.controller) && size_matches(holder_copy, first_size) &&
        close_owned(&supervisor) && revoked(automatic.holder) && revoked(holder_copy);
  }
  if (ok) {
    phase = "slot retarget and supervisor takeover";
    ok = forwarder_slot(first, FILE_RIGHT_READ, 0, 0, &slot) == CALL_OK &&
        handle_copy(slot.controller, &supervisor) == CALL_OK &&
        close_owned(&slot.controller) && size_matches(slot.holder, first_size) &&
        forwarder_retarget(supervisor, second) == CALL_OK &&
        size_matches(slot.holder, second_size) &&
        forwarder_retarget(supervisor, root) == CALL_WRONG_TYPE &&
        handle_copy_restricted(first, 0, 0, &weak) == CALL_OK &&
        forwarder_retarget(supervisor, weak) == CALL_DENIED &&
        size_matches(slot.holder, second_size) &&
        forwarder_retarget(supervisor, HANDLE_INVALID) == CALL_OK;
  }
  if (ok) {
    phase = "empty slot and unmanaged target retention";
    uint64_t size;
    ok = file_size(slot.holder, &size) == CALL_NOT_FOUND &&
        forwarder_retarget(supervisor, first) == CALL_OK &&
        size_matches(slot.holder, first_size) && close_owned(&supervisor) &&
        size_matches(slot.holder, first_size);
  }
  if (ok) {
    phase = "terminal slot cut";
    ok = close_pair(&slot) && forwarder_slot(first, FILE_RIGHT_READ, 0, 0, &slot) == CALL_OK &&
        forwarder_cut(slot.controller) == CALL_OK && revoked(slot.holder) &&
        forwarder_retarget(slot.controller, second) == CALL_DENIED;
  }
  bool cleanup = close_pair(&directory);
  cleanup = close_pair(&nested) && cleanup;
  cleanup = close_pair(&automatic) && cleanup;
  cleanup = close_pair(&slot) && cleanup;
  cleanup = close_owned(&child) && cleanup;
  cleanup = close_owned(&child_copy) && cleanup;
  cleanup = close_owned(&holder_copy) && cleanup;
  cleanup = close_owned(&supervisor) && cleanup;
  cleanup = close_owned(&weak) && cleanup;
  cleanup = close_owned(&first) && cleanup;
  cleanup = close_owned(&second) && cleanup;
  if (!cleanup && ok) {
    phase = "owned handle cleanup";
  }
  ok = ok && cleanup;
  if (ok) {
    fputs("forwarders: lineage, cut, controller release and slot takeover complete\n", stdout);
  } else {
    fprintf(stderr, "forwarders: qualification failed at %s\n", phase);
  }
  return ok ? 0 : 1;
}
