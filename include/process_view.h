#ifndef USERSPACE_PROCESS_VIEW_H
#define USERSPACE_PROCESS_VIEW_H

#include <abi/handle.h>
#include <abi/process_view.h>
#include <abi/syscall.h>
#include <stddef.h>

/* A listing in caller storage: BUFFER holds the header, then `returned`
 * records. See abi/process_view.h for the row contract. */
struct process_list {
  const struct process_list_header *header;
  const struct process_list_record *records;
};

/* Lists the caller's own space, with no handle. BUFFER is CAPACITY bytes;
 * PROCESS_LIST_REPLY_MAX is the most a call uses. A short buffer still
 * succeeds with PROCESS_LIST_TRUNCATED set in the header. */
enum call_status process_list_own_space(void *buffer, size_t capacity, struct process_list *list);
/* Lists every space through a held process view with LIST. */
enum call_status process_list_all(handle_t view, void *buffer, size_t capacity,
    struct process_list *list);

#endif
