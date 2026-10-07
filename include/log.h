#ifndef USERSPACE_LOG_H
#define USERSPACE_LOG_H

#include <abi/handle.h>
#include <abi/log.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrowed READ grant. Both helpers preserve caller outputs on failure. */
enum call_status log_get_snapshot(handle_t log, struct log_snapshot *snapshot);
/* Capacity is 1..LOG_READ_MAX text bytes. Reuse reply->next as the next cursor;
 * an empty successful read is caught up. end is a snapshot boundary or
 * {UINT64_MAX, UINT64_MAX} for the current end. */
enum call_status log_read(handle_t log, struct log_cursor cursor,
    struct log_cursor end, void *bytes, size_t capacity, struct log_read_reply *reply);

#endif
