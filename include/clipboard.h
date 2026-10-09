#ifndef USERSPACE_CLIPBOARD_H
#define USERSPACE_CLIPBOARD_H

#include <abi/clipboard.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Separate explicitly delegated local/shared controller grants. Each operation
 * consumes one native action; none discovers or reads current clipboard data. */
enum call_status clipboard_publish(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, const void *bytes, size_t length);
enum call_status clipboard_paste(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, handle_t attachment,
    uint64_t *transaction_id);
enum call_status clipboard_clear(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity);
/* Consume a refused controller action without publishing or inserting data. */
enum call_status clipboard_refuse(handle_t clipboard, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, uint64_t operation);

#endif
