#ifndef USERSPACE_PANE_BROKER_H
#define USERSPACE_PANE_BROKER_H

#include <abi/endpoint.h>
#include <abi/process.h>
#include <pyxis/working_path.h>
#include <stdbool.h>

#define PANE_BROKER_MAGIC UINT64_C(0x70797862726f6b)
#define PANE_BROKER_TIMEOUT_NS UINT64_C(10000000000)
#define PANE_BROKER_PANES 8

enum pane_broker_operation {
  PANE_BROKER_BEGIN = 1,
  PANE_BROKER_CHUNK,
  PANE_BROKER_COMMIT,
  PANE_BROKER_ACCEPT,
  PANE_BROKER_CANCEL,
};

struct pane_broker_message {
  uint64_t magic, operation, id, deadline;
  uint64_t columns, rows, tab_width;
  uint64_t directory_count, path_size, directory_offset, path_offset;
  uint64_t directories, bytes;
};

struct pane_broker_chunk {
  struct pane_broker_message header;
  char path[ENDPOINT_DATA_MAX - sizeof(struct pane_broker_message)];
};

struct pane_broker_reply {
  uint64_t magic, operation, id, status;
};

struct pane_broker_result {
  handle_t attachment, process, group;
};

/* Both private channels have one reader and transferable SEND-only callers.
 * CLOSE every receipt, including rejected records. */
bool pane_broker_envelope(const struct endpoint_packet *packet);
bool pane_broker_grant(const struct endpoint_grant *grant, uint64_t protocol,
    uint64_t transport);
void pane_broker_packet_close(struct endpoint_packet *packet);
enum call_status pane_broker_receive(handle_t receiver, uint64_t deadline,
    struct endpoint_packet *packet);
bool pane_broker_cwd_fits(uint64_t directories, uint64_t path_size);

/* Bootstrap is a borrowed mux-owned SEND caller. Returned grants are owned.
 * OUTCOME_UNKNOWN after COMMIT is fatal to mux: its outer-group exit closes
 * admission and cleans any publication whose cancellation could not be sent. */
enum call_status pane_broker_start(size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, handle_t bootstrap,
    struct pane_broker_result *result);

#endif
