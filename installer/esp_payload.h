#ifndef USERSPACE_INSTALLER_ESP_PAYLOAD_H
#define USERSPACE_INSTALLER_ESP_PAYLOAD_H

#include "installer.h"
#include <abi/mount.h>

#define INSTALL_ESP_PAYLOAD_COUNT 4u
#define INSTALL_ESP_PAYLOAD_MAX_BYTES UINT64_C(1073741824)
#define INSTALL_ESP_PAYLOAD_PATH_BYTES 192u
#define INSTALL_ESP_PAYLOAD_MANIFEST "ownership.record"

enum install_esp_payload_index {
  INSTALL_ESP_KERNEL, INSTALL_ESP_ARCHIVE, INSTALL_ESP_REVISION, INSTALL_ESP_ENTRY, INSTALL_ESP_OWNERSHIP
};

struct install_esp_payload_digest {
  uint64_t size;
  char sha256[65];
};

struct install_esp_payload_request {
  handle_t root, control;
  /* Frozen, issuer-bound OBSERVE facts, not caller-authored authority. */
  struct mount_state_reply expected;
  const char *revision, *attempt;
  struct install_source sources[INSTALL_ESP_PAYLOAD_COUNT];
  struct install_esp_payload_digest expected_payloads[INSTALL_ESP_PAYLOAD_COUNT];
  bool entry_present;
};

struct install_esp_payload_record {
  char revision[64], attempt[33];
  char paths[INSTALL_ESP_PAYLOAD_COUNT][INSTALL_ESP_PAYLOAD_PATH_BYTES];
  struct install_esp_payload_digest payloads[INSTALL_ESP_PAYLOAD_COUNT];
  char manifest_path[INSTALL_ESP_PAYLOAD_PATH_BYTES];
  struct install_esp_payload_digest manifest;
  bool entry_present, attempt_created, staged_verified;
};

enum install_esp_payload_phase {
  INSTALL_ESP_PAYLOAD_CHECK, INSTALL_ESP_PAYLOAD_STAGE,
  INSTALL_ESP_PAYLOAD_SYNC, INSTALL_ESP_PAYLOAD_VERIFY, INSTALL_ESP_PAYLOAD_DONE
};

struct install_esp_payload_report {
  enum call_status status;
  enum install_esp_payload_phase phase;
  char artifact[INSTALL_ESP_PAYLOAD_PATH_BYTES];
};

struct install_esp_payloads;

/* Pure plan-time layout; a path is descriptive and confers no authority. */
bool install_esp_payload_path(const char *revision, const char *attempt,
    enum install_esp_payload_index index, char path[INSTALL_ESP_PAYLOAD_PATH_BYTES]);

/* Initialize *result to NULL. The request, source descriptors and bytes stay
 * unchanged through stage. Retain the hash_init() context through the final
 * verify call. Root/control are borrowed and must outlive the result. The helper verifies the
 * bound root and reviewed target, creates one fresh attempt, syncs and reopens
 * each retained object before checking its exact extent/hash. No REMOVE rights,
 * raw access, admission, BEGIN/CHECK/END, configuration or activation.
 * Root and traversal handles need ENUMERATE to check canonical parent names;
 * an 8.3 alias of another name is refused. Stage is synchronous and does not
 * poll cancellation input after the caller confirms Apply.
 *
 * On failure *result may still own partial evidence: close it before retiring
 * the caller's mount. A failed create can have effects regardless of status.
 * Never retry, replace or delete the attempt automatically. */
bool install_esp_payload_stage(const struct install_esp_payload_request *request,
    struct install_esp_payloads **result, struct install_esp_payload_report *report);
/* Rechecks issuer-bound facts, canonical names, retained identities and bytes.
 * Source handles/memory are no longer needed. No writes or interval management. */
bool install_esp_payload_verify(const struct install_esp_payloads *result,
    struct install_esp_payload_report *report);
const struct install_esp_payload_record *install_esp_payload_record(
    const struct install_esp_payloads *result);
/* Releases only held handles and memory; never removes artifacts or retires.
 * Always frees RESULT (NULL succeeds). False reports an invalid/already-absent
 * owned handle. Native CLOSE has no fallible filesystem operation: valid
 * handles are removed synchronously; absent handles need no retry. */
bool install_esp_payload_close(struct install_esp_payloads *result);

#endif
