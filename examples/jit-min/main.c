/* jit-min: one process publishes, calls and rewrites its own x86-64 code.
 * The bundle explicitly requests JIT/manage; its space must enable jit.
 * No other thread enters the generated code while it is writable or released. */
#include <environment.h>
#include <handle.h>
#include <jit.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_INCREMENT_OFFSET 8

static void report_status(const char *operation, enum call_status status)
{
  fprintf(stderr, "jit-min: %s failed (status %u)%s\n", operation, (unsigned)status,
      status == CALL_OUTCOME_UNKNOWN ? "; outcome unknown, not retried" : "");
}

static bool protect_page(handle_t jit, uint64_t identity, size_t offset,
    enum jit_state state, const char *operation)
{
  enum call_status status = jit_protect(jit, identity, offset, JIT_PAGE_SIZE, state);
  if (status != CALL_OK) {
    report_status(operation, status);
    return false;
  }
  return true;
}

static bool run_code(handle_t jit, const struct jit_region *region)
{
  /* mov eax,[rip+4090]; add eax,0; ret. The load reads page 1, while
   * changing the add's immediate on page 0 changes the next result to 43. */
  static const uint8_t code[] = {0x8b, 0x05, 0xfa, 0x0f, 0x00, 0x00,
      0x83, 0xc0, 0x00, 0xc3};
  uint8_t *bytes = (void *)(uintptr_t)region->address;
  uint32_t *constants = (void *)(uintptr_t)(region->address + JIT_PAGE_SIZE);
  memcpy(bytes, code, sizeof(code));
  *constants = 42;

  if (!protect_page(jit, region->identity, JIT_PAGE_SIZE, JIT_READ_ONLY,
        "protect constants R/NX") ||
      !protect_page(jit, region->identity, 0, JIT_EXECUTABLE, "publish code R/X")) {
    return false;
  }
  unsigned (*generated)(void) = (void *)(uintptr_t)region->address;
  unsigned value = generated();
  if (value != 42 || *constants != 42) {
    fprintf(stderr, "jit-min: first result/constants %u/%u; expected 42/42\n", value, *constants);
    return false;
  }

  if (!protect_page(jit, region->identity, 0, JIT_WRITABLE, "withdraw code to RW/NX")) {
    return false;
  }
  bytes[CODE_INCREMENT_OFFSET] = 1;
  if (!protect_page(jit, region->identity, 0, JIT_EXECUTABLE, "republish code R/X")) {
    return false;
  }
  value = generated();
  if (value != 43 || *constants != 42) {
    fprintf(stderr, "jit-min: rewritten result/constants %u/%u; expected 43/42\n", value, *constants);
    return false;
  }
  return true;
}

int main(void)
{
  handle_t jit = HANDLE_INVALID;
  enum call_status status = environment_capability(startup_environment_handle(), "jit", &jit);
  if (status != CALL_OK) {
    fprintf(stderr, "jit-min: jit/manage is required (status %u); run the bundle in a space with jit = true\n",
        (unsigned)status);
    return EXIT_FAILURE;
  }
  struct jit_region region;
  status = jit_allocate(jit, 2 * JIT_PAGE_SIZE, 0, &region);
  if (status != CALL_OK) {
    report_status("allocate two pages; jit/manage and jit = true are required", status);
    if (handle_close(jit) != 0) {
      fputs("jit-min: cannot close the JIT grant\n", stderr);
    }
    return EXIT_FAILURE;
  }

  bool ok = run_code(jit, &region);
  /* A single release attempt follows the last generated call. On refusal or
   * an unknown outcome, process teardown owns any remaining backing. */
  status = jit_release(jit, region.identity);
  if (status != CALL_OK) {
    report_status("release", status);
    fputs("jit-min: process teardown handles any remaining region\n", stderr);
    ok = false;
  }
  if (handle_close(jit) != 0) {
    fputs("jit-min: cannot close the JIT grant\n", stderr);
    ok = false;
  }
  if (!ok) {
    return EXIT_FAILURE;
  }
  puts("jit-min: PASS (42, 43; region released)");
  return EXIT_SUCCESS;
}
