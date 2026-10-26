#ifndef USERSPACE_ENVIRONMENT_H
#define USERSPACE_ENVIRONMENT_H

#include <abi/environment.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <abi/launcher.h>

enum environment_stream_index {
  ENVIRONMENT_STDIN, ENVIRONMENT_STDOUT, ENVIRONMENT_STDERR, ENVIRONMENT_STREAM_COUNT,
};

struct pyxis_stdio_binding {
  uint64_t protocol;
  handle_t handle;
  bool append;
};

#ifdef __cplusplus
extern "C" {
#endif

/* LOOKUP and ENUMERATE allocate no memory and use no libc or TLS state. CAP
 * replies own a grant. TEXT lengths exclude NUL; BUFFER_TOO_SMALL returns the
 * metadata, with no grant or text publication. */
bool environment_name(char target[ENVIRONMENT_NAME_MAX + 1], const char *name);
enum call_status environment_lookup(handle_t table, const char *name,
    struct environment_entry_reply *entry, char *text, size_t capacity);
/* Captures a CAP unchanged or a TEXT value as an owned immutable native READ
 * FILE from the same publication. No borrowed table storage or second lookup. */
enum call_status environment_lookup_file(handle_t table, const char *name, handle_t *handle);
enum call_status environment_enumerate(handle_t snapshot, size_t index,
    struct environment_entry_reply *entry, char *text, size_t capacity);
enum call_status environment_set_batch(handle_t table,
    const struct environment_set_entry *entries, size_t count);
enum call_status environment_unset_batch(handle_t table,
    const struct environment_unset_entry *entries, size_t count);
enum call_status environment_set_capability(handle_t table, const char *name,
    handle_t source, uint64_t rights, uint64_t transport, uint64_t flags);
enum call_status environment_set_text(handle_t table, const char *name,
    const char *text, uint64_t flags, uint64_t set_flags);
enum call_status environment_unset(handle_t table, const char *name);
enum call_status environment_info(handle_t table, struct environment_info_reply *info);
enum call_status environment_snapshot(handle_t table, uint64_t flags,
    handle_t *snapshot, struct environment_info_reply *info);

struct environment_value {
  char name[ENVIRONMENT_NAME_MAX + 1];
  uint64_t kind, flags, identity;
  handle_t handle;
  uint64_t rights, transport;
  char *text;
  size_t length;
};

struct environment_capture {
  struct environment_value *values;
  size_t count;
  uint64_t generation;
};

/* Append an explicitly selected immutable capture to launch arrays. Names,
 * text and sources stay borrowed until synchronous launch capture completes.
 * Failure leaves both counts unchanged. */
enum call_status environment_capture_stage(const struct environment_capture *capture,
    struct launch_grant *grants, size_t grant_capacity, size_t *grant_count,
    struct launch_environment_entry *entries, size_t entry_capacity, size_t *entry_count);

/* Heap-owned immutable selection. Each CAP and TEXT has independent ownership;
 * close the record once. Borrow find results only while the record is live. */
enum call_status environment_capture_init(handle_t table, uint64_t flags,
    struct environment_capture *capture);
void environment_capture_close(struct environment_capture *capture);
const struct environment_value *environment_capture_find(
    const struct environment_capture *capture, const char *name);
enum call_status environment_capability(handle_t table, const char *name, handle_t *handle);
/* Append is output FILE policy, not a resource right. Each native operation
 * selects EOF atomically; larger flushes are not record-atomic.
 * Launch it as the optional TEXT companion stdout.append/stderr.append="1";
 * an absent companion selects ordinary writes. Stage an entry only for an
 * output stream with append set; it borrows static bytes. */
const char *environment_stdio_append_name(enum environment_stream_index index);
struct launch_environment_entry environment_stdio_append_entry(enum environment_stream_index index);
/* Captures owned stream grants from one snapshot. Append rejects stdin,
 * malformed companions, non-FILEs and FILEs without WRITE or working SIZE. */
enum call_status environment_stdio(handle_t snapshot, enum environment_stream_index index,
    struct pyxis_stdio_binding *binding, uint64_t *identity);

#ifdef __cplusplus
}
#endif

#endif
