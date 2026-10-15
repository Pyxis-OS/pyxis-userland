#ifndef USERSPACE_LOG_H
#define USERSPACE_LOG_H

#include <abi/handle.h>
#include <abi/log.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrowed READ grant. Both helpers preserve caller outputs on failure. */
enum call_status log_get_snapshot(handle_t log, struct log_snapshot *snapshot);
/* Capacity is sizeof(struct log_record) + 1 to LOG_READ_MAX bytes of packed
 * records; see <abi/log.h>. Reuse reply->next as the next cursor; an empty
 * successful read is caught up. end is a snapshot boundary or
 * {UINT64_MAX, UINT64_MAX} for the current end. Records are validated. */
enum call_status log_read(handle_t log, struct log_cursor cursor,
    struct log_cursor end, void *bytes, size_t capacity, struct log_read_reply *reply);
/* Decode the record at *offset in the reply->size bytes of a successful
 * log_read and advance *offset past its text. False once all are consumed. */
bool log_next_record(const void *bytes, size_t size, size_t *offset,
    struct log_record *record, const char **text);

#endif
